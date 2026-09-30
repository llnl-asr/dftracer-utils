#ifndef DFTRACER_UTILS_DATAFRAME_OPS_OPS_COMMON_H
#define DFTRACER_UTILS_DATAFRAME_OPS_OPS_COMMON_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe::ops {

/// The index of the column `name` of `df`; throws std::out_of_range naming
/// `who` when it has none.
inline std::size_t column_named(const DataFrame& df, const std::string& name,
                                const char* who) {
    const std::int64_t i = df.column_index(name);
    if (i < 0)
        throw std::out_of_range(std::string(who) + ": no column named " + name);
    return static_cast<std::size_t>(i);
}

/// One view per named column of `df`, in order; an unsupported type throws
/// std::invalid_argument naming `who` and the column.
inline std::vector<ColumnView> views_of(const DataFrame& df,
                                        const std::vector<std::string>& names,
                                        const char* who) {
    std::vector<ColumnView> out;
    out.reserve(names.size());
    for (const std::string& n : names) {
        const Series& c = df.columns[column_named(df, n, who)];
        if (!ColumnView::supports(c.type()))
            throw std::invalid_argument(std::string(who) + ": column '" + n +
                                        "' of type " + type_name(c.type()) +
                                        " has no order");
        out.emplace_back(c);
    }
    return out;
}

/// A validity bitmap (Arrow layout, 1 = valid) with room for `n` rows, all
/// valid.
inline std::vector<std::uint8_t> valid_bits(std::size_t n) {
    return std::vector<std::uint8_t>((n + 7) / 8, 0xFF);
}

inline void set_null(std::vector<std::uint8_t>& bits, std::size_t i) {
    bits[i >> 3] &= static_cast<std::uint8_t>(~(1U << (i & 7)));
}

/// The order of the key tuple of row `a` of `left` against row `b` of `right`
/// (each a run of ColumnViews of equal kinds): a null sorts after every value
/// and equals a null.
inline int compare_keys(const std::vector<ColumnView>& left, std::int64_t a,
                        const std::vector<ColumnView>& right, std::int64_t b) {
    for (std::size_t k = 0; k < left.size(); ++k) {
        const bool na = left[k].is_null(a);
        const bool nb = right[k].is_null(b);
        if (na || nb) {
            if (na && nb) continue;
            return na ? 1 : -1;
        }
        const int c = left[k].compare(a, right[k], b);
        if (c != 0) return c;
    }
    return 0;
}

/// A frame of `left`'s columns gathered at `left_rows` followed by the columns
/// of `right` not in `skip`, gathered at `right_rows` (-1 gathers a null). A
/// right name that a left name or an earlier right name holds gets "_right";
/// a second collision throws std::invalid_argument naming `who`.
inline DataFrame join_columns(const DataFrame& left, const DataFrame& right,
                              const std::vector<std::size_t>& skip,
                              const std::vector<std::int64_t>& left_rows,
                              const std::vector<std::int64_t>& right_rows,
                              const char* who) {
    DataFrame out;
    for (std::size_t c = 0; c < left.columns.size(); ++c) {
        out.names.push_back(left.names[c]);
        out.columns.push_back(left.columns[c].take(left_rows));
    }
    for (std::size_t c = 0; c < right.columns.size(); ++c) {
        if (std::find(skip.begin(), skip.end(), c) != skip.end()) continue;
        std::string name = right.names[c];
        auto taken = [&](const std::string& n) {
            return std::find(out.names.begin(), out.names.end(), n) !=
                   out.names.end();
        };
        if (taken(name)) {
            name += "_right";
            if (taken(name))
                throw std::invalid_argument(
                    std::string(who) +
                    ": output column name collision: " + name);
        }
        out.names.push_back(std::move(name));
        out.columns.push_back(right.columns[c].take(right_rows));
    }
    return out;
}

}  // namespace dftracer::utils::dataframe::ops

#endif  // DFTRACER_UTILS_DATAFRAME_OPS_OPS_COMMON_H
