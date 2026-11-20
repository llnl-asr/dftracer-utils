// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/reader/file_process_types.h>
#include <dftracer/utils/trace/reader/indexed_file_reader_utility.h>
#include <dftracer/utils/trace/reader/internal/stream_config.h>
#include <dftracer/utils/trace/reader/internal/stream_type.h>
#include <dftracer/utils/trace/table/table_decode.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <dftracer/utils/trace/table/table_source.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dftracer::utils::trace::table {

namespace {

using common::ErrorCode;
using dataframe::Cursor;
using dataframe::DataFrame;
using dataframe::Morsel;
using dataframe::MorselResult;
using dataframe::Pushed;
using dataframe::ScanRequest;
using dataframe::ScanResult;
using dataframe::Schema;
using dataframe::Series;
using dataframe::TypeId;
using detail::Buffer;
using detail::Header;
using detail::Kind;

constexpr std::size_t PROBE_READ = std::size_t{16} << 10;
constexpr std::size_t PROBE_MAX_LINE = std::size_t{256} << 20;
constexpr std::size_t DEFAULT_BATCH = std::size_t{4} << 20;
constexpr std::size_t MIN_BATCH = std::size_t{1} << 20;
constexpr std::size_t MAX_BATCH = std::size_t{8} << 20;
// The fewest rows of a decoded file that one morsel carries.
constexpr std::int64_t DIRECT_MORSEL_ROWS = std::int64_t{1} << 20;
// A gzip table of JSON lines inflates to about this many bytes per byte.
constexpr std::uint64_t INFLATE_GUESS = 12;
constexpr dstd::StringView FORMAT_NAME = detail::FORMAT;

std::atomic<std::uint64_t> g_chunks_read{0};
std::atomic<std::uint64_t> g_chunks_skipped{0};
std::atomic<std::uint64_t> g_indexed_files{0};
std::atomic<std::uint64_t> g_streamed_files{0};
std::atomic<std::uint64_t> g_direct_files{0};
std::atomic<std::uint64_t> g_bytes_decoded{0};

void count(std::atomic<std::uint64_t>& c, std::uint64_t n = 1) {
    c.fetch_add(n, std::memory_order_relaxed);
}

// -- the first line ---------------------------------------------------------

// The text of line 1 of `path`, inflating only as much of its first gzip
// member as that takes; empty when the file does not open, is not gzip or a
// JSON object per line, or its first line is cut short by an inflate error.
dstd::Optional<dstd::String> first_line(dstd::StringView path) {
    auto file = dstd::File::open(dstd::PathView(path), {});
    if (!file) return dstd::nullopt;
    dstd::Vector<std::uint8_t> in(PROBE_READ);
    auto first =
        file->read_at(0, dstd::Span<std::uint8_t>(in.data(), in.size()));
    if (!first || *first == 0) return dstd::nullopt;
    std::uint64_t got = *first;
    const bool gzip = detail::has_gzip_magic(in.data(), got);
    dstd::String line;
    if (!gzip) {
        std::uint64_t at = 0;
        for (;;) {
            const auto* text = reinterpret_cast<const char*>(in.data());
            if (at == 0) {
                std::size_t i = 0;
                while (i < got && (text[i] == ' ' || text[i] == '\t')) ++i;
                if (i == got || text[i] != '{') return dstd::nullopt;
            }
            const void* nl = std::memchr(text, '\n', got);
            if (nl != nullptr) {
                line.append(text, static_cast<std::size_t>(
                                      static_cast<const char*>(nl) - text));
                return line;
            }
            line.append(text, got);
            if (line.size() > PROBE_MAX_LINE) return dstd::nullopt;
            at += got;
            auto more = file->read_at(
                at, dstd::Span<std::uint8_t>(in.data(), in.size()));
            if (!more || *more == 0) return line;
            got = *more;
        }
    }

    fileio::compress::GzipStreamInflater inflater;
    if (!inflater.init(15 + 16)) return dstd::nullopt;
    dstd::Vector<unsigned char> out(std::size_t{64} << 10);
    std::size_t produced = 0;
    std::uint64_t at = 0;
    for (;;) {
        inflater.set_input(in.data(), static_cast<std::size_t>(got));
        for (;;) {
            if (produced == out.size()) {
                if (out.size() >= PROBE_MAX_LINE) return dstd::nullopt;
                out.resize(out.size() * 2);
            }
            inflater.set_output(out.data() + produced, out.size() - produced);
            const auto status = inflater.inflate();
            produced = out.size() - inflater.avail_out();
            const void* nl = std::memchr(out.data(), '\n', produced);
            if (nl != nullptr) {
                line.append(
                    reinterpret_cast<const char*>(out.data()),
                    static_cast<std::size_t>(
                        static_cast<const unsigned char*>(nl) - out.data()));
                return line;
            }
            if (status == fileio::compress::InflateStatus::StreamEnd) {
                // The line goes on in the next member, when there is one.
                if (!inflater.reset()) return dstd::nullopt;
                if (inflater.avail_in() == 0) break;
                continue;
            }
            if (status == fileio::compress::InflateStatus::DataError ||
                status == fileio::compress::InflateStatus::Error)
                return dstd::nullopt;
            if (inflater.avail_in() == 0) break;
            if (status == fileio::compress::InflateStatus::BufError &&
                inflater.avail_out() != 0)
                return dstd::nullopt;
        }
        at += got;
        auto more =
            file->read_at(at, dstd::Span<std::uint8_t>(in.data(), in.size()));
        if (!more || *more == 0) {
            if (produced == 0) return dstd::nullopt;
            line.append(reinterpret_cast<const char*>(out.data()), produced);
            return line;
        }
        got = *more;
    }
}

struct Probed {
    Header header;
    dstd::String args_json;
};

dstd::Result<dstd::Optional<Probed>> probe_file(dstd::StringView path) {
    dstd::Optional<dstd::String> line = first_line(path);
    if (!line) return dstd::Optional<Probed>{};
    std::size_t i = 0;
    while (i < line->size() && ((*line)[i] == ' ' || (*line)[i] == '\t')) ++i;
    if (i == line->size() || (*line)[i] != '{') return dstd::Optional<Probed>{};
    if (line->find(FORMAT_NAME) == dstd::String::npos)
        return dstd::Optional<Probed>{};

    bool name_ok = false;
    bool phase_ok = false;
    bool format_ok = false;
    {
        json::Document doc;
        auto root = doc.parse(*line);
        if (!root)
            return common::make_error<
                "the first line of {} names dftu.table but is not JSON">(
                ErrorCode::Parse, path);
        if (auto n = root->find("name")) {
            auto s = n->as_string();
            name_ok = s && *s == FORMAT_NAME;
        }
        if (auto p = root->find("ph")) {
            auto n = p->as_int();
            phase_ok = n && *n == 4;
        }
        if (auto a = root->find("args")) {
            if (auto f = a->find("format")) {
                auto s = f->as_string();
                format_ok = s && *s == FORMAT_NAME;
            }
        }
    }
    if (!name_ok && !format_ok) return dstd::Optional<Probed>{};
    if (!name_ok || !format_ok || !phase_ok)
        return common::make_error<
            "the first line of {} claims dftu.table, but a table header has ph "
            "4, name 'dftu.table' and args.format 'dftu.table'">(
            ErrorCode::Parse, path);

    json::Document doc;
    auto root = doc.parse(*line);
    if (!root)
        return common::make_error<
            "the first line of {} names dftu.table but is not JSON">(
            ErrorCode::Parse, path);
    auto args = root->find("args");
    if (!args)
        return common::make_error<"the table header of {} has no args">(
            ErrorCode::Parse, path);
    Probed out;
    out.args_json = dstd::String(args->raw());
    auto parsed = detail::parse_header(*args);
    if (!parsed)
        return common::make_error<"the table header of {}: {}">(
            ErrorCode::Parse, path, parsed.error().message());
    out.header = std::move(*parsed);
    return dstd::Optional<Probed>(std::move(out));
}

// -- part order -------------------------------------------------------------

dstd::StringView base_name(dstd::StringView path) {
    const std::size_t slash = path.rfind('/');
    return slash == dstd::StringView::npos ? path : path.substr(slash + 1);
}

dstd::StringView parent_of(dstd::StringView path) {
    const std::size_t slash = path.rfind('/');
    return slash == dstd::StringView::npos ? dstd::StringView()
                                           : path.substr(0, slash);
}

bool is_part_name(dstd::StringView name) {
    if (!name.starts_with("part-")) return false;
    name.remove_prefix(5);
    std::size_t digits = 0;
    while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9')
        ++digits;
    if (digits == 0) return false;
    const dstd::StringView rest = name.substr(digits);
    return rest == ".pfw" || rest == ".pfw.gz";
}

std::uint64_t part_number(dstd::StringView name) {
    name.remove_prefix(5);
    std::uint64_t n = 0;
    for (const char c : name) {
        if (c < '0' || c > '9') break;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n;
}

// Orders `inputs`: the files of one folder by part number when every one of
// them is a `part-N` file, folders by the position of their first file.
dstd::Vector<std::size_t> read_order(dstd::Span<const TableInput> inputs) {
    dstd::Vector<dstd::StringView> folders;
    dstd::Vector<dstd::Vector<std::size_t>> groups;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const dstd::StringView folder = parent_of(inputs[i].path);
        std::size_t g = 0;
        while (g < folders.size() && folders[g] != folder) ++g;
        if (g == folders.size()) {
            folders.push_back(folder);
            groups.emplace_back();
        }
        groups[g].push_back(i);
    }
    dstd::Vector<std::size_t> order;
    order.reserve(inputs.size());
    for (dstd::Vector<std::size_t>& group : groups) {
        const bool parts =
            std::all_of(group.begin(), group.end(), [&](std::size_t i) {
                return is_part_name(base_name(inputs[i].path));
            });
        if (parts)
            dstd::stable_sort(
                group.begin(), group.end(), [&](std::size_t a, std::size_t b) {
                    return part_number(base_name(inputs[a].path)) <
                           part_number(base_name(inputs[b].path));
                });
        for (const std::size_t i : group) order.push_back(i);
    }
    return order;
}

// -- the filter, as a question for the index --------------------------------

struct Prunable {
    dstd::String name;  ///< `args.<field>` of a column the index can answer
    bool usable = false;
};

dstd::Optional<duql::Expr> as_leaf(const dataframe::Expr& e,
                                   dstd::Span<const Prunable> columns) {
    std::int32_t ci = -1;
    dataframe::CmpOp op{};
    dataframe::Scalar rhs;
    if (dataframe::expr_as_col_cmp(e, &ci, &op, &rhs)) {
        if (ci < 0 || static_cast<std::size_t>(ci) >= columns.size() ||
            !columns[static_cast<std::size_t>(ci)].usable)
            return dstd::nullopt;
        const dstd::String& field = columns[static_cast<std::size_t>(ci)].name;
        duql::CompareOp qop;
        switch (op) {
            case dataframe::CmpOp::Gt:
                qop = duql::CompareOp::Gt;
                break;
            case dataframe::CmpOp::Ge:
                qop = duql::CompareOp::Ge;
                break;
            case dataframe::CmpOp::Lt:
                qop = duql::CompareOp::Lt;
                break;
            case dataframe::CmpOp::Le:
                qop = duql::CompareOp::Le;
                break;
            case dataframe::CmpOp::Eq:
                qop = duql::CompareOp::Eq;
                break;
            default:
                // A null or NaN cell decides what != means, so it never
                // prunes.
                return dstd::nullopt;
        }
        duql::LiteralNode lit;
        switch (rhs.tag()) {
            case DFTU_SCALAR_TAG_I64:
                lit = duql::LiteralNode{rhs.i64()};
                break;
            case DFTU_SCALAR_TAG_U64:
                lit = duql::LiteralNode{rhs.u64()};
                break;
            case DFTU_SCALAR_TAG_STR:
                lit = duql::LiteralNode{dstd::String(rhs.str())};
                break;
            case DFTU_SCALAR_TAG_F64:
                if (!std::isfinite(rhs.f64())) return dstd::nullopt;
                lit = duql::LiteralNode{rhs.f64()};
                break;
            default:
                return dstd::nullopt;
        }
        return duql::field_cmp(field, qop, std::move(lit));
    }
    dataframe::Series values;
    if (dataframe::expr_as_col_is_in(e, &ci, &values)) {
        if (ci < 0 || static_cast<std::size_t>(ci) >= columns.size() ||
            !columns[static_cast<std::size_t>(ci)].usable ||
            values.null_count() != 0)
            return dstd::nullopt;
        const dstd::String& field = columns[static_cast<std::size_t>(ci)].name;
        if (!values.is_flat()) values = values.materialize();
        if (values.type() == TypeId::Int64) {
            dstd::Vector<std::int64_t> v(
                values.data<std::int64_t>(),
                values.data<std::int64_t>() + values.length());
            return duql::field_in(field, v);
        }
        if (values.type() == TypeId::String) {
            dstd::Vector<dstd::String> v;
            v.reserve(static_cast<std::size_t>(values.length()));
            for (std::int64_t i = 0; i < values.length(); ++i)
                v.emplace_back(values.string_at(i));
            return duql::field_in(field, v);
        }
    }
    return dstd::nullopt;
}

// A question whose every row that matches `e` is in a chunk the answer keeps.
// AND may drop a conjunct it cannot ask; OR needs both sides; NOT is never
// asked.
dstd::Optional<duql::Expr> as_question(const dataframe::Expr& e,
                                       dstd::Span<const Prunable> columns) {
    dataframe::LogicalOp lop{};
    dataframe::Expr lhs, rhs;
    if (dataframe::expr_as_logical(e, &lop, &lhs, &rhs)) {
        dstd::Optional<duql::Expr> a = as_question(lhs, columns);
        dstd::Optional<duql::Expr> b = as_question(rhs, columns);
        if (lop == dataframe::LogicalOp::And) {
            if (a && b) return duql::all_of(std::move(*a), std::move(*b));
            if (a) return a;
            return b;
        }
        if (!a || !b) return dstd::nullopt;
        return duql::any_of(std::move(*a), std::move(*b));
    }
    return as_leaf(e, columns);
}

// -- the source -------------------------------------------------------------

struct FileInfo {
    dstd::String path;
    dstd::String index_path;
    Header header;
    dstd::String args_json;
    std::uint64_t file_size = 0;
    bool gzip = false;
};

struct ChunkRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct ReadPlan {
    enum class Mode : std::uint8_t { Direct, Streamed, Indexed };
    Mode mode = Mode::Streamed;
    dstd::Vector<ChunkRange> ranges;  ///< Indexed: the chunks to read
    bool whole = false;               ///< Indexed: the chunks are every one
    std::uint64_t checkpoint_size = 0;
};

struct Settings {
    dstd::Vector<std::size_t> wanted;
    dstd::Optional<duql::Query> question;
    bool has_filters = false;
    bool whole = false;
    std::int64_t limit = -1;
    std::uint64_t budget = 0;
    std::size_t batch = DEFAULT_BATCH;
};

// The chunks of `file` a fresh index leaves for `question`; empty when the
// index is missing, stale, or says nothing usable, which means: read all.
coro::CoroTask<dstd::Result<dstd::Optional<ReadPlan>>> plan_indexed(
    const FileInfo& file, const duql::Query* question) {
    using index::Freshness;
    ReadPlan plan;
    plan.mode = ReadPlan::Mode::Indexed;
    if (file.index_path.empty() || question == nullptr)
        co_return dstd::Optional<ReadPlan>{};
    if (!common::input_exists(index::store_of(file.index_path)))
        co_return dstd::Optional<ReadPlan>{};
    common::InputFileStat st;
    if (!common::stat_input(file.path, st))
        co_return dstd::Optional<ReadPlan>{};
    auto opened = index::plan::open_snapshot(file.index_path);
    if (!opened) co_return dstd::Optional<ReadPlan>{};
    const index::Snapshot& snapshot = *opened;
    const dstd::String logical = fileio::get_logical_path(file.path);
    const auto ref = snapshot.file(logical);
    if (!ref) co_return dstd::Optional<ReadPlan>{};
    if (snapshot.freshness(
            logical, index::SourceStat{static_cast<std::uint64_t>(st.mtime),
                                       st.size}) != Freshness::Current)
        co_return dstd::Optional<ReadPlan>{};
    auto meta = index::file_metadata(snapshot, *ref);
    if (!meta || !*meta || (*meta)->truncated || (*meta)->has_open_line)
        co_return dstd::Optional<ReadPlan>{};
    auto format = index::format_record(snapshot, *ref);
    if (!format || *format) co_return dstd::Optional<ReadPlan>{};
    auto listed = index::spans(snapshot, *ref);
    if (!listed || listed->empty()) co_return dstd::Optional<ReadPlan>{};
    plan.checkpoint_size = (*meta)->checkpoint_size;
    // A chunk is skipped unread only for a file the Indexer read clean when it
    // built the index: every row valid, with every field, as many as nrows.
    // An index built before that fact existed scans.
    if (!(*meta)->table_rows_complete) co_return dstd::Optional<ReadPlan>{};

    index::plan::PruneRequest request{
        dstd::String(dstd::StringView(file.index_path)),
        dstd::String(dstd::StringView(file.path)),
        question,
        &snapshot,
        dstd::nullopt,
        static_cast<std::uint64_t>(listed->size()),
        true,
        index::plan::MetadataUse::None};
    auto pruning = index::plan::prune_file(std::move(request));
    auto pruned = co_await pruning;
    if (!pruned) co_return dstd::Optional<ReadPlan>{};
    if (pruned->total_chunks > 0 && pruned->total_chunks != listed->size())
        co_return dstd::Optional<ReadPlan>{};
    if (!pruned->file_may_match) {
        count(g_chunks_skipped, listed->size());
        co_return dstd::Optional<ReadPlan>(std::move(plan));
    }
    dstd::Vector<std::uint64_t> keep;
    if (pruned->all_chunks) {
        for (std::uint64_t i = 0; i < listed->size(); ++i) keep.push_back(i);
    } else {
        for (const std::uint64_t c : pruned->candidates) {
            if (c >= listed->size()) co_return dstd::Optional<ReadPlan>{};
            keep.push_back(c);
        }
    }
    count(g_chunks_skipped, listed->size() - keep.size());
    plan.whole = keep.size() == listed->size();
    for (const std::uint64_t c : keep) {
        const auto& span = (*listed)[c];
        plan.ranges.push_back(
            ChunkRange{span.uc_offset, span.uc_offset + span.uc_size});
    }
    co_return dstd::Optional<ReadPlan>(std::move(plan));
}

class TableCursor final : public Cursor {
   public:
    TableCursor(dstd::SharedPtr<const dstd::Vector<FileInfo>> files,
                Settings settings)
        : files_(std::move(files)),
          settings_(std::move(settings)),
          acc_(Buffer::with_capacity(4096)) {}

    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override;

