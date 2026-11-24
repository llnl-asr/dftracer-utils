// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/formats/emit.h>
#include <dftracer/utils/python/formats/format.h>
#include <dftracer/utils/python/formats/mutable_value.h>
#include <dftracer/utils/python/formats/value.h>
#include <dftracer/utils/python/formats/writer.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::python::formats {

using core::release_gil;

namespace {

constexpr std::size_t DEFAULT_MEMBER_SIZE = std::size_t{4} << 20;

template <class F>
struct WriterImpl {
    dstd::Optional<fileio::GzipLineWriterBlocking> file;
    dstd::String pending;
    dstd::String record;
    dstd::Optional<Emitter<F>> emitter;
    std::size_t flush_at = DEFAULT_MEMBER_SIZE;
    std::uint64_t record_id = 0;
    std::uint8_t indent = 2;
    bool documents = false;
    bool closed = false;
    bool record_open = false;
    bool busy = false;
};

template <class F>
struct WriterObject {
    PyObject_HEAD dstd::UniquePtr<WriterImpl<F>> impl;
};

template <class F>
struct BuilderObject {
    PyObject_HEAD PyObject *writer;
    PyObject *parent;
    std::uint64_t id;
    bool closed;
    bool child_open;
};

template <class F>
PyTypeObject g_writer_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
PyTypeObject g_object_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
PyTypeObject g_array_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
dstd::String g_writer_name;
template <class F>
dstd::String g_object_name;
template <class F>
dstd::String g_array_name;

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
WriterImpl<F> *impl_of(PyObject *writer) {
    return reinterpret_cast<WriterObject<F> *>(writer)->impl.get();
}

// The writer when it can take a call, else null with a RuntimeError.
template <class F>
WriterImpl<F> *usable(PyObject *writer) {
    WriterImpl<F> *impl = impl_of<F>(writer);
    if (!impl || impl->closed) {
        PyErr_SetString(PyExc_RuntimeError, "the writer is closed");
        return nullptr;
    }
    if (impl->busy) {
        PyErr_SetString(PyExc_RuntimeError,
                        "the writer is in use by another thread");
        return nullptr;
    }
    return impl;
}

// Drops the record being written; the writer takes the next one.
template <class F>
void discard_record(WriterImpl<F> *impl) {
    impl->emitter.reset();
    impl->record.clear();
    impl->record_open = false;
}

// Adds the finished record to the text waiting to be written, as the format
// lays a record out in a file.
template <class F>
void finish_record(WriterImpl<F> *impl) {
    dstd::String &out = impl->pending;
    if constexpr (std::is_same_v<F, JsonFormat>) {
        out += impl->record;
        out += '\n';
    } else if (impl->documents) {
        out += "---\n";
        out += impl->record;
        out += '\n';
    } else {
        out += "- ";
        for (const char c : impl->record) {
            out += c;
            if (c == '\n') out += "  ";
        }
        out += '\n';
    }
    impl->record.clear();
}

// Writes the waiting text to the file, with the GIL released.
template <class F>
bool flush(WriterImpl<F> *impl) {
    if (impl->pending.empty()) return true;
    auto written = [&] {
        BusyScope scope(impl->busy);
        return release_gil([&] { return impl->file->append(impl->pending); });
    }();
    impl->pending.clear();
    if (!written) {
        core::set_py_error(written.error());
        return false;
    }
    return true;
}

template <class F>
bool end_record(WriterImpl<F> *impl) {
    finish_record(impl);
    impl->emitter.reset();
    impl->record_open = false;
    return impl->pending.size() < impl->flush_at || flush(impl);
}

template <class F>
void Writer_dealloc(PyObject *self) {
    std::destroy_at(&reinterpret_cast<WriterObject<F> *>(self)->impl);
    Py_TYPE(self)->tp_free(self);
}

template <class F>
PyObject *Writer_new(PyTypeObject *type, PyObject *, PyObject *) {
    PyObject *self = type->tp_alloc(type, 0);
    if (self)
        new (&reinterpret_cast<WriterObject<F> *>(self)->impl)
            dstd::UniquePtr<WriterImpl<F>>();
    return self;
}

template <class F>
int Writer_init(PyObject *py_self, PyObject *args, PyObject *kwds) {
    auto *self = reinterpret_cast<WriterObject<F> *>(py_self);
    PyObject *path_arg = nullptr;
    PyObject *compress_arg = Py_None;
    int level = 6;
    Py_ssize_t member_size = DEFAULT_MEMBER_SIZE;
    Py_ssize_t indent = 2;
    const char *records = "sequence";
    if constexpr (std::is_same_v<F, JsonFormat>) {
        if (!core::parse_kw<"O|$Oin:Writer">(
                args, kwds, {"path", "compress", "level", "member_size"},
                &path_arg, &compress_arg, &level, &member_size))
            return -1;
    } else {
        if (!core::parse_kw<"O|$sOn:Writer">(
                args, kwds, {"path", "records", "compress", "indent"},
                &path_arg, &records, &compress_arg, &indent))
            return -1;
        if (dstd::StringView(records) != "sequence" &&
            dstd::StringView(records) != "documents") {
            PyErr_SetString(PyExc_ValueError,
                            "records must be 'sequence' or 'documents'");
            return -1;
        }
        if (indent < 1 || indent > 16) {
            PyErr_SetString(PyExc_ValueError,
                            "indent must be between 1 and 16");
            return -1;
        }
    }
    if (member_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "member_size must be positive");
        return -1;
    }
    PyObject *fs = PyOS_FSPath(path_arg);
    if (!fs) return -1;
    if (!PyUnicode_Check(fs)) {
        Py_DECREF(fs);
        PyErr_SetString(PyExc_TypeError, "path must be str or PathLike[str]");
        return -1;
    }
    Py_ssize_t size = 0;
    const char *chars = PyUnicode_AsUTF8AndSize(fs, &size);
    if (!chars) {
        Py_DECREF(fs);
        return -1;
    }
    dstd::String path(chars, static_cast<std::size_t>(size));
    Py_DECREF(fs);
    bool compress = false;
    if (compress_arg == Py_None) {
        compress = dstd::StringView(path).ends_with(".gz");
    } else {
        const int truth = PyObject_IsTrue(compress_arg);
        if (truth < 0) return -1;
        compress = truth != 0;
    }
    fileio::GzipWriterOptions options;
    options.member_size = static_cast<std::size_t>(member_size);
    options.level = level;
    options.workers = 1;
    options.compress = compress;
    auto file = release_gil([&] {
        return fileio::GzipLineWriterBlocking::open(path, std::move(options));
    });
    if (!file) {
        core::set_py_error(file.error());
        return -1;
    }
    auto impl = dstd::make_unique<WriterImpl<F>>();
    impl->file.emplace(std::move(*file));
    impl->flush_at = static_cast<std::size_t>(member_size);
    impl->indent = static_cast<std::uint8_t>(indent);
    impl->documents = dstd::StringView(records) == "documents";
    self->impl = std::move(impl);
    return 0;
}

