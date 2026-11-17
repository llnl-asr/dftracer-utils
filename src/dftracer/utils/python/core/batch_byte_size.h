// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_BATCH_BYTE_SIZE_H
#define DFTRACER_UTILS_PYTHON_CORE_BATCH_BYTE_SIZE_H

#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/python/core/memoryview_batch.h>
#include <dftracer/utils/python/trace/trace_reader_iterator.h>

#include <cstddef>

namespace dftracer::utils::python::core {

inline std::size_t byte_size(const MemoryViewBatchData &b) {
    return b.buffer.capacity() + b.offsets.capacity() * sizeof(Py_ssize_t) +
           b.lengths.capacity() * sizeof(Py_ssize_t);
}

inline std::size_t arrow_array_byte_size(const ArrowArray *arr) {
    if (!arr || !arr->release) return 0;
    std::size_t total = 0;
    for (std::int64_t i = 0; i < arr->n_buffers; ++i) {
        if (arr->buffers[i]) {
            // For variable-length buffers, use length * estimated element size
            // For validity/offset buffers, use (length + 7) / 8 or length * 4
            // Conservative estimate: buffer contributes proportionally to
            // length
            total += static_cast<std::size_t>(arr->length) * 8;
        }
    }
    for (std::int64_t i = 0; i < arr->n_children; ++i) {
        total += arrow_array_byte_size(arr->children[i]);
    }
    return total;
}

inline std::size_t byte_size(const dftracer::utils::arrow::Batch &b) {
    return arrow_array_byte_size(b.array());
}

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_BATCH_BYTE_SIZE_H
