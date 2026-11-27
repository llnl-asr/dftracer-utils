// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/spawn_future.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/hex.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/process.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/write_feed.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <dftracer/utils/trace/writer/output_writer.h>
#include <dftracer/utils/trace/writer/trace_writer.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::writer {

namespace {

using common::ErrorCode;
using dataframe::Series;
using dataframe::TypeId;
using table::detail::Field;
using table::detail::Header;
using table::detail::Kind;

constexpr dstd::StringView HEADER_PREFIX =
    "{\"name\":\"dftu.table\",\"cat\":\"dftu\",\"pid\":0,\"tid\":0,"
    "\"ts\":0,\"ph\":4,\"args\":";
constexpr dstd::StringView ARGS_OPEN = "{\"nrows\":";
constexpr dstd::StringView ARGS_KEY = "args.";
constexpr std::size_t LINES_FLUSH_BYTES = 1024 * 1024;
// Rows the serial loop formats at a time. A wave of the parallel loop holds at
// most about WAVE_TEXT_BYTES of text, cut into about SLICES_PER_THREAD slices
// per runtime thread of at least MIN_SLICE_ROWS rows, so the threads end
// together.
constexpr std::size_t FORMAT_SLICE_ROWS = 4096;
constexpr std::size_t WAVE_TEXT_BYTES = std::size_t{64} << 20;
constexpr std::size_t SLICES_PER_THREAD = 4;
constexpr std::size_t MIN_SLICE_ROWS = 256;
constexpr std::size_t GZIP_MEMBER_HEADER_BYTES = 10;
constexpr std::size_t STORED_BLOCK_HEADER_BYTES = 5;

std::atomic<std::uint64_t> g_staging_counter{0};

// ---------------------------------------------------------------- columns

struct OutColumn {
    Series series;
    TypeId type = TypeId::Unknown;
    bool raw = false;
    bool has_nulls = false;
    const void* data = nullptr;
};

bool is_string_type(TypeId t) {
    return t == TypeId::String || t == TypeId::LargeString;
}

bool is_fixed_type(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
            return true;
        default:
            return false;
    }
}

unsigned width_of(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Uint8:
            return 8;
        case TypeId::Int16:
        case TypeId::Uint16:
            return 16;
        case TypeId::Int32:
        case TypeId::Uint32:
            return 32;
        default:
            return 64;
    }
}

bool is_signed_type(TypeId t) {
    return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
           t == TypeId::Int64;
}

bool is_unsigned_type(TypeId t) {
    return t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32 ||
           t == TypeId::Uint64;
}

bool is_integer_type(TypeId t) {
    return is_signed_type(t) || is_unsigned_type(t);
}

// Whether a column of type `t` can carry the field's dtype.
bool kind_accepts(const Field& f, TypeId t, bool raw) {
    switch (f.kind) {
        case Kind::Int:
            return is_signed_type(t) && width_of(t) <= f.bits;
        case Kind::UInt:
            return is_unsigned_type(t) && width_of(t) <= f.bits;
        case Kind::Float:
            return t == TypeId::Float32 ||
                   (t == TypeId::Float64 && f.bits == 64);
        case Kind::Bool:
            return t == TypeId::Bool;
        case Kind::String:
            return is_string_type(t) && !raw;
        case Kind::Raw:
        case Kind::List:
            return is_string_type(t) && raw;
    }
    return false;
}

// The pandas-style dtype name a list header gives its elements, or null for a
// type a list cannot hold.
const char* element_dtype_name(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
            return dataframe::type_name(t);
        case TypeId::LargeString:
            return "string";
        default:
            return nullptr;
    }
}

// The element dtype name of a List or LargeList column, or null.
const char* list_element_name(const dataframe::Series& column) {
    const TypeId t = column.type();
    if ((t != TypeId::List && t != TypeId::LargeList) ||
        column.num_children() != 1)
        return nullptr;
    return element_dtype_name(column.child(0).type());
}

template <class T>
ArgValue number_at(const void* data, std::size_t row) {
    return ArgValue(static_cast<const T*>(data)[row]);
}

ArgValue value_at(const OutColumn& c, std::size_t row) {
    if (c.has_nulls && c.series.is_null(static_cast<std::int64_t>(row)))
        return ArgValue();
    switch (c.type) {
        case TypeId::Bool:
            return ArgValue(
                ((static_cast<const std::uint8_t*>(c.data)[row >> 3] >>
                  (row & 7)) &
                 1) != 0);
        case TypeId::Int8:
            return number_at<std::int8_t>(c.data, row);
        case TypeId::Int16:
            return number_at<std::int16_t>(c.data, row);
        case TypeId::Int32:
            return number_at<std::int32_t>(c.data, row);
        case TypeId::Int64:
            return number_at<std::int64_t>(c.data, row);
        case TypeId::Uint8:
            return number_at<std::uint8_t>(c.data, row);
        case TypeId::Uint16:
            return number_at<std::uint16_t>(c.data, row);
        case TypeId::Uint32:
            return number_at<std::uint32_t>(c.data, row);
        case TypeId::Uint64:
            return number_at<std::uint64_t>(c.data, row);
        case TypeId::Float32:
            return ArgValue(
                static_cast<double>(static_cast<const float*>(c.data)[row]));
        case TypeId::Float64:
            return number_at<double>(c.data, row);
        default: {
            const dstd::StringView text =
                c.series.string_at(static_cast<std::int64_t>(row));
            return c.raw ? ArgValue::raw(text) : ArgValue(text);
        }
    }
}

// One list element as a JSON value. A NaN or infinite float has no JSON
// form, and the pandas table writer refuses it too.
bool write_element(json::Writer& w, const dataframe::Series& e,
                   std::int64_t j) {
    if (e.is_null(j)) {
        w.null();
        return true;
    }
    const TypeId t = e.type();
    if (is_string_type(t)) {
        w.value(e.string_at(j));
    } else if (t == TypeId::Bool) {
        w.value(dataframe::read_i64(e, j) != 0);
    } else if (is_signed_type(t)) {
        w.value(dataframe::read_i64(e, j));
    } else if (is_unsigned_type(t)) {
        w.value(dataframe::read_u64(e, j));
    } else if (t == TypeId::Float32 || t == TypeId::Float64) {
        const double x = dataframe::read_f64(e, j);
        if (!std::isfinite(x)) return false;
        w.value(x);
    } else {
        return false;
    }
    return true;
}

// A List column as the JSON text of each cell, the form a table stores for a
// field with elem "list". The text is a copy: a record holds each cell as
// JSON text.
dstd::Result<dataframe::Series> list_json(const dataframe::Series& list,
                                          dstd::StringView name) {
    dataframe::Series flat = list.is_flat() ? list.share() : list.materialize();
    if (flat.type() == TypeId::LargeList)
        flat = dataframe::Series{dataframe::narrow_large_list(flat.handle())};
    if (!flat.valid())
        return common::make_error<"list column {} has more than 2^31 items">(
            ErrorCode::InvalidArgument, name);
    const std::int32_t* off = dftu_series_offsets(flat.handle());
    if (flat.num_children() != 1 || (!off && flat.length() > 0))
        return common::make_error<"list column {} has no offsets">(
            ErrorCode::Internal, name);
    // The element readers see a FLAT child's buffers only, so a dictionary
    // or run-length child is decoded first.
    const dataframe::Series raw_child = flat.child(0);
    const dataframe::Series child =
        raw_child.is_flat() ? raw_child.share() : raw_child.materialize();
    const std::int64_t n = flat.length();
    dstd::String data;
    dstd::Vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1, 0);
    dstd::Vector<std::uint8_t> validity(static_cast<std::size_t>((n + 7) / 8),
                                        0xFF);
    for (std::int64_t i = 0; i < n; ++i) {
        if (flat.is_null(i)) {
            validity[static_cast<std::size_t>(i >> 3)] &=
                static_cast<std::uint8_t>(~(1u << (i & 7)));
        } else {
            json::Writer w(data);
            w.begin_array();
            bool stored = true;
            for (std::int64_t j = off[i]; stored && j < off[i + 1]; ++j)
                stored = write_element(w, child, j);
            // Close the array first: the writer checks it is closed when it
            // is destroyed.
            w.end_array();
            if (!stored)
                return common::make_error<
                    "row {}, column {}: a list element is NaN, infinite "
                    "or of a type a table cannot store">(
                    ErrorCode::InvalidArgument, i, name);
        }
        if (data.size() > static_cast<std::size_t>(INT32_MAX))
            return common::make_error<
                "column {}: the JSON text of its lists passes 2 GiB">(
                ErrorCode::InvalidArgument, name);
        offsets[static_cast<std::size_t>(i) + 1] =
            static_cast<std::int32_t>(data.size());
    }
    return dataframe::Series{dftu_series_new_string(
        static_cast<dftu_dtype>(TypeId::String), offsets.data(), data.data(), n,
        flat.null_count() > 0 ? validity.data() : nullptr)};
}

dstd::Result<OutColumn> prepare_column(const dataframe::DataFrame& frame,
                                       std::size_t i, bool raw) {
    const dstd::StringView name(frame.names[i]);
    OutColumn c;
    c.type = frame.columns[i].type();
    if (raw && (c.type == TypeId::List || c.type == TypeId::LargeList)) {
        auto text = list_json(frame.columns[i], name);
        if (!text) return text.error();
        c.series = std::move(*text);
        c.type = c.series.type();
    } else {
        c.series = frame.columns[i].is_flat() || is_string_type(c.type)
                       ? frame.columns[i].share()
                       : frame.columns[i].materialize();
    }
    c.raw = raw;
    if (c.raw && !is_string_type(c.type))
        return common::make_error<"raw column {} is not a string column">(
            ErrorCode::InvalidArgument, name);
    if (!is_fixed_type(c.type) && !is_string_type(c.type))
        return common::make_error<
            "column {} has a type the output does not carry">(
            ErrorCode::InvalidArgument, name);
    c.has_nulls = c.series.null_count() > 0;
    if (is_fixed_type(c.type)) {
        c.data = c.series.is_flat() ? c.series.template data<std::uint8_t>()
                                    : nullptr;
        if (c.data == nullptr && frame.num_rows() > 0)
            return common::make_error<"column {} has no value buffer">(
                ErrorCode::Internal, name);
    }
    return c;
}

// An integer cell as the unsigned value an event field holds.
dstd::Result<std::uint64_t> event_uint(const OutColumn& c, std::size_t row,
                                       dstd::StringView field) {
    if (c.has_nulls && c.series.is_null(static_cast<std::int64_t>(row)))
        return common::make_error<"row {}: the event field {} is null">(
            ErrorCode::InvalidArgument, row, field);
    const ArgValue v = value_at(c, row);
    if (v.kind == ArgValue::Kind::UInt) return v.u;
    if (v.kind == ArgValue::Kind::Int && v.i >= 0)
        return static_cast<std::uint64_t>(v.i);
    return common::make_error<"row {}: the event field {} is negative">(
        ErrorCode::InvalidArgument, row, field);
}

