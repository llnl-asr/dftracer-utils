// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <dftracer/utils/trace/writer/trace_writer.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::table {

namespace {

using common::ErrorCode;
using dataframe::Series;
using dataframe::TypeId;

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

// Whether a column of type `t` can carry the field's dtype.
bool kind_accepts(const detail::Field& f, TypeId t, bool raw) {
    using detail::Kind;
    switch (f.kind) {
        case Kind::Int:
            return is_signed_type(t) && width_of(t) <= f.bits;
        case Kind::UInt:
            return is_unsigned_type(t) && width_of(t) <= f.bits;
        case Kind::Float:
            return t == TypeId::Float32 || t == TypeId::Float64;
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
writer::ArgValue number_at(const void* data, std::size_t row) {
    return writer::ArgValue(static_cast<const T*>(data)[row]);
}

writer::ArgValue value_at(const OutColumn& c, std::size_t row) {
    if (c.has_nulls && c.series.is_null(static_cast<std::int64_t>(row)))
        return writer::ArgValue();
    switch (c.type) {
        case TypeId::Bool:
            return writer::ArgValue(
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
            return writer::ArgValue(
                static_cast<double>(static_cast<const float*>(c.data)[row]));
        case TypeId::Float64:
            return number_at<double>(c.data, row);
        default: {
            const dstd::StringView text =
                c.series.string_at(static_cast<std::int64_t>(row));
            return c.raw ? writer::ArgValue::raw(text) : writer::ArgValue(text);
        }
    }
}

}  // namespace

dstd::Result<void> write_table(const dataframe::DataFrame& frame,
                               dstd::StringView path,
                               dstd::StringView header_args_json,
                               const WriteOptions& options) {
    json::Document doc;
    auto root = doc.parse(header_args_json);
    if (!root) return root.error();
    const dstd::StringView args_text = root->raw();
    if (args_text.find('\n') != dstd::StringView::npos)
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header text holds a newline");
    auto header = detail::parse_header(*root);
    if (!header) return header.error();

    const std::size_t ncols = frame.num_columns();
    const auto nrows = static_cast<std::size_t>(frame.num_rows());
    if (header->fields.size() != ncols)
        return common::make_error<
            "the header has {} fields, the frame {} "
            "columns">(ErrorCode::InvalidArgument, header->fields.size(),
                       ncols);
    if (header->nrows != frame.num_rows())
        return common::make_error<"the header says {} rows, the frame has {}">(
            ErrorCode::InvalidArgument, header->nrows, frame.num_rows());

    dstd::Vector<OutColumn> columns;
    columns.reserve(ncols);
    for (std::size_t i = 0; i < ncols; ++i) {
        const dstd::StringView name(frame.names[i]);
        if (name != dstd::StringView(header->fields[i].name))
            return common::make_error<
                "column {} is named {} in the header, "
                "{} in the frame">(ErrorCode::InvalidArgument, i,
                                   dstd::StringView(header->fields[i].name),
                                   name);
        OutColumn c;
        c.type = frame.columns[i].type();
        c.series = frame.columns[i].is_flat() || is_string_type(c.type)
                       ? frame.columns[i].share()
                       : frame.columns[i].materialize();
        for (const dstd::StringView raw : options.raw_columns)
            if (raw == name) c.raw = true;
        if (c.raw && !is_string_type(c.type))
            return common::make_error<"raw column {} is not a string column">(
                ErrorCode::InvalidArgument, name);
        if (!is_fixed_type(c.type) && !is_string_type(c.type))
            return common::make_error<
                "column {} has a type the table format "
                "does not carry">(ErrorCode::InvalidArgument, name);
        if (!kind_accepts(header->fields[i], c.type, c.raw))
            return common::make_error<
                "column {} does not fit the dtype of its "
                "header field">(ErrorCode::InvalidArgument, name);
        c.has_nulls = c.series.null_count() > 0;
        if (c.has_nulls && !header->fields[i].nulls_ok)
            return common::make_error<
                "column {} has nulls, its dtype has "
                "none">(ErrorCode::InvalidArgument, name);
        if (is_fixed_type(c.type)) {
            c.data = c.series.is_flat() ? c.series.template data<std::uint8_t>()
                                        : nullptr;
            if (c.data == nullptr && nrows > 0)
                return common::make_error<"column {} has no value buffer">(
                    ErrorCode::Internal, name);
        }
        columns.push_back(std::move(c));
    }
    for (const dstd::StringView raw : options.raw_columns) {
        bool found = false;
        for (std::size_t i = 0; i < ncols; ++i)
            if (dstd::StringView(frame.names[i]) == raw) found = true;
        if (!found)
            return common::make_error<"raw column {} is not in the frame">(
                ErrorCode::InvalidArgument, raw);
    }

    writer::TraceWriterOptions wopts;
    wopts.level = options.level;
    wopts.workers = options.workers;
    wopts.member_size = options.member_size;
    wopts.compress = options.compress;
    auto opened = writer::TraceWriter::open(path, wopts);
    if (!opened) return opened.error();
    writer::TraceWriter out = std::move(*opened);

    dstd::String line =
        "{\"name\":\"dftu.table\",\"cat\":\"dftu\",\"pid\":0,\"tid\":0,"
        "\"ts\":0,\"ph\":4,\"args\":";
    line += args_text;
    line += "}\n";
    if (auto wrote = out.write_raw(line); !wrote) return wrote.error();

    dstd::Vector<writer::Arg> args(ncols);
    for (std::size_t i = 0; i < ncols; ++i)
        args[i].key = dstd::StringView(frame.names[i]);
    const dstd::StringView table(header->table);
    for (std::size_t row = 0; row < nrows; ++row) {
        for (std::size_t i = 0; i < ncols; ++i) {
            args[i].value = value_at(columns[i], row);
            if (columns[i].raw &&
                args[i].value.kind == writer::ArgValue::Kind::Raw) {
                const dstd::StringView text = args[i].value.s;
                if (text.empty() || text.find('\n') != dstd::StringView::npos)
                    return common::make_error<
                        "row {}, column {}: a raw cell "
                        "is empty or holds a newline">(
                        ErrorCode::InvalidArgument, row,
                        dstd::StringView(frame.names[i]));
            }
        }
        if (auto wrote = out.write_aggregated(table, "dftu", row, 0, 0, args);
            !wrote)
            return wrote.error();
    }
    return out.close();
}

}  // namespace dftracer::utils::trace::table