template <class F>
PyObject *Writer_write(PyObject *self, PyObject *value) {
    WriterImpl<F> *impl = usable<F>(self);
    if (!impl) return nullptr;
    if (impl->record_open) {
        PyErr_SetString(PyExc_RuntimeError, "a record is still open");
        return nullptr;
    }
    if constexpr (std::is_same_v<F, YamlFormat>) {
        // A record written in the layout it was read in goes out as it was.
        const RawSpan *raw = raw_of<YamlFormat>(value);
        if (!raw) raw = unchanged_raw<YamlFormat>(value);
        if (raw && raw->style == (impl->documents ? 2 : 1)) {
            impl->pending.append(raw->data, raw->size);
            impl->pending += '\n';
            if (impl->pending.size() >= impl->flush_at && !flush(impl))
                return nullptr;
            Py_RETURN_NONE;
        }
    }
    impl->record.clear();
    bool ok;
    {
        Emitter<F> emitter(impl->record, impl->indent);
        ok = emit_py<F>(emitter, value, 0);
    }
    if (!ok) {
        impl->record.clear();
        return nullptr;
    }
    finish_record(impl);
    if (impl->pending.size() >= impl->flush_at && !flush(impl)) return nullptr;
    Py_RETURN_NONE;
}

template <class F>
PyObject *new_builder(PyObject *writer, PyObject *parent, bool array) {
    PyTypeObject *type = array ? &g_array_type<F> : &g_object_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    auto *builder = reinterpret_cast<BuilderObject<F> *>(self);
    Py_INCREF(writer);
    builder->writer = writer;
    Py_XINCREF(parent);
    builder->parent = parent;
    builder->id = impl_of<F>(writer)->record_id;
    return self;
}

