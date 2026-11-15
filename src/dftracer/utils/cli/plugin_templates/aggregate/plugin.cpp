// SPDX-License-Identifier: MIT
// The @NAME@ aggregate plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

/// One value of a number column, whatever width it holds.
double number_at(const dftu_series* column, dftu_dtype type,
                 std::uint64_t row) {
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

}  // namespace

/// A user-defined aggregate keeps one state per group: update feeds a batch to
/// the states of its rows, combine merges two states, and finalize turns the
/// states into one column.
struct Total {
    struct State {
        double sum = 0;
    };
    dftu_dtype type;

    static dstd::Result<dstd::UniquePtr<Total>> bind(UdfBind& bind) {
        const dftu_dtype type = bind.type(0);
        if (type != DFTU_TYPE_INT64 && type != DFTU_TYPE_UINT64 &&
            type != DFTU_TYPE_FLOAT64)
            return dstd::Unexpected(
                make_error(DFTU_E_INVALID, "total takes a number column"));
        bind.result(DFTU_TYPE_FLOAT64);
        return dstd::make_unique<Total>(Total{type});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        for (std::uint64_t i = 0; i < states.size(); ++i) {
            State* state = states[i];
            if (!state) continue;
            state->sum += number_at(batch.arg(0), type, i);
        }
        return {};
    }

    dstd::Result<void> combine(State& into, const State& from) const {
        into.sum += from.sum;
        return {};
    }

    dstd::Result<dftu_series*> finalize(UdafStateSet<State>& states) const {
        dstd::Vector<double> out(states.size());
        for (std::size_t i = 0; i < states.size(); ++i)
            out[i] = states[i] ? states[i]->sum : 0.0;
        Column column = Column::from_double(std::move(out));
        if (!column)
            return dstd::Unexpected(
                make_error(DFTU_E_INVALID, "total could not build a column"));
        return column.release();
    }
};

DFTU_PLUGIN_INFO("@NAME@", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config)
        .udaf<Total>("@NAME@.total", UdafOptions{DFTU_AGG_PARALLEL_SAFE, 1, 1})
        .build(out);
}
