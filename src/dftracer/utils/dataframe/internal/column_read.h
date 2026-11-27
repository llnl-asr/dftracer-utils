// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/decimal.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/internal/string_reader.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/string.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Read one FLAT numeric cell widened to a common type, dispatching on the
// column's TypeId. Shared by every kernel that needs a domain-agnostic value
// (stats, aggregation, hashing, ...) so the per-type switch lives in one place.
namespace dftracer::utils::dataframe {

/// Bit `j` of the bit-packed bytes `p`.
template <class P>
inline bool bit_at(const P* p, std::int64_t j) noexcept {
    return ((static_cast<std::uint32_t>(p[j >> 3]) >> (j & 7)) &
            std::uint32_t{1}) != 0;
}

/// Whether a value of type `t` is read as a double (@ref read_f64), the types
/// with no integer physical layout, where read_i64 would give 0 for every row.
inline bool is_double_decoded(TypeId t) {
    return t == TypeId::Float32 || t == TypeId::Float64 ||
           t == TypeId::Float16 || t == TypeId::Decimal128 ||
           t == TypeId::Decimal256;
}

/// `f(chunk, local)` for row `i` of the CHUNKED column `c`: the chunk holding
/// the row, borrowed for the call, and the row inside it.
template <class F>
inline auto at_chunk(const Series& c, std::int64_t i, F&& f) {
    std::int64_t local = 0;
    Series chunk{const_cast<dftu_series*>(&c.handle()->chunk_at(i, local))};
    auto r = f(chunk, local);
    chunk.release();
    return r;
}

/// Cell `i` widened to int64 (Bool -> 0/1). Dispatches on physical_type(),
/// so Date32/Time32 read as Int32 and Date64/Time64/Timestamp/Duration read
/// as Int64: their exact on-disk value, not a lossy conversion. 0 for a
/// column with no integer physical layout.
inline std::int64_t read_i64(const Series& c, std::int64_t i) {
    if (c.handle()->is_chunked())
        return at_chunk(c, i, [](const Series& k, std::int64_t j) {
            return read_i64(k, j);
        });
    switch (physical_type(c.type())) {
        case TypeId::Bool: {
            const std::uint8_t* b = c.data<std::uint8_t>();
            return bit_at(b, i);
        }
        case TypeId::Int8:
            return c.data<std::int8_t>()[i];
        case TypeId::Int16:
            return c.data<std::int16_t>()[i];
        case TypeId::Int32:
            return c.data<std::int32_t>()[i];
        case TypeId::Int64:
            return c.data<std::int64_t>()[i];
        default:
            return 0;
    }
}

/// Cell `i` widened to uint64: unsigned columns keep their value; signed/Bool
/// are read as int64 then reinterpreted (so hashing sees a stable bit pattern).
/// 0 for non-integer columns.
inline std::uint64_t read_u64(const Series& c, std::int64_t i) {
    if (c.handle()->is_chunked())
        return at_chunk(c, i, [](const Series& k, std::int64_t j) {
            return read_u64(k, j);
        });
    switch (c.type()) {
        case TypeId::Uint8:
            return c.data<std::uint8_t>()[i];
        case TypeId::Uint16:
            return c.data<std::uint16_t>()[i];
        case TypeId::Uint32:
            return c.data<std::uint32_t>()[i];
        case TypeId::Uint64:
            return c.data<std::uint64_t>()[i];
        default:
            return static_cast<std::uint64_t>(read_i64(c, i));
    }
}

/// Cell `i` widened to double (Bool -> 0/1). Float16 decodes its half bits
/// exactly; Decimal128/256 decode the scaled integer at the documented Float64
/// precision loss, so use read_bytes when equality must be exact. 0 for a
/// column whose value_domain() is not Numeric.
inline double read_f64(const Series& c, std::int64_t i) {
    if (c.handle()->is_chunked())
        return at_chunk(c, i, [](const Series& k, std::int64_t j) {
            return read_f64(k, j);
        });
    switch (c.type()) {
        case TypeId::Float16:
            return static_cast<double>(
                half_to_float(c.data<std::uint16_t>()[i]));
        case TypeId::Float32:
            return static_cast<double>(c.data<float>()[i]);
        case TypeId::Float64:
            return c.data<double>()[i];
        case TypeId::Decimal128:
            return decimal128_to_double(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 16,
                c.data_type().decimal_scale());
        case TypeId::Decimal256:
            return decimal256_to_double(
                c.data<std::uint8_t>() + static_cast<std::size_t>(i) * 32,
                c.data_type().decimal_scale());
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return static_cast<double>(read_u64(c, i));
        default:
            return static_cast<double>(read_i64(c, i));
    }
}

/// Byte view of cell `i` for a ValueDomain::Bytes column, valid while `c`
/// lives. A string column reads in any layout the way Series::string_at does; a
/// FixedSizeBinary or Decimal column must be FLAT. Empty for any other column.
inline dstd::StringView read_bytes(const Series& c, std::int64_t i) {
    if (c.handle()->is_chunked())
        return at_chunk(c, i, [](const Series& k, std::int64_t j) {
            return read_bytes(k, j);
        });
    const TypeId t = c.type();
    if (narrow_varwidth_type(t) == TypeId::String ||
        narrow_varwidth_type(t) == TypeId::Binary)
        return c.string_at(i);
    std::size_t width;
    if (t == TypeId::FixedSizeBinary)
        width = static_cast<std::size_t>(c.data_type().fixed_size());
    else if (t == TypeId::Decimal128 || t == TypeId::Decimal256)
        width = byte_width(t).value_or(0);
    else
        return {};
    const char* d = static_cast<const char*>(dftu_series_data(c.handle()));
    if (d == nullptr || width == 0) return {};
    return dstd::StringView(d + static_cast<std::size_t>(i) * width, width);
}

/// Moves (`c`, `i`) down every CHUNKED, DICTIONARY and SELECTION layer to the
/// FLAT, VIEW or CONSTANT column that holds row `i`. False for a null row,
/// which has no value to read: the own validity of any layer, a SELECTION index
/// below 0 or a null DICTIONARY entry.
inline bool find_leaf(const dftu_series*& c, std::int64_t& i) {
    for (;;) {
        if (c->is_chunked()) {
            std::int64_t local = 0;
            c = &c->chunk_at(i, local);
            i = local;
            continue;
        }
        if (c->validity.block() && !bit_at(c->validity.data(), i)) return false;
        if (c->encoding == Encoding::Dictionary) {
            i = reinterpret_cast<const std::int32_t*>(c->data.data())[i];
        } else if (c->encoding == Encoding::Selection) {
            i = reinterpret_cast<const std::int64_t*>(c->data.data())[i];
            if (i < 0) return false;
        } else {
            return true;
        }
        c = c->child().get();
    }
}

/// Whether `c` holds its own values, with no layer to walk down.
inline bool is_leaf(const dftu_series& c) {
    return c.encoding == Encoding::Flat || c.encoding == Encoding::View ||
           c.encoding == Encoding::Constant;
}

/// Whether a CONSTANT layer sits at any depth of `c`.
inline bool has_constant_layer(const dftu_series& c) {
    if (c.encoding == Encoding::Constant) return true;
    if (c.is_chunked()) {
        for (const dftu_nested& chunk : c.nested)
            if (has_constant_layer(*chunk.series)) return true;
        return false;
    }
    return (c.encoding == Encoding::Selection ||
            c.encoding == Encoding::Dictionary) &&
           has_constant_layer(*c.child());
}

/// The one value of the CONSTANT `c` as a one-row FLAT column on the same
/// buffers, with no null.
inline dftu_series constant_row(const dftu_series& c) {
    dftu_series one(c);
    one.encoding = Encoding::Flat;
    one.length = 1;
    one.validity = {};
    one.null_count = 0;
    return one;
}

/// Whether row `i` of `c` is valid in `c`'s own bitmap.
inline bool own_valid(const dftu_series& c, std::int64_t i) noexcept {
    return !c.validity.block() || bit_at(c.validity.data(), i);
}

/// Whether row `i` of `c`, in any layout, is null (see @ref find_leaf).
inline bool cell_is_null(const dftu_series* c, std::int64_t i) {
    return !find_leaf(c, i);
}

/// The one byte of a bit-packed Bool value, as `value_ids` keys a Bool.
inline constexpr char BOOL_BYTES[2] = {0, 1};

/// The key bytes of row `j` of a fixed-width leaf whose data starts at `data`:
/// the value's own `width` bytes, one byte of 0 or 1 for a bit-packed Bool.
inline dstd::StringView leaf_key(const std::uint8_t* data, std::int64_t j,
                                 std::size_t width, bool is_bool) {
    if (is_bool) return dstd::StringView(BOOL_BYTES + bit_at(data, j), 1);
    return dstd::StringView(reinterpret_cast<const char*>(data) +
                                static_cast<std::size_t>(j) * width,
                            width);
}

/// Raw bytes of row `i` of a fixed-width column in any layout, valid while `c`
/// lives: the value's own `width` bytes, one byte of 0 or 1 for a bit-packed
/// Bool. The bytes a FLAT column holds, so a key, a dedupe and a group match
/// it exactly. False for a null row.
inline bool key_at(const dftu_series* c, std::int64_t i, std::size_t width,
                   bool is_bool, dstd::StringView& out) {
    if (!find_leaf(c, i)) return false;
    if (!c->data.block()) return false;
    const std::int64_t j = c->encoding == Encoding::Constant ? 0 : i;
    out = leaf_key(c->data.data(), j, width, is_bool);
    return true;
}

/// The key bytes of row `i` of a column in any layout: the entry text of a
/// string (@ref str_at), else the value's own bytes (@ref key_at). False for a
/// null row.
inline bool key_bytes(const dftu_series& c, std::int64_t i, bool str,
                      bool is_bool, std::size_t width, dstd::StringView& out) {
    if (str) return str_at(&c, i, out);
    return key_at(&c, i, width, is_bool, out);
}

/// The null rows of `c` in any layout. Unlike @ref Series::null_count it counts
/// the rows of a null DICTIONARY entry. Walks the rows of a DICTIONARY,
/// SELECTION or CONSTANT column, in parallel when a backend is installed.
inline std::int64_t cell_null_count(const dftu_series& c) {
    if (c.encoding == Encoding::Flat || c.encoding == Encoding::View)
        return c.null_count;
    if (c.is_chunked()) {
        std::int64_t n = 0;
        for (const dftu_nested& chunk : c.nested)
            n += cell_null_count(*chunk.series);
        return n;
    }
    return parallel_reduce<std::int64_t>(
        c.length, std::int64_t{1} << 16, 0,
        [&](std::int64_t b, std::int64_t e) {
            std::int64_t n = 0;
            for (std::int64_t i = b; i < e; ++i) n += cell_is_null(&c, i);
            return n;
        },
        [](std::int64_t a, std::int64_t b) { return a + b; });
}

/// `read(leaf, j)` on the column that holds row `i` of `c`, borrowed for the
/// call. Row `i` must not be null (see @ref cell_is_null).
template <class Read>
inline auto at_cell(const dftu_series* c, std::int64_t i, Read read) {
    using T = std::invoke_result_t<Read, const Series&, std::int64_t>;
    if (!is_leaf(*c) && !find_leaf(c, i)) return T{};
    if (c->encoding == Encoding::Constant) {
        dftu_series one = constant_row(*c);
        Series leaf{&one};
        T r = read(leaf, 0);
        leaf.release();
        return r;
    }
    Series leaf{const_cast<dftu_series*>(c)};
    T r = read(leaf, i);
    leaf.release();
    return r;
}

/// @ref read_i64 of row `i` of a column in any layout, under the rule of
/// @ref at_cell.
inline std::int64_t cell_i64(const dftu_series* c, std::int64_t i) {
    if (!is_leaf(*c) && !find_leaf(c, i)) return 0;
    // The usual leaf, read with no borrowed handle.
    if (c->encoding == Encoding::Flat && c->type == TypeId::Int64)
        return reinterpret_cast<const std::int64_t*>(c->data.data())[i];
    return at_cell(
        c, i, [](const Series& k, std::int64_t j) { return read_i64(k, j); });
}

/// @ref read_u64 of row `i` of a column in any layout, under the rule of
/// @ref at_cell.
inline std::uint64_t cell_u64(const dftu_series* c, std::int64_t i) {
    return at_cell(
        c, i, [](const Series& k, std::int64_t j) { return read_u64(k, j); });
}

/// @ref read_f64 of row `i` of a column in any layout, under the rule of
/// @ref at_cell.
inline double cell_f64(const dftu_series* c, std::int64_t i) {
    if (!is_leaf(*c) && !find_leaf(c, i)) return 0.0;
    // The usual leaf, read with no borrowed handle.
    if (c->encoding == Encoding::Flat && c->type == TypeId::Float64)
        return reinterpret_cast<const double*>(c->data.data())[i];
    return at_cell(
        c, i, [](const Series& k, std::int64_t j) { return read_f64(k, j); });
}

/// @ref cell_f64 of row `i` into `out`, with one walk of the layers. False,
/// with `out` untouched, for a null row.
inline bool try_cell_f64(const dftu_series* c, std::int64_t i, double& out) {
    if (!find_leaf(c, i)) return false;
    out = cell_f64(c, i);
    return true;
}

/// @ref cell_i64 of row `i` into `out`, as @ref try_cell_f64.
inline bool try_cell_i64(const dftu_series* c, std::int64_t i,
                         std::int64_t& out) {
    if (!find_leaf(c, i)) return false;
    out = cell_i64(c, i);
    return true;
}

/// @ref read_bytes of row `i` of a column in any layout, valid while `c`
/// lives, under the rule of @ref at_cell.
inline dstd::StringView cell_bytes(const dftu_series* c, std::int64_t i) {
    return at_cell(
        c, i, [](const Series& k, std::int64_t j) { return read_bytes(k, j); });
}

/// Raw value of row `i` of a column in any layout, kept in the column's own
/// type: the same bits a FLAT column holds. 0 for a null row, under the rule of
/// @ref at_cell.
template <class T>
T cell_value(const dftu_series& c, std::int64_t i) {
    if constexpr (std::is_floating_point_v<T>)
        return static_cast<T>(cell_f64(&c, i));
    else if constexpr (std::is_unsigned_v<T>)
        return static_cast<T>(cell_u64(&c, i));
    else
        return static_cast<T>(cell_i64(&c, i));
}

/// The layer chain from a column down to the column that holds its values,
/// resolved once so a row costs pointer arithmetic over at most MAX_DEPTH
/// steps: no handle, no type dispatch, no find_leaf walk. Not resolvable for a
/// CHUNKED column, whose chunks differ per row (resolve per chunk instead).
class ReadPath {
   public:
    static constexpr int MAX_DEPTH = 4;

