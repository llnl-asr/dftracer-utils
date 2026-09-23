#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H

#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/query/builder.h>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::plugins {

// Re-export the query builder so a plugin writes `F("dur") > 1000` with only
// this header and no query:: qualifier.
using dftracer::utils::query::Expr;
using dftracer::utils::query::F;
using dftracer::utils::query::Field;
using dftracer::utils::query::resolved;

/// Scoped mirror of dftu_phase for Event::phase; each enumerator is its ABI
/// constant, so static_cast<dftu_phase> recovers the raw value.
enum class Phase : std::int32_t {
    Unknown = DFTU_PH_UNKNOWN,
    Complete = DFTU_PH_COMPLETE,
    Counter = DFTU_PH_COUNTER,
    Aggregated = DFTU_PH_AGGREGATED,
    Metadata = DFTU_PH_METADATA
};
static_assert(static_cast<dftu_phase>(Phase::Unknown) == DFTU_PH_UNKNOWN);
static_assert(static_cast<dftu_phase>(Phase::Complete) == DFTU_PH_COMPLETE);
static_assert(static_cast<dftu_phase>(Phase::Counter) == DFTU_PH_COUNTER);
static_assert(static_cast<dftu_phase>(Phase::Aggregated) == DFTU_PH_AGGREGATED);
static_assert(static_cast<dftu_phase>(Phase::Metadata) == DFTU_PH_METADATA);

/// Scoped mirror of dftu_log_level for Host::log; each enumerator is its ABI
/// constant, so static_cast<dftu_log_level> recovers the raw value.
enum class LogLevel : std::int32_t {
    Trace = DFTU_LOG_TRACE,
    Debug = DFTU_LOG_DEBUG,
    Info = DFTU_LOG_INFO,
    Warn = DFTU_LOG_WARN,
    Error = DFTU_LOG_ERROR
};
static_assert(static_cast<dftu_log_level>(LogLevel::Trace) == DFTU_LOG_TRACE);
static_assert(static_cast<dftu_log_level>(LogLevel::Debug) == DFTU_LOG_DEBUG);
static_assert(static_cast<dftu_log_level>(LogLevel::Info) == DFTU_LOG_INFO);
static_assert(static_cast<dftu_log_level>(LogLevel::Warn) == DFTU_LOG_WARN);
static_assert(static_cast<dftu_log_level>(LogLevel::Error) == DFTU_LOG_ERROR);

/// Scoped mirror of dftu_value_kind for ConfigValue::kind; each enumerator is
/// its ABI constant, so static_cast<dftu_value_kind> recovers the raw value.
enum class ValueKind : std::int32_t {
    Null = DFTU_VAL_NULL,
    Bool = DFTU_VAL_BOOL,
    I64 = DFTU_VAL_I64,
    F64 = DFTU_VAL_F64,
    Str = DFTU_VAL_STR,
    Array = DFTU_VAL_ARRAY,
    Object = DFTU_VAL_OBJECT
};
static_assert(static_cast<dftu_value_kind>(ValueKind::Null) == DFTU_VAL_NULL);
static_assert(static_cast<dftu_value_kind>(ValueKind::Bool) == DFTU_VAL_BOOL);
static_assert(static_cast<dftu_value_kind>(ValueKind::I64) == DFTU_VAL_I64);
static_assert(static_cast<dftu_value_kind>(ValueKind::F64) == DFTU_VAL_F64);
static_assert(static_cast<dftu_value_kind>(ValueKind::Str) == DFTU_VAL_STR);
static_assert(static_cast<dftu_value_kind>(ValueKind::Array) == DFTU_VAL_ARRAY);
static_assert(static_cast<dftu_value_kind>(ValueKind::Object) ==
              DFTU_VAL_OBJECT);

