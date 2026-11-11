// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/yaml/document_test.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/line_value.h>
#include <dftracer/utils/yaml/records.h>
#include <dftracer/utils/yaml/tape.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dftracer::utils::yaml {

namespace {

constexpr std::size_t READ_BLOCK = std::size_t{1} << 20;

enum class Mode : std::uint8_t { Unknown, Empty, Seq, Docs, Single };

enum class Head : std::uint8_t { NeedMore, Empty, Seq, Docs, Single };

bool is_blank(char c) { return c == ' ' || c == '\t'; }

// True when `line` starts with `marker` and then a blank or the end of the
// line (a CR counts as the end).
bool starts_with_marker(dstd::StringView line, dstd::StringView marker) {
    if (line.substr(0, marker.size()) != marker) return false;
    if (line.size() == marker.size()) return true;
    const char next = line[marker.size()];
    return is_blank(next) || next == '\r';
}

// What the first content line of `text` says the text is.
Head detect_head(dstd::StringView text, bool eof) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        if (end == dstd::StringView::npos && !eof) return Head::NeedMore;
        dstd::StringView line = text.substr(0, end);
        text = end == dstd::StringView::npos ? dstd::StringView{}
                                             : text.substr(end + 1);
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == dstd::StringView::npos || line[first] == '#') continue;
        if (starts_with_marker(line, "---")) return Head::Docs;
        if (line[0] == '%' || starts_with_marker(line, "...")) continue;
        if (starts_with_marker(line, "-")) return Head::Seq;
        return Head::Single;
    }
    return eof ? Head::Empty : Head::NeedMore;
}

// Whether a line of `text` is anything but a blank, a comment or a directive.
bool has_content(dstd::StringView text) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const dstd::StringView line = text.substr(0, end);
        text = end == dstd::StringView::npos ? dstd::StringView{}
                                             : text.substr(end + 1);
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == dstd::StringView::npos || line[first] == '#') continue;
        if (line[0] != '%') return true;
    }
    return false;
}

}  // namespace

struct RecordsImpl {
    dstd::String name;
    RecordsOptions options;
    /// The text of a string source, parsed in place.
    dstd::String owned;
    /// The text of a file read so far, which skips the zero fill of a string.
    /// A chunk is parsed in place in it, and the unparsed rest follows.
    dstd::RawBuffer<char> raw;
    /// The start of the chunk, or of the next one, in `buffer`.
    std::size_t begin = 0;
    std::size_t chunk_end = 0;
    dstd::File file;
    std::uint64_t file_offset = 0;
    bool is_file = false;
    bool eof = true;
    Mode mode = Mode::Unknown;
    bool chunk_loaded = false;
    bool failed = false;
    std::uint64_t lines_before = 0;
    DocumentStorage storage;
    TapeParser parser;
    std::size_t cursor = 0;
    std::size_t cursor_end = 0;
    /// The walk of a chunk that the line index holds: the next line of the
    /// top sequence in Seq mode, the line after the last document otherwise.
    std::uint32_t line_cursor = 0;
    std::uint32_t line_limit = 0;
    /// The lines of the chunk, counted before a tape parse rewrites its text.
    std::uint64_t chunk_lines = 0;

    char* chars() { return is_file ? raw.data() : owned.data(); }
    std::size_t chars_size() const {
        return is_file ? raw.size() : owned.size();
    }
    dstd::StringView text() { return dstd::StringView(chars(), chars_size()); }

    dstd::Result<void> read_more(std::size_t at_least);
    dstd::Result<bool> load_chunk();
    dstd::Result<void> find_chunk_end();
    dstd::Result<void> start_chunk();
};

dstd::Result<void> RecordsImpl::read_more(std::size_t at_least) {
    if (eof) return {};
    const std::size_t want = std::max(at_least, READ_BLOCK);
    const std::size_t old_size = raw.size();
    raw.resize(old_size + want);
    auto got = file.read_at(
        file_offset,
        dstd::Span<std::uint8_t>(
            reinterpret_cast<std::uint8_t*>(raw.data()) + old_size, want));
    if (!got) {
        raw.resize(old_size);
        return got.error();
    }
    raw.resize(old_size + *got);
    file_offset += *got;
    if (*got == 0) eof = true;
    return {};
}

