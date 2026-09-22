#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_TEMPORAL_ARITH_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_TEMPORAL_ARITH_H

#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

enum class ArithOp : std::int32_t { Add, Sub, Mul, Div };

/// Nanoseconds per tick of `unit`, used to compare and rescale between
/// TimeUnit granularities.
constexpr std::int64_t nanos_per_unit(TimeUnit unit) noexcept {
    switch (unit) {
        case TimeUnit::Second:
            return 1'000'000'000LL;
        case TimeUnit::Milli:
            return 1'000'000LL;
        case TimeUnit::Micro:
            return 1'000LL;
        case TimeUnit::Nano:
            return 1LL;
    }
    return 1LL;
}

/// The finer (higher-resolution, smaller tick) of `a` and `b`.
constexpr TimeUnit finer_unit(TimeUnit a, TimeUnit b) noexcept {
    return nanos_per_unit(a) <= nanos_per_unit(b) ? a : b;
}

/// The result type of a temporal column-column arithmetic op.
struct TemporalResult {
    TypeId id;
    TimeUnit unit;
    /// Timestamp only; empty means naive (no timezone).
    std::string timezone;
};

/// Result of a Timestamp/Duration/Time64 column-column arithmetic op; see
/// the Timestamp/Duration/Time64 TypeId docs (types.h) for the full rule
/// table. The result unit is the finer of the two operand units - the
/// coarser side rescales up exactly, never down (which would truncate).
/// `Timestamp - Timestamp` additionally refuses a timezone mismatch,
/// including naive-vs-aware, rather than guessing which one wins.
/// Date32/Date64/Time32 operands are always refused: Date32/64 have no
/// TimeUnit to reconcile against a Duration tick, and Time32's Int32
/// physical storage would need cross-width widening this pass does not
/// build.
inline std::optional<TemporalResult> temporal_binop_result(
    TypeId a_id, TimeUnit a_unit, const std::string& a_tz, TypeId b_id,
    TimeUnit b_unit, const std::string& b_tz, ArithOp op) {
    auto is_ts = [](TypeId t) { return t == TypeId::Timestamp; };
    auto is_dur = [](TypeId t) { return t == TypeId::Duration; };
    auto is_time64 = [](TypeId t) { return t == TypeId::Time64; };

    if (!((is_ts(a_id) || is_dur(a_id) || is_time64(a_id)) &&
          (is_ts(b_id) || is_dur(b_id) || is_time64(b_id))))
        return std::nullopt;

    const TimeUnit unit = finer_unit(a_unit, b_unit);

    if (op == ArithOp::Add) {
        if (is_ts(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Timestamp, unit, a_tz};
        if (is_dur(a_id) && is_ts(b_id))
            return TemporalResult{TypeId::Timestamp, unit, b_tz};
        if (is_dur(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Duration, unit, ""};
        if (is_time64(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Time64, unit, ""};
        if (is_dur(a_id) && is_time64(b_id))
            return TemporalResult{TypeId::Time64, unit, ""};
        return std::nullopt;
    }
    if (op == ArithOp::Sub) {
        if (is_ts(a_id) && is_ts(b_id)) {
            if (a_tz != b_tz) return std::nullopt;
            return TemporalResult{TypeId::Duration, unit, ""};
        }
        if (is_ts(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Timestamp, unit, a_tz};
        if (is_time64(a_id) && is_time64(b_id))
            return TemporalResult{TypeId::Duration, unit, ""};
        if (is_time64(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Time64, unit, ""};
        if (is_dur(a_id) && is_dur(b_id))
            return TemporalResult{TypeId::Duration, unit, ""};
        return std::nullopt;
    }
    return std::nullopt;
}

/// Result of a Duration-by-scalar Mul/Div (the only defined temporal-scalar
/// op): the unit is unchanged, since scaling a tick count by a dimensionless
/// number does not change what a tick means. Timestamp and Time64 stay
/// refused for every scalar op (Add/Sub included): a bare scalar carries no
/// unit, so "timestamp + 5" cannot mean anything without guessing one.
inline std::optional<TemporalResult> temporal_scalarop_result(TypeId a_id,
                                                              TimeUnit a_unit,
                                                              ArithOp op) {
    if (a_id == TypeId::Duration && (op == ArithOp::Mul || op == ArithOp::Div))
        return TemporalResult{TypeId::Duration, a_unit, ""};
    return std::nullopt;
}

/// Rescales `n` int64 ticks in `from` up to the finer `to` (multiplying by
/// the exact integer ratio between the two granularities), checked for
/// int64 overflow. Only called when `from != to`, coarse-to-fine (see
/// finer_unit): the caller never asks this to divide, which would truncate.
inline std::optional<std::vector<std::int64_t>> rescale_ticks_up(
    const std::int64_t* p, std::size_t n, TimeUnit from, TimeUnit to) {
    const std::int64_t ratio = nanos_per_unit(from) / nanos_per_unit(to);
    std::vector<std::int64_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::int64_t r;
        if (__builtin_mul_overflow(p[i], ratio, &r)) return std::nullopt;
        out[i] = r;
    }
    return out;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_TEMPORAL_ARITH_H
