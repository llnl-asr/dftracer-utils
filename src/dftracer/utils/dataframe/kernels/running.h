#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_RUNNING_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_RUNNING_H

// The per-row steps of the window functions that walk a partition in order:
// running sum, product and extreme, delta, lag, lead and forward fill. One
// copy of the arithmetic, its overflow checks and its order, used by the window
// op, the native group-wise transform and the lazy window stream.
//
// Every step is a small state struct fed one row at a time: no virtual call,
// no allocation per row. A null is not fed to a running step (the running
// functions skip nulls); the caller decides what a null row outputs.

#include <dftracer/utils/core/common/int128.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe::steps {

using Wide = ::dftracer::utils::int128_t;

[[noreturn]] inline void overflow(const char* func, const char* type) {
    throw std::overflow_error(std::string("window: ") + func + " overflows " +
                              type);
}

inline std::int64_t to_int64(Wide v, const char* func) {
    if (v < std::numeric_limits<std::int64_t>::min() ||
        v > std::numeric_limits<std::int64_t>::max())
        overflow(func, "int64");
    return static_cast<std::int64_t>(v);
}

inline std::uint64_t to_uint64(Wide v, const char* func) {
    if (v < 0 ||
        v > static_cast<Wide>(std::numeric_limits<std::uint64_t>::max()))
        overflow(func, "uint64");
    return static_cast<std::uint64_t>(v);
}

/// RUNNING_SUM of integers: one checked add. The narrow unsigned types sum as
/// int64, so only a 64-bit unsigned column uses SumU64.
struct SumI64 {
    std::int64_t acc = 0;
    void add(std::int64_t v) {
        if (__builtin_add_overflow(acc, v, &acc))
            overflow("RUNNING_SUM", "int64");
    }
};
struct SumU64 {
    std::uint64_t acc = 0;
    void add(std::uint64_t v) {
        if (__builtin_add_overflow(acc, v, &acc))
            overflow("RUNNING_SUM", "uint64");
    }
};
struct SumF64 {
    double acc = 0.0;
    void add(double v) { acc += v; }
};

/// RUNNING_PROD.
struct Prod {
    double acc = 1.0;
    void mul(double v) { acc *= v; }
};

/// RUNNING_MIN and RUNNING_MAX: the extreme value so far. The first of equal
/// values wins; a float is ordered by compare_doubles (NaN greatest). The value
/// is read as the type of the column, so one state is fed only through one of
/// offer_i, offer_u and offer_f.
struct Extreme {
    bool has = false;
    union Value {
        std::int64_t i = 0;
        std::uint64_t u;
        double d;
    } value{};

    static bool wins(int order, bool smallest) {
        return smallest ? order < 0 : order > 0;
    }
    /// True when `v` became the extreme.
    bool offer_i(std::int64_t v, bool smallest) {
        if (has && !wins(v < value.i ? -1 : (v > value.i ? 1 : 0), smallest))
            return false;
        value.i = v;
        has = true;
        return true;
    }
    bool offer_u(std::uint64_t v, bool smallest) {
        if (has && !wins(v < value.u ? -1 : (v > value.u ? 1 : 0), smallest))
            return false;
        value.u = v;
        has = true;
        return true;
    }
    bool offer_f(double v, bool smallest) {
        if (has && !wins(compare_doubles(v, value.d), smallest)) return false;
        value.d = v;
        has = true;
        return true;
    }
};

/// The sum of a frame that rows enter and leave, in the exact integers the
/// FRAME_SUM and FRAME_MEAN kernels use: a 128-bit sum that the caller turns
/// into int64 or uint64 once per output (an error past the type).
struct FrameSumInt {
    Wide sum = 0;
    void enter_i(std::int64_t v) { sum += static_cast<Wide>(v); }
    void enter_u(std::uint64_t v) { sum += static_cast<Wide>(v); }
    void leave_i(std::int64_t v) { sum -= static_cast<Wide>(v); }
    void leave_u(std::uint64_t v) { sum -= static_cast<Wide>(v); }
    std::int64_t i64() const { return to_int64(sum, "FRAME_SUM"); }
    std::uint64_t u64() const { return to_uint64(sum, "FRAME_SUM"); }
};

/// The float sum of a frame. Rows leave in the order they entered, so the
/// rounding is that of the add then subtract the window kernel performs.
struct FrameSumF64 {
    double sum = 0.0;
    void enter(double v) { sum += v; }
    void leave(double v) { sum -= v; }
    double mean(std::int64_t count) const {
        return sum / static_cast<double>(count);
    }
};

/// FRAME_MIN and FRAME_MAX over a window that slides forward: a deque of the
/// positions that can still be the extreme, best first. A row that enters pops
/// the positions `beats` says it beats; whether that includes an equal value
/// decides if the front is the first or the last of the equal extremes.
struct SlidingExtreme {
    std::deque<std::int64_t> candidates;

    /// `beats(back)` is true when the entering row removes position `back`.
    template <class Beats>
    void enter(std::int64_t pos, Beats&& beats) {
        while (!candidates.empty() && beats(candidates.back()))
            candidates.pop_back();
        candidates.push_back(pos);
    }
    /// Drops the positions before `lo`, which left the window.
    void drop_before(std::int64_t lo) {
        while (!candidates.empty() && candidates.front() < lo)
            candidates.pop_front();
    }
    bool empty() const { return candidates.empty(); }
    std::int64_t best() const { return candidates.front(); }
};

/// DELTA of integers (an error past int64) and of doubles. The caller makes a
/// delta null when the row or the one before it is null.
inline std::int64_t delta_i64(std::int64_t cur, std::int64_t prev) {
    return to_int64(static_cast<Wide>(cur) - static_cast<Wide>(prev), "DELTA");
}
inline std::int64_t delta_u64(std::uint64_t cur, std::uint64_t prev) {
    return to_int64(static_cast<Wide>(cur) - static_cast<Wide>(prev), "DELTA");
}
inline double delta_f64(double cur, double prev) { return cur - prev; }

/// LAG by `n` rows of a group: the cell `n` rows back. A cell is whatever the
/// caller carries (a row position, a value and its presence).
template <class Cell>
struct Lag {
    std::vector<Cell> ring;
    std::int64_t seen = 0;

    /// Feeds the next row's cell. True, with the cell `n` rows back in `out`,
    /// when the group has that many rows before this one.
    bool step(std::int64_t n, const Cell& in, Cell& out) {
        if (n == 0) {
            out = in;
            return true;
        }
        if (ring.empty()) ring.resize(static_cast<std::size_t>(n));
        Cell& slot = ring[static_cast<std::size_t>(seen % n)];
        const bool have = seen >= n;
        if (have) out = slot;
        slot = in;
        ++seen;
        return have;
    }
};

/// LEAD / LAG by position: the position `n` rows after `local` (before it for
/// a negative `n`) in a partition of `size` rows, or -1 outside it.
inline std::int64_t offset_position(std::int64_t local, std::int64_t size,
                                    std::int64_t n) {
    const std::int64_t at = local + n;
    return at >= 0 && at < size ? at : -1;
}

/// FILL_FORWARD: the last present cell so far.
template <class Cell>
struct FillForward {
    bool has = false;
    Cell last{};

    /// Feeds one row; true, with the cell to output in `out`, once a present
    /// one has been seen.
    bool step(bool present, const Cell& in, Cell& out) {
        if (present) {
            last = in;
            has = true;
        }
        if (has) out = last;
        return has;
    }
};

}  // namespace dftracer::utils::dataframe::steps

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_RUNNING_H