/// Scoped mirror of dftu_agg_op for AggCol/Host::agg; each enumerator is its
/// DFTU_AGG_* constant, so static_cast<dftu_agg_op> recovers the raw value.
/// See abi.h/dftu_agg_col for each op's semantics and which of
/// value/out/param/by it consumes.
enum class AggOp : std::int32_t {
    Count = DFTU_AGG_COUNT,
    Sum = DFTU_AGG_SUM,
    Min = DFTU_AGG_MIN,
    Max = DFTU_AGG_MAX,
    Mean = DFTU_AGG_MEAN,
    Var = DFTU_AGG_VAR,
    Std = DFTU_AGG_STD,
    Skew = DFTU_AGG_SKEW,
    Kurt = DFTU_AGG_KURT,
    First = DFTU_AGG_FIRST,
    Last = DFTU_AGG_LAST,
    Pct = DFTU_AGG_PCT,
    Hist = DFTU_AGG_HIST,
    Argmax = DFTU_AGG_ARGMAX,
    Sumsq = DFTU_AGG_SUMSQ,
    SetUnion = DFTU_AGG_SET_UNION,
    Busy = DFTU_AGG_BUSY,
    Concurrency = DFTU_AGG_CONCURRENCY,
    Utilization = DFTU_AGG_UTILIZATION,
    Active = DFTU_AGG_ACTIVE,
    CountValid = DFTU_AGG_COUNT_VALID
};
static_assert(static_cast<dftu_agg_op>(AggOp::Count) == DFTU_AGG_COUNT);
static_assert(static_cast<dftu_agg_op>(AggOp::Sum) == DFTU_AGG_SUM);
static_assert(static_cast<dftu_agg_op>(AggOp::Min) == DFTU_AGG_MIN);
static_assert(static_cast<dftu_agg_op>(AggOp::Max) == DFTU_AGG_MAX);
static_assert(static_cast<dftu_agg_op>(AggOp::Mean) == DFTU_AGG_MEAN);
static_assert(static_cast<dftu_agg_op>(AggOp::Var) == DFTU_AGG_VAR);
static_assert(static_cast<dftu_agg_op>(AggOp::Std) == DFTU_AGG_STD);
static_assert(static_cast<dftu_agg_op>(AggOp::Skew) == DFTU_AGG_SKEW);
static_assert(static_cast<dftu_agg_op>(AggOp::Kurt) == DFTU_AGG_KURT);
static_assert(static_cast<dftu_agg_op>(AggOp::First) == DFTU_AGG_FIRST);
static_assert(static_cast<dftu_agg_op>(AggOp::Last) == DFTU_AGG_LAST);
static_assert(static_cast<dftu_agg_op>(AggOp::Pct) == DFTU_AGG_PCT);
static_assert(static_cast<dftu_agg_op>(AggOp::Hist) == DFTU_AGG_HIST);
static_assert(static_cast<dftu_agg_op>(AggOp::Argmax) == DFTU_AGG_ARGMAX);
static_assert(static_cast<dftu_agg_op>(AggOp::Sumsq) == DFTU_AGG_SUMSQ);
static_assert(static_cast<dftu_agg_op>(AggOp::SetUnion) == DFTU_AGG_SET_UNION);
static_assert(static_cast<dftu_agg_op>(AggOp::Busy) == DFTU_AGG_BUSY);
static_assert(static_cast<dftu_agg_op>(AggOp::Concurrency) ==
              DFTU_AGG_CONCURRENCY);
static_assert(static_cast<dftu_agg_op>(AggOp::Utilization) ==
              DFTU_AGG_UTILIZATION);
static_assert(static_cast<dftu_agg_op>(AggOp::Active) == DFTU_AGG_ACTIVE);
static_assert(static_cast<dftu_agg_op>(AggOp::CountValid) ==
              DFTU_AGG_COUNT_VALID);

/// Fluent builder for one dftu_agg_col spec passed to Host::agg. All names
/// (value/out/by) are borrowed const char* held only for the agg_new call, per
/// the SDK's borrowed-name convention; they must outlive that call.
class AggCol {
   public:
    explicit AggCol(AggOp op) noexcept : op_(op) {}

    AggCol& value(const char* v) noexcept {
        value_ = v;
        return *this;
    }
    AggCol& out(const char* o) noexcept {
        out_ = o;
        return *this;
    }
    AggCol& param(double p) noexcept {
        param_ = p;
        return *this;
    }
    AggCol& by(const char* b) noexcept {
        by_ = b;
        return *this;
    }

    dftu_agg_col raw() const noexcept {
        return dftu_agg_col{static_cast<std::int32_t>(op_), value_, out_,
                            param_, by_};
    }

   private:
    AggOp op_;
    const char* value_ = nullptr;
    const char* out_ = nullptr;
    double param_ = 0.0;
    const char* by_ = nullptr;
};