dstd::Result<dstd::StringView> event_text(const OutColumn& c, std::size_t row,
                                          dstd::StringView field) {
    if (c.has_nulls && c.series.is_null(static_cast<std::int64_t>(row)))
        return common::make_error<"row {}: the event field {} is null">(
            ErrorCode::InvalidArgument, row, field);
    return c.series.string_at(static_cast<std::int64_t>(row));
}

// ------------------------------------------------------------------- paths

bool all_digits(dstd::StringView text) {
    if (text.empty()) return false;
    for (const char c : text)
        if (c < '0' || c > '9') return false;
    return true;
}

// Whether `name` is `prefix` followed by `<pid>-<counter>` of another process
// and, for the side files of a staged output, `.<suffix>`, when that process is
// gone: a live writer may still be using its staging.
bool is_leftover_id(dstd::StringView name, dstd::StringView prefix) {
    if (!name.starts_with(prefix)) return false;
    const dstd::StringView id = name.substr(prefix.size());
    const std::size_t dash = id.find('-');
    if (dash == dstd::StringView::npos) return false;
    const dstd::StringView pid = id.substr(0, dash);
    dstd::StringView counter = id.substr(dash + 1);
    counter = counter.substr(0, counter.find('.'));
    if (!all_digits(pid) || !all_digits(counter)) return false;
    std::uint64_t number = 0;
    for (const char c : pid)
        number = number * 10 + static_cast<std::uint64_t>(c - '0');
    return number != static_cast<std::uint64_t>(::getpid()) &&
           !dstd::process_is_alive(number);
}

constexpr dstd::StringView GZ_EXTENSION = ".pfw.gz";
constexpr dstd::StringView PLAIN_EXTENSION = ".pfw";

dstd::StringView without_trailing_slashes(dstd::StringView path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    return path;
}

dstd::String part_file_name(std::size_t index, bool gzip) {
    dstd::String digits = dstd::fmt<"{}">(index);
    dstd::String name = "part-";
    for (std::size_t i = digits.size(); i < 5; ++i) name += '0';
    name += digits;
    name += gzip ? GZ_EXTENSION : PLAIN_EXTENSION;
    return name;
}

// The bytes of `text` as one gzip member of stored blocks.
std::uint64_t stored_member_bytes(std::size_t text_size) {
    const std::size_t blocks = std::max<std::size_t>(
        1, (text_size + fileio::GZIP_STORED_BLOCK_MAX - 1) /
               fileio::GZIP_STORED_BLOCK_MAX);
    return GZIP_MEMBER_HEADER_BYTES + STORED_BLOCK_HEADER_BYTES * blocks +
           text_size + 8;
}

dstd::String slot_text(std::int64_t value) {
    dstd::String text = dstd::fmt<"{}">(value);
    while (text.size() < NROWS_SLOT_WIDTH) text += ' ';
    return text;
}

dstd::Error io_error(dstd::StringView what, dstd::StringView path,
                     const dstd::Error& cause) {
    return common::make_error<"{} {}: {}">(ErrorCode::Io, what, path,
                                           cause.message())
        .error();
}

dstd::Result<void> sync_directory(dstd::StringView path) {
    auto dir = dstd::fs::Dir::open(dstd::PathView(path));
    if (!dir)
        return common::make_error<"cannot open {} to sync it: {}">(
            ErrorCode::Io, path, dir.error().message());
    if (::fsync(dir->fd()) != 0)
        return common::make_error<"cannot sync {}">(ErrorCode::Io, path);
    return {};
}

dstd::Result<bool> exists_no_follow(dstd::StringView path) {
    auto st = dstd::fs::lstat(dstd::PathView(path));
    if (st) return true;
    if (st.error().is(dstd::Condition::NotFound)) return false;
    return io_error("cannot stat", path, st.error());
}

constexpr std::size_t MAX_COMPONENT_BYTES = 255;
constexpr std::size_t MAX_RELATIVE_PATH_BYTES = 768;

// Keeps A-Z a-z 0-9 . _ - ~ and writes every other byte as %XX, so a name or a
// value never holds a path separator, a glob character or a byte that a file
// system treats specially. A leading dot would hide the folder.
void percent_encode(dstd::String& out, dstd::StringView in,
                    bool encode_leading_dot) {
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto c = static_cast<unsigned char>(in[i]);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                           c == '~' ||
                           (c == '.' && !(encode_leading_dot && i == 0));
        if (plain) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += dstd::HEX_UPPER[c >> 4];
            out += dstd::HEX_UPPER[c & 15];
        }
    }
}

// The text of a partition key cell; false for a null.
bool key_text(dstd::String& text, const ArgValue& v) {
    switch (v.kind) {
        case ArgValue::Kind::Null:
            return false;
        case ArgValue::Kind::Bool:
            text = v.b ? "true" : "false";
            return true;
        case ArgValue::Kind::Int:
            text = dstd::fmt<"{}">(v.i);
            return true;
        case ArgValue::Kind::UInt:
            text = dstd::fmt<"{}">(v.u);
            return true;
        case ArgValue::Kind::Double:
            text = dstd::fmt<"{}">(v.d);
            return true;
        default:
            text.assign(v.s.data(), v.s.size());
            return true;
    }
}

}  // namespace

bool is_file_output(dstd::StringView path) {
    return path.ends_with(GZ_EXTENSION) || path.ends_with(PLAIN_EXTENSION);
}

namespace {

// Compressors for the members that a partitioned write compresses beside the
// writer, one per task in flight. The tasks share this, so it outlives a
// writer that failed while they ran.
struct CompressState {
    explicit CompressState(int member_level, std::uint64_t budget)
        : level(member_level), bytes(budget) {}

    dstd::UniquePtr<fileio::compress::GzipMemberCompressor> take() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!free.empty()) {
                auto taken = std::move(free.back());
                free.pop_back();
                return taken;
            }
        }
        return dstd::make_unique<fileio::compress::GzipMemberCompressor>(level);
    }
    void give(dstd::UniquePtr<fileio::compress::GzipMemberCompressor> c) {
        std::lock_guard<std::mutex> lock(mutex);
        free.push_back(std::move(c));
    }

    int level;
    coro::AsyncSemaphore bytes;
    std::mutex mutex;
    dstd::Vector<dstd::UniquePtr<fileio::compress::GzipMemberCompressor>> free;
};

struct CompressedMember {
    dstd::String text;
    dstd::Vector<std::uint8_t> bytes;
    std::uint64_t rows = 0;
};

coro::CoroTask<dstd::Result<CompressedMember>> compress_member(
    dstd::SharedPtr<CompressState> state, dstd::String text,
    std::uint64_t rows) {
    CompressedMember out;
    auto compressor = state->take();
    if (!compressor->valid() ||
        !compressor->compress_member_into(out.bytes, text.data(), text.size()))
        co_return common::make_error(ErrorCode::Compression,
                                     "member compression failed");
    state->give(std::move(compressor));
    out.text = std::move(text);
    out.rows = rows;
    co_return out;
}

}  // namespace

struct OutputWriter::Impl {
    struct PartDone {
        dstd::String staged;
        dstd::String name;
        std::uint64_t rows = 0;
    };

    OutputOptions opts;
    dstd::Optional<memory::JobBudgetScope> idle_budget;
    bool is_file = false;
    bool committed = false;
    dstd::String target;
    dstd::String parent;
    dstd::String leaf;  // the target's own name
    dstd::String staging;
    dstd::String id;

    // records
    Header header;
    bool header_ready = false;
    dstd::String header_line;  // the header line with its slot at slot_pos
    std::size_t slot_pos = 0;
    dstd::Vector<OutColumn> columns;
    dstd::String table_name;

    // the part being written
    dstd::Optional<fileio::GzipLineWriter> gz;
    dstd::String part_path;
    std::uint64_t part_rows = 0;
    std::uint64_t part_text = 0;  // uncompressed bytes of the part's rows
    std::uint64_t rows = 0;
    dstd::Vector<PartDone> done;
    dstd::String lines;
    dstd::Vector<Arg> scratch_args;
    // Text bytes per row of the rows formatted so far, 0 before any; the
    // parallel loop sizes its waves by it.
    std::size_t row_bytes = 0;
    dstd::Vector<dstd::String> wave_texts;
    dstd::Vector<dstd::Vector<Arg>> wave_args;

    // partitioned output
    struct Partition {
        dstd::String rel;  // `key=value/...` under the folder
        dstd::String dir;  // the staged folder of the partition
        dstd::String buf;  // rows that wait to become a member
        std::uint64_t part_rows = 0;
        std::uint64_t buf_rows = 0;
        std::size_t part_index = 0;
        std::uint64_t c_end = 0;
        std::uint64_t last_use = 0;
        bool created = false;
        dstd::Optional<dstd::File> file;
        dstd::Vector<PartDone> finished;
        trace::index::WriteFeed::File* fed = nullptr;
        dstd::Vector<trace::index::FedMember> placed;
        std::size_t in_flight = 0;
    };
    dstd::Vector<dstd::UniquePtr<Partition>> partitions;
    dstd::HashMap<dstd::String, std::size_t> partition_of;
    dstd::HashMap<dstd::String, dstd::String> folded_dirs;
    dstd::Vector<std::size_t> key_columns;
    dstd::String key_scratch;
    dstd::String key_value;
    // Members of partitions that compress beside the writer, oldest first.
    // Each partition appends its own in order.
    struct Inflight {
        Partition* part = nullptr;
        std::uint64_t held = 0;
        coro::SpawnFuture<dstd::Result<CompressedMember>> future;
    };
    dstd::SharedPtr<CompressState> compression;
    dstd::Deque<Inflight> inflight;
    std::size_t open_files = 0;
    std::uint64_t tick = 0;
    std::uint64_t buffered = 0;
    std::uint64_t peak_buffered = 0;
    std::uint64_t buffer_budget = 0;
    SinkBudget shares{};

    // the index fed while writing
    dstd::Optional<trace::index::WriteFeed> feed;
    coro::Scope* scope = nullptr;
    coro::Scope* bound_scope = nullptr;
    trace::index::WriteFeed::File* part_feed = nullptr;
    std::size_t fed_file_limit = 0;
    std::size_t fed_files = 0;
    dstd::Vector<dstd::String> unfed;
    dstd::Vector<trace::index::FedMember> placed;
    dstd::Vector<trace::index::FedMember> fed_prefix;
    std::uint64_t stream_pos = 0;    // data bytes of the part fed so far
    std::uint64_t member_start = 0;  // where the member being fed began