   private:
    const FileInfo& file() const { return (*files_)[file_idx_]; }

    coro::CoroTask<dstd::Result<void>> open_file();
    coro::CoroTask<dstd::Result<void>> open_range();
    coro::CoroTask<dstd::Result<bool>> fill();
    coro::CoroTask<dstd::Result<bool>> pull();
    dstd::Result<void> finish_file();
    void consume(std::size_t n);
    void grow(std::size_t more);
    dstd::Result<void> start_streamed();

    dstd::SharedPtr<const dstd::Vector<FileInfo>> files_;
    Settings settings_;

    std::size_t file_idx_ = 0;
    bool file_open_ = false;
    ReadPlan plan_;
    std::size_t range_idx_ = 0;
    bool range_open_ = false;
    std::size_t rows_in_file_ = 0;
    std::int64_t batch_index_ = 0;

    // Direct: the decoded file and the rows handed out.
    dstd::Optional<DataFrame> whole_;
    std::int64_t whole_at_ = 0;

    // Streamed and Indexed: the bytes read and not yet decoded.
    Buffer acc_;
    bool header_pending_ = false;
    bool unit_done_ = false;
    std::size_t batch_end_ = 0;
    dstd::Optional<dstd::File> handle_;
    dstd::Optional<coro::AsyncGenerator<dstd::Result<dstd::StringView>>>
        members_;
    std::uint64_t plain_at_ = 0;
    dstd::SharedPtr<trace::reader::internal::Reader> reader_;
    dstd::UniquePtr<trace::reader::internal::ReaderStream> stream_;
};

// Room for `more` bytes after the ones held, growing by doubling.
void TableCursor::grow(std::size_t more) {
    const std::size_t need = acc_.size + more;
    if (need > acc_.capacity) acc_.reserve(std::max(need, acc_.capacity * 2));
}

void TableCursor::consume(std::size_t n) {
    if (n >= acc_.size) {
        acc_.size = 0;
        return;
    }
    std::memmove(acc_.data.get(), acc_.data.get() + n, acc_.size - n);
    acc_.size -= n;
}

coro::CoroTask<dstd::Result<void>> TableCursor::open_range() {
    const ChunkRange& range = plan_.ranges[range_idx_];
    reader::IndexedReadInput input =
        reader::IndexedReadInput::from_file(file().path)
            .with_index(file().index_path);
    if (plan_.checkpoint_size > 0)
        input.with_checkpoint_size(
            static_cast<std::size_t>(plan_.checkpoint_size));
    if (!reader_) {
        reader::IndexedFileReaderUtility utility;
        auto making = utility(input);
        auto made = co_await making;
        if (!made) co_return made.error();
        reader_ = std::move(*made);
    }
    auto opened = reader_->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::MultiLinesBytes)
            .range_type(reader::internal::RangeType::ByteRange)
            .buffer_size(settings_.batch)
            .extend_to_line_boundary(true)
            .from(static_cast<std::size_t>(range.start))
            .to(static_cast<std::size_t>(range.end)));
    if (!opened) co_return opened.error();
    stream_ = std::move(*opened);
    acc_.size = 0;
    header_pending_ = range.start == 0;
    unit_done_ = false;
    range_open_ = true;
    count(g_chunks_read);
    co_return dstd::Result<void>{};
}

