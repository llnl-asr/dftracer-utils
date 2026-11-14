// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_BATCH_H
#define DFTRACER_UTILS_ARROW_BATCH_H

// Supplies ArrowSchema/ArrowArray under the ARROW_C_DATA_INTERFACE guard, so a
// consumer needs no Arrow library; a real Arrow header, if included first,
// wins.
#include <dftracer/utils/arrow/c_api.h>

#include <cstdint>

namespace dftracer::utils::arrow {

/**
 * @brief One record batch of the Arrow C Data Interface: an @ref ArrowSchema
 * and the @ref ArrowArray it describes, both owned.
 *
 * Move-only. The destructor calls the release callback of whichever struct is
 * still live, so the buffers a producer exported stay alive exactly as long as
 * this batch. Hand the pair to a consumer with @ref release_into, which moves
 * both structs out and leaves this batch empty.
 */
class Batch {
   public:
    Batch() = default;
    Batch(Batch&& other) noexcept { steal(other); }
    Batch& operator=(Batch&& other) noexcept {
        if (this != &other) {
            reset();
            steal(other);
        }
        return *this;
    }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    ~Batch() { reset(); }

    /**
     * @brief Takes over the contents of the structs @p schema and @p array
     * point to, which are left empty, so their previous owner must not release
     * them. Ownership of every buffer passes to the returned batch.
     *
     * A struct without a release callback makes an invalid batch.
     */
    static Batch adopt(ArrowSchema* schema, ArrowArray* array) noexcept {
        Batch batch;
        if (schema != nullptr) {
            batch.schema_ = *schema;
            *schema = ArrowSchema{};
        }
        if (array != nullptr) {
            batch.array_ = *array;
            *array = ArrowArray{};
        }
        return batch;
    }

    /// The owned schema (never null; possibly an empty struct).
    const ArrowSchema* schema() const noexcept { return &schema_; }
    /// The owned array (never null; possibly an empty struct).
    const ArrowArray* array() const noexcept { return &array_; }

    /// True while both structs carry a live release callback.
    bool is_valid() const noexcept {
        return schema_.release != nullptr && array_.release != nullptr;
    }

    std::int64_t num_rows() const noexcept { return array_.length; }
    std::int64_t num_columns() const noexcept { return array_.n_children; }

    /**
     * @brief Moves both structs into @p schema and @p array, which must be
     * writable and hold nothing this batch would leak. The caller then owns
     * them and must call their release callbacks.
     */
    void release_into(ArrowSchema* schema, ArrowArray* array) noexcept {
        *schema = schema_;
        *array = array_;
        schema_ = ArrowSchema{};
        array_ = ArrowArray{};
    }

    /// Releases both structs now (idempotent); leaves this batch empty.
    void reset() noexcept {
        if (array_.release != nullptr) array_.release(&array_);
        if (schema_.release != nullptr) schema_.release(&schema_);
        array_ = ArrowArray{};
        schema_ = ArrowSchema{};
    }

   private:
    void steal(Batch& other) noexcept {
        schema_ = other.schema_;
        array_ = other.array_;
        other.schema_ = ArrowSchema{};
        other.array_ = ArrowArray{};
    }

    ArrowSchema schema_{};
    ArrowArray array_{};
};

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_BATCH_H
