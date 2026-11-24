// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/writer.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/writer/output_writer.h>

#include <utility>

namespace dftracer::utils::trace::table {

namespace {

using common::ErrorCode;

coro::CoroTask<dstd::Result<writer::OutputSummary>> write_one(
    writer::OutputOptions options, const dataframe::DataFrame& frame) {
    auto opening = writer::OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    writer::OutputWriter out = std::move(*opening);
    auto writing = out.write(frame);
    auto written = co_await std::move(writing);
    if (!written) co_return written.error();
    auto closing = out.close();
    co_return co_await std::move(closing);
}

}  // namespace

dstd::Result<void> write_table(const dataframe::DataFrame& frame,
                               dstd::StringView path,
                               dstd::StringView header_args_json,
                               const WriteOptions& options) {
    json::Document doc;
    auto root = doc.parse(header_args_json);
    if (!root) return root.error();
    if (header_args_json.find('\n') != dstd::StringView::npos)
        return common::make_error(ErrorCode::InvalidArgument,
                                  "the header text holds a newline");
    // The writer owns the row count: take it out of the caller's header.
    std::int64_t nrows = -1;
    dstd::String args;
    {
        json::Writer w(args);
        w.begin_object();
        root->for_each_member([&](dstd::StringView key, json::Value v) {
            if (key == "nrows") {
                if (auto n = v.as_int()) nrows = *n;
                return;
            }
            w.key(key);
            w.raw_value(v.raw());
        });
        w.end_object();
    }
    if (nrows != frame.num_rows())
        return common::make_error<"the header says {} rows, the frame has {}">(
            ErrorCode::InvalidArgument, nrows, frame.num_rows());

    writer::OutputOptions out;
    out.path = dstd::String(path);
    out.rows = writer::OutputRows::Records;
    out.mode = writer::OutputMode::Overwrite;
    out.compress = options.compress;
    out.level = options.level;
    out.member_size = options.member_size;
    out.workers = options.workers;
    out.build_index = false;
    out.header_args_json = std::move(args);
    for (const dstd::StringView raw : options.raw_columns)
        out.raw_columns.emplace_back(raw);
    out.known_rows = nrows;
    auto ran = default_runtime().block_on(
        coro::named("write_table", write_one(std::move(out), frame)));
    if (!ran) return ran.error();
    return {};
}

}  // namespace dftracer::utils::trace::table