// Reads the file in batches: the open file, and the member decoder of a gzip
// file whose steps are one batch long.
dstd::Result<void> TableCursor::start_streamed() {
    const FileInfo& f = file();
    plan_.mode = ReadPlan::Mode::Streamed;
    count(g_streamed_files);
    auto opened = dstd::File::open(dstd::PathView(f.path), {});
    if (!opened) return opened.error();
    handle_.emplace(std::move(*opened));
    plain_at_ = 0;
    acc_.size = 0;
    header_pending_ = true;
    unit_done_ = false;
    if (f.gzip)
        members_.emplace(fileio::compress::decode_gzip_members(
            handle_->native_handle(), f.file_size, settings_.batch, false));
    return {};
}

coro::CoroTask<dstd::Result<void>> TableCursor::open_file() {
    const FileInfo& f = file();
    rows_in_file_ = 0;
    whole_.reset();
    whole_at_ = 0;
    range_idx_ = 0;
    range_open_ = false;
    reader_.reset();
    stream_.reset();
    members_.reset();
    handle_.reset();
    plan_ = ReadPlan{};

    if (settings_.question) {
        auto planning = plan_indexed(f, &*settings_.question);
        auto planned = co_await planning;
        if (!planned) co_return planned.error();
        if (*planned) {
            plan_ = std::move(**planned);
            count(g_indexed_files);
            file_open_ = true;
            co_return dstd::Result<void>{};
        }
    }
    const std::uint64_t estimate =
        f.gzip ? f.file_size * INFLATE_GUESS : f.file_size;
    const bool fits = settings_.budget == 0 || estimate <= settings_.budget / 2;
    if (!settings_.has_filters && settings_.limit < 0 && fits) {
        plan_.mode = ReadPlan::Mode::Direct;
    } else if (auto started = start_streamed(); !started) {
        co_return started.error();
    }
    file_open_ = true;
    co_return dstd::Result<void>{};
}

