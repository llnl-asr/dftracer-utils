// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/vector.h>

// nanoarrow must precede arrow.h: c_api.h (pulled in by arrow.h) defines
// ARROW_FLAG_DICTIONARY_ORDERED, nanoarrow's outer include guard, which would
// otherwise suppress its ArrowArrayStream definition.
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::arrow {

using dataframe::DataFrame;
using dataframe::Series;

OwnedArrow to_arrow(const Series& col) {
    OwnedArrow out;
    to_arrow(col, out.schema(), out.array());
    return out;
}

namespace {

Series as_struct(const DataFrame& frame, bool materialize) {
    dstd::Vector<dstd::String> field_names = dstd::to_strings(frame.names);
    dstd::Vector<Series> field_columns;
    field_columns.reserve(frame.columns.size());
    for (const Series& col : frame.columns)
        field_columns.push_back(
            materialize && !col.is_flat() ? col.materialize() : col.share());
    return Series::structs(std::move(field_names), std::move(field_columns));
}

}  // namespace

OwnedArrow to_arrow(const DataFrame& frame) {
    Series st = as_struct(frame, false);
    OwnedArrow out;
    to_arrow(st, out.schema(), out.array());
    return out;
}

dstd::Optional<DataFrame> from_ipc(dstd::StringView bytes) {
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    if (ArrowBufferAppend(&buf, bytes.data(),
                          static_cast<std::int64_t>(bytes.size())) !=
        NANOARROW_OK) {
        ArrowBufferReset(&buf);
        return dstd::nullopt;
    }
    ArrowIpcInputStream in;
    if (ArrowIpcInputStreamInitBuffer(&in, &buf) != NANOARROW_OK) {
        ArrowBufferReset(&buf);
        return dstd::nullopt;
    }
    ArrowArrayStream stream;
    if (ArrowIpcArrayStreamReaderInit(&stream, &in, nullptr) != NANOARROW_OK) {
        in.release(&in);
        return dstd::nullopt;
    }
    dstd::Optional<DataFrame> out;
    ArrowSchema schema;
    if (stream.get_schema(&stream, &schema) == 0) {
        ArrowArray array;
        if (stream.get_next(&stream, &array) == 0 && array.release)
            out = dataframe_from_arrow(&schema, &array);
        schema.release(&schema);
    }
    stream.release(&stream);
    return out;
}

dstd::Result<dstd::Vector<std::uint8_t>> to_ipc(const DataFrame& frame) {
    // Export the frame as one struct (record batch) - schema + array.
    Series st = as_struct(frame, true);
    ArrowSchema schema;
    ArrowArray array;
    to_arrow(st, &schema, &array);

    // A one-batch array stream (takes ownership of schema + array on success).
    ArrowArrayStream stream;
    if (ArrowBasicArrayStreamInit(&stream, &schema, 1) != NANOARROW_OK) {
        if (schema.release) schema.release(&schema);
        if (array.release) array.release(&array);
        return common::make_error<"to_ipc: array stream init failed">(
            common::ErrorCode::Internal);
    }
    ArrowBasicArrayStreamSetArray(&stream, 0, &array);

    // Encode schema message + record batch + EOS into a caller-owned buffer.
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    ArrowIpcOutputStream out;
    if (ArrowIpcOutputStreamInitBuffer(&out, &buf) != NANOARROW_OK) {
        ArrowBufferReset(&buf);
        stream.release(&stream);
        return common::make_error<"to_ipc: output stream init failed">(
            common::ErrorCode::Internal);
    }
    ArrowIpcWriter writer;
    if (ArrowIpcWriterInit(&writer, &out) != NANOARROW_OK) {  // adopts `out`
        out.release(&out);
        ArrowBufferReset(&buf);
        stream.release(&stream);
        return common::make_error<"to_ipc: writer init failed">(
            common::ErrorCode::Internal);
    }
    ArrowError err;
    err.message[0] = '\0';
    const int code = ArrowIpcWriterWriteArrayStream(&writer, &stream, &err);
    dstd::Vector<std::uint8_t> bytes;
    if (code == NANOARROW_OK) bytes.assign(buf.data, buf.data + buf.size_bytes);
    ArrowIpcWriterReset(&writer);  // releases `out`
    ArrowBufferReset(&buf);
    stream.release(&stream);       // releases schema + array
    if (code != NANOARROW_OK)
        return common::make_error<"to_ipc: {}">(common::ErrorCode::Internal,
                                                err.message);
    return bytes;
}

}  // namespace dftracer::utils::arrow
