// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/line_value.h>
#include <dftracer/utils/yaml/records.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dftracer::utils::yaml {

namespace {

enum class Mode : std::uint8_t { Unknown, Empty, Seq, Docs, Single };

enum class Head : std::uint8_t { NeedMore, Empty, Seq, Docs, Single };

enum class Load : std::uint8_t { Chunk, End, NeedInput };

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
Head detect_head(dstd::StringView text, bool is_ended) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        if (end == dstd::StringView::npos && !is_ended) return Head::NeedMore;
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
    return is_ended ? Head::Empty : Head::NeedMore;
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
    /// The text of a string source.
    dstd::String owned;
    /// The text given in blocks, from the start of the chunk, which skips the
    /// zero fill of a string. A chunk is parsed where it lies in it, and the
    /// unparsed rest follows. Bytes past `filled` are input space.
    dstd::RawBuffer<char> raw;
    std::size_t filled = 0;
    /// The start of the chunk, or of the next one, in the text.
    std::size_t begin = 0;
    std::size_t chunk_end = 0;
    /// Where the search for the end of the chunk goes on when more text comes,
    /// or 0 before it starts.
    std::size_t scan_from = 0;
    bool after_end_marker = false;
    bool is_blocks = false;
    bool is_ended = true;
    bool needs_input = false;
    Mode mode = Mode::Unknown;
    bool chunk_loaded = false;
    bool is_done = false;
    std::uint64_t lines_before = 0;
    DocumentStorage storage;
    /// The walk of a chunk: the next line of the top sequence in Seq mode, the
    /// line after the last document otherwise.
    std::uint32_t line_cursor = 0;
    std::uint32_t line_limit = 0;
    std::uint64_t chunk_lines = 0;

    char* chars() { return is_blocks ? raw.data() : owned.data(); }
    std::size_t chars_size() const { return is_blocks ? filled : owned.size(); }
    dstd::StringView text() { return dstd::StringView(chars(), chars_size()); }

    dstd::Result<Load> load_chunk();
    bool find_chunk_end();
    dstd::Result<void> start_chunk();
};

namespace {

// Whether the line that starts at `line` is where a chunk may start.
bool starts_chunk(Mode mode, dstd::StringView line) {
    if (mode == Mode::Seq) return starts_with_marker(line, "-");
    return starts_with_marker(line, "---");
}

}  // namespace

// Sets `chunk_end` to the first line start at or after the chunk size that
// starts a record, or to the end of the text. False when that needs more text.
bool RecordsImpl::find_chunk_end() {
    if (scan_from == 0)
        scan_from = begin + std::max<std::size_t>(options.chunk_bytes, 1);
    const dstd::StringView text = this->text();
    if (scan_from > text.size() && !is_ended) return false;
    std::size_t pos = std::min(scan_from, text.size());
    if (pos < text.size() && text[pos - 1] != '\n') {
        const std::size_t newline = text.find('\n', pos);
        pos = newline == dstd::StringView::npos ? text.size() : newline + 1;
    }
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        if (newline == dstd::StringView::npos && !is_ended) break;
        const dstd::StringView line = text.substr(
            pos, newline == dstd::StringView::npos ? newline : newline - pos);
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
                scan_from = 0;
                after_end_marker = false;
                return true;
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
    if (is_ended) {
        chunk_end = text.size();
        scan_from = 0;
        after_end_marker = false;
        return true;
    }
    scan_from = pos;
    return false;
}

dstd::Result<void> RecordsImpl::start_chunk() {
    const std::size_t bom =
        lines_before == 0 ? bom_size(chars() + begin, chunk_end - begin) : 0;
    storage.base = chars() + begin + bom;
    storage.size = chunk_end - begin - bom;
    chunk_lines = static_cast<std::uint64_t>(
        std::count(chars() + begin, chars() + chunk_end, '\n'));
    if (storage.lines.build(storage.base, storage.size) == BuildResult::Invalid)
        return line_problem_error(storage.lines.problem, name,
                                  lines_before + 1);
    std::uint32_t pos = 0;
    std::uint32_t first = 0;
    std::uint32_t limit = 0;
    bool is_empty = false;
    const bool has_document =
        LineAccess::next_document(&storage, pos, first, limit, is_empty);
    if (mode == Mode::Seq && has_document) {
        const LineKind kind =
            is_empty ? LineKind::Blank : storage.lines.entries[first].kind;
        const bool is_seq =
            kind == LineKind::SeqItem || kind == LineKind::SeqOpen ||
            kind == LineKind::SeqMap || kind == LineKind::SeqMapOpen ||
            kind == LineKind::NestItem || kind == LineKind::NestItemOpen ||
            kind == LineKind::NestMap || kind == LineKind::NestMapOpen;
        std::uint32_t more_pos = pos;
        std::uint32_t more_first = 0;
        std::uint32_t more_limit = 0;
        bool more_empty = false;
        const bool has_more = LineAccess::next_document(
            &storage, more_pos, more_first, more_limit, more_empty);
        if (!is_seq || has_more) {
            LineProblem problem;
            problem.line = (has_more ? limit : first) + 1;
            problem.col = 1;
            problem.reason = "a document marker inside a top-level sequence";
            return line_problem_error(problem, name, lines_before + 1);
        }
        line_cursor = first;
    } else if (mode == Mode::Seq) {
        line_cursor = 0;
        limit = 0;
    } else {
        line_cursor = 0;
    }
    line_limit = limit;
    chunk_loaded = true;
    return {};
}