    dstd::String final_path_of(dstd::StringView relative) const {
        return relative.empty() ? target : dstd::fmt<"{}/{}">(target, relative);
    }
    dstd::Result<trace::index::WriteFeed::File*> begin_fed_file(
        dstd::StringView final_path);
    coro::CoroTask<dstd::Result<void>> feed_header(
        trace::index::WriteFeed::File& file,
        dstd::Vector<trace::index::FedMember>& members);
    coro::CoroTask<dstd::Result<void>> feed_text(dstd::StringView text);
    coro::CoroTask<dstd::Result<void>> finish_feed(
        trace::index::WriteFeed::File& file,
        dstd::Vector<trace::index::FedMember> members, std::int64_t rows);
    fileio::GzipWriterOptions gzip_options(std::size_t part_index);

    bool partitioned() const { return !opts.partition_by.empty(); }
    std::size_t max_partitions() const {
        return opts.max_partitions ? opts.max_partitions
                                   : OUTPUT_MAX_PARTITIONS;
    }
    coro::CoroTask<dstd::Result<void>> write_partitioned(
        const dataframe::DataFrame& batch);
    dstd::Result<Partition*> partition_for(const dataframe::DataFrame& batch,
                                           std::size_t row);
    coro::CoroTask<dstd::Result<void>> open_part_file(Partition& p);
    coro::CoroTask<dstd::Result<void>> flush_partition(Partition& p);
    coro::CoroTask<dstd::Result<void>> reap_oldest();
    coro::CoroTask<dstd::Result<void>> reap_partition(Partition& p);
    coro::CoroTask<dstd::Result<void>> reap_all();
    coro::CoroTask<dstd::Result<void>> finish_partition_part(Partition& p);
    coro::CoroTask<dstd::Result<void>> finish_partitions();

    ~Impl() {
        gz.reset();
        if (!committed && !staging.empty())
            (void)dstd::fs::remove_all(dstd::PathView(staging));
    }

    bool gzip() const { return opts.compress; }
    bool records() const { return opts.rows == OutputRows::Records; }
    std::size_t member_size() const {
        return opts.member_size ? opts.member_size : OUTPUT_AUTO_MEMBER_SIZE;
    }

    // The rows part `index` will hold when the total is known up front.
    std::int64_t known_part_rows(std::size_t index) const {
        if (opts.known_rows < 0) return -1;
        if (opts.part_size == 0) return opts.known_rows;
        const auto begin = static_cast<std::int64_t>(index * opts.part_size);
        const auto left = std::max<std::int64_t>(0, opts.known_rows - begin);
        return std::min<std::int64_t>(
            left, static_cast<std::int64_t>(opts.part_size));
    }

    dstd::String header_with(std::int64_t nrows) const {
        dstd::String text = header_line;
        const dstd::String slot = slot_text(nrows);
        text.replace(slot_pos, NROWS_SLOT_WIDTH, slot);
        return text;
    }

    dstd::String part_staged_path(std::size_t index) const {
        if (is_file) return staging;
        return dstd::fmt<"{}/{}">(staging, part_file_name(index, gzip()));
    }

    dstd::Result<void> sweep_leftovers() const;
    dstd::Result<void> prepare_header(dstd::StringView args);
    dstd::Result<void> prepare_header_from(const dataframe::DataFrame* batch);
    dstd::Result<void> prepare_batch(const dataframe::DataFrame& batch);
    /// Rows [begin, end) as lines appended to `out`; `first_row` is the
    /// part's number of row `begin`, `row_base` its number in the whole
    /// output (for errors), and `args` is the caller's scratch, so calls with
    /// their own `out` and `args` may run at once.
    dstd::Result<void> format_records(const dataframe::DataFrame& batch,
                                      std::size_t begin, std::size_t end,
                                      dstd::String& out,
                                      std::uint64_t first_row,
                                      std::uint64_t row_base,
                                      dstd::Vector<Arg>& args) const;
    dstd::Result<void> format_events(const dataframe::DataFrame& batch,
                                     std::size_t begin, std::size_t end,
                                     dstd::String& out,
                                     dstd::Vector<Arg>& args) const;
    dstd::Result<void> format_rows(const dataframe::DataFrame& batch,
                                   std::size_t begin, std::size_t end,
                                   dstd::String& out, std::uint64_t first_row,
                                   std::uint64_t row_base,
                                   dstd::Vector<Arg>& args) const {
        return records() ? format_records(batch, begin, end, out, first_row,
                                          row_base, args)
                         : format_events(batch, begin, end, out, args);
    }
    coro::CoroTask<dstd::Result<OutputSummary>> finish_all();
    coro::CoroTask<dstd::Result<void>> drain_feeds();
    coro::CoroTask<dstd::Result<void>> write_batch(
        const dataframe::DataFrame& batch);
    coro::CoroTask<dstd::Result<void>> write_waves(
        const dataframe::DataFrame& batch, std::size_t begin, std::size_t end);
    coro::CoroTask<dstd::Result<void>> write_text(dstd::StringView text);
    coro::CoroTask<dstd::Result<void>> write_scoped(
        const dataframe::DataFrame* batch, dstd::StringView text);
    coro::CoroTask<dstd::Result<void>> append_text(dstd::StringView text);
    coro::CoroTask<dstd::Result<void>> roll_part();
    bool part_full() const {
        return (opts.part_size != 0 && part_rows >= opts.part_size) ||
               (opts.part_bytes != 0 && part_text >= opts.part_bytes);
    }
    coro::CoroTask<dstd::Result<void>> start_part();
    coro::CoroTask<dstd::Result<void>> finish_part();
    coro::CoroTask<dstd::Result<void>> flush_lines();
    dstd::Result<void> patch_and_sync(dstd::StringView path,
                                      std::uint64_t part_rows,
                                      std::int64_t written_slot) const;
    dstd::Result<void> write_empty_part(dstd::StringView path) const;
    dstd::Result<void> commit();

    // events
    struct EventColumns {
        std::size_t name = 0, cat = 0, pid = 0, tid = 0, ts = 0, ph = 0;
        dstd::Optional<std::size_t> dur;
        dstd::Vector<std::size_t> args;
    };
    EventColumns event_columns;
};

dstd::Result<void> OutputWriter::Impl::sweep_leftovers() const {
    const dstd::String tmp = dstd::fmt<".{}.tmp-">(leaf);
    const dstd::String old = dstd::fmt<".{}.old-">(leaf);
    auto exists = dstd::fs::exists(dstd::PathView(parent));
    if (!exists.has_value())
        return io_error("cannot stat", parent, exists.error());
    if (!*exists) return {};
    auto dir = dstd::fs::Dir::open(dstd::PathView(parent));
    if (!dir) return io_error("cannot open", parent, dir.error());
    auto entries = dir->entries();
    if (!entries) return io_error("cannot list", parent, entries.error());
    dstd::Vector<dstd::String> stale;
    for (const dstd::fs::DirEntry& e : *entries) {
        if (is_leftover_id(e.name, tmp) || is_leftover_id(e.name, old))
            stale.emplace_back(e.name);
    }
    for (const dstd::String& name : stale) {
        auto removed = dstd::fs::remove_all(
            dstd::PathView(dstd::fmt<"{}/{}">(parent, name)));
        if (!removed)
            return io_error("cannot remove the leftover", name,
                            removed.error());
    }
    return {};
}

dstd::Result<void> OutputWriter::Impl::prepare_header_from(
    const dataframe::DataFrame* batch) {
    dstd::String args(opts.header_args_json);
    if (!opts.fields_from_first_batch) return prepare_header(args);
    if (args.size() < 2 || args.front() != '{' || args.back() != '}')
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header args are not a JSON object");
    dstd::String fields = "{\"fields\":[";
    const std::size_t ncols = batch ? batch->num_columns() : 0;
    for (std::size_t i = 0; i < ncols; ++i) {
        const TypeId type = batch->columns[i].type();
        const char* dtype = nullptr;
        const char* list_elem = nullptr;
        switch (type) {
            case TypeId::Bool:
                dtype = "boolean";
                break;
            case TypeId::Int8:
                dtype = "Int8";
                break;
            case TypeId::Int16:
                dtype = "Int16";
                break;
            case TypeId::Int32:
                dtype = "Int32";
                break;
            case TypeId::Int64:
                dtype = "Int64";
                break;
            case TypeId::Uint8:
                dtype = "UInt8";
                break;
            case TypeId::Uint16:
                dtype = "UInt16";
                break;
            case TypeId::Uint32:
                dtype = "UInt32";
                break;
            case TypeId::Uint64:
                dtype = "UInt64";
                break;
            case TypeId::Float32:
                dtype = "Float32";
                break;
            case TypeId::Float64:
                dtype = "Float64";
                break;
            case TypeId::String:
            case TypeId::LargeString:
                dtype = "object";
                break;
            case TypeId::List:
            case TypeId::LargeList:
                dtype = "object";
                list_elem = list_element_name(batch->columns[i]);
                if (!list_elem)
                    return common::make_error<
                        "column {} is a list of a type a table cannot "
                        "store">(ErrorCode::InvalidArgument,
                                 dstd::StringView(batch->names[i]));
                break;
            default:
                return common::make_error<
                    "column {} has a type a table cannot store">(
                    ErrorCode::InvalidArgument,
                    dstd::StringView(batch->names[i]));
        }
        if (i) fields += ',';
        fields += "{\"name\":\"";
        json::append_json_escaped(fields, dstd::StringView(batch->names[i]));
        fields += "\",\"dtype\":\"";
        fields += dtype;
        if (list_elem) {
            fields += "\",\"elem\":\"list\",\"elem_dtype\":\"";
            fields += list_elem;
        }
        fields += "\"}";
    }
    fields += ']';
    if (args.size() > 2) fields += ',';
    fields += dstd::StringView(args).substr(1);
    return prepare_header(fields);
}

dstd::Result<void> OutputWriter::Impl::prepare_header(dstd::StringView args) {
    if (args.size() < 2 || args.front() != '{' || args.back() != '}')
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header args are not a JSON object");
    if (args.find('\n') != dstd::StringView::npos)
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header text holds a newline");
    dstd::String line(HEADER_PREFIX);
    line += ARGS_OPEN;
    slot_pos = line.size();
    line += slot_text(-1);
    const dstd::StringView rest = args.substr(1);
    if (rest.size() > 1) line += ',';
    line += rest;
    line += "}\n";
    header_line = std::move(line);
    // The slot must sit in the first stored block.
    if (slot_pos + NROWS_SLOT_WIDTH > fileio::GZIP_STORED_BLOCK_MAX)
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header does not start with its slot");
    // Check the header as a reader will see it.
    const dstd::String probe = header_with(0);
    json::Document doc;
    auto parsed = doc.parse(dstd::StringView(probe));
    if (!parsed)
        return common::make_error<"the header is not valid JSON: {}">(
            ErrorCode::InvalidArgument, parsed.error().message());
    auto record_args = parsed->find("args");
    if (!record_args)
        return common::make_error(ErrorCode::Internal,
                                  "the header record has no args");
    auto h = table::detail::parse_header(*record_args);
    if (!h) return h.error();
    header = std::move(*h);
    table_name = header.table;
    header_ready = true;
    return {};
}