    explicit ReadPath(const dftu_series* c) noexcept { resolve(c); }

    bool ok() const noexcept { return ok_; }
    /// Whether a row may be null: a bitmap on any layer, a SELECTION index
    /// below 0 or a null DICTIONARY entry.
    bool maybe_null() const noexcept { return nullable_; }
    const dftu_series* leaf() const noexcept { return leaf_; }

    int depth() const noexcept { return depth_; }
    const std::int64_t* selection(int s) const noexcept {
        return steps_[s].sel;
    }
    const std::int32_t* dictionary(int s) const noexcept {
        return steps_[s].codes;
    }
    bool constant_at(int s) const noexcept { return steps_[s].constant; }
    bool has_validity(int s) const noexcept {
        return steps_[s].validity != nullptr;
    }
    const std::uint8_t* validity(int s) const noexcept {
        return steps_[s].validity;
    }
    std::size_t validity_size(int s) const noexcept {
        return steps_[s].validity_size;
    }

    /// 1 when row `i` of the leaf is `selection(0)[i]` and 2 when it is
    /// `dictionary(0)[i]`, so the index needs no chain walk; 0 otherwise. The
    /// fast shape is one non-constant layer with no bitmap of its own over a
    /// leaf with no bitmap either.
    int direct() const noexcept {
        if (depth_ != 2 || steps_[0].validity != nullptr ||
            steps_[0].constant || steps_[1].validity != nullptr ||
            steps_[1].codes != nullptr || steps_[1].sel != nullptr ||
            steps_[1].constant)
            return 0;
        if (steps_[0].sel != nullptr) return 1;
        if (steps_[0].codes != nullptr) return 2;
        return 0;
    }

