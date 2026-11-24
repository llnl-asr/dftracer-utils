// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_WRITER_SINK_PLAN_H
#define DFTRACER_UTILS_TRACE_WRITER_SINK_PLAN_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/writer/output_writer.h>

namespace dftracer::utils::trace::writer {

/// `plan` wrapped so that collecting it writes every frame `plan` streams as a
/// DFTracer trace (`rows` Events) or table (Records) at `path`, and yields one
/// row of `rows` and `batches`. The writer is opened when the wrapped plan is
/// scanned. `options` are the text options of a duql `sink trace` or
/// `sink table`: mode, part_size, partition_by (comma separated column names),
/// max_open_files, compress, level, member_size, build_index, and for a table
/// name and meta (a JSON object). An unknown key or a value that does not
/// parse is an InvalidArgument error.
dstd::Result<dataframe::LazyFrame> output_sink_plan(
    const dataframe::LazyFrame& plan, OutputRows rows, dstd::StringView path,
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options);

}  // namespace dftracer::utils::trace::writer

#endif  // DFTRACER_UTILS_TRACE_WRITER_SINK_PLAN_H
