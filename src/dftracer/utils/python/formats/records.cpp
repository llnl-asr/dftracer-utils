// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/memory/config.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/formats/format.h>
#include <dftracer/utils/python/formats/records.h>
#include <dftracer/utils/python/formats/text_source.h>
#include <dftracer/utils/python/formats/value.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::python::formats {

using core::g_dft_error;
using core::g_dft_reader_error;
using core::release_gil;

namespace {

constexpr std::size_t READ_BLOCK_MIN = std::size_t{64} << 10;
constexpr std::size_t READ_BLOCK_MAX = std::size_t{1} << 20;

// What a record costs a batch beside its parsed form and its text: its entries
// in the tables of the batch.
constexpr std::size_t RECORD_OVERHEAD = 96;

// How many bytes a batch holds for each byte of the text it was cut from: the
// text, the copy kept for `raw` and the parsed form of a YAML record.
constexpr std::size_t YAML_COST_FACTOR = 5;

// Bytes of memory that parsing one byte of a whole text needs, at most.
template <class F>
constexpr std::size_t PARSE_FACTOR = std::is_same_v<F, JsonFormat> ? 13 : 7;

// The source text of the records of a batch, which counts in its size.
struct BatchText {
    struct Span {
        std::size_t offset;
        std::size_t size;
    };

    dstd::String text;
    dstd::Vector<Span> spans;
    int style = 0;

    RawSpan raw_at(std::size_t i) const {
        if (i >= spans.size()) return {};
        return {text.data() + spans[i].offset, spans[i].size, style};
    }
};

template <class F>
struct BatchData;

template <>
struct BatchData<JsonFormat> : BatchText {
    dstd::Vector<json::DomDocument> docs;
    dstd::Vector<json::DomValue> vals;
};

template <>
struct BatchData<YamlFormat> : BatchText {
    dstd::Optional<yaml::Records> records;
    dstd::Vector<yaml::Value> vals;
};

template <class F>
struct BatchObject {
    PyObject_HEAD dstd::UniquePtr<BatchData<F>> data;
};

// Marks the iterator in use while the GIL is released, so a second thread that
// calls it meanwhile gets an error instead of a data race.
class BusyScope {
   public:
    explicit BusyScope(bool &flag) : flag_(flag) { flag_ = true; }
    ~BusyScope() { flag_ = false; }
    BusyScope(const BusyScope &) = delete;
    BusyScope &operator=(const BusyScope &) = delete;

