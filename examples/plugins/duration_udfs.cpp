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
// gives a null result without the UDF doing anything for it.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o duration_udfs.so
// duration_udfs.cpp

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t CHECK_EVERY = 4096;

bool is_number(dftu_dtype type) {
    return type == DFTU_TYPE_INT64 || type == DFTU_TYPE_UINT64 ||
           type == DFTU_TYPE_FLOAT64;
}

double number_at(const dftu_series* column, dftu_dtype type, std::int64_t row) {
    const void* data = dftu_series_data(column);
    switch (type) {
        case DFTU_TYPE_INT64:
            return static_cast<double>(
                static_cast<const std::int64_t*>(data)[row]);
        case DFTU_TYPE_UINT64:
            return static_cast<double>(
                static_cast<const std::uint64_t*>(data)[row]);
        default:
            return static_cast<const double*>(data)[row];
    }
}

dstd::Unexpected<dstd::Error> refuse(const char* why) {
    return dstd::Unexpected(make_error(DFTU_E_INVALID, why));
}

}  // namespace

struct ToMs {
    dftu_dtype type;

    static dstd::Result<dstd::UniquePtr<ToMs>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0)))
            return refuse("to_ms takes a number column");
        bind.result(DFTU_TYPE_FLOAT64);
        return dstd::make_unique<ToMs>(ToMs{bind.type(0)});
    }

    dstd::Result<dftu_series*> exec(UdfBatch& batch) const {
        const auto rows = static_cast<std::int64_t>(batch.rows());
        dstd::Vector<double> out(static_cast<std::size_t>(rows));
        for (std::int64_t r = 0; r < rows; ++r) {
            if (r % CHECK_EVERY == 0) {
                DFTRACER_UTILS_TRY_VOID(batch.check());
            }
            out[static_cast<std::size_t>(r)] =
                number_at(batch.arg(0), type, r) / 1000.0;
        }
        dftu_series* column =
            dftu_series_new_flat(DFTU_TYPE_FLOAT64, out.data(), rows, nullptr);
        if (!column) return refuse("to_ms could not build its column");
        return column;
    }
};

struct IsSlow {
    dftu_dtype type;
    double limit;

    static dstd::Result<dstd::UniquePtr<IsSlow>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0)))
            return refuse("is_slow takes a number column");
        const dftu_scalar* limit = bind.constant(1);
        if (!limit || limit->kind == DFTU_SCALAR_TAG_STR)
            return refuse("is_slow takes a constant number as its limit");
        const double value = limit->kind == DFTU_SCALAR_TAG_F64 ? limit->value.d
                             : limit->kind == DFTU_SCALAR_TAG_U64
                                 ? static_cast<double>(limit->value.u)
                                 : static_cast<double>(limit->value.i);
        bind.result(DFTU_TYPE_BOOL);
        return dstd::make_unique<IsSlow>(IsSlow{bind.type(0), value});
    }

    dstd::Result<dftu_series*> exec(UdfBatch& batch) const {
        const auto rows = static_cast<std::int64_t>(batch.rows());
        dstd::Vector<std::uint8_t> bits(
            static_cast<std::size_t>((rows + 7) / 8), 0);
        for (std::int64_t r = 0; r < rows; ++r) {
            if (r % CHECK_EVERY == 0) {
                DFTRACER_UTILS_TRY_VOID(batch.check());
            }
            if (number_at(batch.arg(0), type, r) > limit)
                bits[static_cast<std::size_t>(r >> 3)] |=
                    static_cast<std::uint8_t>(1u << (r & 7));
        }
        dftu_series* column =
            dftu_series_new_flat(DFTU_TYPE_BOOL, bits.data(), rows, nullptr);
        if (!column) return refuse("is_slow could not build its column");
        return column;
    }
};

DFTU_PLUGIN_INFO("duration_udfs", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config)
        .udf<ToMs>("to_ms")
        .udf<IsSlow>("is_slow", UdfOptions{0, 2, 2})
        .build(out);
}
