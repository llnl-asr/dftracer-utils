// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: two user-defined aggregates, one state per
// group.
//
//   geomean(x)  the geometric mean of the positive values; a POD state
//   p50(x)      the median; a state that owns a vector, so it has a serialize
//               pair for spills and merges
//
// A UDAF runs on whole columns: update feeds a batch to the states of its
// rows, and a row with a null argument has a null state and is skipped. The
// argument arrives as a borrowed column view, so a read copies nothing, and
// finalize gives a Column the host takes over.
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o stats_udafs.so
// stats_udafs.cpp

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
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

bool is_number(dataframe::TypeId type) {
    return type == dataframe::TypeId::Int64 ||
           type == dataframe::TypeId::Uint64 ||
           type == dataframe::TypeId::Float64;
}

dstd::Unexpected<dstd::Error> refuse(dstd::StringView why) {
    return dstd::Unexpected<dstd::Error>(invalid_argument(why));
}

/// A number column resolved once per batch: the span of the type its values
/// have, picked by one switch, so a read is a load and never a call.
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

template <class Make>
dstd::Result<Column> float_column(std::uint64_t rows, Make value) {
    dstd::Vector<double> out(static_cast<std::size_t>(rows));
    for (std::uint64_t i = 0; i < rows; ++i)
        out[static_cast<std::size_t>(i)] = value(i);
    Column column = Column::from_double(std::move(out));
    if (column.is_null()) return refuse("could not build the result column");
    return column;
}

}  // namespace

struct Geomean {
    struct State {
        double log_sum = 0;
        std::int64_t count = 0;
    };
    dataframe::TypeId type = dataframe::TypeId::Unknown;

    static dstd::Result<dstd::UniquePtr<Geomean>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) return refuse("geomean takes a number");
        bind.result(dataframe::TypeId::Float64);
        return dstd::make_unique<Geomean>(Geomean{bind.type(0)});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        const Numbers values(batch.arg(0));
        for (std::uint64_t i = 0; i < states.size(); ++i) {
            State* s = states[i];
            if (!s) continue;
            const double x = values[i];
            if (x > 0) {
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

    dstd::Result<Column> finalize(UdafStateSet<State>& states) const {
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
    dataframe::TypeId type = dataframe::TypeId::Unknown;

    static dstd::Result<dstd::UniquePtr<P50>> bind(UdfBind& bind) {
        if (!is_number(bind.type(0))) return refuse("p50 takes a number");
        bind.result(dataframe::TypeId::Float64);
        return dstd::make_unique<P50>(P50{bind.type(0)});
    }

    dstd::Result<void> update(UdafStateSet<State>& states,
                              const UdfBatch& batch) const {
        const Numbers values(batch.arg(0));
        for (std::uint64_t i = 0; i < states.size(); ++i)
            if (State* s = states[i]) s->values.push_back(values[i]);
        return {};
    }

    dstd::Result<void> combine(State& into, const State& from) const {
        into.values.insert(into.values.end(), from.values.begin(),
                           from.values.end());
        return {};
    }

    dstd::Result<Column> finalize(UdafStateSet<State>& states) const {
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

DFTU_PLUGIN("stats_udafs", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.udaf<Geomean>("geomean",
                                UdafOptions{UdafFlag::ParallelSafe, 1, 1});
                p.udaf<P50>("p50", UdafOptions{UdafFlag::ParallelSafe, 1, 1});
                return {};
            })