dstd::Result<void> OutputWriter::Impl::prepare_batch(
    const dataframe::DataFrame& batch) {
    const std::size_t ncols = batch.num_columns();
    if (records()) {
        if (!header_ready) {
            if (auto prepared = prepare_header_from(&batch); !prepared)
                return prepared;
        }
        if (header.fields.size() != ncols)
            return common::make_error<
                "the header has {} fields, the frame {} "
                "columns">(ErrorCode::InvalidArgument, header.fields.size(),
                           ncols);
        columns.clear();
        columns.reserve(ncols);
        for (std::size_t i = 0; i < ncols; ++i) {
            const dstd::StringView name(batch.names[i]);
            if (name != dstd::StringView(header.fields[i].name))
                return common::make_error<
                    "column {} is named {} in the header, "
                    "{} in the frame">(ErrorCode::InvalidArgument, i,
                                       dstd::StringView(header.fields[i].name),
                                       name);
            const Field& field = header.fields[i];
            bool raw = field.kind == Kind::List;
            for (const dstd::String& r : opts.raw_columns)
                if (dstd::StringView(r) == name) raw = true;
            if (field.kind == Kind::List) {
                const char* elem = list_element_name(batch.columns[i]);
                if (!elem || dstd::StringView(field.elem_dtype) != elem)
                    return common::make_error<
                        "column {} is not a list of {}, the element dtype of "
                        "its header field">(ErrorCode::InvalidArgument, name,
                                            dstd::StringView(field.elem_dtype));
            }
            auto c = prepare_column(batch, i, raw);
            if (!c) return c.error();
            if (!kind_accepts(header.fields[i], c->type, c->raw))
                return common::make_error<
                    "column {} does not fit the dtype of its "
                    "header field">(ErrorCode::InvalidArgument, name);
            if (c->has_nulls && !header.fields[i].nulls_ok)
                return common::make_error<
                    "column {} has nulls, its dtype has "
                    "none">(ErrorCode::InvalidArgument, name);
            columns.push_back(std::move(*c));
        }
        for (const dstd::String& r : opts.raw_columns) {
            bool found = false;
            for (std::size_t i = 0; i < ncols; ++i)
                if (dstd::StringView(batch.names[i]) == dstd::StringView(r))
                    found = true;
            if (!found)
                return common::make_error<"raw column {} is not in the frame">(
                    ErrorCode::InvalidArgument, dstd::StringView(r));
        }
        return {};
    }
    EventColumns ec;
    bool has[6] = {false, false, false, false, false, false};
    columns.clear();
    columns.reserve(ncols);
    for (std::size_t i = 0; i < ncols; ++i) {
        const dstd::StringView name(batch.names[i]);
        auto c = prepare_column(batch, i, false);
        if (!c) return c.error();
        const auto need_text = [&]() -> dstd::Result<void> {
            if (!is_string_type(c->type))
                return common::make_error<"event column {} is not text">(
                    ErrorCode::InvalidArgument, name);
            return {};
        };
        const auto need_int = [&]() -> dstd::Result<void> {
            if (!is_integer_type(c->type))
                return common::make_error<"event column {} is not an integer">(
                    ErrorCode::InvalidArgument, name);
            return {};
        };
        dstd::Result<void> ok;
        if (name == "name") {
            ec.name = i;
            has[0] = true;
            ok = need_text();
        } else if (name == "cat") {
            ec.cat = i;
            has[1] = true;
            ok = need_text();
        } else if (name == "pid") {
            ec.pid = i;
            has[2] = true;
            ok = need_int();
        } else if (name == "tid") {
            ec.tid = i;
            has[3] = true;
            ok = need_int();
        } else if (name == "ts") {
            ec.ts = i;
            has[4] = true;
            ok = need_int();
        } else if (name == "ph") {
            ec.ph = i;
            has[5] = true;
            ok = need_int();
        } else if (name == "dur") {
            ec.dur = i;
            ok = need_int();
        } else if ((name.starts_with(ARGS_KEY) &&
                    name.size() > ARGS_KEY.size()) ||
                   name == "hhash" || name == "fhash" || name == "shash") {
            ec.args.push_back(i);
        } else {
            return common::make_error<
                "column {} is not part of an event: events carry name, cat, "
                "pid, tid, ts, ph, dur, hhash, fhash, shash and args.<key> "
                "columns">(ErrorCode::InvalidArgument, name);
        }
        if (!ok) return ok.error();
        columns.push_back(std::move(*c));
    }
    static constexpr dstd::StringView NEEDED[6] = {"name", "cat", "pid",
                                                   "tid",  "ts",  "ph"};
    for (std::size_t k = 0; k < 6; ++k)
        if (!has[k])
            return common::make_error<"the events lack the column {}">(
                ErrorCode::InvalidArgument, NEEDED[k]);
    event_columns = std::move(ec);
    return {};
}

dstd::Result<void> OutputWriter::Impl::format_records(
    const dataframe::DataFrame& batch, std::size_t begin, std::size_t end,
    dstd::String& out, std::uint64_t first_row, std::uint64_t row_base,
    dstd::Vector<Arg>& args) const {
    const std::size_t ncols = columns.size();
    args.resize(ncols);
    for (std::size_t i = 0; i < ncols; ++i)
        args[i].key = dstd::StringView(batch.names[i]);
    const dstd::StringView table(table_name);
    for (std::size_t row = begin; row < end; ++row) {
        for (std::size_t i = 0; i < ncols; ++i) {
            args[i].value = value_at(columns[i], row);
            if (columns[i].raw && args[i].value.kind == ArgValue::Kind::Raw) {
                const dstd::StringView text = args[i].value.s;
                if (text.empty() || text.find('\n') != dstd::StringView::npos)
                    return common::make_error<
                        "row {}, column {}: a raw cell "
                        "is empty or holds a newline">(
                        ErrorCode::InvalidArgument, row_base + (row - begin),
                        dstd::StringView(batch.names[i]));
            }
        }
        format_aggregated(out, table, "dftu", first_row + (row - begin), 0, 0,
                          args);
    }
    return {};
}

dstd::Result<void> OutputWriter::Impl::format_events(
    const dataframe::DataFrame& batch, std::size_t begin, std::size_t end,
    dstd::String& out, dstd::Vector<Arg>& args) const {
    const EventColumns& ec = event_columns;
    for (std::size_t row = begin; row < end; ++row) {
        auto name = event_text(columns[ec.name], row, "name");
        if (!name) return name.error();
        auto cat = event_text(columns[ec.cat], row, "cat");
        if (!cat) return cat.error();
        auto pid = event_uint(columns[ec.pid], row, "pid");
        if (!pid) return pid.error();
        auto tid = event_uint(columns[ec.tid], row, "tid");
        if (!tid) return tid.error();
        auto ts = event_uint(columns[ec.ts], row, "ts");
        if (!ts) return ts.error();
        auto ph = event_uint(columns[ec.ph], row, "ph");
        if (!ph) return ph.error();
        std::uint64_t dur = 0;
        if (ec.dur) {
            const OutColumn& dc = columns[*ec.dur];
            const bool null = dc.has_nulls &&
                              dc.series.is_null(static_cast<std::int64_t>(row));
            if (!null) {
                auto d = event_uint(dc, row, "dur");
                if (!d) return d.error();
                dur = *d;
            }
        }
        args.clear();
        for (const std::size_t i : ec.args) {
            const ArgValue v = value_at(columns[i], row);
            if (v.kind == ArgValue::Kind::Null) continue;
            const dstd::StringView column(batch.names[i]);
            args.push_back({column.starts_with(ARGS_KEY)
                                ? column.substr(ARGS_KEY.size())
                                : column,
                            v});
        }
        switch (*ph) {
            case 1:
                format_event(out, *name, *cat, *ts, dur, *pid, *tid, args);
                break;
            case 2:
                format_counter(out, *name, *cat, *ts, *pid, *tid, args);
                break;
            case 3:
                format_aggregated(out, *name, *cat, *ts, *pid, *tid, args);
                break;
            case 4:
                format_metadata(out, *name, *pid, *tid, args);
                break;
            default:
                return common::make_error<
                    "row {}: the phase {} is not one of 1, 2, 3 and 4">(
                    ErrorCode::InvalidArgument, row, *ph);
        }
    }
    return {};
}