    /// Row `i` as row `j` of the leaf. False for a null row.
    bool locate(std::int64_t i, std::int64_t& j) const noexcept {
        j = i;
        for (int s = 0; s < depth_; ++s) {
            const Step& st = steps_[s];
            if (st.validity != nullptr && !bit_at(st.validity, j)) return false;
            if (st.codes != nullptr) {
                j = st.codes[j];
            } else if (st.sel != nullptr) {
                j = st.sel[j];
                if (j < 0) return false;
            } else if (st.constant) {
                j = 0;  // CONSTANT: its one value
            }
        }
        return true;
    }

   private:
    struct Step {
        const std::uint8_t* validity = nullptr;
        std::size_t validity_size = 0;
        const std::int32_t* codes = nullptr;
        const std::int64_t* sel = nullptr;
        bool constant = false;
    };

    void resolve(const dftu_series* c) noexcept {
        for (;;) {
            if (c == nullptr || depth_ == MAX_DEPTH || c->is_chunked()) {
                ok_ = false;
                return;
            }
            Step& st = steps_[depth_++];
            if (c->validity.block()) {
                st.validity = c->validity.data();
                st.validity_size = c->validity.size();
                nullable_ = true;
            }
            if (c->encoding == Encoding::Dictionary) {
                st.codes =
                    reinterpret_cast<const std::int32_t*>(c->data.data());
                nullable_ = true;
            } else if (c->encoding == Encoding::Selection) {
                st.sel = reinterpret_cast<const std::int64_t*>(c->data.data());
                nullable_ = true;
            } else {
                if (c->encoding == Encoding::Constant) st.constant = true;
                leaf_ = c;
                ok_ = true;
                return;
            }
            c = c->child().get();
        }
    }

