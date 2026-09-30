#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_ORDER_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_ORDER_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::dataframe {

/// A column resolved once for typed per-row reads, so a kernel's inner loop
/// does pointer arithmetic instead of a call per cell. Holds its own reference
/// to the (materialized) column. Numeric types read as Signed, Unsigned or
/// Float by their physical layout; String and Binary read as Bytes.
class ColumnView {
   public:
    enum class Kind : std::uint8_t { Signed, Unsigned, Float, Bytes };

    /// Whether a column of type `t` can be viewed.
    static bool supports(TypeId t) noexcept;

    /// Throws std::invalid_argument for an unsupported type.
    explicit ColumnView(const Series& column);

    /// A view of the same buffers.
    ColumnView(const ColumnView& other);
    ColumnView(ColumnView&&) noexcept = default;
    ColumnView& operator=(const ColumnView& other);
    ColumnView& operator=(ColumnView&&) noexcept = default;

    Kind kind() const noexcept { return kind_; }
    bool is_bytes() const noexcept { return kind_ == Kind::Bytes; }
    TypeId type() const noexcept { return type_; }
    std::int64_t size() const noexcept { return length_; }

    bool is_null(std::int64_t i) const noexcept {
        return validity_ != nullptr && !((validity_[i >> 3] >> (i & 7)) & 1);
    }

    /// Whether row `i` is null, or a NaN of a Float column: a value that no
    /// order or range can place.
    bool is_missing(std::int64_t i) const noexcept {
        return is_null(i) ||
               (kind_ == Kind::Float && get_double(i) != get_double(i));
    }

    /// The value of a Signed column (Bool reads as 0 or 1).
    std::int64_t get_int(std::int64_t i) const noexcept;
    /// The value of an Unsigned column.
    std::uint64_t get_uint(std::int64_t i) const noexcept;
    /// The value of any numeric column as a double.
    double get_double(std::int64_t i) const noexcept;
    /// The bytes of a Bytes column.
    std::string_view get_bytes(std::int64_t i) const noexcept;

    /// Three-way order of non-null rows `a` and `b` of this column: numeric
    /// by value with NaN last and equal to NaN, bytes by memcmp then length.
    int compare(std::int64_t a, std::int64_t b) const noexcept {
        return compare(a, *this, b);
    }
    /// The same order between row `a` of this column and row `b` of `other`,
    /// which must have the same kind.
    int compare(std::int64_t a, const ColumnView& other,
                std::int64_t b) const noexcept;

   private:
    Series column_;
    Kind kind_ = Kind::Signed;
    TypeId type_ = TypeId::Int64;
    TypeId physical_ = TypeId::Int64;
    std::int64_t length_ = 0;
    const std::uint8_t* data_ = nullptr;
    const std::uint8_t* validity_ = nullptr;
    const std::int32_t* off32_ = nullptr;
    const std::int64_t* off64_ = nullptr;
};

/// The row indices [0, n) ordered by `keys` ascending, a null sorting after
/// every value, ties in row order.
std::vector<std::int64_t> order_rows(const std::vector<ColumnView>& keys,
                                     std::int64_t n);

/// Whether rows `a` and `b` hold the same key tuple, a null equal to a null.
bool same_keys(const std::vector<ColumnView>& keys, std::int64_t a,
               std::int64_t b);

/// Given `order` from order_rows, the end of the run of rows from position
/// `begin` that share its key tuple.
std::int64_t run_end(const std::vector<ColumnView>& keys,
                     const std::vector<std::int64_t>& order,
                     std::int64_t begin);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_ORDER_H