// Appends the next bytes of the unit being read to `acc_`; sets `unit_done_`
// at its end.
coro::CoroTask<dstd::Result<bool>> TableCursor::pull() {
    if (plan_.mode == ReadPlan::Mode::Indexed) {
        if (stream_->done()) {
            unit_done_ = true;
            co_return false;
        }
        auto reading = stream_->read_async();
        auto read = co_await reading;
        if (!read) co_return read.error();
        if (read->empty()) {
            unit_done_ = true;
            co_return false;
        }
        grow(read->size());
        std::memcpy(acc_.data.get() + acc_.size, read->data(), read->size());
        acc_.size += read->size();
        count(g_bytes_decoded, read->size());
        co_return true;
    }
    if (file().gzip) {
        auto chunk = co_await members_->next();
        if (!chunk) {
            unit_done_ = true;
            co_return false;
        }
        if (!*chunk) co_return chunk->error();
        const dstd::StringView bytes = **chunk;
        grow(bytes.size());
        std::memcpy(acc_.data.get() + acc_.size, bytes.data(), bytes.size());
        acc_.size += bytes.size();
        count(g_bytes_decoded, bytes.size());
        co_return true;
    }
    if (plain_at_ >= file().file_size) {
        unit_done_ = true;
        co_return false;
    }
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(settings_.batch, file().file_size - plain_at_));
    grow(want);
    auto read = handle_->read_at(
        plain_at_, dstd::Span<std::uint8_t>(acc_.data.get() + acc_.size, want));
    if (!read) co_return read.error();
    if (*read == 0) {
        unit_done_ = true;
        co_return false;
    }
    plain_at_ += *read;
    acc_.size += *read;
    count(g_bytes_decoded, *read);
    co_return true;
}