   private:
    bool &flag_;
};

template <class F>
struct ReaderState;

template <class F>
struct IteratorObject {
    PyObject_HEAD dstd::UniquePtr<ReaderState<F>> state;
    bool done;
    bool busy;
};

template <class F>
PyTypeObject g_batch_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
PyTypeObject g_iterator_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
dstd::String g_batch_name;
template <class F>
dstd::String g_iterator_name;

void raise_reader(dstd::StringView message) {
    PyObject *text = make_str(message);
    if (!text) return;
    PyErr_SetObject(g_dft_reader_error, text);
    Py_DECREF(text);
}

// An error of a read: a parse error is a reader error, a missing file is a
// FileNotFoundError, and any other is typed by its code.
void raise(const dstd::Error &error, dstd::StringView path) {
    if (error.condition() == dstd::Condition::NotFound) {
        PyObject *text = make_str(path);
        if (text) {
            PyErr_SetObject(PyExc_FileNotFoundError, text);
            Py_DECREF(text);
        }
    } else if (error.condition() == dstd::Condition::Parse ||
               error.condition() == dstd::Condition::InvalidArgument) {
        raise_reader(error.message());
    } else {
        core::set_py_error(error);
    }
}

// The path argument as text: a str or an os.PathLike of one.
bool path_of(PyObject *arg, dstd::String &out) {
    PyObject *fs = PyOS_FSPath(arg);
    if (!fs) return false;
    PyObject *text = PyUnicode_Check(fs) ? fs : nullptr;
    if (!text) {
        PyErr_SetString(PyExc_TypeError, "path must be str or PathLike[str]");
        Py_DECREF(fs);
        return false;
    }
    Py_ssize_t size = 0;
    const char *chars = PyUnicode_AsUTF8AndSize(text, &size);
    if (chars) out.assign(chars, static_cast<std::size_t>(size));
    Py_DECREF(fs);
    return chars != nullptr;
}

// A reader error message for a line of a file: the parser counted from 1 in
// the text it was given, which starts at `first_line` of the file.
dstd::String locate(dstd::StringView message, dstd::StringView path,
                    dstd::StringView prefix, std::uint64_t first_line) {
    dstd::String out(path);
    out += ':';
    dstd::StringView rest = message;
    if (rest.starts_with(prefix)) rest.remove_prefix(prefix.size());
    std::size_t digits = 0;
    std::uint64_t line = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
        line = line * 10 + static_cast<std::uint64_t>(rest[digits++] - '0');
    if (digits == 0 || digits >= rest.size() || rest[digits] != ':') {
        dstd::fmt_append<"{}: {}">(out, first_line, rest);
        return out;
    }
    dstd::fmt_append<"{}{}">(out, first_line + line - 1, rest.substr(digits));
    return out;
}

// Reads the whole text of a file, refusing one that the memory budget cannot
// hold once parsed.
template <class F>
dstd::Result<dstd::String> read_whole(dstd::StringView path) {
    auto source = TextSource::open(path);
    if (!source) return dstd::Unexpected<dstd::Error>(source.error());
    const std::size_t budget = memory::config().budget;
    const std::size_t factor = PARSE_FACTOR<F>;
    const auto too_big = [&](std::uint64_t bytes) {
        return common::make_error<
            "{}: {} bytes need about {} bytes to parse and the memory budget "
            "is {} bytes; read it in batches with read_records">(
            common::ErrorCode::MemoryBudgetExceeded, path, bytes,
            bytes * factor, budget);
    };
    if (!source->is_gzip() && budget != 0 &&
        source->file_size() * factor > budget)
        return too_big(source->file_size());
    dstd::String text;
    std::size_t have = 0;
    if (!source->is_gzip()) {
        // A plain file is read at its size in one string, and one more byte
        // is asked for to see that the file did not grow.
        text.resize(source->file_size());
        while (have < text.size()) {
            auto got = source->read(text.data() + have, text.size() - have);
            if (!got) return dstd::Unexpected<dstd::Error>(got.error());
            if (*got == 0) break;
            have += *got;
        }
        text.resize(have);
        if (have == source->file_size()) {
            char extra = 0;
            auto got = source->read(&extra, 1);
            if (!got) return dstd::Unexpected<dstd::Error>(got.error());
            if (*got == 0) return text;
            text.push_back(extra);
            have = text.size();
        }
    } else {
        have = 0;
    }
    for (;;) {
        text.resize(have + std::max<std::size_t>(READ_BLOCK_MIN, have / 2));
        auto got = source->read(text.data() + have, text.size() - have);
        if (!got) return dstd::Unexpected<dstd::Error>(got.error());
        have += *got;
        text.resize(have);
        if (*got == 0) break;
        if (budget != 0 && text.size() * factor > budget)
            return too_big(text.size());
    }
    return text;
}

template <class F>
dstd::Result<typename F::Doc> parse_text(dstd::StringView text,
                                         dstd::StringView source_name) {
    if constexpr (std::is_same_v<F, JsonFormat>) {
        auto doc = json::DomDocument::parse(text);
        if (!doc) {
            if (source_name.empty()) return doc;
            return common::make_error(
                common::ErrorCode::Parse,
                locate(doc.error().message(), source_name, "", 1));
        }
        return doc;
    } else {
        return yaml::Document::parse(dstd::String(text), source_name);
    }
}

// The root of a parsed document as a Value that keeps the document and its
// source text alive. A JSON root writes as its text without the white space
// around it.
template <class F>
PyObject *document_root(typename F::Doc &&doc, dstd::String &&source) {
    PyObject *owner = make_document<F>(std::move(doc), std::move(source));
    if (!owner) return nullptr;
    auto *object = reinterpret_cast<DocumentObject<F> *>(owner);
    dstd::StringView text(object->source);
    if constexpr (std::is_same_v<F, JsonFormat>) {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        const std::size_t last = text.find_last_not_of(" \t\r\n");
        text = first == dstd::StringView::npos
                   ? dstd::StringView()
                   : text.substr(first, last - first + 1);
    }
    PyObject *root = make_value<F>(owner, object->doc->root(),
                                   RawSpan{text.data(), text.size(), 0});
    Py_DECREF(owner);
    return root;
}

template <class F>
PyObject *do_parse(PyObject *, PyObject *args, PyObject *kwds) {
    PyObject *text = nullptr;
    if (!core::parse_kw_for<"O!">(F::PARSE, args, kwds, {"text"},
                                  &PyUnicode_Type, &text))
        return nullptr;
    Py_ssize_t size = 0;
    const char *chars = PyUnicode_AsUTF8AndSize(text, &size);
    if (!chars) return nullptr;
    const dstd::StringView view(chars, static_cast<std::size_t>(size));
    auto doc = release_gil([&] { return parse_text<F>(view, ""); });
    if (!doc) {
        raise(doc.error(), "");
        return nullptr;
    }
    return document_root<F>(std::move(*doc), dstd::String(view));
}

template <class F>
PyObject *do_loads(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *text = nullptr;
    if (!core::parse_kw_for<"O!">(F::LOADS, args, kwds, {"text"},
                                  &PyUnicode_Type, &text))
        return nullptr;
    PyObject *tuple = PyTuple_Pack(1, text);
    if (!tuple) return nullptr;
    PyObject *root = do_parse<F>(self, tuple, nullptr);
    Py_DECREF(tuple);
    if (!root) return nullptr;
    PyObject *data = to_python<F>(*value_of<F>(root), 0);
    Py_DECREF(root);
    return data;
}

template <class F>
PyObject *do_read(PyObject *, PyObject *args, PyObject *kwds) {
    PyObject *path_arg = nullptr;
    if (!core::parse_kw_for<"O">(F::READ, args, kwds, {"path"}, &path_arg))
        return nullptr;
    dstd::String path;
    if (!path_of(path_arg, path)) return nullptr;
    dstd::String source;
    auto doc = release_gil([&]() -> dstd::Result<typename F::Doc> {
        auto text = read_whole<F>(path);
        if (!text) return dstd::Unexpected<dstd::Error>(text.error());
        source = std::move(*text);
        return parse_text<F>(source, path);
    });
    if (!doc) {
        raise(doc.error(), path);
        return nullptr;
    }
    return document_root<F>(std::move(*doc), std::move(source));
}

template <class F>
PyObject *do_load(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *path_arg = nullptr;
    if (!core::parse_kw_for<"O">(F::LOAD, args, kwds, {"path"}, &path_arg))
        return nullptr;
    PyObject *tuple = PyTuple_Pack(1, path_arg);
    if (!tuple) return nullptr;
    PyObject *root = do_read<F>(self, tuple, nullptr);
    Py_DECREF(tuple);
    if (!root) return nullptr;
    PyObject *data = to_python<F>(*value_of<F>(root), 0);
    Py_DECREF(root);
    return data;
}

bool is_blank(dstd::StringView line) {
    return line.find_first_not_of(" \t\r") == dstd::StringView::npos;
}

bool is_sequence_entry(dstd::StringView line) {
    if (line.empty() || line[0] != '-') return false;
    return line.size() == 1 || line[1] == ' ' || line[1] == '\t' ||
           line[1] == '\r';
}

bool is_document_start(dstd::StringView line) {
    return line.starts_with("---") && (line.size() == 3 || line[3] == ' ' ||
                                       line[3] == '\t' || line[3] == '\r');
}

enum class Cut { Unknown, Sequence, Documents, Whole };

template <class F>
struct ReaderState {
    TextSource source;
    dstd::String path;
    std::size_t batch_bytes;
    dstd::String buf;
    bool eof = false;
    bool failed = false;
    std::uint64_t line = 1;
    Cut cut = Cut::Unknown;
    dstd::Optional<json::DomParser> parser;

