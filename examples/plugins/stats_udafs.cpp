// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: two user-defined aggregates, one state per
// group.
//
//   geomean(x)  the geometric mean of the positive values; a POD state
//   p50(x)      the median; a state that owns a vector, so it has a serialize
//               pair for spills and merges
//
// A UDAF runs on whole columns: update feeds a batch to the states of its
// rows, and a row with a null argument has a null state and is skipped.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o stats_udafs.so
// stats_udafs.cpp

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

bool is_number(dftu_dtype type) {
    return type == DFTU_TYPE_INT64 || type == DFTU_TYPE_UINT64 ||
           type == DFTU_TYPE_FLOAT64;
}

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

dstd::Unexpected<dstd::Error> refuse(const char* why) {
    return dstd::Unexpected(make_error(DFTU_E_INVALID, why));
}

template <class Make>
dstd::Result<dftu_series*> float_column(std::uint64_t rows, Make value) {
    dstd::Vector<double> out(static_cast<std::size_t>(rows));
    for (std::uint64_t i = 0; i < rows; ++i)
        out[static_cast<std::size_t>(i)] = value(i);
    Column column = Column::from_double(std::move(out));
    if (!column) return refuse("could not build the result column");
    return column.release();
}

}  // namespace

struct Geomean {
    struct State {
        double log_sum = 0;
        std::int64_t count = 0;
    };
    dftu_dtype type;

    static dstd::Result<dstd::UniquePtr<Geomean>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) return refuse("geomean takes a number");
        bind.result(DFTU_TYPE_FLOAT64);
        return dstd::make_unique<Geomean>(Geomean{bind.type(0)});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        for (std::uint64_t i = 0; i < states.size(); ++i) {
            State* s = states[i];
            const double x = s ? number_at(batch.arg(0), type, i) : 0;
            if (s && x > 0) {
                s->log_sum += std::log(x);
                ++s->count;
            }
        }
        return {};
    }

    dstd::Result<void> combine(State& into, const State& from) const {
        into.log_sum += from.log_sum;
        into.count += from.count;
        return {};
    }

    dstd::Result<dftu_series*> finalize(UdafStateSet<State>& states) const {
        return float_column(states.size(), [&](std::uint64_t i) {
            const State& s = *states[i];
            return s.count ? std::exp(s.log_sum / static_cast<double>(s.count))
                           : 0.0;
        });
    }
};

struct P50 {
    struct State {
        dstd::Vector<double> values;
    };
    dftu_dtype type;

    static dstd::Result<dstd::UniquePtr<P50>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) return refuse("p50 takes a number");
        bind.result(DFTU_TYPE_FLOAT64);
        return dstd::make_unique<P50>(P50{bind.type(0)});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        for (std::uint64_t i = 0; i < states.size(); ++i)
            if (State* s = states[i])
                s->values.push_back(number_at(batch.arg(0), type, i));
        return {};
    }

    dstd::Result<void> combine(State& into, const State& from) const {
        into.values.insert(into.values.end(), from.values.begin(),
                           from.values.end());
        return {};
    }

    dstd::Result<dftu_series*> finalize(UdafStateSet<State>& states) const {
        return float_column(states.size(), [&](std::uint64_t i) {
            dstd::Vector<double>& v = states[i]->values;
            if (v.empty()) return 0.0;
            auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
            std::nth_element(v.begin(), mid, v.end());
            return *mid;
        });
    }

    void serialize(const State& s, dstd::String& out) const {
        out.append(reinterpret_cast<const char*>(s.values.data()),
                   s.values.size() * sizeof(double));
    }

    dstd::Result<void> deserialize(State& s, dstd::StringView in) const {
        if (in.size() % sizeof(double) != 0)
            return refuse("p50 state bytes are not whole doubles");
        const std::size_t n = in.size() / sizeof(double);
        s.values.resize(n);
        if (n) std::memcpy(s.values.data(), in.data(), in.size());
        return {};
    }
};

DFTU_PLUGIN_INFO("stats_udafs", "1.0")

extern "C" DFTU_PLUGIN_EXPORT dftu_status
dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h, const dftu_value* config,
                dftu_plugin** out) {
    return plugin(ctx, h, config)
        .udaf<Geomean>("geomean", UdafOptions{DFTU_AGG_PARALLEL_SAFE, 1, 1})
        .udaf<P50>("p50", UdafOptions{DFTU_AGG_PARALLEL_SAFE, 1, 1})
        .build(out);
}
