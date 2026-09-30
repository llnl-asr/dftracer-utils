#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <stdexcept>

#include "ops_common.h"

namespace dftracer::utils::dataframe {

namespace {

constexpr std::int64_t GRID_MAX_POINTS = 10'000'000;

bool is_integer_time(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

// floor(t / width) as a grid index; the grid point is index * width.
std::int64_t floor_index(std::int64_t t, std::int64_t width) {
    std::int64_t q = t / width;
    if (t % width != 0 && t < 0) --q;
    return q;
}

// `values` narrowed to the integer type `type`.
Series integer_column(TypeId type, const std::vector<std::int64_t>& values) {
    const auto n = static_cast<std::int64_t>(values.size());
    auto narrow = [&]<class T>() {
        std::vector<T> out(values.size());
        for (std::size_t i = 0; i < values.size(); ++i)
            out[i] = static_cast<T>(values[i]);
        return Series::flat(type, out.data(), n);
    };
    switch (type) {
        case TypeId::Int8:
            return narrow.operator()<std::int8_t>();
        case TypeId::Int16:
            return narrow.operator()<std::int16_t>();
        case TypeId::Int32:
            return narrow.operator()<std::int32_t>();
        case TypeId::Int64:
            return narrow.operator()<std::int64_t>();
        case TypeId::Uint8:
            return narrow.operator()<std::uint8_t>();
        case TypeId::Uint16:
            return narrow.operator()<std::uint16_t>();
        case TypeId::Uint32:
            return narrow.operator()<std::uint32_t>();
        default:
            return narrow.operator()<std::uint64_t>();
    }
}

struct Real {
    std::int64_t row;
    std::int64_t time;
    std::int64_t bucket;
};

}  // namespace

DataFrame gap_fill(const DataFrame& df,
                   const std::vector<std::string>& partition_by,
                   const std::string& time, std::int64_t bucket,
                   const std::vector<std::string>& values, GapFillMode mode,
                   std::optional<std::pair<std::int64_t, std::int64_t>> range) {
    if (bucket <= 0)
        throw std::invalid_argument("gap_fill: bucket_width must be positive");
    const std::size_t time_at = ops::column_named(df, time, "gap_fill");
    const TypeId time_type = df.columns[time_at].type();
    if (!is_integer_time(time_type))
        throw std::invalid_argument("gap_fill: time column must be integer");
    std::vector<std::size_t> part_at;
    std::vector<std::size_t> value_at;
    for (const std::string& n : partition_by)
        part_at.push_back(ops::column_named(df, n, "gap_fill"));
    for (const std::string& n : values) {
        const std::size_t at = ops::column_named(df, n, "gap_fill");
        if (mode == GapFillMode::Linear &&
            value_domain(df.columns[at].type()) != ValueDomain::Numeric)
            throw std::invalid_argument(
                "gap_fill: LINEAR fill over a non-numeric column");
        value_at.push_back(at);
    }

    const std::vector<ColumnView> parts =
        ops::views_of(df, partition_by, "gap_fill");
    std::vector<ColumnView> keys = parts;
    keys.emplace_back(df.columns[time_at]);
    const ColumnView& times = keys.back();
    std::vector<ColumnView> linear;
    if (mode == GapFillMode::Linear)
        for (const std::size_t at : value_at)
            linear.emplace_back(df.columns[at]);
    const bool unsigned_time = times.kind() == ColumnView::Kind::Unsigned;
    const auto read_time = [&](std::int64_t row) {
        return unsigned_time ? static_cast<std::int64_t>(times.get_uint(row))
                             : times.get_int(row);
    };

    const std::int64_t n = df.num_rows();
    const std::vector<std::int64_t> order = order_rows(keys, n);

    // Per output row: its own row (-1 generated), its partition's row, the
    // last real row before it (Locf) and its time.
    std::vector<std::int64_t> src;
    std::vector<std::int64_t> part_src;
    std::vector<std::int64_t> carried;
    std::vector<std::int64_t> time_out;
    std::vector<std::vector<double>> lin_value(linear.size());
    std::vector<std::vector<std::uint8_t>> lin_valid(linear.size());

    std::vector<Real> reals;
    for (std::int64_t p = 0; p < n;) {
        const std::int64_t q = parts.empty() ? n : run_end(parts, order, p);
        reals.clear();
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = order[static_cast<std::size_t>(r)];
            if (times.is_null(g)) continue;
            const std::int64_t t = read_time(g);
            const std::int64_t b = floor_index(t, bucket);
            if (!reals.empty() && reals.back().bucket == b) continue;
            reals.push_back({g, t, b});
        }
        if (!reals.empty()) {
            const std::int64_t start = range ? floor_index(range->first, bucket)
                                             : reals.front().bucket;
            const std::int64_t end = range ? floor_index(range->second, bucket)
                                           : reals.back().bucket;
            if (end >= start) {
                const std::uint64_t span = static_cast<std::uint64_t>(end) -
                                           static_cast<std::uint64_t>(start);
                if (span >= static_cast<std::uint64_t>(GRID_MAX_POINTS))
                    throw std::invalid_argument(
                        "gap_fill: grid exceeds 10,000,000 points; narrow the "
                        "range or widen the bucket");
                const std::int64_t rep = reals.front().row;
                std::int64_t j = 0;
                const auto count = static_cast<std::int64_t>(reals.size());
                for (std::int64_t k = start; k <= end; ++k) {
                    while (j < count &&
                           reals[static_cast<std::size_t>(j)].bucket < k)
                        ++j;
                    const bool has_real =
                        j < count &&
                        reals[static_cast<std::size_t>(j)].bucket == k;
                    const std::int64_t prev = j - 1;
                    const std::int64_t next = j < count ? j : -1;
                    part_src.push_back(rep);
                    if (has_real) {
                        const Real& r = reals[static_cast<std::size_t>(j)];
                        src.push_back(r.row);
                        carried.push_back(r.row);
                        time_out.push_back(r.time);
                    } else {
                        src.push_back(-1);
                        carried.push_back(
                            prev >= 0
                                ? reals[static_cast<std::size_t>(prev)].row
                                : -1);
                        time_out.push_back(k * bucket);
                    }
                    for (std::size_t v = 0; v < linear.size(); ++v) {
                        const ColumnView& col = linear[v];
                        double out = 0.0;
                        bool valid = false;
                        if (has_real) {
                            const std::int64_t row =
                                reals[static_cast<std::size_t>(j)].row;
                            if (!col.is_null(row)) {
                                out = col.get_double(row);
                                valid = true;
                            }
                        } else if (prev >= 0 && next >= 0) {
                            const Real& lo =
                                reals[static_cast<std::size_t>(prev)];
                            const Real& hi =
                                reals[static_cast<std::size_t>(next)];
                            if (!col.is_null(lo.row) && !col.is_null(hi.row)) {
                                const double v0 = col.get_double(lo.row);
                                const double v1 = col.get_double(hi.row);
                                const double t0 = static_cast<double>(lo.time);
                                const double t1 = static_cast<double>(hi.time);
                                const double g =
                                    static_cast<double>(k * bucket);
                                out = v0 + (v1 - v0) * (g - t0) / (t1 - t0);
                                valid = true;
                            }
                        }
                        lin_value[v].push_back(out);
                        lin_valid[v].push_back(valid ? 1 : 0);
                    }
                }
            }
        }
        p = q;
    }