namespace {

// Whether the line that starts at `line` is where a chunk may start.
bool starts_chunk(Mode mode, dstd::StringView line) {
    if (mode == Mode::Seq) return starts_with_marker(line, "-");
    return starts_with_marker(line, "---");
}

}  // namespace

// Sets `chunk_end` to the first line start at or after the chunk size that
// starts a record, or to the end of the text.
dstd::Result<void> RecordsImpl::find_chunk_end() {
    std::size_t from = begin + std::max<std::size_t>(options.chunk_bytes, 1);
    bool after_end_marker = false;
    for (;;) {
        while (!eof && chars_size() < from + READ_BLOCK) {
            if (auto more = read_more(from + READ_BLOCK - chars_size()); !more)
                return more;
        }
        const dstd::StringView text = this->text();
        std::size_t pos = std::min(from, text.size());
        if (pos < text.size() && text[pos - 1] != '\n') {
            const std::size_t newline = text.find('\n', pos);
            pos = newline == dstd::StringView::npos ? text.size() : newline + 1;
        }
        while (pos < text.size()) {
            const std::size_t newline = text.find('\n', pos);
            if (newline == dstd::StringView::npos && !eof) break;
            const dstd::StringView line = text.substr(
                pos,
                newline == dstd::StringView::npos ? newline : newline - pos);
            if (after_end_marker || starts_chunk(mode, line)) {
                // A directive belongs to the document that follows it.
                std::size_t cut = pos;
                while (mode == Mode::Docs && cut > begin + 1) {
                    const std::size_t previous = text.rfind('\n', cut - 2);
                    const std::size_t start =
                        previous == dstd::StringView::npos ? 0 : previous + 1;
                    if (start <= begin || text[start] != '%') break;
                    cut = start;
                }
                // Directives and comments alone are not a document.
                if (mode != Mode::Docs ||
                    has_content(text.substr(begin, cut - begin))) {
                    chunk_end = cut;
                    return {};
                }
            }
            after_end_marker =
                mode == Mode::Docs && starts_with_marker(line, "...");
            if (newline == dstd::StringView::npos) {
                pos = text.size();
                break;
            }
            pos = newline + 1;
        }
        if (eof) {
            chunk_end = text.size();
            return {};
        }
        from = pos;
    }
}

dstd::Result<void> RecordsImpl::start_chunk() {
    const std::size_t bom =
        lines_before == 0 ? bom_size(chars() + begin, chunk_end - begin) : 0;
    storage.base = chars() + begin + bom;
    storage.size = chunk_end - begin - bom;
    storage.is_lines = false;
    chunk_lines = static_cast<std::uint64_t>(
        std::count(chars() + begin, chars() + chunk_end, '\n'));
    if (!detail::is_force_tape()) {
        const BuildResult built =
            storage.lines.build(storage.base, storage.size);
        if (built == BuildResult::Invalid) {
            return line_problem_error(storage.lines.problem, name,
                                      lines_before + 1);
        }
        if (built == BuildResult::Accepted) {
            std::uint32_t pos = 0;
            std::uint32_t first = 0;
            std::uint32_t limit = 0;
            LineAccess::next_document(&storage, pos, first, limit);
            const LineKind kind = storage.lines.entries[first].kind;
            const bool is_seq =
                kind == LineKind::SeqItem || kind == LineKind::SeqOpen ||
                kind == LineKind::SeqMap || kind == LineKind::SeqMapOpen;
            if (mode != Mode::Seq || (is_seq && !storage.lines.has_markers)) {
                storage.is_lines = true;
                storage.reset_tape();
                line_cursor = mode == Mode::Seq ? first : 0;
                line_limit = limit;
                chunk_loaded = true;
                return {};
            }
        }
    }
    if (auto parsed = parser.parse(storage, name, lines_before + 1); !parsed)
        return parsed;
    cursor = 0;
    cursor_end = storage.tape.size();
    if (mode == Mode::Seq && !storage.tape.empty()) {
        const TapeEntry& root = storage.tape[0];
        if (root.kind != TapeKind::Seq || root.pos != storage.tape.size())
            return common::make_error<
                "{}: a document marker inside a top-level sequence">(
                common::ErrorCode::InvalidArgument, name);
        cursor = 1;
        cursor_end = root.pos;
    }
    chunk_loaded = true;
    return {};
}