// Leaves a run of whole lines in acc_[0, batch_end_) and returns true; false
// when the unit has no more lines.
coro::CoroTask<dstd::Result<bool>> TableCursor::fill() {
    for (;;) {
        if (header_pending_) {
            const void* nl = std::memchr(acc_.data.get(), '\n', acc_.size);
            if (nl != nullptr) {
                consume(static_cast<std::size_t>(
                            static_cast<const unsigned char*>(nl) -
                            acc_.data.get()) +
                        1);
                header_pending_ = false;
            } else if (unit_done_) {
                acc_.size = 0;
                header_pending_ = false;
            }
        }
        if (!header_pending_) {
            std::size_t cut = 0;
            if (unit_done_) {
                cut = acc_.size;
            } else if (acc_.size >= settings_.batch) {
                const unsigned char* base = acc_.data.get();
                std::size_t i = acc_.size;
                while (i > 0 && base[i - 1] != '\n') --i;
                cut = i;
            }
            if (cut > 0) {
                batch_end_ = cut;
                co_return true;
            }
            if (unit_done_) co_return false;
        }
        auto pulling = pull();
        auto more = co_await pulling;
        if (!more.has_value()) co_return more.error();
    }
}

dstd::Result<void> TableCursor::finish_file() {
    if ((plan_.mode != ReadPlan::Mode::Indexed || plan_.whole) &&
        rows_in_file_ != static_cast<std::size_t>(file().header.nrows))
        return common::make_error<
            "the header of {} says {} rows, the file has {}">(
            ErrorCode::Parse, dstd::StringView(file().path),
            file().header.nrows, rows_in_file_);
    return {};
}