    Step steps_[MAX_DEPTH];
    const dftu_series* leaf_ = nullptr;
    int depth_ = 0;
    bool ok_ = false;
    bool nullable_ = false;
};

/// Up to CAPACITY rows of a numeric column of type `T` gathered out of any
/// non-CHUNKED layout: the layers are resolved once (one @ref ReadPath) and a
/// row is one indexed load. The gathered block is a bounded copy of at most
/// CAPACITY rows, so a kernel runs its FLAT path (SIMD included) on it in place
/// of a copy of the whole column.
template <class T>
class RowBlock {
   public:
    static constexpr std::int64_t CAPACITY = 1024;

    RowBlock(const dftu_series& c, bool packed = false) noexcept
        : path_(&c), packed_(packed) {
        const dftu_series* leaf = path_.leaf();
        if (!path_.ok() || leaf == nullptr || !leaf->data.block()) return;
        if (leaf->encoding == Encoding::Flat ||
            leaf->encoding == Encoding::Constant) {
            p_ = reinterpret_cast<const T*>(leaf->data.data());
            usable_ = true;
        }
        // One layer over a leaf with no bitmap: a tight loop, the same read
        // materialize does, in place of the general chain walk.
        if (usable_) {
            direct_ = path_.direct();
            if (direct_ == 1) sel_ = path_.selection(0);
            if (direct_ == 2) codes_ = path_.dictionary(0);
        }
    }

