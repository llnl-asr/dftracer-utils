// SPDX-License-Identifier: MIT
// The @NAME@ udf plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// A scalar UDF runs on whole columns, never row by row. A row with a null
/// argument gives a null result without the UDF running for it.
struct Doubled {
    dftu_dtype type;

    /// Called once per query with the argument types: check them and declare
    /// the result type.
    static dstd::Result<dstd::UniquePtr<Doubled>> bind(UdfBind& bind) {
        const dftu_dtype type = bind.type(0);
        if (type != DFTU_TYPE_INT64 && type != DFTU_TYPE_UINT64 &&
            type != DFTU_TYPE_FLOAT64)
            return dstd::Unexpected(
                make_error(DFTU_E_INVALID, "doubled takes a number column"));
        bind.result(DFTU_TYPE_FLOAT64);
        return dstd::make_unique<Doubled>(Doubled{type});
    }

    dstd::Result<dftu_series*> exec(UdfBatch& batch) const {
        const auto rows = static_cast<std::int64_t>(batch.rows());
        dstd::Vector<double> out(static_cast<std::size_t>(rows));
        const void* data = dftu_series_data(batch.arg(0));
        for (std::int64_t r = 0; r < rows; ++r) {
            double value = 0;
            if (type == DFTU_TYPE_INT64)
                value = static_cast<double>(
                    static_cast<const std::int64_t*>(data)[r]);
            else if (type == DFTU_TYPE_UINT64)
                value = static_cast<double>(
                    static_cast<const std::uint64_t*>(data)[r]);
            else
                value = static_cast<const double*>(data)[r];
            out[static_cast<std::size_t>(r)] = 2 * value;
        }
        dftu_series* column =
            dftu_series_new_flat(DFTU_TYPE_FLOAT64, out.data(), rows, nullptr);
        if (!column)
            return dstd::Unexpected(
                make_error(DFTU_E_INVALID, "doubled could not build a column"));
        return column;
    }
};

DFTU_PLUGIN_INFO("@NAME@", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config).udf<Doubled>("@NAME@.doubled").build(out);
}