coro::CoroTask<MorselResult> TableCursor::next(std::int64_t max_rows) {
    for (;;) {
        if (file_idx_ >= files_->size()) co_return MorselResult(dstd::nullopt);
        if (!file_open_) {
            auto opening = open_file();
            auto opened = co_await opening;
            if (!opened) co_return opened.error();
        }

        if (plan_.mode == ReadPlan::Mode::Direct) {
            if (!whole_) {
                auto raw = detail::read_whole(file().path);
                if (!raw) co_return raw.error();
                Buffer text;
                if (file().gzip) {
                    detail::InflateStats stats;
                    auto inflated = detail::inflate_gzip(
                        *raw, stats,
                        settings_.budget == 0
                            ? std::numeric_limits<std::size_t>::max()
                            : static_cast<std::size_t>(settings_.budget / 2));
                    if (!inflated) co_return inflated.error();
                    if (stats.exceeded) {
                        // A file that compresses far better than guessed:
                        // read it in batches instead.
                        if (auto started = start_streamed(); !started)
                            co_return started.error();
                        continue;
                    }
                    text = std::move(*inflated);
                } else {
                    text = std::move(*raw);
                }
                raw = Buffer{};
                count(g_direct_files);
                const char* base = text.text();
                const char* end = base + text.size;
                const void* nl = std::memchr(base, '\n', text.size);
                const char* body = nl ? static_cast<const char*>(nl) + 1 : end;
                auto decoded = detail::decode_lines(
                    file().header, settings_.wanted,
                    dstd::StringView(body,
                                     static_cast<std::size_t>(end - body)));
                if (!decoded) co_return decoded.error();
                rows_in_file_ = decoded->rows;
                if (auto ok = finish_file(); !ok) co_return ok.error();
                DataFrame frame;
                for (std::size_t k = 0; k < decoded->columns.size(); ++k) {
                    frame.names.push_back(dstd::SharedString(
                        file().header.fields[settings_.wanted[k]].name));
                    frame.columns.push_back(std::move(decoded->columns[k]));
                }
                whole_.emplace(std::move(frame));
                whole_at_ = 0;
            }
            const std::int64_t total = static_cast<std::int64_t>(rows_in_file_);
            if (whole_at_ >= total) {
                ++file_idx_;
                file_open_ = false;
                continue;
            }
            // The rows are decoded and held already, so a larger morsel costs
            // no memory, and each morsel saved is one pipeline hop less. That
            // holds only for a bare scan into a collect: an operator after the
            // scan runs per morsel and keeps the hint's parallelism.
            const std::int64_t step =
                settings_.whole ? std::max(max_rows, DIRECT_MORSEL_ROWS)
                                : max_rows;
            Morsel m;
            if (max_rows <= 0 || total - whole_at_ <= step) {
                if (whole_at_ == 0) {
                    for (Series& c : whole_->columns)
                        m.columns.push_back(c.share());
                } else {
                    DataFrame part =
                        whole_->slice(whole_at_, total - whole_at_);
                    m.columns = std::move(part.columns);
                }
                m.rows = total - whole_at_;
                whole_at_ = total;
            } else {
                DataFrame part = whole_->slice(whole_at_, step);
                m.columns = std::move(part.columns);
                m.rows = step;
                whole_at_ += step;
            }
            m.batch_index = batch_index_++;
            m.ordering = dataframe::Ordering::Sequence;
            co_return MorselResult(std::move(m));
        }

        if (plan_.mode == ReadPlan::Mode::Indexed && !range_open_) {
            if (range_idx_ >= plan_.ranges.size()) {
                if (auto ok = finish_file(); !ok) co_return ok.error();
                ++file_idx_;
                file_open_ = false;
                continue;
            }
            auto opening = open_range();
            auto opened = co_await opening;
            if (!opened) co_return opened.error();
        }

        auto filling = fill();
        auto filled = co_await filling;
        if (!filled.has_value()) co_return filled.error();
        if (!*filled) {
            if (plan_.mode == ReadPlan::Mode::Indexed) {
                range_open_ = false;
                stream_.reset();
                ++range_idx_;
                continue;
            }
            if (auto ok = finish_file(); !ok) co_return ok.error();
            ++file_idx_;
            file_open_ = false;
            continue;
        }
        auto decoded =
            detail::decode_lines(file().header, settings_.wanted,
                                 dstd::StringView(acc_.text(), batch_end_));
        if (!decoded) co_return decoded.error();
        consume(batch_end_);
        batch_end_ = 0;
        rows_in_file_ += decoded->rows;
        if (decoded->rows == 0) continue;
        Morsel m;
        m.rows = static_cast<std::int64_t>(decoded->rows);
        m.columns = std::move(decoded->columns);
        m.batch_index = batch_index_++;
        m.ordering = dataframe::Ordering::Sequence;
        co_return MorselResult(std::move(m));
    }
}

