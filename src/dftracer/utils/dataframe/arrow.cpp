#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

// nanoarrow must precede arrow.h: arrow_abi.h (pulled in by arrow.h) defines
// ARROW_FLAG_DICTIONARY_ORDERED, nanoarrow's outer include guard, which would
// otherwise suppress its ArrowArrayStream definition.
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <stdexcept>
#endif

#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/arrow_bridge.h>
#include <dftracer/utils/dataframe/internal/ipc.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

OwnedArrow Series::to_arrow() const {
    OwnedArrow out;
    dftracer::utils::dataframe::to_arrow(*this, out.schema(), out.array());
    return out;
}

Series Series::from_arrow(const ArrowSchema* schema, const ArrowArray* array) {
    return dftracer::utils::dataframe::from_arrow(
        schema, const_cast<ArrowArray*>(array));
}

OwnedArrow DataFrame::to_arrow() const {
    std::vector<std::string> field_names = names;
    std::vector<Series> field_columns;
    field_columns.reserve(columns.size());
    for (const Series& col : columns) field_columns.push_back(col.share());
    Series st =
        Series::structs(std::move(field_names), std::move(field_columns));
    OwnedArrow out;
    dftracer::utils::dataframe::to_arrow(st, out.schema(), out.array());
    return out;
}

DataFrame DataFrame::from_arrow(const ArrowSchema* schema,
                                const ArrowArray* array) {
    return dftracer::utils::dataframe::dataframe_from_arrow(
        schema, const_cast<ArrowArray*>(array));
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
std::optional<DataFrame> frame_from_ipc(std::string_view bytes) {
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    if (ArrowBufferAppend(&buf, bytes.data(),
                          static_cast<std::int64_t>(bytes.size())) !=
        NANOARROW_OK) {
        ArrowBufferReset(&buf);
        return std::nullopt;
    }
    ArrowIpcInputStream in;
    if (ArrowIpcInputStreamInitBuffer(&in, &buf) != NANOARROW_OK) {
        ArrowBufferReset(&buf);
        return std::nullopt;
    }
    ArrowArrayStream stream;
    if (ArrowIpcArrayStreamReaderInit(&stream, &in, nullptr) != NANOARROW_OK) {
        in.release(&in);
        return std::nullopt;
    }
    std::optional<DataFrame> out;
    ArrowSchema schema;
    if (stream.get_schema(&stream, &schema) == 0) {
        ArrowArray array;
        if (stream.get_next(&stream, &array) == 0 && array.release)
            out = DataFrame::from_arrow(&schema, &array);
        schema.release(&schema);
    }
    stream.release(&stream);
    return out;
}
#endif

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
std::vector<std::uint8_t> DataFrame::to_ipc() const {
    // Export the frame as one struct (record batch) - schema + array.
    std::vector<std::string> field_names = names;
    std::vector<Series> field_columns;
    field_columns.reserve(columns.size());
    for (const Series& col : columns)
        field_columns.push_back(col.is_flat() ? col.share()
                                              : col.materialize());
    Series st =
        Series::structs(std::move(field_names), std::move(field_columns));
    ArrowSchema schema;
    ArrowArray array;
    dftracer::utils::dataframe::to_arrow(st, &schema, &array);

    // A one-batch array stream (takes ownership of schema + array on success).
    ArrowArrayStream stream;
    if (ArrowBasicArrayStreamInit(&stream, &schema, 1) != NANOARROW_OK) {
        if (schema.release) schema.release(&schema);
        if (array.release) array.release(&array);
        throw std::runtime_error("to_ipc: array stream init failed");
    }
    ArrowBasicArrayStreamSetArray(&stream, 0, &array);

    // Encode schema message + record batch + EOS into a caller-owned buffer.
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    ArrowIpcOutputStream out;
    if (ArrowIpcOutputStreamInitBuffer(&out, &buf) != NANOARROW_OK) {
        ArrowBufferReset(&buf);
        stream.release(&stream);
        throw std::runtime_error("to_ipc: output stream init failed");
    }
    ArrowIpcWriter writer;
    if (ArrowIpcWriterInit(&writer, &out) != NANOARROW_OK) {  // adopts `out`
        out.release(&out);
        ArrowBufferReset(&buf);
        stream.release(&stream);
        throw std::runtime_error("to_ipc: writer init failed");
    }
    ArrowError err;
    err.message[0] = '\0';
    const int code = ArrowIpcWriterWriteArrayStream(&writer, &stream, &err);
    std::vector<std::uint8_t> bytes;
    if (code == NANOARROW_OK) bytes.assign(buf.data, buf.data + buf.size_bytes);
    ArrowIpcWriterReset(&writer);  // releases `out`
    ArrowBufferReset(&buf);
    stream.release(&stream);       // releases schema + array
    if (code != NANOARROW_OK)
        throw std::runtime_error(std::string("to_ipc: ") + err.message);
    return bytes;
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_ENABLE_ARROW