template <class F>
PyObject *Writer_record(PyObject *self, PyObject *) {
    WriterImpl<F> *impl = usable<F>(self);
    if (!impl) return nullptr;
    if (impl->record_open) {
        PyErr_SetString(PyExc_RuntimeError, "a record is still open");
        return nullptr;
    }
    impl->record.clear();
    ++impl->record_id;
    impl->emitter.emplace(impl->record, impl->indent);
    impl->emitter->begin_object();
    impl->record_open = true;
    PyObject *builder = new_builder<F>(self, nullptr, false);
    if (!builder) discard_record(impl);
    return builder;
}

template <class F>
PyObject *Writer_close(PyObject *self, PyObject *) {
    WriterImpl<F> *impl = impl_of<F>(self);
    if (!impl || impl->closed) Py_RETURN_NONE;
    if (impl->busy) {
        PyErr_SetString(PyExc_RuntimeError,
                        "the writer is in use by another thread");
        return nullptr;
    }
    if (impl->record_open) {
        PyErr_SetString(PyExc_RuntimeError, "a record is still open");
        return nullptr;
    }
    if (!flush(impl)) {
        impl->closed = true;
        impl->file.reset();
        return nullptr;
    }
    auto closed = [&] {
        BusyScope scope(impl->busy);
        return release_gil([&] { return impl->file->close(); });
    }();
    impl->closed = true;
    impl->file.reset();
    if (!closed) {
        core::set_py_error(closed.error());
        return nullptr;
    }
    Py_RETURN_NONE;
}

template <class F>
PyObject *Writer_enter(PyObject *self, PyObject *) {
    Py_INCREF(self);
    return self;
}

template <class F>
PyObject *Writer_exit(PyObject *self, PyObject *args) {
    PyObject *type =
        PyTuple_GET_SIZE(args) > 0 ? PyTuple_GET_ITEM(args, 0) : Py_None;
    if (type == Py_None) {
        PyObject *closed = Writer_close<F>(self, nullptr);
        if (!closed) return nullptr;
        Py_DECREF(closed);
    } else if (WriterImpl<F> *impl = impl_of<F>(self); impl && !impl->closed) {
        discard_record(impl);
        impl->closed = true;
        impl->file.reset();
    }
    Py_RETURN_FALSE;
}