dataframe::DataType type_of(Kind kind) {
    switch (kind) {
        case Kind::Int:
            return dataframe::scalar(TypeId::Int64);
        case Kind::UInt:
            return dataframe::scalar(TypeId::Uint64);
        case Kind::Float:
            return dataframe::scalar(TypeId::Float64);
        case Kind::Bool:
            return dataframe::scalar(TypeId::Bool);
        default:
            return dataframe::scalar(TypeId::String);
    }
}

bool answers_index(const detail::Field& f) {
    switch (f.kind) {
        case Kind::Int:
        case Kind::UInt:
        case Kind::Float:
            return true;
        case Kind::String:
            return f.elem.empty();
        default:
            return false;
    }
}

class TableSource final : public dataframe::Source {
   public:
    explicit TableSource(dstd::Vector<FileInfo> files)
        : files_(dstd::make_shared<const dstd::Vector<FileInfo>>(
              std::move(files))) {
        for (const dstd::String& n : (*files_)[0].header.index)
            index_names_.push_back(dstd::SharedString(n));
    }

    Schema schema() const override {
        Schema s;
        for (const detail::Field& f : (*files_)[0].header.fields)
            s.fields.push_back(dataframe::Field{f.name, type_of(f.kind), true});
        return s;
    }

    dstd::Span<const dstd::SharedString> index_names() const override {
        return index_names_;
    }

    dstd::Result<ScanResult> scan(const ScanRequest& req) const override {
        const Header& header = (*files_)[0].header;
        Settings settings;
        dstd::Vector<Prunable> columns;
        if (req.projection.empty()) {
            for (std::size_t i = 0; i < header.fields.size(); ++i)
                settings.wanted.push_back(i);
        } else {
            for (const dstd::String& name : req.projection) {
                std::size_t at = 0;
                while (at < header.fields.size() &&
                       header.fields[at].name != name)
                    ++at;
                if (at == header.fields.size())
                    return common::make_error<"scan: no column named {}">(
                        ErrorCode::InvalidArgument, dstd::StringView(name));
                settings.wanted.push_back(at);
            }
        }
        for (const std::size_t i : settings.wanted) {
            Prunable p;
            p.usable = answers_index(header.fields[i]);
            p.name =
                dstd::fmt<"args.{}">(dstd::StringView(header.fields[i].name));
            columns.push_back(std::move(p));
        }
        settings.has_filters = !req.filters.empty();
        settings.whole = req.whole;
        settings.limit = req.limit;
        settings.budget = req.memory_budget != 0
                              ? req.memory_budget
                              : common::compute_memory_budget();
        settings.batch = std::clamp<std::size_t>(settings.budget / 128,
                                                 MIN_BATCH, MAX_BATCH);

        ScanResult result;
        dstd::Optional<duql::Expr> question;
        for (const dataframe::Expr& filter : req.filters) {
            dstd::Optional<duql::Expr> q = as_question(filter, columns);
            if (!q) {
                result.filters.push_back(Pushed::No);
                continue;
            }
            result.filters.push_back(Pushed::Inexact);
            if (question)
                question = duql::all_of(std::move(*question), std::move(*q));
            else
                question = std::move(q);
        }
        if (question) {
            auto built = std::move(*question).build();
            if (built.has_value())
                settings.question.emplace(std::move(built.value()));
            else
                for (Pushed& p : result.filters) p = Pushed::No;
        }
        result.cursor =
            dstd::make_unique<TableCursor>(files_, std::move(settings));
        return result;
    }