// Loads the next chunk. False when the text has no more.
dstd::Result<bool> RecordsImpl::load_chunk() {
    if (is_file && begin > 0) {
        std::memmove(raw.data(), raw.data() + begin, raw.size() - begin);
        raw.resize(raw.size() - begin);
        chunk_end -= begin;
        begin = 0;
    }
    if (mode == Mode::Unknown) {
        Head head = Head::NeedMore;
        for (;;) {
            head = detect_head(text().substr(begin), eof);
            if (head != Head::NeedMore) break;
            if (auto more = read_more(READ_BLOCK); !more) return more.error();
        }
        switch (head) {
            case Head::Seq:
                mode = Mode::Seq;
                break;
            case Head::Docs:
                mode = Mode::Docs;
                break;
            case Head::Single:
                mode = Mode::Single;
                break;
            default:
                mode = Mode::Empty;
                break;
        }
    }
    if (mode == Mode::Empty) return false;
    while (!eof && chars_size() == begin) {
        if (auto more = read_more(READ_BLOCK); !more) return more.error();
    }
    if (chars_size() == begin) return false;
    if (mode == Mode::Single) {
        while (!eof) {
            if (auto more = read_more(READ_BLOCK); !more) return more.error();
        }
        chunk_end = chars_size();
    } else if (auto found = find_chunk_end(); !found) {
        return found.error();
    }
    if (auto started = start_chunk(); !started) return started.error();
    return true;
}

Records::Records(dstd::UniquePtr<RecordsImpl> impl) : impl_(std::move(impl)) {}
Records::Records(Records&&) noexcept = default;
Records& Records::operator=(Records&&) noexcept = default;
Records::~Records() = default;

Records Records::from_text(dstd::String text, dstd::StringView source_name,
                           RecordsOptions options) {
    auto impl = dstd::make_unique<RecordsImpl>();
    impl->name = dstd::String(source_name);
    impl->options = options;
    impl->owned = std::move(text);
    return Records(std::move(impl));
}

dstd::Result<Records> Records::open(dstd::PathView path,
                                    RecordsOptions options) {
    auto file = dstd::File::open(path, dstd::OpenOptions{});
    if (!file) return file.error();
    auto impl = dstd::make_unique<RecordsImpl>();
    impl->name = dstd::String(path.str());
    impl->options = options;
    impl->file = std::move(*file);
    impl->is_file = true;
    impl->eof = false;
    return Records(std::move(impl));
}

dstd::Result<dstd::Optional<Value>> Records::next() {
    RecordsImpl& impl = *impl_;
    for (;;) {
        if (impl.failed) return dstd::Optional<Value>{};
        if (impl.chunk_loaded && impl.storage.is_lines) {
            const LineIndex& ix = impl.storage.lines;
            if (impl.mode == Mode::Seq) {
                if (impl.line_cursor < impl.line_limit &&
                    ix.entries[impl.line_cursor].indent == 0) {
                    const std::uint32_t line = impl.line_cursor;
                    impl.line_cursor = ix.next[line];
                    return dstd::Optional<Value>(
                        LineAccess::element(&impl.storage, line));
                }
            } else {
                std::uint32_t first = 0;
                std::uint32_t limit = 0;
                if (LineAccess::next_document(&impl.storage, impl.line_cursor,
                                              first, limit))
                    return dstd::Optional<Value>(
                        LineAccess::container(&impl.storage, first, limit));
            }
            impl.lines_before += impl.chunk_lines;
            impl.begin = impl.chunk_end;
            impl.chunk_loaded = false;
            impl.storage.is_lines = false;
        } else if (impl.chunk_loaded) {
            if (impl.cursor < impl.cursor_end) {
                const std::size_t id = impl.cursor;
                const TapeEntry& entry = impl.storage.tape[id];
                impl.cursor = entry.is_container() ? entry.pos : id + 1;
                return dstd::Optional<Value>(Value(&impl.storage, id));
            }
            impl.lines_before += impl.chunk_lines;
            impl.begin = impl.chunk_end;
            impl.chunk_loaded = false;
            impl.storage.reset_tape();
        }
        auto loaded = impl.load_chunk();
        if (!loaded.has_value()) {
            impl.failed = true;
            return loaded.error();
        }
        if (!*loaded) {
            impl.failed = true;
            return dstd::Optional<Value>{};
        }
    }
}

}  // namespace dftracer::utils::yaml