    ReaderState(TextSource &&s, dstd::String p, std::size_t bytes)
        : source(std::move(s)), path(std::move(p)), batch_bytes(bytes) {
        if constexpr (std::is_same_v<F, JsonFormat>) parser.emplace();
    }

    // Reads one more block of text onto the buffer.
    dstd::Result<void> read_more() {
        const std::size_t block =
            std::clamp(batch_bytes / 8, READ_BLOCK_MIN, READ_BLOCK_MAX);
        const std::size_t old = buf.size();
        buf.resize(old + block);
        auto got = source.read(buf.data() + old, block);
        if (!got) {
            buf.resize(old);
            return dstd::Unexpected<dstd::Error>(got.error());
        }
        buf.resize(old + *got);
        if (*got == 0) eof = true;
        return {};
    }

    dstd::Result<void> fill(std::size_t want) {
        while (!eof && buf.size() < want) {
            auto more = read_more();
            if (!more) return more;
        }
        return {};
    }

    // The first content line of the text decides how a YAML text is cut.
    dstd::Result<void> detect_cut() {
        std::size_t at = 0;
        for (;;) {
            const std::size_t nl = buf.find('\n', at);
            if (nl == dstd::String::npos && !eof) {
                auto more = read_more();
                if (!more) return more;
                continue;
            }
            const dstd::StringView text(buf);
            const dstd::StringView row = text.substr(
                at, (nl == dstd::String::npos ? buf.size() : nl) - at);
            if (!is_blank(row) && row[0] != '#' && row[0] != '%') {
                cut = is_document_start(row)   ? Cut::Documents
                      : is_sequence_entry(row) ? Cut::Sequence
                                               : Cut::Whole;
                return {};
            }
            if (nl == dstd::String::npos) {
                cut = Cut::Whole;
                return {};
            }
            at = nl + 1;
        }
    }

