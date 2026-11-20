// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: two scalar UDFs that duql calls by name.
//
//   to_ms(dur)           the duration, in microseconds, as milliseconds
//   is_slow(dur, limit)  whether dur is above the constant limit
//
// Load it with `--plugin ./duration_udfs.so` and call them in a query:
//
//   derive ms = to_ms(dur) | where is_slow(dur, 1000) | select name, ms
//
// A UDF runs on whole columns, never row by row. A row with a null argument
// gives a null result without the UDF doing anything for it. The arguments
// arrive as borrowed column views, so a read copies nothing, and the result is
// a Column the host takes over.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o duration_udfs.so
// duration_udfs.cpp

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t CHECK_EVERY = 4096;
constexpr double MICROS_PER_MS = 1000.0;

bool is_number(dataframe::TypeId type) {
    return type == dataframe::TypeId::Int64 ||
           type == dataframe::TypeId::Uint64 ||
           type == dataframe::TypeId::Float64;
}

dstd::Unexpected<dstd::Error> refuse(dstd::StringView why) {
    return dstd::Unexpected<dstd::Error>(invalid_argument(why));
}

/// `out[r] = values[r] / 1000`, straight out of the column's buffers.
template <class T>
dstd::Result<void> to_ms_into(dstd::Span<const T> values,
                              dstd::Span<double> out, UdfBatch& batch) {
    for (std::int64_t r = 0; r < static_cast<std::int64_t>(values.size());
         ++r) {
        if (r % CHECK_EVERY == 0) {
            DFTRACER_UTILS_TRY_VOID(batch.check());
        }
        out[static_cast<std::size_t>(r)] =
            static_cast<double>(values[static_cast<std::size_t>(r)]) /
            MICROS_PER_MS;
    }
    return {};
}

/// One bit per row of `bits`, set where the value is above `limit`.
template <class T>
dstd::Result<void> mark_above(dstd::Span<const T> values, double limit,
                              dstd::Span<std::uint8_t> bits, UdfBatch& batch) {
    for (std::int64_t r = 0; r < static_cast<std::int64_t>(values.size());
         ++r) {
        if (r % CHECK_EVERY == 0) {
            DFTRACER_UTILS_TRY_VOID(batch.check());
        }
        if (static_cast<double>(values[static_cast<std::size_t>(r)]) > limit) {
            bits[static_cast<std::size_t>(r >> 3)] |=
                static_cast<std::uint8_t>(1u << (r & 7));
        }
    }
    return {};
}

}  // namespace

struct ToMs {
    static dstd::Result<dstd::UniquePtr<ToMs>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) {
            return refuse("to_ms takes a number column");
        }
        bind.result(dataframe::TypeId::Float64);
        return dstd::make_unique<ToMs>();
    }

    dstd::Result<Column> exec(UdfBatch& batch) const {
        const ColumnView values = batch.arg(0);
        const auto rows = static_cast<std::int64_t>(batch.rows());
        dstd::Vector<double> out(static_cast<std::size_t>(rows));
        const dstd::Span<double> into(out.data(), out.size());
        switch (values.type()) {
            case dataframe::TypeId::Int64:
                DFTRACER_UTILS_TRY_VOID(
                    to_ms_into(values.values<std::int64_t>(), into, batch));
                break;
            case dataframe::TypeId::Uint64:
                DFTRACER_UTILS_TRY_VOID(
                    to_ms_into(values.values<std::uint64_t>(), into, batch));
                break;
            case dataframe::TypeId::Float64:
                DFTRACER_UTILS_TRY_VOID(
                    to_ms_into(values.values<double>(), into, batch));
                break;
            default:
                return refuse("to_ms takes a number column");
        }
        Column column = Column::from_double(std::move(out));
        if (column.is_null()) return refuse("to_ms could not build its column");
        return column;
    }
};

struct IsSlow {
    double limit = 0.0;

    static dstd::Result<dstd::UniquePtr<IsSlow>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) {
            return refuse("is_slow takes a number column");
        }
        const dstd::Optional<ScalarView> limit = bind.constant(1);
        if (!limit || limit->is_str()) {
            return refuse("is_slow takes a constant number as its limit");
        }
        bind.result(dataframe::TypeId::Bool);
        return dstd::make_unique<IsSlow>(IsSlow{limit->f64()});
    }

    dstd::Result<Column> exec(UdfBatch& batch) const {
        const ColumnView values = batch.arg(0);
        const auto rows = static_cast<std::int64_t>(batch.rows());
        dstd::Vector<std::uint8_t> bytes(
            static_cast<std::size_t>((rows + 7) / 8), 0);
        const dstd::Span<std::uint8_t> bits(bytes.data(), bytes.size());
        switch (values.type()) {
            case dataframe::TypeId::Int64:
                DFTRACER_UTILS_TRY_VOID(mark_above(
                    values.values<std::int64_t>(), limit, bits, batch));
                break;
            case dataframe::TypeId::Uint64:
                DFTRACER_UTILS_TRY_VOID(mark_above(
                    values.values<std::uint64_t>(), limit, bits, batch));
                break;
            case dataframe::TypeId::Float64:
                DFTRACER_UTILS_TRY_VOID(
                    mark_above(values.values<double>(), limit, bits, batch));
                break;
            default:
                return refuse("is_slow takes a number column");
        }
        Column column = Column::from_bool(std::move(bytes), rows);
        if (column.is_null())
            return refuse("is_slow could not build its column");
        return column;
    }
};

DFTU_PLUGIN("duration_udfs", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.udf<ToMs>("to_ms");
                p.udf<IsSlow>("is_slow", UdfOptions{UdfFlag::None, 2, 2});
                return {};
            })