/// Per-op AggCol factories: each function's signature takes exactly the
/// fields its DFTU_AGG_* op consumes (see dftu_agg_col in abi.h), so a
/// mismatched param combination (e.g. a quantile on sum()) fails to compile
/// instead of silently building an AggCol with an unused or missing field.
namespace agg {

/// Group row count; no value column.
inline AggCol count(const char* out) noexcept {
    return AggCol(AggOp::Count).out(out);
}

/// Count of non-null values in `value` (Int64), distinct from count()'s row
/// count.
inline AggCol count_valid(const char* value, const char* out) noexcept {
    return AggCol(AggOp::CountValid).value(value).out(out);
}

inline AggCol sum(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Sum).value(value).out(out);
}
inline AggCol min(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Min).value(value).out(out);
}
inline AggCol max(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Max).value(value).out(out);
}
inline AggCol mean(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Mean).value(value).out(out);
}
/// Sample variance.
inline AggCol var(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Var).value(value).out(out);
}
/// Sample standard deviation.
inline AggCol std_dev(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Std).value(value).out(out);
}
/// Population skewness.
inline AggCol skew(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Skew).value(value).out(out);
}
/// Excess (population) kurtosis.
inline AggCol kurt(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Kurt).value(value).out(out);
}
/// First non-null value in row order (order-independent merge).
inline AggCol first(const char* value, const char* out) noexcept {
    return AggCol(AggOp::First).value(value).out(out);
}
/// Last non-null value in row order.
inline AggCol last(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Last).value(value).out(out);
}
inline AggCol sumsq(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Sumsq).value(value).out(out);
}
/// Distinct String values of `value`, sorted and joined.
inline AggCol set_union(const char* value, const char* out) noexcept {
    return AggCol(AggOp::SetUnion).value(value).out(out);
}
/// The DDSketch histogram over `value`: a list<struct{lo,hi,count}> column of
/// the sketch's occupied bins (no bin-count parameter).
inline AggCol hist(const char* value, const char* out) noexcept {
    return AggCol(AggOp::Hist).value(value).out(out);
}

/// A DDSketch quantile of `value`; `q` is the level in [0, 1].
inline AggCol pct(const char* value, const char* out, double q) noexcept {
    return AggCol(AggOp::Pct).value(value).out(out).param(q);
}

/// The String repr of `value` at the row maximizing `by`.
inline AggCol argmax(const char* value, const char* out,
                     const char* by) noexcept {
    return AggCol(AggOp::Argmax).value(value).out(out).by(by);
}

/// Occupancy reductions over a (ts, dur) interval pair per group: `ts` is the
/// event start column, `dur` its duration column, and `occ_cell_us` an
/// optional endpoint-snap tolerance (0 = exact). Busy is the exact
/// interval-union length (us) where depth > 0.
inline AggCol busy(const char* ts, const char* dur, const char* out,
                   double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Busy).value(ts).by(dur).out(out).param(occ_cell_us);
}
/// sum(dur) / busy over the same (ts, dur) window; see busy().
inline AggCol concurrency(const char* ts, const char* dur, const char* out,
                          double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Concurrency)
        .value(ts)
        .by(dur)
        .out(out)
        .param(occ_cell_us);
}
/// busy / (max_end - min_ts) over the same (ts, dur) window; see busy().
inline AggCol utilization(const char* ts, const char* dur, const char* out,
                          double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Utilization)
        .value(ts)
        .by(dur)
        .out(out)
        .param(occ_cell_us);
}
/// Peak overlap depth over the same (ts, dur) window; see busy().
inline AggCol active(const char* ts, const char* dur, const char* out,
                     double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Active).value(ts).by(dur).out(out).param(occ_cell_us);
}

}  // namespace agg

/// Opaque strong handle for an interned-string id (the C ABI's dftu_str, which
/// is an id, not bytes). Compare and hash by identity; resolve to bytes with
/// Host::str. absent() is the DFTU_STR_NONE sentinel. raw() exposes the
/// underlying id only to cross the C ABI seam - it is never an integer to a
/// plugin author.
class StrId {
   public:
    StrId() = default;
    constexpr explicit StrId(dftu_str id) noexcept : id_(id) {}
    constexpr dftu_str raw() const noexcept { return id_; }
    constexpr bool absent() const noexcept { return id_ == DFTU_STR_NONE; }
    constexpr bool operator==(StrId o) const noexcept { return id_ == o.id_; }
    constexpr bool operator!=(StrId o) const noexcept { return id_ != o.id_; }

   private:
    dftu_str id_ = DFTU_STR_NONE;
};

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H */