    DataFrame out;
    out.names = df.names;
    for (std::size_t c = 0; c < df.columns.size(); ++c) {
        const Series& col = df.columns[c];
        if (c == time_at) {
            out.columns.push_back(integer_column(time_type, time_out));
            continue;
        }
        if (std::find(part_at.begin(), part_at.end(), c) != part_at.end()) {
            out.columns.push_back(col.take(part_src));
            continue;
        }
        const auto v = std::find(value_at.begin(), value_at.end(), c);
        if (v == value_at.end()) {
            out.columns.push_back(col.take(src));
        } else if (mode == GapFillMode::Locf) {
            out.columns.push_back(col.take(carried));
        } else if (mode == GapFillMode::Linear) {
            const auto at = static_cast<std::size_t>(v - value_at.begin());
            std::vector<std::uint8_t> bits =
                ops::valid_bits(lin_value[at].size());
            for (std::size_t i = 0; i < lin_valid[at].size(); ++i)
                if (!lin_valid[at][i]) ops::set_null(bits, i);
            out.columns.push_back(Series::flat_f64(
                lin_value[at].data(),
                static_cast<std::int64_t>(lin_value[at].size()), bits.data()));
        } else {
            out.columns.push_back(col.take(src));
        }
    }
    return out;
}

}  // namespace dftracer::utils::dataframe
