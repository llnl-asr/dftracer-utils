// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/reshape.h>
#include <dftracer/utils/dataframe/internal/value_ids.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>

namespace dftracer::utils::dataframe {

namespace {

Series flat_view(const Series& s) {
    return s.is_flat() ? s.share() : s.materialize();
}

}  // namespace

dstd::Result<RowNumbers> number_rows(dstd::Span<const Series> keys,
                                     bool sorted) {
    const std::int64_t n = keys.empty() ? 0 : keys.front().length();
    dstd::Vector<std::int32_t> comb;
    dstd::Vector<std::int64_t> first;
    for (const Series& key : keys)
        if (value_domain(key.type()) == ValueDomain::None)
            return invalid_argument<
                "unstack: a level of type '{}' has no value to compare">(
                type_name(key.type()));
    for (std::size_t c = 0; n > 0 && c < keys.size(); ++c) {
        ValueIds ids;
        value_ids(*keys[c].handle(), ids);
        // A column of values that all differ leaves nothing to tell apart.
        if (c == 0 || static_cast<std::int64_t>(ids.first.size()) == n) {
            comb = std::move(ids.ids);
            first = std::move(ids.first);
            if (static_cast<std::int64_t>(first.size()) == n) break;
            continue;
        }
        combine_value_ids(comb, ids.ids, first);
    }
    const auto count = static_cast<std::int64_t>(first.size());
    dstd::Vector<std::int64_t> out(static_cast<std::size_t>(n));
    if (!sorted || count < 2) {
        for (std::int64_t i = 0; i < n; ++i)
            out[static_cast<std::size_t>(i)] =
                comb[static_cast<std::size_t>(i)];
        return RowNumbers{Series::flat_i64(out.data(), n),
                          Series::flat_i64(first.data(), count)};
    }
    DataFrame reps;
    dstd::Vector<dstd::String> names;
    for (std::size_t c = 0; c < keys.size(); ++c) {
        names.push_back(dstd::fmt<"k{}">(c));
        reps.names.emplace_back(dstd::StringView(names.back()));
        reps.columns.push_back(keys[c].take(first));
    }
    dstd::Vector<std::int64_t> group(static_cast<std::size_t>(count));
    std::iota(group.begin(), group.end(), std::int64_t{0});
    reps.names.emplace_back(dstd::StringView("g"));
    reps.columns.push_back(Series::flat_i64(group.data(), count));
    const std::uint8_t ascending[] = {0};
    const std::uint8_t nulls_first[] = {0};
    dstd::Result<DataFrame> ordered =
        reps.sort_by_multi(names, dstd::Span<const std::uint8_t>(ascending),
                           dstd::Span<const std::uint8_t>(nulls_first));
    if (!ordered) return ordered.error();
    const Series order = flat_view(ordered->column("g"));
    const std::int64_t* old_of = order.data<std::int64_t>();
    dstd::Vector<std::int64_t> rank(static_cast<std::size_t>(count));
    dstd::Vector<std::int64_t> sorted_first(static_cast<std::size_t>(count));
    for (std::int64_t j = 0; j < count; ++j) {
        rank[static_cast<std::size_t>(old_of[j])] = j;
        sorted_first[static_cast<std::size_t>(j)] =
            first[static_cast<std::size_t>(old_of[j])];
    }
    for (std::int64_t i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] =
            rank[static_cast<std::size_t>(comb[static_cast<std::size_t>(i)])];
    return RowNumbers{Series::flat_i64(out.data(), n),
                      Series::flat_i64(sorted_first.data(), count)};
}

dstd::Result<Series> unstack_positions(const Series& rows, const Series& cols,
                                       std::int64_t row_count,
                                       std::int64_t col_count) {
    if (row_count > 0 &&
        col_count > std::numeric_limits<std::int32_t>::max() / row_count)
        return invalid_argument<
            "Unstacked DataFrame is too big, causing int32 overflow">();
    const Series r = flat_view(rows);
    const Series c = flat_view(cols);
    const std::int64_t n = r.length();
    const std::int64_t* ri = r.data<std::int64_t>();
    const std::int64_t* ci = c.data<std::int64_t>();
    dstd::Vector<std::int64_t> pos(
        static_cast<std::size_t>(row_count * col_count), -1);
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t& cell =
            pos[static_cast<std::size_t>(ci[i] * row_count + ri[i])];
        if (cell >= 0)
            return invalid_argument<"unstack: two rows fill the same cell">();
        cell = i;
    }
    return Series::flat_i64(pos.data(), row_count * col_count);
}

