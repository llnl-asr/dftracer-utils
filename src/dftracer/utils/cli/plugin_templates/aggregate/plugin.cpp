// SPDX-License-Identifier: MIT
// The @NAME@ aggregate plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

/// A number column, whatever width it holds: the span of its type is picked
/// once per batch, so a read is a load and never a call.
struct Numbers {
    dataframe::TypeId type = dataframe::TypeId::Unknown;
    dstd::Span<const std::int64_t> i64;
    dstd::Span<const std::uint64_t> u64;
    dstd::Span<const double> f64;

    explicit Numbers(const ColumnView& column) : type(column.type()) {
        switch (type) {
            case dataframe::TypeId::Int64:
                i64 = column.values<std::int64_t>();
                break;
            case dataframe::TypeId::Uint64:
                u64 = column.values<std::uint64_t>();
                break;
            case dataframe::TypeId::Float64:
                f64 = column.values<double>();
                break;
            default:
                break;
        }
    }

    double operator[](std::uint64_t row) const noexcept {
        const auto r = static_cast<std::size_t>(row);
        if (type == dataframe::TypeId::Int64)
            return static_cast<double>(i64[r]);
        if (type == dataframe::TypeId::Uint64)
            return static_cast<double>(u64[r]);
        return f64[r];
    }
};

}  // namespace

/// A user-defined aggregate keeps one state per group: update feeds a batch to
/// the states of its rows, combine merges two states, and finalize turns the
/// states into one column.
struct Total {
    struct State {
        double sum = 0;
    };
    dataframe::TypeId type = dataframe::TypeId::Unknown;

    static dstd::Result<dstd::UniquePtr<Total>> bind(UdfBind& bind) {
        const dataframe::TypeId type = bind.type(0);
        if (type != dataframe::TypeId::Int64 &&
            type != dataframe::TypeId::Uint64 &&
            type != dataframe::TypeId::Float64)
            return dstd::Unexpected<dstd::Error>(
                invalid_argument("total takes a number column"));
        bind.result(dataframe::TypeId::Float64);
        return dstd::make_unique<Total>(Total{type});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        const Numbers values(batch.arg(0));
        for (std::uint64_t i = 0; i < states.size(); ++i) {
            State* state = states[i];
            if (!state) continue;
            state->sum += values[i];
        }
        return {};
    }

    dstd::Result<void> combine(State& into, const State& from) const {
        into.sum += from.sum;
        return {};
    }

    dstd::Result<Column> finalize(UdafStateSet<State>& states) const {
        dstd::Vector<double> out(states.size());
        for (std::size_t i = 0; i < states.size(); ++i)
            out[i] = states[i] ? states[i]->sum : 0.0;
        Column column = Column::from_double(std::move(out));
        if (column.is_null())
            return dstd::Unexpected<dstd::Error>(
                invalid_argument("total could not build a column"));
        return column;
    }
};

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.udaf<Total>("@NAME@.total",
                              UdafOptions{UdafFlag::ParallelSafe, 1, 1});
                return {};
            })