    const dstd::Vector<FileInfo>& files() const { return *files_; }

   private:
    dstd::SharedPtr<const dstd::Vector<FileInfo>> files_;
    dstd::Vector<dstd::SharedString> index_names_;
};

}  // namespace

ReadCounters read_counters() {
    ReadCounters c;
    c.chunks_read = g_chunks_read.load(std::memory_order_relaxed);
    c.chunks_skipped = g_chunks_skipped.load(std::memory_order_relaxed);
    c.indexed_files = g_indexed_files.load(std::memory_order_relaxed);
    c.streamed_files = g_streamed_files.load(std::memory_order_relaxed);
    c.direct_files = g_direct_files.load(std::memory_order_relaxed);
    c.bytes_decoded = g_bytes_decoded.load(std::memory_order_relaxed);
    return c;
}

void reset_read_counters() {
    g_chunks_read = 0;
    g_chunks_skipped = 0;
    g_indexed_files = 0;
    g_streamed_files = 0;
    g_direct_files = 0;
    g_bytes_decoded = 0;
}

dstd::Result<dstd::Optional<dstd::SharedPtr<const dataframe::Source>>>
open_table_inputs(dstd::Span<const TableInput> inputs) {
    using Out = dstd::Optional<dstd::SharedPtr<const dataframe::Source>>;
    const std::size_t n = inputs.size();
    if (n == 0) return Out{};
    dstd::Vector<dstd::Optional<Probed>> probed(n);
    dstd::Vector<dstd::Optional<dstd::Error>> errors(n);
    auto ran = default_runtime().parallel_for(
        static_cast<std::int64_t>(n), 1,
        [&](std::int64_t first, std::int64_t last) {
            for (std::int64_t k = first; k < last; ++k) {
                const auto i = static_cast<std::size_t>(k);
                auto one = probe_file(inputs[i].path);
                if (!one)
                    errors[i] = one.error();
                else
                    probed[i] = std::move(*one);
            }
        });
    if (!ran) return ran.error();
    for (const auto& e : errors)
        if (e) return *e;

    std::size_t tables = 0;
    for (const auto& p : probed)
        if (p) ++tables;
    if (tables == 0) return Out{};
    if (tables != n) {
        std::size_t table_at = 0;
        std::size_t trace_at = 0;
        while (!probed[table_at]) ++table_at;
        while (probed[trace_at]) ++trace_at;
        return common::make_error<
            "the inputs mix table files and trace files: {} is a table, {} is "
            "a trace">(ErrorCode::InvalidArgument,
                       dstd::StringView(inputs[table_at].path),
                       dstd::StringView(inputs[trace_at].path));
    }

    dstd::Vector<FileInfo> files;
    files.reserve(n);
    for (const std::size_t i : read_order(inputs)) {
        FileInfo f;
        f.path = inputs[i].path;
        f.index_path = inputs[i].index_path;
        f.header = std::move(probed[i]->header);
        f.args_json = std::move(probed[i]->args_json);
        auto stat = dstd::File::open(dstd::PathView(f.path), {});
        if (!stat) return stat.error();
        auto size = stat->size();
        if (!size) return size.error();
        f.file_size = *size;
        unsigned char magic[2] = {0, 0};
        auto head = stat->read_at(0, dstd::Span<std::uint8_t>(magic, 2));
        f.gzip = head && *head == 2 && detail::has_gzip_magic(magic, 2);
        files.push_back(std::move(f));
    }
    for (std::size_t i = 1; i < files.size(); ++i)
        if (auto why = detail::disagreement(files[0].header, files[i].header))
            return common::make_error<"the table parts {} and {} differ: {}">(
                ErrorCode::InvalidArgument, dstd::StringView(files[0].path),
                dstd::StringView(files[i].path), dstd::StringView(*why));
    return Out(dstd::SharedPtr<const dataframe::Source>(
        dstd::make_shared<const TableSource>(std::move(files))));
}

dstd::Vector<dstd::String> table_headers(const dataframe::Source& source) {
    dstd::Vector<dstd::String> out;
    if (const auto* t = dynamic_cast<const TableSource*>(&source))
        for (const FileInfo& f : t->files()) out.push_back(f.args_json);
    return out;
}

dstd::Vector<dstd::String> table_paths(const dataframe::Source& source) {
    dstd::Vector<dstd::String> out;
    if (const auto* t = dynamic_cast<const TableSource*>(&source))
        for (const FileInfo& f : t->files()) out.push_back(f.path);
    return out;
}

}  // namespace dftracer::utils::trace::table