    /// False when the column is CHUNKED (or deeper than MAX_DEPTH): the caller
    /// then resolves a chunk instead.
    bool usable() const noexcept { return usable_; }
    bool maybe_null() const noexcept { return path_.maybe_null(); }

    /// Gather rows [from, from+count) into `out` and their validity into the
    /// Arrow-layout bitmap `bits` (an all-valid block needs no bitmap, so a
    /// `bits` of nullptr is enough); returns the number of valid rows.
    std::int64_t gather(std::int64_t from, std::int64_t count, T* out,
                        std::uint8_t* bits) const noexcept {
        if (bits != nullptr)
            std::memset(bits, 0xff, static_cast<std::size_t>((count + 7) / 8));
        if (direct_ == 1) {
            if (packed_)
                return bits != nullptr
                           ? gather_sel<true, true>(from, count, out, bits)
                           : gather_sel<true, false>(from, count, out, bits);
            return bits != nullptr
                       ? gather_sel<false, true>(from, count, out, bits)
                       : gather_sel<false, false>(from, count, out, bits);
        }
        if (direct_ == 2) {
            if (packed_) return gather_codes<true>(from, count, out);
            return gather_codes<false>(from, count, out);
        }
        std::int64_t nvalid = 0;
        for (std::int64_t k = 0; k < count; ++k) {
            std::int64_t j = 0;
            const bool ok = path_.locate(from + k, j);
            if (!ok) {
                out[k] = T{};
                if (bits != nullptr)
                    bits[k >> 3] &= static_cast<std::uint8_t>(~(1u << (k & 7)));
                continue;
            }
            out[k] = packed_ ? static_cast<T>(bit_at(p_, j)) : p_[j];
            ++nvalid;
        }
        return nvalid;
    }

   private:
    /// Selection indices over a leaf with no bitmap, the tight loop
    /// materialize runs: PACKED reads a bit-packed Bool leaf, BITS writes the
    /// block bitmap.
    template <bool PACKED, bool BITS>
    std::int64_t gather_sel(std::int64_t from, std::int64_t count, T* out,
                            std::uint8_t* bits) const noexcept {
        const std::int64_t* sel = sel_;
        std::int64_t nvalid = 0;
        // Four rows per turn: the random loads are independent, so their
        // misses overlap the way they do in a copy of the whole column.
        std::int64_t k = 0;
        for (; k + 4 <= count; k += 4) {
            const std::int64_t j0 = sel[from + k];
            const std::int64_t j1 = sel[from + k + 1];
            const std::int64_t j2 = sel[from + k + 2];
            const std::int64_t j3 = sel[from + k + 3];
            nvalid += take<0, PACKED, BITS>(j0, k, out, bits);
            nvalid += take<1, PACKED, BITS>(j1, k, out, bits);
            nvalid += take<2, PACKED, BITS>(j2, k, out, bits);
            nvalid += take<3, PACKED, BITS>(j3, k, out, bits);
        }
        for (; k < count; ++k)
            nvalid += take<0, PACKED, BITS>(sel[from + k], k, out, bits);
        return nvalid;
    }

