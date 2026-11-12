// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/vector.h>
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

struct RecordChunkImpl {
    const RecordsImpl* owner = nullptr;
    DocumentStorage storage;
    /// The chunk, from its start in the text, with the byte order mark.
    const char* begin = nullptr;
    std::size_t size = 0;
    /// The start of the batch in the text, and the lines before it, to count
    /// the line of an error.
    const char* batch_begin = nullptr;
    std::uint64_t batch_lines = 0;
    std::uint64_t line_count = 0;
    bool is_first = false;
    bool is_parsed = false;
    bool is_failed = false;
    /// The walk: the next line of the top sequence in Seq mode, the line after
    /// the last document otherwise.
    std::uint32_t line_cursor = 0;
    std::uint32_t line_limit = 0;

    dstd::Result<void> parse();
    dstd::Optional<Value> next_record();
};

struct RecordsImpl {
    dstd::String name;
    RecordsOptions options;
    /// The text of a string source.
    dstd::String owned;
    /// The text given in blocks, from the start of the batch, which skips the
    /// zero fill of a string. A chunk is parsed where it lies in it, and the
    /// unparsed rest follows. Bytes past `filled` are input space.
    dstd::RawBuffer<char> raw;
    std::size_t filled = 0;
    /// The start of the batch, or of the next one, in the text.
    std::size_t begin = 0;
    /// The start of the chunk that find_chunk_end looks for the end of.
    std::size_t chunk_begin = 0;
    std::size_t chunk_end = 0;
    /// Where the search for the end of the chunk goes on when more text comes,
    /// or 0 before it starts.
    std::size_t scan_from = 0;
    bool after_end_marker = false;
    bool is_blocks = false;
    bool is_ended = true;
    bool needs_input = false;
    Mode mode = Mode::Unknown;
    bool is_done = false;
    bool at_text_start = true;
    std::uint64_t lines_before = 0;
    /// The ends of the chunks of the batch found so far.
    dstd::Vector<std::size_t> cuts;
    dstd::Vector<dstd::UniquePtr<RecordChunkImpl>> chunks;
    dstd::Vector<RecordChunk> handles;
    /// The chunks given in the batch, and the one that @ref Records::next
    /// reads.
    std::size_t given = 0;
    std::size_t serial = 0;

    char* chars() { return is_blocks ? raw.data() : owned.data(); }
    std::size_t chars_size() const { return is_blocks ? filled : owned.size(); }
    dstd::StringView text() { return dstd::StringView(chars(), chars_size()); }

    Load load_batch();
    bool find_chunk_end();
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
    const std::size_t from = chunk_begin;
    if (scan_from == 0)
        scan_from = from + std::max<std::size_t>(options.chunk_bytes, 1);
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
            while (mode == Mode::Docs && cut > from + 1) {
                const std::size_t previous = text.rfind('\n', cut - 2);
                const std::size_t start =
                    previous == dstd::StringView::npos ? 0 : previous + 1;
                if (start <= from || text[start] != '%') break;
                cut = start;
            }
            // Directives and comments alone are not a document.
            if (mode != Mode::Docs ||
                has_content(text.substr(from, cut - from))) {
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

dstd::Result<void> RecordChunkImpl::parse() {
    is_parsed = true;
    line_count = detail::count_newlines(begin, size);
    const std::size_t bom = is_first ? bom_size(begin, size) : 0;
    storage.base = begin + bom;
    storage.size = size - bom;
    const auto fail = [&](const LineProblem& problem) {
        is_failed = true;
        const std::uint64_t first_line =
            batch_lines +
            detail::count_newlines(
                batch_begin, static_cast<std::size_t>(begin - batch_begin));
        return line_problem_error(problem, owner->name, first_line + 1);
    };
    if (storage.lines.build(storage.base, storage.size) == BuildResult::Invalid)
        return fail(storage.lines.problem);
    std::uint32_t pos = 0;
    std::uint32_t first = 0;
    std::uint32_t limit = 0;
    bool is_empty = false;
    const bool has_document =
        LineAccess::next_document(&storage, pos, first, limit, is_empty);
    const Mode mode = owner->mode;
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
            return fail(problem);
        }
        line_cursor = first;
    } else if (mode == Mode::Seq) {
        line_cursor = 0;
        limit = 0;
    } else {
        line_cursor = 0;
    }
    line_limit = limit;
    return {};
}