    // The start of the first whole line at or after `from` that starts a
    // record, or npos when the buffer holds none yet.
    std::size_t first_boundary(std::size_t from) const {
        const dstd::StringView text(buf);
        std::size_t start = from;
        if (start > 0) {
            const std::size_t nl = buf.find('\n', start - 1);
            if (nl == dstd::String::npos) return dstd::String::npos;
            start = nl + 1;
        }
        while (start < buf.size()) {
            const std::size_t nl = buf.find('\n', start);
            if (nl == dstd::String::npos) return dstd::String::npos;
            const dstd::StringView row = text.substr(start, nl - start);
            if (start > 0 && (cut == Cut::Sequence ? is_sequence_entry(row)
                                                   : is_document_start(row))) {
                while (cut == Cut::Documents && start > 0) {
                    const std::size_t before = start >= 2
                                                   ? buf.rfind('\n', start - 2)
                                                   : dstd::String::npos;
                    const std::size_t s =
                        before == dstd::String::npos ? 0 : before + 1;
                    if (text[s] != '%') break;
                    start = s;
                }
                return start;
            }
            start = nl + 1;
        }
        return dstd::String::npos;
    }

    // The start of every record in a YAML text cut as `cut` says, by the
    // rules the records are cut by.
    dstd::Vector<std::size_t> record_starts(dstd::StringView text) const {
        dstd::Vector<std::size_t> starts;
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t nl = text.find('\n', start);
            const std::size_t stop =
                nl == dstd::StringView::npos ? text.size() : nl;
            const dstd::StringView row = text.substr(start, stop - start);
            if (cut == Cut::Sequence ? is_sequence_entry(row)
                                     : is_document_start(row)) {
                std::size_t from = start;
                while (cut == Cut::Documents && !starts.empty() &&
                       from > starts.back()) {
                    const std::size_t before = from >= 2
                                                   ? text.rfind('\n', from - 2)
                                                   : dstd::StringView::npos;
                    const std::size_t s =
                        before == dstd::StringView::npos ? 0 : before + 1;
                    if (s <= starts.back() || text[s] != '%') break;
                    from = s;
                }
                starts.push_back(from);
            }
            start = nl == dstd::StringView::npos ? text.size() : nl + 1;
        }
        return starts;
    }

