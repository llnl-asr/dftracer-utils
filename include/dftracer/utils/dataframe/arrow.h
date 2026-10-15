#ifndef DFTRACER_UTILS_DATAFRAME_ARROW_H
#define DFTRACER_UTILS_DATAFRAME_ARROW_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
// Supplies ArrowSchema/ArrowArray under the ARROW_C_DATA_INTERFACE guard, so a
// consumer needs no Arrow library; a real Arrow header, if included first,
// wins.
#include <dftracer/utils/plugins/arrow_abi.h>

namespace dftracer::utils::dataframe {

/**
 * Move-only RAII owner of a paired Arrow C Data Interface `ArrowSchema` and
 * `ArrowArray`. On destruction it calls each struct's `release` callback when
 * non-null, so a caller of `Series::to_arrow` / `DataFrame::to_arrow` never
 * leaks or double-frees the exported buffers. The accessors hand out pointers
 * suitable for passing to a consumer's Arrow import (e.g. pyarrow's
 * `_import_from_c`, or `from_arrow` for a round trip).
 */
class OwnedArrow {
   public:
    OwnedArrow() = default;
    OwnedArrow(OwnedArrow&& other) noexcept { steal(other); }
    OwnedArrow& operator=(OwnedArrow&& other) noexcept {
        if (this != &other) {
            reset();
            steal(other);
        }
        return *this;
    }
    OwnedArrow(const OwnedArrow&) = delete;
    OwnedArrow& operator=(const OwnedArrow&) = delete;
    ~OwnedArrow() { reset(); }

    /// The owned schema struct (never null; may be an empty, unreleased
    /// struct).
    ArrowSchema* schema() noexcept { return &schema_; }
    const ArrowSchema* schema() const noexcept { return &schema_; }
    /// The owned array struct (never null; may be an empty, unreleased struct).
    ArrowArray* array() noexcept { return &array_; }
    const ArrowArray* array() const noexcept { return &array_; }

    /// True once both the array and schema carry a live release callback.
    explicit operator bool() const noexcept {
        return array_.release != nullptr && schema_.release != nullptr;
    }

    /// Release both structs early (idempotent); leaves them empty.
    void reset() noexcept {
        if (array_.release != nullptr) array_.release(&array_);
        if (schema_.release != nullptr) schema_.release(&schema_);
        array_ = ArrowArray{};
        schema_ = ArrowSchema{};
    }

   private:
    void steal(OwnedArrow& other) noexcept {
        schema_ = other.schema_;
        array_ = other.array_;
        other.schema_ = ArrowSchema{};
        other.array_ = ArrowArray{};
    }

    ArrowSchema schema_{};
    ArrowArray array_{};
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_DATAFRAME_ARROW_H