dstd::Optional<Value> RecordChunkImpl::next_record() {
    if (is_failed) return dstd::nullopt;
    const LineIndex& ix = storage.lines;
    if (owner->mode == Mode::Seq) {
        if (line_cursor < line_limit && ix.entries[line_cursor].indent == 0) {
            const std::uint32_t line = line_cursor;
            line_cursor = ix.next[line];
            return LineAccess::element(&storage, line);
        }
        return dstd::nullopt;
    }
    std::uint32_t first = 0;
    std::uint32_t limit = 0;
    bool is_empty = false;
    if (LineAccess::next_document(&storage, line_cursor, first, limit,
                                  is_empty))
        return is_empty ? LineAccess::null(&storage)
                        : LineAccess::container(&storage, first, limit);
    return dstd::nullopt;
}

dstd::Result<dstd::Optional<Value>> RecordChunk::next() {
    if (!impl_->is_parsed) {
        if (auto parsed = impl_->parse(); !parsed) return parsed.error();
    }
    return impl_->next_record();
}

// Ends the batch that was given and loads the next one.
Load RecordsImpl::load_batch() {
    if (given > 0) {
        for (std::size_t i = 0; i < given; ++i) {
            RecordChunkImpl& chunk = *chunks[i];
            lines_before += chunk.is_parsed ? chunk.line_count
                                            : detail::count_newlines(
                                                  chunk.begin, chunk.size);
        }
        begin = cuts[given - 1];
        cuts.clear();
        given = 0;
        serial = 0;
        at_text_start = false;
    }
    if (is_blocks && begin > 0) {
        std::memmove(raw.data(), raw.data() + begin, filled - begin);
        filled -= begin;
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
    const std::size_t batch = std::max<std::size_t>(options.batch_chunks, 1);
    while (cuts.size() < batch) {
        chunk_begin = cuts.empty() ? begin : cuts.back();
        if (chunk_begin == chars_size()) {
            if (!is_ended) return Load::NeedInput;
            break;
        }
        if (mode == Mode::Single) {
            if (!is_ended) return Load::NeedInput;
            chunk_end = chars_size();
        } else if (!find_chunk_end()) {
            return Load::NeedInput;
        }
        cuts.push_back(chunk_end);
    }
    if (cuts.empty()) return Load::End;
    while (chunks.size() < cuts.size()) {
        chunks.push_back(dstd::make_unique<RecordChunkImpl>());
        chunks.back()->owner = this;
        handles.push_back(RecordChunk(chunks.back().get()));
    }
    std::size_t start = begin;
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        RecordChunkImpl& chunk = *chunks[i];
        chunk.begin = chars() + start;
        chunk.size = cuts[i] - start;
        chunk.batch_begin = chars() + begin;
        chunk.batch_lines = lines_before;
        chunk.is_first = at_text_start && i == 0;
        chunk.is_parsed = false;
        chunk.is_failed = false;
        start = cuts[i];
    }
    given = cuts.size();
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
    // Room for a whole batch at once, so the text of a batch is not copied
    // as the buffer grows block by block; past that, it grows by half.
    const std::size_t batch_bytes =
        impl.options.chunk_bytes *
        std::max<std::size_t>(impl.options.batch_chunks, 1);
    if (need > impl.raw.capacity())
        impl.raw.reserve(
            std::max({need, batch_bytes + size,
                      impl.raw.capacity() + impl.raw.capacity() / 2}));
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
        if (impl.serial < impl.given) {
            auto record = impl.handles[impl.serial].next();
            if (!record.has_value()) {
                impl.is_done = true;
                return record.error();
            }
            if (*record) return record;
            ++impl.serial;
            continue;
        }
        const Load loaded = impl.load_batch();
        if (loaded == Load::NeedInput) {
            impl.needs_input = true;
            return dstd::Optional<Value>{};
        }
        if (loaded == Load::End) {
            impl.is_done = true;
            return dstd::Optional<Value>{};
        }
    }
}

dstd::Result<dstd::Span<RecordChunk>> Records::next_chunks() {
    RecordsImpl& impl = *impl_;
    if (impl.is_done) return dstd::Span<RecordChunk>{};
    const Load loaded = impl.load_batch();
    if (loaded == Load::NeedInput) {
        impl.needs_input = true;
        return dstd::Span<RecordChunk>{};
    }
    if (loaded == Load::End) {
        impl.is_done = true;
        return dstd::Span<RecordChunk>{};
    }
    return dstd::Span<RecordChunk>(impl.handles.data(), impl.given);
}

}  // namespace dftracer::utils::yaml