    /// Row `k + U` of the block from leaf row `j`, 0 when it is null.
    template <int U, bool PACKED, bool BITS>
    int take(std::int64_t j, std::int64_t k, T* out,
             std::uint8_t* bits) const noexcept {
        const std::int64_t r = k + U;
        if (j < 0) {
            out[r] = T{};
            if constexpr (BITS)
                bits[r >> 3] &= static_cast<std::uint8_t>(~(1u << (r & 7)));
            return 0;
        }
        out[r] = PACKED ? static_cast<T>(bit_at(p_, j)) : p_[j];
        return 1;
    }

    template <bool PACKED>
    std::int64_t gather_codes(std::int64_t from, std::int64_t count,
                              T* out) const noexcept {
        const std::int32_t* codes = codes_;
        const T* p = p_;
        for (std::int64_t k = 0; k < count; ++k) {
            const std::int64_t j = codes[from + k];
            out[k] = PACKED ? static_cast<T>(bit_at(p, j)) : p[j];
        }
        return count;
    }

    ReadPath path_;
    const T* p_ = nullptr;
    const std::int64_t* sel_ = nullptr;
    const std::int32_t* codes_ = nullptr;
    bool packed_ = false;
    bool usable_ = false;
    int direct_ = 0;
};

/// Rows per range of @ref each_block: whole CAPACITY-row blocks, and a multiple
/// of 8 so every range owns its own bytes of a packed bitmap.
template <class T>
inline constexpr std::int64_t BLOCK_GRAIN = 8 * RowBlock<T>::CAPACITY;

/// Runs `body(partial, from, count, buf, valid, nvalid)` over `n` rows of
/// `block`'s column starting at row `offset`, in CAPACITY-row blocks, fanning
/// whole-block ranges out over the parallel backend (the one a copy of the
/// column gathers through) and combining the range partials with `merge` in
/// range order, so a fold over the ranges or over the rows gives the same
/// result. `identity` must be neutral for `merge`; `from` is relative to
/// `offset`. The per-range buffer is the bounded copy @ref RowBlock makes.
template <class T, class Partial, class Body, class Merge>
Partial each_block(const RowBlock<T>& block, std::int64_t offset,
                   std::int64_t n, Partial identity, Body&& body,
                   Merge&& merge) {
    constexpr std::int64_t CAP = RowBlock<T>::CAPACITY;
    return parallel_reduce<Partial>(
        n, BLOCK_GRAIN<T>, identity,
        [&](std::int64_t b, std::int64_t e) {
            alignas(64) T buf[CAP];
            std::uint8_t valid[(CAP + 7) / 8];
            Partial acc = identity;
            for (std::int64_t from = b; from < e; from += CAP) {
                const std::int64_t count = std::min(CAP, e - from);
                std::memset(valid, 0xff,
                            static_cast<std::size_t>((count + 7) / 8));
                const std::int64_t nvalid =
                    block.gather(offset + from, count, buf, valid);
                body(acc, from, count, buf, valid, nvalid);
            }
            return acc;
        },
        merge);
}

/// Bits of up to CAPACITY rows of a Bool column, gathered out of any
/// non-CHUNKED layout: the selected rows' bits, a null row reading 0 and
/// cleared in the block's validity. The chain is resolved once, and a chain of
/// bitmaps over a bit-packed leaf is copied a word at a time, so a block costs
/// what materialize's copy costs. The block is a bounded copy of at most
/// CAPACITY rows.
class MaskBits {
   public:
    static constexpr std::int64_t CAPACITY = 1024;

    explicit MaskBits(const dftu_series& c) noexcept : path_(&c) {
        const dftu_series* leaf = path_.leaf();
        if (!path_.ok() || leaf == nullptr || !leaf->data.block() ||
            c.type != TypeId::Bool)
            return;
        if (leaf->encoding == Encoding::Constant) {
            src_ = reinterpret_cast<const std::uint8_t*>(leaf->data.data());
        } else if (leaf->encoding == Encoding::Flat) {
            src_ = reinterpret_cast<const std::uint8_t*>(leaf->data.data());
            words_ = (leaf->data.size() >> 3) << 3;
        } else {
            return;
        }
        // The word path needs every layer to be a bitmap: a code or an index
        // moves a row, so those go through the chain walk instead.
        word_ok_ = true;
        for (int i = 0; i < path_.depth(); ++i) {
            if (path_.selection(i) != nullptr || path_.dictionary(i) != nullptr)
                word_ok_ = false;
            if (path_.validity(i) == nullptr) continue;
            const std::size_t size = path_.validity_size(i);
            words_ = std::min<std::int64_t>(words_, (size >> 3) << 3);
        }
        if (path_.depth() == 2 && path_.selection(0) != nullptr &&
            path_.validity(0) == nullptr && path_.validity(1) == nullptr &&
            path_.dictionary(0) == nullptr && leaf->encoding == Encoding::Flat)
            sel_ = path_.selection(0);
        usable_ = true;
    }

