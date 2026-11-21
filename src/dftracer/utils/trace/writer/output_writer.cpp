// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/process.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <dftracer/utils/trace/writer/output_writer.h>
#include <dftracer/utils/trace/writer/trace_writer.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
            return is_string_type(t) && raw;
    }
    return false;
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

dstd::Result<OutColumn> prepare_column(const dataframe::DataFrame& frame,
                                       std::size_t i, bool raw) {
    const dstd::StringView name(frame.names[i]);
    OutColumn c;
    c.type = frame.columns[i].type();
    c.series = frame.columns[i].is_flat() || is_string_type(c.type)
                   ? frame.columns[i].share()
                   : frame.columns[i].materialize();
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

}  // namespace

bool is_file_output(dstd::StringView path) {
    return path.ends_with(GZ_EXTENSION) || path.ends_with(PLAIN_EXTENSION);
}

struct OutputWriter::Impl {
    struct PartDone {
        dstd::String staged;
        dstd::String name;
        std::uint64_t rows = 0;
    };

    OutputOptions opts;
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
    std::uint64_t rows = 0;
    dstd::Vector<PartDone> done;
    dstd::String lines;

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
    dstd::Result<void> format_records(const dataframe::DataFrame& batch,
                                      std::size_t begin, std::size_t end);
    dstd::Result<void> format_events(const dataframe::DataFrame& batch,
                                     std::size_t begin, std::size_t end);
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
            bool raw = false;
            for (const dstd::String& r : opts.raw_columns)
                if (dstd::StringView(r) == name) raw = true;
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
    const dataframe::DataFrame& batch, std::size_t begin, std::size_t end) {
    const std::size_t ncols = columns.size();
    dstd::Vector<Arg> args(ncols);
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
                        ErrorCode::InvalidArgument, rows + (row - begin),
                        dstd::StringView(batch.names[i]));
            }
        }
        format_aggregated(lines, table, "dftu", part_rows + (row - begin), 0, 0,
                          args);
    }
    return {};
}

dstd::Result<void> OutputWriter::Impl::format_events(
    const dataframe::DataFrame& batch, std::size_t begin, std::size_t end) {
    const EventColumns& ec = event_columns;
    dstd::Vector<Arg> args;
    args.reserve(ec.args.size());
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
                format_event(lines, *name, *cat, *ts, dur, *pid, *tid, args);
                break;
            case 2:
                format_counter(lines, *name, *cat, *ts, *pid, *tid, args);
                break;
            case 3:
                format_aggregated(lines, *name, *cat, *ts, *pid, *tid, args);
                break;
            case 4:
                format_metadata(lines, *name, *pid, *tid, args);
                break;
            default:
                return common::make_error<
                    "row {}: the phase {} is not one of 1, 2, 3 and 4">(
                    ErrorCode::InvalidArgument, row, *ph);
        }
    }
    return {};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::start_part() {
    part_path = part_staged_path(done.size());
    fileio::GzipWriterOptions g;
    g.member_size = member_size();
    g.level = opts.level;
    g.workers = opts.workers;
    g.compress = opts.compress;
    g.memory_budget = opts.memory_budget;
    if (records()) {
        const std::int64_t known = known_part_rows(done.size());
        g.part_header = header_with(known);
        g.part_header_stored = opts.compress;
    }
    auto opening = fileio::GzipLineWriter::open(part_path, std::move(g));
    auto opened = co_await std::move(opening);
    if (!opened) co_return opened.error();
    gz.emplace(std::move(*opened));
    part_rows = 0;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> OutputWriter::Impl::flush_lines() {
    if (lines.empty()) co_return dstd::Result<void>{};
    auto appending = gz->append(lines);
    auto appended = co_await std::move(appending);
    lines.clear();
    co_return appended;
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

dstd::Result<OutputWriter> OutputWriter::open(OutputOptions options) {
    auto impl = dstd::make_unique<Impl>();
    Impl& s = *impl;
    s.opts = std::move(options);
    OutputOptions& o = s.opts;
    if (o.path.empty())
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the output path is empty");
    s.is_file = is_file_output(without_trailing_slashes(o.path));
    s.target = dstd::String(without_trailing_slashes(o.path));
    if (s.is_file) {
        if (o.compress != s.target.ends_with(GZ_EXTENSION))
            return common::make_error<
                "compress does not agree with the extension of {}">(
                ErrorCode::InvalidArgument, dstd::StringView(s.target));
        if (o.part_size != 0)
            return common::make_error<
                "part_size needs a folder, but {} names one file">(
                ErrorCode::InvalidArgument, dstd::StringView(s.target));
    } else if (!o.compress) {
        return common::make_error<
            "{} is a folder of parts, which is gzip; write a single .pfw file "
            "for plain output">(ErrorCode::InvalidArgument,
                                dstd::StringView(s.target));
    }
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

coro::CoroTask<dstd::Result<void>> OutputWriter::write(
    const dataframe::DataFrame& batch) {
    Impl& s = *impl_;
    if (auto prepared = s.prepare_batch(batch); !prepared)
        co_return prepared.error();
    const auto total = static_cast<std::size_t>(batch.num_rows());
    std::size_t at = 0;
    while (at < total) {
        if (s.gz && s.opts.part_size != 0 && s.part_rows >= s.opts.part_size) {
            auto finishing = s.finish_part();
            auto finished = co_await std::move(finishing);
            if (!finished) co_return finished.error();
        }
        if (!s.gz) {
            auto starting = s.start_part();
            auto started = co_await std::move(starting);
            if (!started) co_return started.error();
        }
        std::size_t end = total;
        if (s.opts.part_size != 0)
            end = std::min<std::size_t>(
                end,
                at + static_cast<std::size_t>(s.opts.part_size - s.part_rows));
        // Format in slices so the scratch text stays small.
        std::size_t slice = at;
        while (slice < end) {
            const std::size_t stop = std::min<std::size_t>(end, slice + 4096);
            auto formatted = s.records() ? s.format_records(batch, slice, stop)
                                         : s.format_events(batch, slice, stop);
            if (!formatted) co_return formatted.error();
            s.part_rows += stop - slice;
            s.rows += stop - slice;
            slice = stop;
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

coro::CoroTask<dstd::Result<OutputSummary>> OutputWriter::close() {
    Impl& s = *impl_;
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
    if (auto committed = s.commit(); !committed) co_return committed.error();
    OutputSummary out;
    out.root = s.target;
    out.rows = s.rows;
    for (const Impl::PartDone& p : s.done)
        out.files.push_back(s.is_file ? s.target
                                      : dstd::fmt<"{}/{}">(s.target, p.name));
    co_return out;
}

dstd::Result<void> index_output(const OutputSummary& summary,
                                const OutputOptions& options) {
    if (summary.files.empty()) return {};
    trace::index::IndexerOptions io;
    io.index_dir = options.index_dir;
    io.memory_budget = options.memory_budget;
    dstd::Vector<dstd::String> paths = summary.files;
    auto indexer = trace::index::Indexer::open(std::move(paths), std::move(io));
    if (!indexer) return indexer.error();
    auto built = indexer->build();
    if (!built) return built.error();
    return {};
}

}  // namespace dftracer::utils::trace::writer