// Loads the next chunk.
dstd::Result<Load> RecordsImpl::load_chunk() {
    if (is_blocks && begin > 0) {
        std::memmove(raw.data(), raw.data() + begin, filled - begin);
        filled -= begin;
        chunk_end -= begin;
        begin = 0;
    }
    if (mode == Mode::Unknown) {
        switch (detect_head(text().substr(begin), is_ended)) {
            case Head::NeedMore:
                return Load::NeedInput;
            case Head::Seq:
                mode = Mode::Seq;
                break;
            case Head::Docs:
                mode = Mode::Docs;
                break;
            case Head::Single:
                mode = Mode::Single;
                break;
            case Head::Empty:
                mode = Mode::Empty;
                break;
        }
    }
    if (mode == Mode::Empty) return Load::End;
    if (chars_size() == begin) return is_ended ? Load::End : Load::NeedInput;
    if (mode == Mode::Single) {
        if (!is_ended) return Load::NeedInput;
        chunk_end = chars_size();
    } else if (!find_chunk_end()) {
        return Load::NeedInput;
    }
    if (auto started = start_chunk(); !started) return started.error();
    return Load::Chunk;
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

Records Records::from_blocks(dstd::StringView source_name,
                             RecordsOptions options) {
    auto impl = dstd::make_unique<RecordsImpl>();
    impl->name = dstd::String(source_name);
    impl->options = options;
    impl->is_blocks = true;
    impl->is_ended = false;
    return Records(std::move(impl));
}

dstd::Span<char> Records::input_space(std::size_t size) {
    RecordsImpl& impl = *impl_;
    const std::size_t need = impl.filled + size;
    // Room for a whole chunk at once, so the text of a chunk is not copied
    // as the buffer grows block by block; past that, it doubles.
    if (need > impl.raw.capacity())
        impl.raw.reserve(std::max(
            {need, impl.options.chunk_bytes + size, impl.raw.capacity() * 2}));
    impl.raw.resize(need);
    return dstd::Span<char>(impl.raw.data() + impl.filled, size);
}

void Records::commit_input(std::size_t size) {
    RecordsImpl& impl = *impl_;
    impl.filled += size;
    impl.raw.resize(impl.filled);
    impl.needs_input = false;
}

void Records::end_input() {
    impl_->is_ended = true;
    impl_->needs_input = false;
}

bool Records::needs_input() const { return impl_->needs_input; }

dstd::Result<dstd::Optional<Value>> Records::next() {
    RecordsImpl& impl = *impl_;
    for (;;) {
        if (impl.is_done) return dstd::Optional<Value>{};
        if (impl.chunk_loaded) {
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
                bool is_empty = false;
                if (LineAccess::next_document(&impl.storage, impl.line_cursor,
                                              first, limit, is_empty))
                    return dstd::Optional<Value>(
                        is_empty ? LineAccess::null(&impl.storage)
                                 : LineAccess::container(&impl.storage, first,
                                                         limit));
            }
            impl.lines_before += impl.chunk_lines;
            impl.begin = impl.chunk_end;
            impl.chunk_loaded = false;
        }
        auto loaded = impl.load_chunk();
        if (!loaded.has_value()) {
            impl.is_done = true;
            return loaded.error();
        }
        if (*loaded == Load::NeedInput) {
            impl.needs_input = true;
            return dstd::Optional<Value>{};
        }
        if (*loaded == Load::End) {
            impl.is_done = true;
            return dstd::Optional<Value>{};
        }
    }
}

}  // namespace dftracer::utils::yaml