    /// False when the column is CHUNKED (or deeper than the chain holds): the
    /// caller then resolves a chunk or a row instead.
    bool usable() const noexcept { return usable_; }

    /// Gather rows [from, from+count) into `bits` and their validity into
    /// `valid`; the last word is byte copied, so a block never reads past the
    /// column's bitmap.
    void gather(std::int64_t from, std::int64_t count, std::uint8_t* bits,
                std::uint8_t* valid) const noexcept {
        const std::size_t bytes = static_cast<std::size_t>((count + 7) / 8);
        std::memset(bits, 0, bytes);
        std::memset(valid, 0xff, bytes);
        if (sel_ != nullptr) {
            // One selection over a bitmap-free leaf: the same tight random
            // load loop a copy does, eight rows per output byte.
            const std::int64_t* sel = sel_;
            std::int64_t k = 0;
            for (; k + 8 <= count; k += 8) {
                std::uint8_t b = 0, v = 0xff;
                for (int t = 0; t < 8; ++t) {
                    const std::int64_t j = sel[from + k + t];
                    if (j < 0) {
                        v &= static_cast<std::uint8_t>(~(1u << t));
                        continue;
                    }
                    if (bit_at(src_, j))
                        b |= static_cast<std::uint8_t>(1u << t);
                }
                bits[static_cast<std::size_t>(k >> 3)] |= b;
                valid[static_cast<std::size_t>(k >> 3)] = v;
            }
            for (; k < count; ++k) {
                const std::int64_t j = sel[from + k];
                if (j < 0) {
                    valid[static_cast<std::size_t>(k >> 3)] &=
                        static_cast<std::uint8_t>(~(1u << (k & 7)));
                    continue;
                }
                if (bit_at(src_, j))
                    bits[static_cast<std::size_t>(k >> 3)] |=
                        static_cast<std::uint8_t>(1u << (k & 7));
            }
            return;
        }
        const bool word =
            word_ok_ && (from & 7) == 0 && from + count <= words_ * 8;
        if (word) {
            const std::size_t at = static_cast<std::size_t>(from >> 3);
            const std::size_t whole = bytes / 8;
            for (std::size_t w = 0; w < whole; ++w) {
                std::uint64_t b = 0, v = ~std::uint64_t{0};
                std::memcpy(&b, src_ + at + w * 8, 8);
                for (int i = 0; i < path_.depth(); ++i) {
                    const std::uint8_t* vb = path_.validity(i);
                    if (vb == nullptr) continue;
                    std::uint64_t x = 0;
                    std::memcpy(&x, vb + at + w * 8, 8);
                    v &= x;
                }
                b &= v;
                std::memcpy(bits + w * 8, &b, 8);
                std::memcpy(valid + w * 8, &v, 8);
            }
            for (std::size_t i = whole * 8; i < bytes;) {
                const std::size_t left = std::min<std::size_t>(8, bytes - i);
                std::uint64_t b = 0, v = ~std::uint64_t{0};
                std::memcpy(&b, src_ + at + i, left);
                for (int l = 0; l < path_.depth(); ++l) {
                    const std::uint8_t* vb = path_.validity(l);
                    if (vb == nullptr) continue;
                    std::uint64_t x = 0;
                    std::memcpy(&x, vb + at + i, left);
                    v &= x;
                }
                b &= v;
                std::memcpy(bits + i, &b, left);
                std::memcpy(valid + i, &v, left);
                i += left;
            }
            return;
        }
        for (std::int64_t k = 0; k < count; ++k) {
            std::int64_t j = 0;
            if (!path_.locate(from + k, j)) {
                valid[k >> 3] &= static_cast<std::uint8_t>(~(1u << (k & 7)));
                continue;
            }
            if (bit_at(src_, j))
                bits[k >> 3] |= static_cast<std::uint8_t>(1u << (k & 7));
        }
    }