template <class F>
PyMethodDef g_writer_methods[] = {
    {"write", DFTU_PYCFUNCTION(Writer_write<F>), METH_O,
     "write(value) -> None\n\nWrite one record."},
    {"record", DFTU_PYCFUNCTION(Writer_record<F>), METH_NOARGS,
     "record() -> ObjectBuilder\n\nStart one record that is built member by "
     "member."},
    {"close", DFTU_PYCFUNCTION(Writer_close<F>), METH_NOARGS,
     "close() -> None\n\nFinish the file. Safe to call twice."},
    {"__enter__", DFTU_PYCFUNCTION(Writer_enter<F>), METH_NOARGS,
     "Enter the context manager."},
    {"__exit__", DFTU_PYCFUNCTION(Writer_exit<F>), METH_VARARGS,
     "Close on success; remove the output when the block raised."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
void Builder_dealloc(PyObject *self) {
    auto *builder = reinterpret_cast<BuilderObject<F> *>(self);
    Py_XDECREF(builder->writer);
    Py_XDECREF(builder->parent);
    Py_TYPE(self)->tp_free(self);
}

// The writer of a builder that can take a call, else null with a RuntimeError.
template <class F>
WriterImpl<F> *live(PyObject *self) {
    auto *builder = reinterpret_cast<BuilderObject<F> *>(self);
    WriterImpl<F> *impl = usable<F>(builder->writer);
    if (!impl) return nullptr;
    if (!impl->record_open || impl->record_id != builder->id) {
        PyErr_SetString(PyExc_RuntimeError, "the record was discarded");
        return nullptr;
    }
    if (builder->closed) {
        PyErr_SetString(PyExc_RuntimeError, "the builder is closed");
        return nullptr;
    }
    if (builder->child_open) {
        PyErr_SetString(PyExc_RuntimeError, "an inner builder is still open");
        return nullptr;
    }
    return impl;
}

template <class F>
bool key_of(PyObject *key, dstd::StringView &out) {
    if (!PyUnicode_Check(key)) {
        PyErr_Format(PyExc_TypeError, "a key must be str, not %.200s",
                     Py_TYPE(key)->tp_name);
        return false;
    }
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(key, &size);
    if (!text) return false;
    out = dstd::StringView(text, static_cast<std::size_t>(size));
    return true;
}

// Writes a value into the open container; a value that cannot be written
// discards the record.
template <class F>
PyObject *add_value(WriterImpl<F> *impl, PyObject *value) {
    if (!emit_py<F>(*impl->emitter, value, 1)) {
        discard_record(impl);
        return nullptr;
    }
    Py_RETURN_NONE;
}

template <class F>
PyObject *open_child(PyObject *self, WriterImpl<F> *impl, bool array) {
    auto *builder = reinterpret_cast<BuilderObject<F> *>(self);
    if (array)
        impl->emitter->begin_array();
    else
        impl->emitter->begin_object();
    PyObject *child = new_builder<F>(builder->writer, self, array);
    if (!child) {
        discard_record(impl);
        return nullptr;
    }
    builder->child_open = true;
    return child;
}

template <class F>
PyObject *Object_member(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *key = nullptr;
    PyObject *value = nullptr;
    if (!core::parse_kw_for<"OO">("member", args, kwds, {"key", "value"}, &key,
                                  &value))
        return nullptr;
    WriterImpl<F> *impl = live<F>(self);
    dstd::StringView name;
    if (!impl || !key_of<F>(key, name)) return nullptr;
    impl->emitter->key(name);
    return add_value<F>(impl, value);
}

template <class F>
PyObject *Object_object(PyObject *self, PyObject *key) {
    WriterImpl<F> *impl = live<F>(self);
    dstd::StringView name;
    if (!impl || !key_of<F>(key, name)) return nullptr;
    impl->emitter->key(name);
    return open_child<F>(self, impl, false);
}

template <class F>
PyObject *Object_array(PyObject *self, PyObject *key) {
    WriterImpl<F> *impl = live<F>(self);
    dstd::StringView name;
    if (!impl || !key_of<F>(key, name)) return nullptr;
    impl->emitter->key(name);
    return open_child<F>(self, impl, true);
}

template <class F>
PyObject *Array_value(PyObject *self, PyObject *value) {
    WriterImpl<F> *impl = live<F>(self);
    if (!impl) return nullptr;
    return add_value<F>(impl, value);
}

template <class F>
PyObject *Array_object(PyObject *self, PyObject *) {
    WriterImpl<F> *impl = live<F>(self);
    return impl ? open_child<F>(self, impl, false) : nullptr;
}

template <class F>
PyObject *Array_array(PyObject *self, PyObject *) {
    WriterImpl<F> *impl = live<F>(self);
    return impl ? open_child<F>(self, impl, true) : nullptr;
}

template <class F>
PyObject *Builder_enter(PyObject *self, PyObject *) {
    if (!live<F>(self)) return nullptr;
    Py_INCREF(self);
    return self;
}

template <class F>
PyObject *Builder_exit(PyObject *self, PyObject *args) {
    auto *builder = reinterpret_cast<BuilderObject<F> *>(self);
    PyObject *type =
        PyTuple_GET_SIZE(args) > 0 ? PyTuple_GET_ITEM(args, 0) : Py_None;
    WriterImpl<F> *impl = impl_of<F>(builder->writer);
    if (!impl || impl->closed || builder->closed || !impl->record_open ||
        impl->record_id != builder->id)
        Py_RETURN_FALSE;
    if (type != Py_None) {
        discard_record(impl);
        Py_RETURN_FALSE;
    }
    if (builder->child_open) {
        discard_record(impl);
        PyErr_SetString(PyExc_RuntimeError, "an inner builder is still open");
        return nullptr;
    }
    impl->emitter->end();
    builder->closed = true;
    if (builder->parent) {
        reinterpret_cast<BuilderObject<F> *>(builder->parent)->child_open =
            false;
    } else if (!end_record(impl)) {
        return nullptr;
    }
    Py_RETURN_FALSE;
}

template <class F>
PyMethodDef g_object_methods[] = {
    {"member", DFTU_PYCFUNCTION(Object_member<F>), METH_VARARGS | METH_KEYWORDS,
     "member(key, value) -> None\n\nAdd a member."},
    {"object", DFTU_PYCFUNCTION(Object_object<F>), METH_O,
     "object(key) -> ObjectBuilder\n\nAdd a member that is an object."},
    {"array", DFTU_PYCFUNCTION(Object_array<F>), METH_O,
     "array(key) -> ArrayBuilder\n\nAdd a member that is an array."},
    {"__enter__", DFTU_PYCFUNCTION(Builder_enter<F>), METH_NOARGS,
     "Enter the context manager."},
    {"__exit__", DFTU_PYCFUNCTION(Builder_exit<F>), METH_VARARGS,
     "Close the object, or drop the record when the block raised."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
PyMethodDef g_array_methods[] = {
    {"value", DFTU_PYCFUNCTION(Array_value<F>), METH_O,
     "value(value) -> None\n\nAdd an element."},
    {"object", DFTU_PYCFUNCTION(Array_object<F>), METH_NOARGS,
     "object() -> ObjectBuilder\n\nAdd an element that is an object."},
    {"array", DFTU_PYCFUNCTION(Array_array<F>), METH_NOARGS,
     "array() -> ArrayBuilder\n\nAdd an element that is an array."},
    {"__enter__", DFTU_PYCFUNCTION(Builder_enter<F>), METH_NOARGS,
     "Enter the context manager."},
    {"__exit__", DFTU_PYCFUNCTION(Builder_exit<F>), METH_VARARGS,
     "Close the array, or drop the record when the block raised."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
void fill_builder_type(PyTypeObject &type, dstd::String &name, const char *leaf,
                       PyMethodDef *methods, const char *doc) {
    name = dstd::String("dftracer.utils.formats.") + F::NAME + "." + leaf;
    type.tp_name = name.c_str();
    type.tp_basicsize = sizeof(BuilderObject<F>);
    type.tp_dealloc = Builder_dealloc<F>;
    type.tp_flags = Py_TPFLAGS_DEFAULT;
    type.tp_doc = doc;
    type.tp_methods = methods;
}

}  // namespace

template <class F>
int init_writer(PyObject *m) {
    g_writer_name<F> =
        dstd::String("dftracer.utils.formats.") + F::NAME + ".Writer";
    PyTypeObject &writer = g_writer_type<F>;
    writer.tp_name = g_writer_name<F>.c_str();
    writer.tp_basicsize = sizeof(WriterObject<F>);
    writer.tp_dealloc = Writer_dealloc<F>;
    writer.tp_flags = Py_TPFLAGS_DEFAULT;
    writer.tp_doc =
        std::is_same_v<F, JsonFormat>
            ? "Writer(path, *, compress=None, level=6, member_size=4194304)\n"
              "--\n\n"
              "Writes records, one JSON line each, to a file that is gzip "
              "members of about\nmember_size bytes. compress=None picks gzip "
              "from a .gz suffix. A writer dropped without close() removes "
              "its output.\nNot thread-safe."
            : "Writer(path, *, records='sequence', compress=None, indent=2)\n"
              "--\n\n"
              "Writes records to a YAML file, as the elements of one block "
              "sequence or as the\ndocuments of a --- stream. compress=None "
              "picks gzip from a .gz suffix. A writer dropped without close() "
              "removes "
              "its output. Not thread-safe.";
    writer.tp_methods = g_writer_methods<F>;
    writer.tp_init = Writer_init<F>;
    writer.tp_new = Writer_new<F>;
    dstd::String prefix(F::TYPE_PREFIX);
    if (register_type(m, &writer, (prefix + "Writer").c_str()) < 0) return -1;
    fill_builder_type<F>(g_object_type<F>, g_object_name<F>, "ObjectBuilder",
                         g_object_methods<F>,
                         "A record or a member that is an object, built "
                         "member by member.");
    if (register_type(m, &g_object_type<F>,
                      (prefix + "ObjectBuilder").c_str()) < 0)
        return -1;
    fill_builder_type<F>(g_array_type<F>, g_array_name<F>, "ArrayBuilder",
                         g_array_methods<F>,
                         "A member or an element that is an array, built "
                         "element by element.");
    return register_type(m, &g_array_type<F>,
                         (prefix + "ArrayBuilder").c_str());
}

template int init_writer<JsonFormat>(PyObject *);
template int init_writer<YamlFormat>(PyObject *);

}  // namespace dftracer::utils::python::formats