    // The bytes of the buffer that make a YAML batch: about a share of
    // batch_bytes, ending on a record boundary unless the text ended.
    dstd::Result<std::size_t> batch_end() {
        const std::size_t target =
            std::max<std::size_t>(1, batch_bytes / YAML_COST_FACTOR);
        auto filled = fill(target);
        if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
        if (cut == Cut::Unknown) {
            auto detected = detect_cut();
            if (!detected)
                return dstd::Unexpected<dstd::Error>(detected.error());
        }
        if (cut == Cut::Whole) {
            while (!eof) {
                auto more = read_more();
                if (!more) return dstd::Unexpected<dstd::Error>(more.error());
            }
            return buf.size();
        }
        for (;;) {
            const std::size_t boundary = first_boundary(target);
            if (boundary != dstd::String::npos) return boundary;
            if (eof) return buf.size();
            auto more = read_more();
            if (!more) return dstd::Unexpected<dstd::Error>(more.error());
        }
    }

    // The next batch of records, or null at the end of the text. A batch ends
    // when what it holds, the parsed records and their text, reaches
    // batch_bytes; a record that is larger makes a batch of its own.
    dstd::Result<dstd::UniquePtr<BatchData<F>>> next() {
        if (failed || (eof && buf.empty()))
            return dstd::UniquePtr<BatchData<F>>();
        auto data = dstd::make_unique<BatchData<F>>();
        if constexpr (std::is_same_v<F, JsonFormat>) {
            std::size_t cost = 0;
            std::size_t pos = 0;
            for (;;) {
                const std::size_t nl = buf.find('\n', pos);
                if (nl == dstd::String::npos && !eof) {
                    buf.erase(0, pos);
                    pos = 0;
                    auto more = read_more();
                    if (!more)
                        return dstd::Unexpected<dstd::Error>(more.error());
                    continue;
                }
                if (nl == dstd::String::npos && pos >= buf.size()) break;
                const std::size_t stop =
                    nl == dstd::String::npos ? buf.size() : nl;
                const dstd::StringView full(buf.data() + pos, stop - pos);
                dstd::StringView row = full;
                if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
                if (!is_blank(row)) {
                    auto doc = parser->parse(row);
                    if (!doc) {
                        failed = true;
                        return common::make_error(
                            common::ErrorCode::Parse,
                            locate(doc.error().message(), path, "", line));
                    }
                    data->spans.push_back(
                        BatchText::Span{data->text.size(), full.size()});
                    data->text.append(full);
                    data->vals.push_back(doc->root());
                    cost += doc->bytes() + full.size() + RECORD_OVERHEAD;
                    data->docs.push_back(std::move(*doc));
                }
                ++line;
                pos = nl == dstd::String::npos ? buf.size() : nl + 1;
                if (cost >= batch_bytes) break;
            }
            buf.erase(0, pos);
            if (data->vals.empty() && eof && buf.empty())
                return dstd::UniquePtr<BatchData<F>>();
        } else {
            auto end = batch_end();
            if (!end) return dstd::Unexpected<dstd::Error>(end.error());
            if (*end == 0) return dstd::UniquePtr<BatchData<F>>();
            data->text.assign(buf.data(), *end);
            dstd::String text(data->text);
            const std::uint64_t first_line = line;
            line += static_cast<std::uint64_t>(
                std::count(text.begin(), text.end(), '\n'));
            yaml::RecordsOptions options;
            options.chunk_bytes = text.size() + 1;
            options.batch_chunks = std::size_t{1} << 30;
            data->records.emplace(
                yaml::Records::from_text(std::move(text), path, options));
            auto chunks = data->records->next_chunks();
            if (!chunks) {
                failed = true;
                return common::make_error(common::ErrorCode::Parse,
                                          locate(chunks.error().message(), path,
                                                 path + ":", first_line));
            }
            for (auto &part : *chunks) {
                for (;;) {
                    auto record = part.next();
                    if (!record) {
                        failed = true;
                        return common::make_error(
                            common::ErrorCode::Parse,
                            locate(record.error().message(), path, path + ":",
                                   first_line));
                    }
                    if (!*record) break;
                    data->vals.push_back(**record);
                }
            }
            if (cut == Cut::Whole) {
                if (data->vals.size() == 1)
                    data->spans.push_back(
                        BatchText::Span{0, data->text.size()});
            } else {
                const auto starts = record_starts(data->text);
                if (starts.size() == data->vals.size()) {
                    data->style = cut == Cut::Sequence ? 1 : 2;
                    for (std::size_t i = 0; i < starts.size(); ++i) {
                        std::size_t stop = i + 1 < starts.size()
                                               ? starts[i + 1]
                                               : data->text.size();
                        if (stop > starts[i] && data->text[stop - 1] == '\n')
                            --stop;
                        data->spans.push_back(
                            BatchText::Span{starts[i], stop - starts[i]});
                    }
                }
            }
            buf.erase(0, *end);
        }
        return data;
    }
};