   private:
    ReadPath path_;
    const std::uint8_t* src_ = nullptr;
    std::int64_t words_ = 0;
    const std::int64_t* sel_ = nullptr;
    bool word_ok_ = false;
    bool usable_ = false;
};

/// Sets in `bits` (zeroed by the caller) bit k for each of the `n` rows from
/// row `from` on that the Bool column `mask`, in any layout, selects; a null
/// row selects none. A block of rows is gathered at a time through @ref
/// MaskBits and its bytes are ORed in, one range of blocks per task, so every
/// write starts on a bitmap byte whatever range the backend hands out. The rows
/// are counted from `from` in `bits`, so `bits` holds `(n + 7) / 8` bytes.
inline void mask_row_bits(const dftu_series& mask, std::int64_t from,
                          std::int64_t n, std::uint8_t* bits) {
    const MaskBits block(mask);
    if (!block.usable()) {
        for (std::int64_t k = 0; k < n; ++k) {
            const std::int64_t p = from + k;
            if (cell_u64(&mask, p) == 0 || cell_is_null(&mask, p)) continue;
            bits[static_cast<std::size_t>(k >> 3)] |=
                static_cast<std::uint8_t>(1u << (k & 7));
        }
        return;
    }
    constexpr std::int64_t CAP = MaskBits::CAPACITY;
    parallel_for((n + CAP - 1) / CAP, 8, [&](std::int64_t b, std::int64_t e) {
        alignas(64) std::uint8_t buf[(CAP + 7) / 8];
        std::uint8_t valid[(CAP + 7) / 8];
        for (std::int64_t blk = b; blk < e; ++blk) {
            const std::int64_t at = blk * CAP;
            const std::int64_t count = std::min(CAP, n - at);
            block.gather(from + at, count, buf, valid);
            std::uint8_t* out = bits + (at >> 3);
            const std::size_t bytes = static_cast<std::size_t>((count + 7) / 8);
            for (std::size_t i = 0; i < bytes; ++i) out[i] |= buf[i];
        }
    });
}

/// The values of a numeric column of type `T`, one row at a time: pointer
/// arithmetic for a FLAT column, the shared cell reader for any other layout.
/// A kernel writes its loop once against this and keeps the raw FLAT path the
/// compiler vectorizes.
template <class T>
class ColumnCells {
   public:
    explicit ColumnCells(const dftu_series& c) noexcept
        : c_(&c), path_(&c), flat_(c.encoding == Encoding::Flat) {
        if (flat_) {
            p_ = reinterpret_cast<const T*>(c.data.data());
            return;
        }
        const dftu_series* leaf = path_.leaf();
        if (path_.ok() && leaf != nullptr && leaf->data.block() &&
            (leaf->encoding == Encoding::Flat ||
             leaf->encoding == Encoding::Constant)) {
            p_ = reinterpret_cast<const T*>(leaf->data.data());
            resolved_ = true;
            packed_ = leaf->type == TypeId::Bool;
            // One layer over a bitmap-free leaf: a row is one indexed load.
            direct_ = path_.direct();
            if (direct_ == 1) sel_ = path_.selection(0);
            if (direct_ == 2) codes_ = path_.dictionary(0);
        }
    }

    /// Whether a row may be null, so the loop must test each row.
    bool maybe_null() const noexcept {
        if (flat_) return static_cast<bool>(c_->validity.block());
        return !resolved_ || path_.maybe_null();
    }

    bool valid(std::int64_t i) const noexcept {
        if (flat_) return own_valid(*c_, i);
        if (direct_ != 0) return sel_ == nullptr || sel_[i] >= 0;
        if (!resolved_) return !cell_is_null(c_, i);
        std::int64_t j = 0;
        return path_.locate(i, j);
    }

    /// The value of row `i`, in the column's own type.
    T at(std::int64_t i) const noexcept {
        if (flat_) return p_[i];
        if (direct_ == 1) return sel_[i] < 0 ? T{} : p_[sel_[i]];
        if (direct_ == 2) return p_[codes_[i]];
        if (!resolved_) return cell_value<T>(*c_, i);
        std::int64_t j = 0;
        if (!path_.locate(i, j)) return T{};
        return p_[j];
    }

    /// The bit of row `i` of a bit-packed Bool column, read at `T` = uint8_t.
    bool bool_at(std::int64_t i) const noexcept {
        if (flat_) return bit_at(p_, i);
        if (!resolved_ || !packed_) return cell_u64(c_, i) != 0;
        std::int64_t j = 0;
        if (!path_.locate(i, j)) return false;
        return bit_at(p_, j);
    }

   private:
    const dftu_series* c_;
    const T* p_ = nullptr;
    const std::int64_t* sel_ = nullptr;
    const std::int32_t* codes_ = nullptr;
    ReadPath path_;
    bool flat_ = false;
    bool resolved_ = false;
    bool packed_ = false;
    int direct_ = 0;
};

/// True when `t` is nested and so has no per-row value for `op` to work on,
/// after logging a named refusal. The caller then returns its own refusal
/// value instead of computing over a zero it never read.
inline bool refuse_nested_value(dstd::StringView op, TypeId t) {
    if (value_domain(t) != ValueDomain::None) return false;
    DFTRACER_UTILS_LOG_ERROR("{}: no per-row value for nested type '{}'", op,
                             type_name(t));
    return true;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_COLUMN_READ_H