dstd::Result<dstd::Vector<Series>> unstack_columns(const Series& values,
                                                   const Series& positions,
                                                   std::int64_t row_count) {
    const Series p = flat_view(positions);
    const std::int64_t cells = p.length();
    const Series all =
        values
            .take(dstd::Span<const std::int64_t>(
                p.data<std::int64_t>(), static_cast<std::size_t>(cells)))
            .materialize();
    dstd::Vector<Series> out;
    if (row_count == 0) return out;
    out.reserve(static_cast<std::size_t>(cells / row_count));
    for (std::int64_t at = 0; at < cells; at += row_count)
        out.push_back(all.slice(at, row_count));
    return out;
}

dstd::Result<Series> dictionary_of(const Series& codes, const Series& values) {
    if (!codes.valid() || !values.valid() || !codes.is_flat() ||
        codes.type() != TypeId::Int32)
        return invalid_argument<
            "dictionary codes must be a flat Int32 column">();
    const dftu_series& c = *codes.handle();
    const std::int64_t n = c.length;
    const std::int64_t size = values.length();
    const auto* in = reinterpret_cast<const std::int32_t*>(c.data.data());
    const std::uint8_t* bits = c.validity.block() ? c.validity.data() : nullptr;
    auto is_valid = [&](std::int64_t i) {
        return bits == nullptr || ((bits[i >> 3] >> (i & 7)) & 1) != 0;
    };

    bool rewrite = false;
    for (std::int64_t i = 0; i < n; ++i) {
        const bool out_of_range = in[i] < 0 || in[i] >= size;
        if (!out_of_range) continue;
        if (is_valid(i) && in[i] >= size)
            return invalid_argument<
                "dictionary code {} is out of range for {} "
                "values">(in[i], size);
        rewrite = true;
    }
    // A dictionary child is FLAT: a lazy one (a selection) counts its nulls
    // only on demand, which a code lookup would miss, so it is made flat (a
    // copy). No values: every row is null (any valid code failed above), so
    // the dictionary is one null entry that no code may miss.
    Series dictionary;
    if (size == 0) {
        dictionary = null_column_like(values, 1);
        if (!dictionary.valid())
            return invalid_argument<
                "an empty dictionary of type '{}' cannot hold null rows">(
                type_name(values.type()));
        rewrite = true;
    } else if (values.is_flat()) {
        dictionary = values.share();
    } else {
        dictionary = values.materialize();
        if (!dictionary.valid())
            return invalid_argument<
                "the values of a dictionary of type '{}' cannot be made "
                "flat">(type_name(values.type()));
    }

    auto* h = new dftu_series();
    Series out{h};
    adopt_type_from(*h, *dictionary.handle());
    h->encoding = Encoding::Dictionary;
    h->length = n;
    h->set_child(adopt_series(dictionary.release()));
    if (!rewrite) {
        h->data = c.data;
        h->validity = c.validity;
        h->null_count = c.null_count;
        return out;
    }
    const auto bytes = static_cast<std::size_t>((n + 7) / 8);
    h->data = new_buffer(static_cast<std::size_t>(n) * sizeof(std::int32_t));
    h->validity = new_buffer(bytes);
    auto* codes_out = reinterpret_cast<std::int32_t*>(writable(h->data));
    std::uint8_t* valid_out = writable(h->validity);
    std::memset(valid_out, 0, bytes);
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        if (is_valid(i) && in[i] >= 0 && in[i] < size) {
            codes_out[i] = in[i];
            valid_out[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
        } else {
            codes_out[i] = 0;
            ++nulls;
        }
    }
    h->null_count = nulls;
    return out;
}

}  // namespace dftracer::utils::dataframe