fileio::GzipWriterOptions OutputWriter::Impl::gzip_options(
    std::size_t part_index) {
    fileio::GzipWriterOptions g;
    g.member_size = member_size();
    g.level = opts.level;
    g.workers = opts.workers;
    g.compress = opts.compress;
    g.memory_budget = shares.members;
    if (records()) {
        g.part_header = header_with(known_part_rows(part_index));
        g.part_header_stored = opts.compress;
    }
    if (feed)
        g.on_member = [this](const fileio::MemberInfo& m) {
            placed.push_back({m.c_offset, m.c_size, m.uc_size, m.lines});
        };
    return g;
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::start_part() {
    part_path = part_staged_path(done.size());
    fileio::GzipWriterOptions g = gzip_options(done.size());
    auto opening = fileio::GzipLineWriter::open(part_path, std::move(g));
    auto opened = co_await std::move(opening);
    if (!opened) co_return opened.error();
    gz.emplace(std::move(*opened));
    part_rows = 0;
    part_text = 0;
    if (feed) {
        const dstd::String name = part_file_name(done.size(), gzip());
        auto added = begin_fed_file(final_path_of(
            is_file ? dstd::StringView() : dstd::StringView(name)));
        if (!added) co_return added.error();
        part_feed = *added;
        placed.clear();
        stream_pos = 0;
        member_start = 0;
        dstd::Vector<trace::index::FedMember> header_member;
        if (records() && part_feed != nullptr) {
            auto feeding = feed_header(*part_feed, header_member);
            auto fed = co_await std::move(feeding);
            if (!fed) co_return fed;
        }
        fed_prefix = std::move(header_member);
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::append_text(
    dstd::StringView text) {
    auto appending = gz->append(text);
    auto appended = co_await std::move(appending);
    if (appended && part_feed != nullptr) {
        auto feeding = feed_text(text);
        appended = co_await std::move(feeding);
    }
    part_text += text.size();
    co_return appended;
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::flush_lines() {
    if (lines.empty()) co_return dstd::Result<void>{};
    auto appending = append_text(lines);
    auto appended = co_await std::move(appending);
    lines.clear();
    co_return appended;
}

// Finishes a full part and starts the next one when none is open.
coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::roll_part() {
    if (gz && part_full()) {
        auto finishing = finish_part();
        auto finished = co_await std::move(finishing);
        if (!finished) co_return finished;
    }
    if (!gz) {
        auto starting = start_part();
        auto started = co_await std::move(starting);
        if (!started) co_return started;
    }
    co_return dstd::Result<void>{};
}

dstd::Result<void> OutputWriter::Impl::patch_and_sync(
    dstd::StringView path, std::uint64_t rows_in_part,
    std::int64_t written_slot) const {
    dstd::OpenOptions open_options;
    open_options.access = dstd::Access::ReadWrite;
    auto file = dstd::File::open(dstd::PathView(path), open_options);
    if (!file) return io_error("cannot reopen", path, file.error());
    if (records() && written_slot != static_cast<std::int64_t>(rows_in_part)) {
        const auto real = static_cast<std::int64_t>(rows_in_part);
        const dstd::String slot = slot_text(real);
        const std::size_t base =
            gzip() ? GZIP_MEMBER_HEADER_BYTES + STORED_BLOCK_HEADER_BYTES : 0;
        auto wrote = file->write_all_at(
            base + slot_pos,
            dstd::ByteView(reinterpret_cast<const std::uint8_t*>(slot.data()),
                           slot.size()));
        if (!wrote)
            return io_error("cannot patch the header of", path, wrote.error());
        if (gzip()) {
            const dstd::String text = header_with(real);
            const std::size_t blocks = std::max<std::size_t>(
                1, (text.size() + fileio::GZIP_STORED_BLOCK_MAX - 1) /
                       fileio::GZIP_STORED_BLOCK_MAX);
            const std::size_t member_size_bytes =
                GZIP_MEMBER_HEADER_BYTES + STORED_BLOCK_HEADER_BYTES * blocks +
                text.size() + 8;
            const std::uint32_t crc =
                fileio::compress::gzip_crc32(0, text.data(), text.size());
            std::uint8_t bytes[4];
            for (int i = 0; i < 4; ++i)
                bytes[i] = static_cast<std::uint8_t>(crc >> (8 * i));
            auto patched = file->write_all_at(member_size_bytes - 8,
                                              dstd::ByteView(bytes, 4));
            if (!patched)
                return io_error("cannot patch the CRC32 of", path,
                                patched.error());
        }
    }
    auto synced = file->sync(dstd::SyncMode::All);
    if (!synced) return io_error("cannot sync", path, synced.error());
    auto closed = file->close();
    if (!closed) return io_error("cannot close", path, closed.error());
    return {};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::finish_part() {
    auto closing = gz->close();
    auto summary = co_await std::move(closing);
    gz.reset();
    if (!summary) co_return summary.error();
    if (summary->lines != part_rows)
        co_return common::make_error<"{} wrote {} lines for {} rows">(
            ErrorCode::Internal, dstd::StringView(part_path), summary->lines,
            part_rows);
    const std::int64_t known = known_part_rows(done.size());
    if (known >= 0 && static_cast<std::uint64_t>(known) != part_rows)
        co_return common::make_error<"{} holds {} rows, its header says {}">(
            ErrorCode::InvalidArgument, dstd::StringView(part_path), part_rows,
            known);
    if (auto synced = patch_and_sync(part_path, part_rows, known); !synced)
        co_return synced.error();
    if (part_feed != nullptr) {
        if (stream_pos > member_start) {
            auto ending = feed->end_member(*part_feed, *scope);
            auto ended = co_await std::move(ending);
            if (!ended) co_return ended.error();
        }
        dstd::Vector<trace::index::FedMember> members = std::move(fed_prefix);
        for (const auto& m : placed) members.push_back(m);
        const std::int64_t table_rows =
            records() ? static_cast<std::int64_t>(part_rows) : -1;
        auto finishing =
            finish_feed(*part_feed, std::move(members), table_rows);
        auto finished_feed = co_await std::move(finishing);
        if (!finished_feed) co_return finished_feed.error();
        part_feed = nullptr;
        placed.clear();
        fed_prefix.clear();
    }
    PartDone p;
    p.staged = part_path;
    p.name = is_file ? leaf : part_file_name(done.size(), gzip());
    p.rows = part_rows;
    done.push_back(std::move(p));
    part_rows = 0;
    co_return dstd::Result<void>{};
}

dstd::Result<void> OutputWriter::Impl::write_empty_part(
    dstd::StringView path) const {
    dstd::String text;
    if (records()) {
        const std::int64_t known = known_part_rows(0);
        text = header_with(known >= 0 ? known : 0);
    }
    dstd::String bytes;
    if (gzip()) {
        const auto member = fileio::gzip_stored_member(text);
        bytes.assign(reinterpret_cast<const char*>(member.data()),
                     member.size());
    } else {
        bytes = std::move(text);
    }
    auto wrote = dstd::fs::write_file(dstd::PathView(path), bytes);
    if (!wrote) return io_error("cannot write", path, wrote.error());
    dstd::OpenOptions open_options;
    open_options.access = dstd::Access::ReadWrite;
    auto file = dstd::File::open(dstd::PathView(path), open_options);
    if (!file) return io_error("cannot reopen", path, file.error());
    auto synced = file->sync(dstd::SyncMode::All);
    if (!synced) return io_error("cannot sync", path, synced.error());
    return {};
}

dstd::Result<void> replace_folder(dstd::StringView staging,
                                  dstd::StringView target,
                                  dstd::StringView aside) {
    auto moved =
        dstd::fs::rename(dstd::PathView(target), dstd::PathView(aside));
    if (!moved) return io_error("cannot move aside", target, moved.error());
    auto renamed =
        dstd::fs::rename(dstd::PathView(staging), dstd::PathView(target));
    if (renamed) return {};
    auto restored =
        dstd::fs::rename(dstd::PathView(aside), dstd::PathView(target));
    if (restored)
        return io_error("cannot rename into", target, renamed.error());
    return common::make_error<
        "cannot rename {} into {}: {}; cannot restore {} from {}: {}">(
        ErrorCode::Io, staging, target, renamed.error().message(), target,
        aside, restored.error().message());
}

dstd::Result<void> OutputWriter::Impl::commit() {
    dstd::String old_aside;
    if (opts.mode == OutputMode::Error) {
        auto moved = dstd::fs::rename_no_replace(dstd::PathView(staging),
                                                 dstd::PathView(target));
        if (!moved.has_value())
            return io_error("cannot rename into", target, moved.error());
        if (!*moved)
            return common::make_error<"{} already exists">(
                ErrorCode::InvalidArgument, dstd::StringView(target));
    } else {
        auto present = exists_no_follow(target);
        if (!present.has_value()) return present.error();
        if (*present) {
            auto st = dstd::fs::lstat(dstd::PathView(target));
            if (!st) return io_error("cannot stat", target, st.error());
            const bool is_dir = st->kind == dstd::fs::FileKind::Directory;
            if (is_dir != !is_file)
                return common::make_error<
                    "{} is {} and cannot be overwritten by {}">(
                    ErrorCode::InvalidArgument, dstd::StringView(target),
                    is_dir ? "a folder" : "not a folder",
                    is_file ? "a file" : "a folder");
            if (!is_file)
                old_aside = dstd::fmt<"{}/.{}.old-{}">(parent, leaf,
                                                       dstd::StringView(id));
        }
        if (!old_aside.empty()) {
            if (auto replaced = replace_folder(staging, target, old_aside);
                !replaced)
                return replaced;
        } else {
            auto renamed = dstd::fs::rename(dstd::PathView(staging),
                                            dstd::PathView(target));
            if (!renamed)
                return io_error("cannot rename into", target, renamed.error());
        }
    }
    committed = true;
    auto synced = sync_directory(parent);
    if (!old_aside.empty())
        (void)dstd::fs::remove_all(dstd::PathView(old_aside));
    return synced;
}

dstd::Result<OutputWriter::Impl::Partition*> OutputWriter::Impl::partition_for(
    const dataframe::DataFrame& batch, std::size_t row) {
    key_scratch.clear();
    for (std::size_t k = 0; k < key_columns.size(); ++k) {
        const std::size_t c = key_columns[k];
        if (k) key_scratch += '/';
        percent_encode(key_scratch, dstd::StringView(batch.names[c]), true);
        key_scratch += '=';
        if (!key_text(key_value, value_at(columns[c], row))) {
            key_scratch += HIVE_DEFAULT_PARTITION;
        } else if (dstd::StringView(key_value) == HIVE_DEFAULT_PARTITION) {
            key_scratch += "%5F";
            percent_encode(key_scratch, dstd::StringView(key_value).substr(1),
                           false);
        } else {
            percent_encode(key_scratch, key_value, false);
        }
    }
    if (auto found = partition_of.find(key_scratch);
        found != partition_of.end())
        return partitions[found->second].get();
    if (partitions.size() >= max_partitions())
        return common::make_error<
            "the write makes at least {} partitions, over the limit of {}">(
            ErrorCode::InvalidArgument, partitions.size() + 1,
            max_partitions());
    dstd::StringView rest(key_scratch);
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::size_t length =
            slash == dstd::StringView::npos ? rest.size() : slash;
        if (length > MAX_COMPONENT_BYTES)
            return common::make_error<
                "the partition folder name {} is {} bytes, over the limit of "
                "{}">(ErrorCode::InvalidArgument, rest.substr(0, 40), length,
                      MAX_COMPONENT_BYTES);
        rest = slash == dstd::StringView::npos ? dstd::StringView()
                                               : rest.substr(slash + 1);
    }
    if (key_scratch.size() > MAX_RELATIVE_PATH_BYTES)
        return common::make_error<
            "the partition path is {} bytes, over the limit of {}">(
            ErrorCode::InvalidArgument, key_scratch.size(),
            MAX_RELATIVE_PATH_BYTES);
    // Folders that differ only in case are one folder on a case-insensitive
    // file system, so the write would fail after it had begun.
    for (std::size_t at = 0; at <= key_scratch.size(); ++at) {
        if (at != key_scratch.size() && key_scratch[at] != '/') continue;
        const dstd::StringView prefix =
            dstd::StringView(key_scratch).substr(0, at);
        dstd::String folded(prefix);
        for (char& c : folded)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        auto seen = folded_dirs.find(folded);
        if (seen == folded_dirs.end()) {
            folded_dirs.emplace(std::move(folded), dstd::String(prefix));
        } else if (dstd::StringView(seen->second) != prefix) {
            return common::make_error<
                "the partition folders {} and {} differ only in case and are "
                "one folder on a case-insensitive file system">(
                ErrorCode::InvalidArgument, dstd::StringView(seen->second),
                prefix);
        }
    }
    auto p = dstd::make_unique<Partition>();
    p->rel = key_scratch;
    p->dir = dstd::fmt<"{}/{}">(staging, dstd::StringView(key_scratch));
    partition_of.emplace(key_scratch, partitions.size());
    partitions.push_back(std::move(p));
    return partitions.back().get();
}

dstd::Result<trace::index::WriteFeed::File*> OutputWriter::Impl::begin_fed_file(
    dstd::StringView final_path) {
    if (!feed) return static_cast<trace::index::WriteFeed::File*>(nullptr);
    if (fed_files >= fed_file_limit) {
        unfed.push_back(dstd::String(final_path));
        return static_cast<trace::index::WriteFeed::File*>(nullptr);
    }
    ++fed_files;
    return feed->add_file(final_path);
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::feed_header(
    trace::index::WriteFeed::File& file,
    dstd::Vector<trace::index::FedMember>& members) {
    // A header that is still to be patched holds -1, which a reader refuses;
    // the index is told the real count when the file is finished.
    const dstd::String text = header_with(0);
    auto feeding = feed->lines(file, text, *scope);
    auto fed = co_await std::move(feeding);
    if (!fed) co_return fed;
    auto ending = feed->end_member(file, *scope);
    auto ended = co_await std::move(ending);
    if (!ended) co_return ended;
    members.push_back({0, stored_member_bytes(text.size()), text.size(), 1});
    co_return dstd::Result<void>{};
}

// Feeds `text`, which is appended to the part's data stream, cutting it into
// the members the gzip writer cuts: at the first newline at or after
// member_size bytes into a member.
coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::feed_text(
    dstd::StringView text) {
    while (!text.empty()) {
        const std::uint64_t cut_from = member_start + member_size();
        std::size_t cut = dstd::StringView::npos;
        if (cut_from < stream_pos + text.size()) {
            const std::size_t from =
                cut_from > stream_pos
                    ? static_cast<std::size_t>(cut_from - stream_pos)
                    : 0;
            cut = text.find('\n', from);
        }
        const dstd::StringView head =
            cut == dstd::StringView::npos ? text : text.substr(0, cut + 1);
        auto feeding = feed->lines(*part_feed, head, *scope);
        auto fed = co_await std::move(feeding);
        if (!fed) co_return fed;
        stream_pos += head.size();
        text = text.substr(head.size());
        if (cut != dstd::StringView::npos) {
            auto ending = feed->end_member(*part_feed, *scope);
            auto ended = co_await std::move(ending);
            if (!ended) co_return ended;
            member_start = stream_pos;
        }
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::finish_feed(
    trace::index::WriteFeed::File& file,
    dstd::Vector<trace::index::FedMember> members, std::int64_t row_count) {
    auto draining = feed->drain(file);
    auto drained = co_await std::move(draining);
    if (!drained) co_return drained;
    co_return feed->finish(file, std::move(members), row_count);
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::open_part_file(
    Partition& p) {
    p.last_use = ++tick;
    if (p.file) co_return dstd::Result<void>{};
    while (open_files >= opts.max_open_files) {
        Partition* victim = nullptr;
        for (const auto& q : partitions)
            if (q->file && (!victim || q->last_use < victim->last_use))
                victim = q.get();
        if (!victim) break;
        auto closed = victim->file->close();
        victim->file.reset();
        --open_files;
        if (!closed)
            co_return io_error("cannot close a part of", victim->dir,
                               closed.error());
    }
    const dstd::String name = part_file_name(p.part_index, true);
    const dstd::String path = dstd::fmt<"{}/{}">(p.dir, name);
    dstd::OpenOptions open_options;
    open_options.access = dstd::Access::ReadWrite;
    if (!p.created) {
        auto made = dstd::fs::create_directories(dstd::PathView(p.dir));
        if (!made.has_value())
            co_return io_error("cannot create", p.dir, made.error());
        open_options.creation = dstd::Creation::OnlyIfNew;
    }
    auto file = dstd::File::open(dstd::PathView(path), open_options);
    if (!file) co_return io_error("cannot open", path, file.error());
    p.file.emplace(std::move(*file));
    ++open_files;
    if (!p.created) {
        p.created = true;
        p.c_end = 0;
        if (records()) {
            const auto first = fileio::gzip_stored_member(header_with(-1));
            auto wrote = p.file->write_all_at(
                0, dstd::ByteView(first.data(), first.size()));
            if (!wrote) co_return io_error("cannot write", path, wrote.error());
            p.c_end = first.size();
        }
        if (feed) {
            auto added = begin_fed_file(final_path_of(dstd::fmt<"{}/{}">(
                dstd::StringView(p.rel), dstd::StringView(name))));
            if (!added) co_return added.error();
            p.fed = *added;
            p.placed.clear();
            if (records() && p.fed != nullptr) {
                auto feeding = feed_header(*p.fed, p.placed);
                auto fed = co_await std::move(feeding);
                if (!fed) co_return fed;
            }
        }
    }
    co_return dstd::Result<void>{};
}

// Starts compressing what the partition buffered as one member. Its bytes are
// appended to the part file, and its text fed to the index, by reap_oldest()
// in the order the members were started.
coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::flush_partition(
    Partition& p) {
    if (p.buf.empty()) co_return dstd::Result<void>{};
    auto opening = open_part_file(p);
    auto opened = co_await std::move(opening);
    if (!opened) co_return opened;
    if (!compression)
        compression = dstd::make_shared<CompressState>(
            opts.level, std::max<std::uint64_t>(buffer_budget / 2, 1));
    // A member holds its text and, until it is appended, its compressed bytes,
    // which are at most a little over the text.
    const std::uint64_t held = 2 * static_cast<std::uint64_t>(p.buf.size());
    // Members that are already compressed are appended first, which does not
    // suspend, so the writer waits only when the bytes in flight are spent.
    while (!inflight.empty() && inflight.front().future.await_ready()) {
        auto reaping = reap_oldest();
        auto reaped = co_await std::move(reaping);
        if (!reaped) co_return reaped;
    }
    while (!inflight.empty() && !compression->bytes.try_acquire(held)) {
        auto reaping = reap_oldest();
        auto reaped = co_await std::move(reaping);
        if (!reaped) co_return reaped;
    }
    if (inflight.empty()) {
        auto acquiring = compression->bytes.acquire(held);
        co_await acquiring;
    }
    const std::uint64_t text_size = p.buf.size();
    const std::uint64_t member_rows = p.buf_rows;
    inflight.push_back(
        Inflight{&p, held,
                 scope->spawn_future(compress_member(
                     compression, std::move(p.buf), member_rows))});
    ++p.in_flight;
    buffered -= text_size;
    p.buf = dstd::String();
    p.buf_rows = 0;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::reap_oldest() {
    Inflight member = std::move(inflight.front());
    inflight.pop_front();
    Partition& p = *member.part;
    --p.in_flight;
    auto compressed = co_await member.future;
    compression->bytes.release(member.held);
    if (!compressed) co_return compressed.error();
    auto opening = open_part_file(p);
    auto opened = co_await std::move(opening);
    if (!opened) co_return opened;
    const dstd::Vector<std::uint8_t>& bytes = compressed->bytes;
    auto wrote = p.file->write_all_at(
        p.c_end, dstd::ByteView(bytes.data(), bytes.size()));
    if (!wrote) co_return io_error("cannot write", p.dir, wrote.error());
    if (p.fed != nullptr) {
        auto feeding = feed->lines(*p.fed, compressed->text, *scope);
        auto fed = co_await std::move(feeding);
        if (!fed) co_return fed;
        auto ending = feed->end_member(*p.fed, *scope);
        auto ended = co_await std::move(ending);
        if (!ended) co_return ended;
        p.placed.push_back(
            {p.c_end, bytes.size(), compressed->text.size(), compressed->rows});
    }
    p.c_end += bytes.size();
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::reap_partition(
    Partition& p) {
    while (p.in_flight > 0) {
        auto reaping = reap_oldest();
        auto reaped = co_await std::move(reaping);
        if (!reaped) co_return reaped;
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::reap_all() {
    while (!inflight.empty()) {
        auto reaping = reap_oldest();
        auto reaped = co_await std::move(reaping);
        if (!reaped) co_return reaped;
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::finish_partition_part(
    Partition& p) {
    auto flushing = flush_partition(p);
    auto flushed = co_await std::move(flushing);
    if (!flushed) co_return flushed;
    auto reaping = reap_partition(p);
    auto reaped = co_await std::move(reaping);
    if (!reaped) co_return reaped;
    if (p.file) {
        auto closed = p.file->close();
        p.file.reset();
        --open_files;
        if (!closed) co_return io_error("cannot close", p.dir, closed.error());
    }
    const dstd::String name = part_file_name(p.part_index, true);
    const dstd::String path = dstd::fmt<"{}/{}">(p.dir, name);
    if (auto synced = patch_and_sync(path, p.part_rows, -1); !synced)
        co_return synced;
    if (p.fed != nullptr) {
        auto finishing = finish_feed(
            *p.fed, std::move(p.placed),
            records() ? static_cast<std::int64_t>(p.part_rows) : -1);
        auto finished_feed = co_await std::move(finishing);
        if (!finished_feed) co_return finished_feed;
        p.fed = nullptr;
        p.placed.clear();
    }
    PartDone d;
    d.staged = path;
    d.name =
        dstd::fmt<"{}/{}">(dstd::StringView(p.rel), dstd::StringView(name));
    d.rows = p.part_rows;
    p.finished.push_back(std::move(d));
    ++p.part_index;
    p.part_rows = 0;
    p.created = false;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::write_partitioned(
    const dataframe::DataFrame& batch) {
    if (buffer_budget == 0)
        buffer_budget = std::max<std::uint64_t>(shares.partitions,
                                                std::uint64_t{64} * 1024);
    key_columns.clear();
    for (const dstd::String& name : opts.partition_by) {
        std::size_t found = batch.num_columns();
        for (std::size_t i = 0; i < batch.num_columns(); ++i)
            if (dstd::StringView(batch.names[i]) == dstd::StringView(name))
                found = i;
        if (found == batch.num_columns())
            co_return common::make_error<
                "the partition column {} is not a column of the frame">(
                ErrorCode::InvalidArgument, dstd::StringView(name));
        if (columns[found].raw)
            co_return common::make_error<
                "the partition column {} holds JSON text">(
                ErrorCode::InvalidArgument, dstd::StringView(name));
        key_columns.push_back(found);
    }
    const auto total = static_cast<std::size_t>(batch.num_rows());
    for (std::size_t row = 0; row < total; ++row) {
        auto found = partition_for(batch, row);
        if (!found) co_return found.error();
        Partition& p = **found;
        if (opts.part_size != 0 && p.part_rows >= opts.part_size) {
            auto finishing = finish_partition_part(p);
            auto step = co_await std::move(finishing);
            if (!step) co_return step;
        }
        const std::size_t before = p.buf.size();
        auto formatted = format_rows(batch, row, row + 1, p.buf, p.part_rows,
                                     rows, scratch_args);
        if (!formatted) co_return formatted;
        ++p.part_rows;
        ++p.buf_rows;
        ++rows;
        buffered += p.buf.size() - before;
        peak_buffered = std::max(peak_buffered, buffered);
        if (p.buf.size() >= member_size()) {
            auto flushing = flush_partition(p);
            auto step = co_await std::move(flushing);
            if (!step) co_return step;
        }
        while (buffered >= buffer_budget) {
            Partition* largest = nullptr;
            for (const auto& q : partitions)
                if (!largest || q->buf.size() > largest->buf.size())
                    largest = q.get();
            auto flushing = flush_partition(*largest);
            auto step = co_await std::move(flushing);
            if (!step) co_return step;
        }
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::finish_partitions() {
    dstd::Vector<Partition*> order;
    for (const auto& p : partitions) order.push_back(p.get());
    std::sort(order.begin(), order.end(),
              [](const Partition* a, const Partition* b) {
                  return dstd::StringView(a->rel) < dstd::StringView(b->rel);
              });
    for (Partition* p : order) {
        auto flushing = flush_partition(*p);
        auto flushed = co_await std::move(flushing);
        if (!flushed) co_return flushed;
    }
    for (Partition* p : order) {
        if (p->part_rows > 0 || !p->buf.empty()) {
            auto finishing = finish_partition_part(*p);
            auto step = co_await std::move(finishing);
            if (!step) co_return step;
        }
        for (PartDone& d : p->finished) done.push_back(std::move(d));
        p->finished.clear();
        dstd::StringView dir(p->dir);
        while (dir.size() > staging.size()) {
            if (auto synced = sync_directory(dir); !synced) co_return synced;
            dir = dir.substr(0, dir.rfind('/'));
        }
    }
    co_return dstd::Result<void>{};
}

SinkBudget split_sink_budget(std::uint64_t budget) {
    if (budget == memory::NO_SPILL_BUDGET)
        return {budget, budget, budget, budget};
    const std::uint64_t total = memory::resolve_spill_budget(budget);
    const std::uint64_t quarter = total / 4;
    const std::uint64_t index = total / 8 * 3;
    return {quarter, quarter, index, total - 2 * quarter - index};
}

dstd::Result<OutputWriter> OutputWriter::open(OutputOptions options) {
    auto impl = dstd::make_unique<Impl>();
    Impl& s = *impl;
    s.opts = std::move(options);
    OutputOptions& o = s.opts;
    s.shares = split_sink_budget(o.memory_budget);
    if (o.memory_budget != memory::NO_SPILL_BUDGET)
        s.idle_budget.emplace(memory::resolve_spill_budget(o.memory_budget));
    if (o.path.empty())
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the output path is empty");
    s.is_file = o.one_file || is_file_output(without_trailing_slashes(o.path));
    s.target = dstd::String(without_trailing_slashes(o.path));
    if (s.is_file) {
        if (is_file_output(s.target) &&
            o.compress != s.target.ends_with(GZ_EXTENSION))
            return common::make_error<
                "compress does not agree with the extension of {}">(
                ErrorCode::InvalidArgument, dstd::StringView(s.target));
        if (o.part_size != 0 || o.part_bytes != 0)
            return common::make_error<
                "part_size and part_bytes need a folder, but {} names one "
                "file">(ErrorCode::InvalidArgument, dstd::StringView(s.target));
    } else if (!o.compress) {
        return common::make_error<
            "{} is a folder of parts, which is gzip; write a single .pfw file "
            "for plain output">(ErrorCode::InvalidArgument,
                                dstd::StringView(s.target));
    }
    if (!o.partition_by.empty() && s.is_file)
        return common::make_error<
            "partition_by needs a folder, but {} names one file">(
            ErrorCode::InvalidArgument, dstd::StringView(s.target));
    if (o.part_bytes != 0 && !o.partition_by.empty())
        return common::make_error(
            ErrorCode::InvalidArgument,
            "part_bytes cannot split a partitioned output");
    if (o.part_bytes != 0 && o.known_rows >= 0)
        return common::make_error(
            ErrorCode::InvalidArgument,
            "part_bytes cannot split an output whose row count is known");
    if (o.build_index && (s.records() || !o.aggregation_tier)) {
        if (!o.compress)
            return common::make_error<"a plain output {} cannot be indexed">(
                ErrorCode::InvalidArgument, dstd::StringView(s.target));
        trace::index::IndexerOptions io;
        io.index_dir = o.index_dir;
        if (io.index_dir.empty() && !s.is_file) io.index_dir = s.target;
        io.memory_budget = s.shares.index;
        if (!s.records()) io.schema = o.schema;
        io.checkpoint_size =
            std::max<std::size_t>(io.checkpoint_size, s.member_size());
        auto feed = trace::index::WriteFeed::open(std::move(io));
        if (!feed) return feed.error();
        s.feed.emplace(std::move(*feed));
        s.fed_file_limit = std::clamp<std::uint64_t>(
            s.shares.index / OUTPUT_FED_FILE_BYTES, OUTPUT_FED_FILES_MIN,
            OUTPUT_FED_FILES_MAX);
    }
    if (o.max_open_files == 0)
        return common::make_error(ErrorCode::InvalidArgument,
                                  "max_open_files must be at least 1");
    for (std::size_t i = 0; i < o.partition_by.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (o.partition_by[i] == o.partition_by[j])
                return common::make_error<"partition_by names {} twice">(
                    ErrorCode::InvalidArgument,
                    dstd::StringView(o.partition_by[i]));
    const dstd::PathView target_path(s.target);
    s.leaf = dstd::String(target_path.filename());
    dstd::StringView parent_view = target_path.parent().str();
    s.parent =
        parent_view.empty() ? dstd::String(".") : dstd::String(parent_view);
    if (s.leaf.empty())
        return common::make_error<"{} names no file or folder">(
            ErrorCode::InvalidArgument, dstd::StringView(s.target));
    if (s.records() && !o.fields_from_first_batch) {
        if (auto prepared = s.prepare_header_from(nullptr); !prepared)
            return prepared.error();
    }
    auto made = dstd::fs::create_directories(dstd::PathView(s.parent));
    if (!made.has_value())
        return io_error("cannot create", s.parent, made.error());
    auto present = exists_no_follow(s.target);
    if (!present.has_value()) return present.error();
    if (*present && o.mode == OutputMode::Error)
        return common::make_error<"{} already exists">(
            ErrorCode::InvalidArgument, dstd::StringView(s.target));
    if (auto swept = s.sweep_leftovers(); !swept) return swept.error();
    s.id = dstd::fmt<"{}-{}">(static_cast<std::uint64_t>(::getpid()),
                              g_staging_counter.fetch_add(1));
    s.staging =
        dstd::fmt<"{}/.{}.tmp-{}">(s.parent, s.leaf, dstd::StringView(s.id));
    if (!s.is_file) {
        auto dir = dstd::fs::create_directory(dstd::PathView(s.staging));
        if (!dir.has_value())
            return io_error("cannot create", s.staging, dir.error());
    }
    return OutputWriter(std::move(impl));
}

OutputWriter::OutputWriter(dstd::UniquePtr<Impl> impl)
    : impl_(std::move(impl)) {}
OutputWriter::OutputWriter(OutputWriter&&) noexcept = default;
OutputWriter& OutputWriter::operator=(OutputWriter&&) noexcept = default;
OutputWriter::~OutputWriter() = default;

// Rows [begin, end) of one part, formatted a wave at a time: each slice of a
// wave on its own runtime thread into its own text, then appended in order, so
// the output is the text the serial loop writes.
coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::write_waves(
    const dataframe::DataFrame& batch, std::size_t begin, std::size_t end) {
    auto flushing = flush_lines();
    auto flushed = co_await std::move(flushing);
    if (!flushed) co_return flushed;
    Runtime& runtime = default_runtime();
    const std::size_t width = std::max<std::size_t>(1, runtime.threads());
    const std::size_t most_slices = width * SLICES_PER_THREAD;
    if (wave_texts.size() < most_slices) {
        wave_texts.resize(most_slices);
        wave_args.resize(most_slices);
    }
    dstd::Vector<dstd::Optional<dstd::Error>> errors(most_slices);
    std::size_t at = begin;
    while (at < end) {
        // The first wave of the writer is one slice, so the size of a row is
        // known before more text is held at once; each wave measures it again
        // for the next.
        const std::size_t wave_rows =
            row_bytes == 0
                ? FORMAT_SLICE_ROWS
                : std::max<std::size_t>(1, WAVE_TEXT_BYTES / row_bytes);
        const std::size_t wave_end = std::min<std::size_t>(end, at + wave_rows);
        const std::size_t span = wave_end - at;
        const std::size_t slices =
            row_bytes == 0 ? 1
                           : std::clamp<std::size_t>(span / MIN_SLICE_ROWS, 1,
                                                     most_slices);
        const std::size_t slice_rows = (span + slices - 1) / slices;
        const std::uint64_t part_base = part_rows;
        const std::uint64_t row_base = rows;
        const std::size_t wave_begin = at;
        for (std::size_t i = 0; i < slices; ++i) errors[i].reset();
        auto ran = runtime.parallel_for(
            static_cast<std::int64_t>(slices), 1,
            [&](std::int64_t first, std::int64_t last) {
                for (std::int64_t k = first; k < last; ++k) {
                    const auto i = static_cast<std::size_t>(k);
                    const std::size_t from = wave_begin + i * slice_rows;
                    const std::size_t to =
                        std::min<std::size_t>(wave_end, from + slice_rows);
                    wave_texts[i].clear();
                    if (from >= to) continue;
                    auto formatted = format_rows(
                        batch, from, to, wave_texts[i],
                        part_base + (from - wave_begin),
                        row_base + (from - wave_begin), wave_args[i]);
                    if (!formatted) errors[i] = formatted.error();
                }
            });
        if (!ran) co_return ran.error();
        for (std::size_t i = 0; i < slices; ++i)
            if (errors[i]) co_return *errors[i];
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < slices; ++i) {
            bytes += wave_texts[i].size();
            auto appending = append_text(wave_texts[i]);
            auto appended = co_await std::move(appending);
            if (!appended) co_return appended;
            // A wave whose rows grew past the estimate keeps no text that
            // large for the next.
            if (wave_texts[i].capacity() > WAVE_TEXT_BYTES)
                wave_texts[i] = dstd::String();
        }
        row_bytes = std::max<std::size_t>(1, bytes / span);
        part_rows += span;
        rows += span;
        at = wave_end;
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::write_batch(
    const dataframe::DataFrame& batch) {
    Impl& s = *this;
    if (auto prepared = s.prepare_batch(batch); !prepared)
        co_return prepared.error();
    if (s.partitioned()) {
        auto writing = s.write_partitioned(batch);
        co_return co_await std::move(writing);
    }
    const auto total = static_cast<std::size_t>(batch.num_rows());
    std::size_t at = 0;
    while (at < total) {
        auto rolling = s.roll_part();
        auto rolled = co_await std::move(rolling);
        if (!rolled) co_return rolled;
        std::size_t end = total;
        if (s.opts.part_size != 0)
            end = std::min<std::size_t>(
                end,
                at + static_cast<std::size_t>(s.opts.part_size - s.part_rows));
        // With no byte cap on a part, format waves of slices in parallel and
        // append them in order; the text in flight stays one wave.
        if (s.opts.part_bytes == 0 && end - at > FORMAT_SLICE_ROWS) {
            auto waving = s.write_waves(batch, at, end);
            auto waved = co_await std::move(waving);
            if (!waved) co_return waved;
            at = end;
            continue;
        }
        // Format in slices so the scratch text stays small.
        std::size_t slice = at;
        while (slice < end) {
            const std::size_t stop =
                std::min<std::size_t>(end, slice + FORMAT_SLICE_ROWS);
            auto formatted = s.format_rows(batch, slice, stop, s.lines,
                                           s.part_rows, s.rows, s.scratch_args);
            if (!formatted) co_return formatted.error();
            s.part_rows += stop - slice;
            s.rows += stop - slice;
            slice = stop;
            if (s.opts.part_bytes != 0 &&
                s.part_text + s.lines.size() >= s.opts.part_bytes) {
                end = slice;
                break;
            }
            if (s.lines.size() >= LINES_FLUSH_BYTES) {
                auto flushing = s.flush_lines();
                auto flushed = co_await std::move(flushing);
                if (!flushed) co_return flushed.error();
            }
        }
        auto flushing = s.flush_lines();
        auto flushed = co_await std::move(flushing);
        if (!flushed) co_return flushed.error();
        at = end;
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::write_text(
    dstd::StringView text) {
    Impl& s = *this;
    if (s.records())
        co_return common::make_error<
            "{} holds table records; write_lines takes event lines">(
            ErrorCode::InvalidArgument, dstd::StringView(s.target));
    if (s.partitioned())
        co_return common::make_error(
            ErrorCode::InvalidArgument,
            "write_lines cannot write a partitioned output");
    if (text.empty()) co_return dstd::Result<void>{};
    if (text.back() != '\n')
        co_return common::make_error(
            ErrorCode::InvalidArgument,
            "write_lines takes whole lines, each ending in a newline");
    const bool limited = s.opts.part_size != 0 || s.opts.part_bytes != 0;
    while (!text.empty()) {
        auto rolling = s.roll_part();
        auto rolled = co_await std::move(rolling);
        if (!rolled) co_return rolled;
        std::size_t take = text.size();
        std::uint64_t n = 0;
        if (!limited) {
            n = static_cast<std::uint64_t>(
                std::count(text.begin(), text.end(), '\n'));
        } else {
            take = 0;
            while (take < text.size()) {
                take = text.find('\n', take) + 1;
                ++n;
                if (s.opts.part_size != 0 &&
                    s.part_rows + n >= s.opts.part_size)
                    break;
                if (s.opts.part_bytes != 0 &&
                    s.part_text + take >= s.opts.part_bytes)
                    break;
            }
        }
        auto appending = s.append_text(text.substr(0, take));
        auto appended = co_await std::move(appending);
        if (!appended) co_return appended;
        s.part_rows += n;
        s.rows += n;
        text = text.substr(take);
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::drain_feeds() {
    auto reaping = reap_all();
    auto reaped = co_await std::move(reaping);
    if (!reaped) co_return reaped;
    if (part_feed != nullptr) {
        auto draining = feed->drain(*part_feed);
        auto drained = co_await std::move(draining);
        if (!drained) co_return drained;
    }
    for (const auto& p : partitions) {
        if (p->fed == nullptr) continue;
        auto draining = feed->drain(*p->fed);
        auto drained = co_await std::move(draining);
        if (!drained) co_return drained;
    }
    co_return dstd::Result<void>{};
}

void OutputWriter::bind_scope(coro::Scope& scope) {
    impl_->bound_scope = &scope;
}

coro::CoroTask<dstd::Result<void>> OutputWriter::write(
    const dataframe::DataFrame& batch) {
    auto writing = impl_->write_scoped(&batch, {});
    co_return co_await std::move(writing);
}

coro::CoroTask<dstd::Result<void>> OutputWriter::write_lines(
    dstd::StringView text) {
    auto writing = impl_->write_scoped(nullptr, text);
    co_return co_await std::move(writing);
}

// Without a bound scope, the index parse tasks a write starts run in a scope of
// its own, drained and joined before it returns.
coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::write_scoped(
    const dataframe::DataFrame* batch, dstd::StringView text) {
    Impl& s = *this;
    if (s.bound_scope != nullptr) {
        s.scope = s.bound_scope;
        if (batch != nullptr) {
            auto bound_writing = s.write_batch(*batch);
            co_return co_await std::move(bound_writing);
        }
        auto bound_writing = s.write_text(text);
        co_return co_await std::move(bound_writing);
    }
    coro::Scope own_scope;
    s.scope = &own_scope;
    dstd::Result<void> written;
    if (batch != nullptr) {
        auto writing = s.write_batch(*batch);
        written = co_await std::move(writing);
    } else {
        auto writing = s.write_text(text);
        written = co_await std::move(writing);
    }
    if (written) {
        auto draining = s.drain_feeds();
        written = co_await std::move(draining);
    }
    if (s.feed) {
        auto abandoning = s.feed->abandon();
        co_await std::move(abandoning);
    }
    auto joining = own_scope.join();
    co_await std::move(joining);
    s.scope = nullptr;
    co_return written;
}

coro::CoroTask<dstd::Result<OutputSummary>> OutputWriter::Impl::finish_all() {
    Impl& s = *this;
    if (s.partitioned()) {
        auto finishing = s.finish_partitions();
        auto finished = co_await std::move(finishing);
        if (!finished) co_return finished.error();
    }
    if (s.gz) {
        auto finishing = s.finish_part();
        auto finished = co_await std::move(finishing);
        if (!finished) co_return finished.error();
    }
    if (s.records() && !s.header_ready) {
        if (auto prepared = s.prepare_header_from(nullptr); !prepared)
            co_return prepared.error();
    }
    if (s.done.empty()) {
        const dstd::String path = s.part_staged_path(0);
        if (auto wrote = s.write_empty_part(path); !wrote)
            co_return wrote.error();
        Impl::PartDone p;
        p.staged = path;
        p.name = s.is_file ? s.leaf : part_file_name(0, s.gzip());
        s.done.push_back(std::move(p));
    }
    if (s.opts.known_rows >= 0 &&
        static_cast<std::uint64_t>(s.opts.known_rows) != s.rows)
        co_return common::make_error<
            "the header says {} rows, {} were written">(
            ErrorCode::InvalidArgument, s.opts.known_rows, s.rows);
    if (!s.is_file) {
        if (auto synced = sync_directory(s.staging); !synced)
            co_return synced.error();
    }
    if (auto renamed = s.commit(); !renamed) co_return renamed.error();
    bool index_failed = false;
    if (s.feed) {
        auto indexing = s.feed->commit();
        auto indexed = co_await std::move(indexing);
        if (!indexed) {
            index_failed = true;
            DFTRACER_UTILS_LOG_WARN(
                "the index of {} was not written: {}", s.target,
                dstd::StringView(indexed.error().message()));
        }
    }
    OutputSummary out;
    out.root = s.target;
    out.rows = s.rows;
    out.peak_buffer_bytes = s.peak_buffered;
    for (const Impl::PartDone& p : s.done)
        out.files.push_back(s.is_file ? s.target
                                      : dstd::fmt<"{}/{}">(s.target, p.name));
    if (s.opts.build_index)
        out.unindexed =
            index_failed || !s.feed ? out.files : std::move(s.unfed);
    co_return out;
}

coro::CoroTask<void> OutputWriter::abort() {
    Impl& s = *impl_;
    while (!s.inflight.empty()) {
        Impl::Inflight member = std::move(s.inflight.front());
        s.inflight.pop_front();
        auto compressed = co_await member.future;
        (void)compressed;
    }
    if (s.gz) {
        // A writer destroyed unclosed is aborted by a detached task, which can
        // outlive this one.
        auto closing = s.gz->close();
        (void)co_await std::move(closing);
        s.gz.reset();
    }
    if (s.feed) {
        auto abandoning = s.feed->abandon();
        co_await std::move(abandoning);
    }
}

coro::CoroTask<dstd::Result<OutputSummary>> OutputWriter::close() {
    Impl& s = *impl_;
    if (s.bound_scope != nullptr) {
        s.scope = s.bound_scope;
        auto bound_finishing = s.finish_all();
        co_return co_await std::move(bound_finishing);
    }
    coro::Scope scope;
    s.scope = &scope;
    auto finishing = s.finish_all();
    auto finished = co_await std::move(finishing);
    if (s.feed) {
        auto abandoning = s.feed->abandon();
        co_await std::move(abandoning);
    }
    auto joining = scope.join();
    co_await std::move(joining);
    s.scope = nullptr;
    co_return finished;
}

dstd::Result<void> index_output(const OutputSummary& summary,
                                const OutputOptions& options) {
    if (summary.unindexed.empty()) return {};
    trace::index::IndexerOptions io;
    io.index_dir = options.index_dir;
    if (io.index_dir.empty() && !is_file_output(summary.root))
        io.index_dir = summary.root;
    io.memory_budget = options.memory_budget;
    if (options.rows == OutputRows::Events) io.schema = options.schema;
    dstd::Vector<dstd::String> paths = summary.unindexed;
    auto indexer = trace::index::Indexer::open(std::move(paths), std::move(io));
    dstd::Optional<dstd::Error> failure;
    if (!indexer) {
        failure = indexer.error();
    } else if (auto built = indexer->build(); !built) {
        failure = built.error();
    }
    if (failure)
        DFTRACER_UTILS_LOG_WARN("the index of {} was not written: {}",
                                summary.root,
                                dstd::StringView(failure->message()));
    return {};
}

}  // namespace dftracer::utils::trace::writer