template <class F>
PyObject *make_batch(dstd::UniquePtr<BatchData<F>> data) {
    PyTypeObject *type = &g_batch_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    new (&reinterpret_cast<BatchObject<F> *>(self)->data)
        dstd::UniquePtr<BatchData<F>>(std::move(data));
    return self;
}

template <class F>
BatchData<F> &batch_data(PyObject *self) {
    return *reinterpret_cast<BatchObject<F> *>(self)->data;
}

template <class F>
void Batch_dealloc(PyObject *self) {
    std::destroy_at(&reinterpret_cast<BatchObject<F> *>(self)->data);
    Py_TYPE(self)->tp_free(self);
}

template <class F>
Py_ssize_t Batch_length(PyObject *self) {
    return static_cast<Py_ssize_t>(batch_data<F>(self).vals.size());
}

template <class F>
PyObject *Batch_item(PyObject *self, Py_ssize_t i) {
    auto &vals = batch_data<F>(self).vals;
    if (i < 0 || i >= static_cast<Py_ssize_t>(vals.size())) {
        PyErr_SetString(PyExc_IndexError, "record index out of range");
        return nullptr;
    }
    return make_value<F>(
        self, vals[static_cast<std::size_t>(i)],
        batch_data<F>(self).raw_at(static_cast<std::size_t>(i)));
}

