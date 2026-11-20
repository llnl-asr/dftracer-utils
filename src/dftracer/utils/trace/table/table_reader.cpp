// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/table/table_decode.h>
#include <dftracer/utils/trace/table/table_header.h>

#include <cstring>
#include <utility>

namespace dftracer::utils::trace::table {

dstd::Result<Table> read_table(dstd::StringView path,
                               const ReadOptions& options) {
    using common::ErrorCode;
    Table table;
    auto raw = detail::read_whole(path);
    if (!raw) return raw.error();
    detail::Buffer text;
    if (detail::has_gzip_magic(raw->data.get(), raw->size)) {
        detail::InflateStats stats;
        auto inflated = detail::inflate_gzip(*raw, stats);
        if (!inflated) return inflated.error();
        text = std::move(*inflated);
        table.members_in_parallel = stats.members_in_parallel;
        table.inflated_in_order = stats.inflated_in_order;
    } else {
        text = std::move(*raw);
    }
    raw = detail::Buffer{};

    const char* base = text.text();
    const char* end = base + text.size;
    const void* first_nl = std::memchr(base, '\n', text.size);
    const char* header_end =
        first_nl ? static_cast<const char*>(first_nl) : end;

    detail::Header header;
    {
        json::Document doc;
        auto root = doc.parse_in_place(
            dstd::StringView(base, static_cast<std::size_t>(header_end - base)),
            text.size + detail::PADDING);
        if (!root)
            return common::make_error(ErrorCode::Parse,
                                      "the first line is not JSON");
        auto args = root->find("args");
        if (!args)
            return common::make_error(ErrorCode::Parse,
                                      "the first line is not a table header");
        table.header_json = dstd::String(args->raw());
        auto parsed = detail::parse_header(*args);
        if (!parsed) return parsed.error();
        header = std::move(*parsed);
    }
    const std::size_t nfields = header.fields.size();
    const auto nrows = static_cast<std::size_t>(header.nrows);
    const char* body = first_nl ? header_end + 1 : end;

    dstd::Vector<std::size_t> wanted(nfields);
    for (std::size_t i = 0; i < nfields; ++i) wanted[i] = i;
    auto decoded = detail::decode_lines(
        header, wanted,
        dstd::StringView(body, static_cast<std::size_t>(end - body)),
        options.parts);
    if (!decoded) return decoded.error();
    if (decoded->rows != nrows)
        return common::make_error<"the header says {} rows, the file has {}">(
            ErrorCode::Parse, nrows, decoded->rows);

    for (std::size_t i = 0; i < nfields; ++i) {
        table.frame.names.push_back(dstd::SharedString(header.fields[i].name));
        table.frame.columns.push_back(std::move(decoded->columns[i]));
    }
    return table;
}

}  // namespace dftracer::utils::trace::table
