// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_ARROW_ARROW_EXPORT_H
#define DFTRACER_UTILS_TRACE_ARROW_ARROW_EXPORT_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <cstdint>
#include <nanoarrow/nanoarrow.hpp>

namespace dftracer::utils::trace::arrow {

/**
 * Move-only RAII container for a finished Arrow record batch.
 *
 * Holds ownership of both the @ref ArrowArray and @ref ArrowSchema produced by
 * RecordBatchBuilder::finish(). Safe to move across threads/channels.
 */
class ArrowExportResult : public dstd::HeapObject {
   public:
    ArrowExportResult() = default;

    ArrowExportResult(nanoarrow::UniqueSchema schema,
                      nanoarrow::UniqueArray array)
        : schema_(std::move(schema)), array_(std::move(array)) {}

    ArrowExportResult(ArrowExportResult&&) = default;
    ArrowExportResult& operator=(ArrowExportResult&&) = default;

    ArrowExportResult(const ArrowExportResult&) = delete;
    ArrowExportResult& operator=(const ArrowExportResult&) = delete;

    ArrowArray* array() noexcept { return array_.get(); }
    const ArrowArray* array() const noexcept { return array_.get(); }

    ArrowSchema* schema() noexcept { return schema_.get(); }
    const ArrowSchema* schema() const noexcept { return schema_.get(); }

    nanoarrow::UniqueArray release_array() { return std::move(array_); }
    nanoarrow::UniqueSchema release_schema() { return std::move(schema_); }

    int64_t num_rows() const noexcept { return array_.get()->length; }

    int64_t num_columns() const noexcept { return array_.get()->n_children; }

    bool valid() const noexcept { return array_.get()->release != nullptr; }

   private:
    nanoarrow::UniqueSchema schema_;
    nanoarrow::UniqueArray array_;
};

}  // namespace dftracer::utils::trace::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_TRACE_ARROW_ARROW_EXPORT_H
