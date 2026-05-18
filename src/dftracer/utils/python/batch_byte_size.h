#ifndef DFTRACER_UTILS_PYTHON_BATCH_BYTE_SIZE_H
#define DFTRACER_UTILS_PYTHON_BATCH_BYTE_SIZE_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/python/memoryview_batch.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

#include <cstddef>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

namespace dftracer::utils::python {

inline std::size_t byte_size(const MemoryViewBatchData &b) {
    return b.buffer.capacity() + b.offsets.capacity() * sizeof(Py_ssize_t) +
           b.lengths.capacity() * sizeof(Py_ssize_t);
}

inline std::size_t byte_size(const JsonDictBatch &b) {
    static constexpr std::size_t ESTIMATED_EVENT_BYTES = 512;
    return b.events.capacity() * ESTIMATED_EVENT_BYTES;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

inline std::size_t arrow_array_byte_size(const ArrowArray *arr) {
    if (!arr || !arr->release) return 0;
    std::size_t total = 0;
    for (int64_t i = 0; i < arr->n_buffers; ++i) {
        if (arr->buffers[i]) {
            // For variable-length buffers, use length * estimated element size
            // For validity/offset buffers, use (length + 7) / 8 or length * 4
            // Conservative estimate: buffer contributes proportionally to
            // length
            total += static_cast<std::size_t>(arr->length) * 8;
        }
    }
    for (int64_t i = 0; i < arr->n_children; ++i) {
        total += arrow_array_byte_size(arr->children[i]);
    }
    return total;
}

inline std::size_t byte_size(
    const dftracer::utils::utilities::common::arrow::ArrowExportResult &b) {
    return arrow_array_byte_size(b.get_array());
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_BATCH_BYTE_SIZE_H