template <class F>
PyObject *Batch_iter(PyObject *self) {
    auto &vals = batch_data<F>(self).vals;
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(vals.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        PyObject *item =
            make_value<F>(self, vals[i], batch_data<F>(self).raw_at(i));
        if (!item) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
    }
    PyObject *it = PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

template <class F>
PyObject *Batch_repr(PyObject *self) {
    return PyUnicode_FromFormat("<%s of %zd records>", Py_TYPE(self)->tp_name,
                                Batch_length<F>(self));
}

template <class F>
PySequenceMethods g_batch_sequence = {.sq_length = Batch_length<F>,
                                      .sq_item = Batch_item<F>};

template <class F>
void Iterator_dealloc(PyObject *self) {
    std::destroy_at(&reinterpret_cast<IteratorObject<F> *>(self)->state);
    Py_TYPE(self)->tp_free(self);
}

template <class F>
PyObject *Iterator_iter(PyObject *self) {
    Py_INCREF(self);
    return self;
}

template <class F>
PyObject *Iterator_next(PyObject *self) {
    auto *it = reinterpret_cast<IteratorObject<F> *>(self);
    if (it->busy) {
        PyErr_SetString(PyExc_RuntimeError,
                        "the record iterator is already running in another "
                        "thread");
        return nullptr;
    }
    while (!it->done) {
        auto batch = [&] {
            BusyScope scope(it->busy);
            return release_gil([&] { return it->state->next(); });
        }();
        if (!batch) {
            it->done = true;
            raise(batch.error(), it->state->path);
            return nullptr;
        }
        if (!*batch) {
            it->done = true;
            break;
        }
        if ((*batch)->vals.empty()) continue;
        return make_batch<F>(std::move(*batch));
    }
    return nullptr;
}

template <class F>
PyObject *Iterator_close(PyObject *self, PyObject *) {
    auto *it = reinterpret_cast<IteratorObject<F> *>(self);
    it->done = true;
    Py_RETURN_NONE;
}

template <class F>
PyMethodDef g_iterator_methods[] = {
    {"close", DFTU_PYCFUNCTION(Iterator_close<F>), METH_NOARGS,
     "close() -> None\n\nStop reading; the iterator then gives nothing."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
PyObject *do_read_records(PyObject *, PyObject *args, PyObject *kwds) {
    PyObject *path_arg = nullptr;
    Py_ssize_t batch_bytes = std::size_t{64} << 20;
    if (!core::parse_kw_for<"O|$n">(F::READ_RECORDS, args, kwds,
                                    {"path", "batch_bytes"}, &path_arg,
                                    &batch_bytes))
        return nullptr;
    if (batch_bytes <= 0) {
        PyErr_SetString(PyExc_ValueError, "batch_bytes must be positive");
        return nullptr;
    }
    dstd::String path;
    if (!path_of(path_arg, path)) return nullptr;
    auto source = release_gil([&] { return TextSource::open(path); });
    if (!source) {
        raise(source.error(), path);
        return nullptr;
    }
    PyTypeObject *type = &g_iterator_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    auto *it = reinterpret_cast<IteratorObject<F> *>(self);
    new (&it->state)
        dstd::UniquePtr<ReaderState<F>>(dstd::make_unique<ReaderState<F>>(
            std::move(*source), std::move(path),
            static_cast<std::size_t>(batch_bytes)));
    it->done = false;
    it->busy = false;
    return self;
}

template <class F>
PyMethodDef g_function_defs[] = {
    {F::PARSE, DFTU_PYCFUNCTION(do_parse<F>), METH_VARARGS | METH_KEYWORDS,
     "parse(text: str) -> Value\n\nParse one whole text."},
    {F::LOADS, DFTU_PYCFUNCTION(do_loads<F>), METH_VARARGS | METH_KEYWORDS,
     "loads(text: str) -> Data\n\nParse one whole text to plain data."},
    {F::READ, DFTU_PYCFUNCTION(do_read<F>), METH_VARARGS | METH_KEYWORDS,
     "read(path) -> Value\n\nParse one whole file, plain or gzip."},
    {F::LOAD, DFTU_PYCFUNCTION(do_load<F>), METH_VARARGS | METH_KEYWORDS,
     "load(path) -> Data\n\nParse one whole file to plain data."},
    {F::READ_RECORDS, DFTU_PYCFUNCTION(do_read_records<F>),
     METH_VARARGS | METH_KEYWORDS,
     "read_records(path, *, batch_bytes=67108864) -> Iterator[RecordBatch]\n\n"
     "Stream the records of a file, a batch at a time."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

template <class F>
int init_records(PyObject *m) {
    const dstd::String module =
        dstd::String("dftracer.utils.formats.") + F::NAME + ".";
    g_batch_name<F> = module + "RecordBatch";
    PyTypeObject &batch = g_batch_type<F>;
    batch.tp_name = g_batch_name<F>.c_str();
    batch.tp_basicsize = sizeof(BatchObject<F>);
    batch.tp_dealloc = Batch_dealloc<F>;
    batch.tp_repr = Batch_repr<F>;
    batch.tp_flags = Py_TPFLAGS_DEFAULT;
    batch.tp_doc =
        "The records of one batch of a file. A record keeps its whole batch\n"
        "alive, so keep only what you need.";
    batch.tp_as_sequence = &g_batch_sequence<F>;
    batch.tp_iter = Batch_iter<F>;
    dstd::String attribute(F::TYPE_PREFIX);
    if (register_type(m, &batch, (attribute + "RecordBatch").c_str()) < 0)
        return -1;

    g_iterator_name<F> = module + "RecordIterator";
    PyTypeObject &iterator = g_iterator_type<F>;
    iterator.tp_name = g_iterator_name<F>.c_str();
    iterator.tp_basicsize = sizeof(IteratorObject<F>);
    iterator.tp_dealloc = Iterator_dealloc<F>;
    iterator.tp_flags = Py_TPFLAGS_DEFAULT;
    iterator.tp_doc = "The record batches of a file, read as they are asked.";
    iterator.tp_iter = Iterator_iter<F>;
    iterator.tp_iternext = Iterator_next<F>;
    iterator.tp_methods = g_iterator_methods<F>;
    if (register_type(m, &iterator, (attribute + "RecordIterator").c_str()) < 0)
        return -1;
    return PyModule_AddFunctions(m, g_function_defs<F>);
}

template int init_records<JsonFormat>(PyObject *);
template int init_records<YamlFormat>(PyObject *);

}  // namespace dftracer::utils::python::formats
