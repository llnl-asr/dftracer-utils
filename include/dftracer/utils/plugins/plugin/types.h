// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H

#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/plugins/abi.h>

#include <cstdint>
#include <type_traits>

namespace dftracer::utils::plugins {

/// The error a failed SDK call or plugin callback carries is a @ref
/// dstd::Error; see @ref dftracer::utils::dataframe::make_error. Its condition
/// is the status the host sees: `status = -(condition + 1)`, the numbering of
/// dftu_status, which @ref report turns back into the callback's status.
///
/// Name a condition with these helpers rather than with @ref dstd::Condition,
/// and fall back to @ref make_error for a condition none of them names.
/// @ref dftracer::utils::dataframe::error_of goes the other way for a host call
/// that returned a dftu_status.
using dataframe::cancelled;
using dataframe::internal;
using dataframe::invalid_argument;
using dataframe::io_error;
using dataframe::make_error;
using dataframe::not_found;
using dataframe::out_of_memory;
using dataframe::parse_error;
using dataframe::report;
using dataframe::status_of;
using dataframe::unsupported;

/// The success value of a callback or a task that returns @ref dstd::Result.
inline dstd::Result<void> ok() noexcept { return {}; }

namespace detail {

/// The result of a host service call that returned `status`.
inline dstd::Result<void> reported(dftu_status status) noexcept {
    if (status < 0) return dataframe::error_of(status);
    return {};
}

/// Runs `fn`, which returns void, a dftu_status or a @ref dstd::Result of void,
/// and gives the status of its outcome. The ABI-edge adapter every SDK
/// trampoline goes through; it holds no catch, so a callback that throws
/// terminates.
template <class Fn>
dftu_status to_status(dftu_ctx* ctx, Fn&& fn) noexcept {
    using R = std::invoke_result_t<Fn>;
    if constexpr (std::is_void_v<R>) {
        fn();
        return DFTU_OK;
    } else if constexpr (std::is_same_v<R, dftu_status>) {
        return fn();
    } else {
        static_assert(std::is_same_v<R, dstd::Result<void>>,
                      "a callback returns void, dftu_status or dstd::Result");
        dstd::Result<void> result = fn();
        return result.has_value() ? DFTU_OK : report(ctx, result.error());
    }
}

}  // namespace detail

// Re-export the query builder so a plugin writes `F("dur") > 1000` with only
// this header and no duql:: qualifier.
using dftracer::utils::duql::Expr;
using dftracer::utils::duql::F;
using dftracer::utils::duql::Field;

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

/// Scoped mirror of dftu_job_where for Host::spawn and make_job; each
/// enumerator is its ABI constant, so static_cast<dftu_job_where> recovers the
/// raw value.
enum class JobWhere : std::int32_t {
    Cpu = DFTU_JOB_CPU,
    Inline = DFTU_JOB_INLINE,
    Blocking = DFTU_JOB_BLOCKING
};
static_assert(static_cast<dftu_job_where>(JobWhere::Cpu) == DFTU_JOB_CPU);
static_assert(static_cast<dftu_job_where>(JobWhere::Inline) == DFTU_JOB_INLINE);
static_assert(static_cast<dftu_job_where>(JobWhere::Blocking) ==
              DFTU_JOB_BLOCKING);

/// Scoped mirror of dftu_file_kind for @ref FileStat::kind and
/// @ref PathEntry::kind; each enumerator is its ABI constant, so
/// static_cast<dftu_file_kind> recovers the raw value.
enum class FileKind : std::int32_t {
    Unknown = DFTU_FILE_UNKNOWN,
    File = DFTU_FILE_FILE,
    Directory = DFTU_FILE_DIRECTORY,
    Symlink = DFTU_FILE_SYMLINK,
    Other = DFTU_FILE_OTHER
};
static_assert(static_cast<dftu_file_kind>(FileKind::Unknown) ==
              DFTU_FILE_UNKNOWN);
static_assert(static_cast<dftu_file_kind>(FileKind::File) == DFTU_FILE_FILE);
static_assert(static_cast<dftu_file_kind>(FileKind::Directory) ==
              DFTU_FILE_DIRECTORY);
static_assert(static_cast<dftu_file_kind>(FileKind::Symlink) ==
              DFTU_FILE_SYMLINK);
static_assert(static_cast<dftu_file_kind>(FileKind::Other) == DFTU_FILE_OTHER);

/// The quantile summary a @ref Sketch returns; the SDK spelling of
/// dftu_quantiles.
struct Quantiles {
    std::uint64_t count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double p50 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

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

namespace detail {
struct AggColAccess;
struct ConfigAccess;
struct ConfigKeyAccess;
struct ConfigValueAccess;
struct FileStatAccess;
struct OpSpecAccess;
struct StatAccess;
struct StatxAccess;
}  // namespace detail

/// What an @ref Io::fstat reports; the SDK spelling of the C ABI's stat
/// subset. The call fills it, so it must outlive the call's task.
class Stat {
   public:
    std::uint64_t size() const noexcept { return raw_.size; }
    /// Nanoseconds since the epoch.
    std::uint64_t mtime_ns() const noexcept { return raw_.mtime_ns; }
    std::uint32_t mode() const noexcept { return raw_.mode; }

   private:
    friend struct detail::StatAccess;

    dftu_stat raw_{};
};
static_assert(sizeof(Stat) == sizeof(dftu_stat));

/// What an @ref Io::statx reports, with the fields the kernel left unfilled at
/// zero and @ref mask saying which it filled. The call fills it, so it must
/// outlive the call's task.
class Statx {
   public:
    std::uint32_t mask() const noexcept { return raw_.mask; }
    std::uint64_t attributes() const noexcept { return raw_.attributes; }
    std::uint64_t attributes_mask() const noexcept {
        return raw_.attributes_mask;
    }
    std::uint64_t ino() const noexcept { return raw_.ino; }
    std::uint64_t size() const noexcept { return raw_.size; }
    std::uint64_t blocks() const noexcept { return raw_.blocks; }
    std::int64_t atime_ns() const noexcept { return raw_.atime_ns; }
    std::int64_t btime_ns() const noexcept { return raw_.btime_ns; }
    std::int64_t ctime_ns() const noexcept { return raw_.ctime_ns; }
    std::int64_t mtime_ns() const noexcept { return raw_.mtime_ns; }
    std::uint32_t blksize() const noexcept { return raw_.blksize; }
    std::uint32_t nlink() const noexcept { return raw_.nlink; }
    std::uint32_t uid() const noexcept { return raw_.uid; }
    std::uint32_t gid() const noexcept { return raw_.gid; }
    std::uint32_t mode() const noexcept { return raw_.mode; }
    std::uint32_t rdev_major() const noexcept { return raw_.rdev_major; }
    std::uint32_t rdev_minor() const noexcept { return raw_.rdev_minor; }
    std::uint32_t dev_major() const noexcept { return raw_.dev_major; }
    std::uint32_t dev_minor() const noexcept { return raw_.dev_minor; }

   private:
    friend struct detail::StatxAccess;

    dftu_statx raw_{};
};
static_assert(sizeof(Statx) == sizeof(dftu_statx));

/// What an @ref Fs::stat_batch slot reports for one path, which it follows if
/// it is a symlink. The call fills an array of them, so the array must outlive
/// the call's task.
class FileStat {
   public:
    std::uint64_t size() const noexcept { return raw_.size; }
    /// Nanoseconds since the epoch.
    std::uint64_t mtime_ns() const noexcept { return raw_.mtime_ns; }
    std::uint64_t dev() const noexcept { return raw_.dev; }
    std::uint64_t ino() const noexcept { return raw_.ino; }
    std::uint32_t mode() const noexcept { return raw_.mode; }
    FileKind kind() const noexcept { return static_cast<FileKind>(raw_.kind); }

   private:
    friend struct detail::FileStatAccess;

    dftu_fs_stat raw_{};
};
static_assert(sizeof(FileStat) == sizeof(dftu_fs_stat));

namespace detail {

struct StatAccess {
    static dftu_stat* raw(Stat& stat) noexcept { return &stat.raw_; }
};

struct StatxAccess {
    static dftu_statx* raw(Statx& stat) noexcept { return &stat.raw_; }
};

struct FileStatAccess {
    static dftu_fs_stat* raw(FileStat& stat) noexcept { return &stat.raw_; }
    /// A @ref FileStat holds nothing but its record, so a span of them is the
    /// one contiguous array the ABI fills.
    static dftu_fs_stat* raw(dstd::Span<FileStat> stats) noexcept {
        return stats.empty() ? nullptr
                             : reinterpret_cast<dftu_fs_stat*>(stats.data());
    }
};

}  // namespace detail

/// Fluent builder for one dftu_agg_col spec passed to Host::agg. Every name
/// (value/out/by) is a @ref dstd::CStringView the builder only borrows, so it
/// must outlive the Host::agg call it is passed to; a literal, a dstd::String
/// and a dstd::CStringView all work. An empty name is the same as no name.
class AggCol {
   public:
    explicit AggCol(AggOp op) noexcept : op_(op) {}

    AggCol& value(dstd::CStringView v) noexcept {
        value_ = v;
        return *this;
    }
    AggCol& out(dstd::CStringView o) noexcept {
        out_ = o;
        return *this;
    }
    AggCol& param(double p) noexcept {
        param_ = p;
        return *this;
    }
    AggCol& by(dstd::CStringView b) noexcept {
        by_ = b;
        return *this;
    }

   private:
    friend struct detail::AggColAccess;

    /// The C ABI spec this builder names, with the names still borrowed from
    /// the views it was given. Reached through @ref detail::agg_col_raw.
    dftu_agg_col raw() const noexcept {
        return dftu_agg_col{static_cast<std::int32_t>(op_), c_name(value_),
                            c_name(out_), param_, c_name(by_)};
    }

    static const char* c_name(dstd::CStringView name) noexcept {
        return name.empty() ? nullptr : name.c_str();
    }

    AggOp op_;
    dstd::CStringView value_;
    dstd::CStringView out_;
    double param_ = 0.0;
    dstd::CStringView by_;
};

/// Per-op @ref AggCol factories: each function's signature takes exactly the
/// fields its DFTU_AGG_* op consumes (see dftu_agg_col in abi.h), so a
/// mismatched param combination (e.g. a quantile on sum()) fails to compile
/// instead of silently building an @ref AggCol with an unused or missing field.
namespace agg {

/// Group row count; no value column.
inline AggCol count(dstd::CStringView out) noexcept {
    return AggCol(AggOp::Count).out(out);
}

/// Count of non-null values in `value` (Int64), distinct from count()'s row
/// count.
inline AggCol count_valid(dstd::CStringView value,
                          dstd::CStringView out) noexcept {
    return AggCol(AggOp::CountValid).value(value).out(out);
}

inline AggCol sum(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Sum).value(value).out(out);
}
inline AggCol min(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Min).value(value).out(out);
}
inline AggCol max(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Max).value(value).out(out);
}
inline AggCol mean(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Mean).value(value).out(out);
}
/// Sample variance.
inline AggCol var(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Var).value(value).out(out);
}
/// Sample standard deviation.
inline AggCol std_dev(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Std).value(value).out(out);
}
/// Population skewness.
inline AggCol skew(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Skew).value(value).out(out);
}
/// Excess (population) kurtosis.
inline AggCol kurt(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Kurt).value(value).out(out);
}
/// First non-null value in row order (order-independent merge).
inline AggCol first(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::First).value(value).out(out);
}
/// Last non-null value in row order.
inline AggCol last(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Last).value(value).out(out);
}
inline AggCol sumsq(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Sumsq).value(value).out(out);
}
/// Distinct String values of `value`, sorted and joined; with `typed`, a list
/// of the values in their own type, ascending.
inline AggCol set_union(dstd::CStringView value, dstd::CStringView out,
                        bool typed = false) noexcept {
    return AggCol(AggOp::SetUnion)
        .value(value)
        .out(out)
        .param(typed ? 1.0 : 0.0);
}
/// The DDSketch histogram over `value`: a list<struct{lo,hi,count}> column of
/// the sketch's occupied bins (no bin-count parameter).
inline AggCol hist(dstd::CStringView value, dstd::CStringView out) noexcept {
    return AggCol(AggOp::Hist).value(value).out(out);
}

/// A DDSketch quantile of `value`; `q` is the level in [0, 1].
inline AggCol pct(dstd::CStringView value, dstd::CStringView out,
                  double q) noexcept {
    return AggCol(AggOp::Pct).value(value).out(out).param(q);
}

/// The String repr of `value` at the row maximizing `by`.
inline AggCol argmax(dstd::CStringView value, dstd::CStringView out,
                     dstd::CStringView by) noexcept {
    return AggCol(AggOp::Argmax).value(value).out(out).by(by);
}

/// Occupancy reductions over a (ts, dur) interval pair per group: `ts` is the
/// event start column, `dur` its duration column, and `occ_cell_us` an
/// optional endpoint-snap tolerance (0 = exact). Busy is the exact
/// interval-union length (us) where depth > 0.
inline AggCol busy(dstd::CStringView ts, dstd::CStringView dur,
                   dstd::CStringView out, double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Busy).value(ts).by(dur).out(out).param(occ_cell_us);
}
/// sum(dur) / busy over the same (ts, dur) window; see busy().
inline AggCol concurrency(dstd::CStringView ts, dstd::CStringView dur,
                          dstd::CStringView out,
                          double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Concurrency)
        .value(ts)
        .by(dur)
        .out(out)
        .param(occ_cell_us);
}
/// busy / (max_end - min_ts) over the same (ts, dur) window; see busy().
inline AggCol utilization(dstd::CStringView ts, dstd::CStringView dur,
                          dstd::CStringView out,
                          double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Utilization)
        .value(ts)
        .by(dur)
        .out(out)
        .param(occ_cell_us);
}
/// Peak overlap depth over the same (ts, dur) window; see busy().
inline AggCol active(dstd::CStringView ts, dstd::CStringView dur,
                     dstd::CStringView out, double occ_cell_us = 0.0) noexcept {
    return AggCol(AggOp::Active).value(ts).by(dur).out(out).param(occ_cell_us);
}

}  // namespace agg

class Host;

/// One config key a plugin reads, for validation and for --describe. The SDK
/// spells its kind as @ref ValueKind; @ref any makes the key accept every kind.
/// All four strings are borrowed and must outlive the plugin descriptor.
class ConfigKey {
   public:
    ConfigKey() = default;
    ConfigKey(dstd::CStringView name, ValueKind kind, bool required = false,
              dstd::CStringView doc = {}) noexcept
        : name_(name),
          kind_(kind),
          required_(required),
          doc_(doc),
          any_(false) {}

    static ConfigKey any(dstd::CStringView name, bool required = false,
                         dstd::CStringView doc = {}) noexcept {
        ConfigKey key;
        key.name_ = name;
        key.required_ = required;
        key.doc_ = doc;
        key.any_ = true;
        return key;
    }

    dstd::CStringView name() const noexcept { return name_; }

   private:
    friend struct detail::ConfigKeyAccess;

    dftu_config_key raw() const noexcept {
        // An empty name reaches the host as "", which refuses it; NULL would
        // end the key array early.
        return dftu_config_key{
            name_.c_str() != nullptr ? name_.c_str() : "",
            any_ ? DFTU_CONFIG_ANY : static_cast<std::int32_t>(kind_),
            required_ ? 1 : 0, c_name(doc_)};
    }

    static const char* c_name(dstd::CStringView name) noexcept {
        return name.empty() ? nullptr : name.c_str();
    }

    dstd::CStringView name_;
    ValueKind kind_ = ValueKind::Null;
    bool required_ = false;
    dstd::CStringView doc_;
    bool any_ = true;
};

namespace detail {
template <auto Fn>
std::int64_t series_i64_thunk(const dftu_series* series);
}  // namespace detail

/// A typed op descriptor for @ref PluginBuilder::op. The one shape a plugin
/// registers through the SDK is an aggregate over one borrowed column returning
/// a count or sum as int64; `Fn` is the plugin's function pointer
/// (`std::int64_t(const ColumnView&)`).
class OpSpec {
   public:
    OpSpec() = default;

    template <auto Fn>
    static OpSpec series_i64(dstd::CStringView name) noexcept {
        static_assert(std::is_invocable_r_v<std::int64_t, decltype(Fn),
                                            dataframe::ColumnView>);
        OpSpec spec;
        spec.name_ = dstd::String(name.c_str(), name.size());
        spec.desc_.sig =
            static_cast<dftu_op_sig>(DFTU_OP_SIG(I64, SERIES, NONE, NONE));
        spec.desc_.fn =
            reinterpret_cast<dftu_op_generic_fn>(&detail::series_i64_thunk<Fn>);
        return spec;
    }

   private:
    friend struct detail::OpSpecAccess;

    const dftu_op_desc* raw() const noexcept {
        desc_.name = name_.c_str();
        return &desc_;
    }

    dstd::String name_;
    mutable dftu_op_desc desc_{};
};

namespace detail {

template <auto Fn>
std::int64_t series_i64_thunk(const dftu_series* series) {
    return Fn(dataframe::ColumnView{series});
}

}  // namespace detail

/// Borrowed view of a registered dataframe op. It wraps the host's registry
/// record without exposing the raw dftu_op_desc, and stays valid while the op
/// is registered.
class OpRef {
   public:
    OpRef() = default;

    dstd::StringView name() const noexcept {
        return desc_ != nullptr ? dstd::StringView{desc_->name}
                                : dstd::StringView{};
    }
    dataframe::OpSig sig() const noexcept {
        return desc_ != nullptr ? dataframe::OpSig{desc_->sig}
                                : dataframe::OpSig{};
    }
    explicit operator bool() const noexcept { return desc_ != nullptr; }

   private:
    friend class Host;

    explicit OpRef(const dftu_op_desc* desc) noexcept : desc_(desc) {}

    const dftu_op_desc* desc_ = nullptr;
};

/// Opaque strong handle for an interned-string id (the C ABI's dftu_str, which
/// is an id, not bytes). Compare and hash by identity; resolve to bytes with
/// @ref Host::str. absent() is the DFTU_STR_NONE sentinel. The id itself
/// crosses the C ABI seam inside @ref Host and nowhere else: it is never an
/// integer to a plugin author.
class StrId {
   public:
    StrId() = default;
    constexpr explicit StrId(dftu_str id) noexcept : id_(id) {}
    constexpr bool absent() const noexcept { return id_ == DFTU_STR_NONE; }
    constexpr bool operator==(StrId o) const noexcept { return id_ == o.id_; }
    constexpr bool operator!=(StrId o) const noexcept { return id_ != o.id_; }

   private:
    friend class Host;

    constexpr dftu_str raw() const noexcept { return id_; }

    dftu_str id_ = DFTU_STR_NONE;
};

namespace detail {

/// The C ABI spec an @ref AggCol names. @ref Host::agg is its only caller in
/// the SDK, and a plugin author passes the builder itself instead of naming a
/// spec; this exists so a test can assert the builder's field mapping.
struct AggColAccess {
    static dftu_agg_col raw(const AggCol& col) noexcept { return col.raw(); }
};

/// @see AggColAccess
inline dftu_agg_col agg_col_raw(const AggCol& col) noexcept {
    return AggColAccess::raw(col);
}

}  // namespace detail

/// Typed, non-owning view over one dftu_value node; a Slice may keep it since
/// the host-owned config tree outlives it. Default-constructed (or an
/// out-of-range/wrong-kind lookup) yields a null ConfigValue: kind() is
/// ValueKind::Null and every predicate but is_null() is false.
class ConfigValue {
   public:
    ConfigValue() = default;

    ValueKind kind() const noexcept {
        return v_ ? static_cast<ValueKind>(v_->kind) : ValueKind::Null;
    }
    bool is_null() const noexcept { return !v_ || v_->kind == DFTU_VAL_NULL; }
    bool is_bool() const noexcept { return v_ && v_->kind == DFTU_VAL_BOOL; }
    bool is_i64() const noexcept { return v_ && v_->kind == DFTU_VAL_I64; }
    bool is_f64() const noexcept { return v_ && v_->kind == DFTU_VAL_F64; }
    bool is_str() const noexcept { return v_ && v_->kind == DFTU_VAL_STR; }
    bool is_array() const noexcept { return v_ && v_->kind == DFTU_VAL_ARRAY; }
    bool is_object() const noexcept {
        return v_ && v_->kind == DFTU_VAL_OBJECT;
    }

    bool as_bool(bool dflt = false) const noexcept {
        return dftu_as_bool(v_, dflt ? 1 : 0) != 0;
    }
    std::int64_t as_i64(std::int64_t dflt = 0) const noexcept {
        return dftu_as_i64(v_, dflt);
    }
    double as_f64(double dflt = 0.0) const noexcept {
        return dftu_as_f64(v_, dflt);
    }
    dstd::StringView as_str(dstd::StringView dflt = {}) const noexcept {
        std::uint32_t n = 0;
        const char* s = dftu_as_str(v_, &n);
        return s ? dstd::StringView{s, n} : dflt;
    }

    /// ARRAY child count or OBJECT member count; 0 for any other kind.
    std::uint32_t size() const noexcept {
        return (is_array() || is_object()) ? v_->count : 0;
    }
    /// Element `i` of an ARRAY; out of range or not an array yields null.
    ConfigValue operator[](std::uint32_t i) const noexcept {
        return (is_array() && i < v_->count) ? ConfigValue{&v_->as.items[i]}
                                             : ConfigValue{};
    }
    /// The key of OBJECT member `i`; empty for any other kind or out of range.
    dstd::StringView key_at(std::uint32_t i) const noexcept {
        if (!is_object() || i >= v_->count) return dstd::StringView();
        const dftu_member& m = v_->as.members[i];
        return dstd::StringView{m.key, m.key_len};
    }
    /// OBJECT member `i` by position; null for any other kind or out of range.
    ConfigValue value_at(std::uint32_t i) const noexcept {
        if (!is_object() || i >= v_->count) return ConfigValue{};
        return ConfigValue{v_->as.members[i].value};
    }
    /// Member `key` of an OBJECT; absent or not an object yields null.
    ConfigValue operator[](dstd::StringView key) const noexcept {
        if (!is_object()) return ConfigValue{};
        for (std::uint32_t i = 0; i < v_->count; ++i) {
            const dftu_member& m = v_->as.members[i];
            if (dstd::StringView{m.key, m.key_len} == key)
                return ConfigValue{m.value};
        }
        return ConfigValue{};
    }

   private:
    friend class Config;
    friend struct detail::ConfigValueAccess;

    explicit ConfigValue(const dftu_value* v) noexcept : v_(v) {}

    const dftu_value* v_ = nullptr;
};

/** View over the config tree; a Slice may keep returned string_views since the
   host-owned tree outlives it. */
class Config {
   public:
    Config() = default;

    /// Typed view of the whole config tree, of any kind; null when there is no
    /// tree.
    ConfigValue value() const { return ConfigValue{root_}; }

    dstd::StringView get(dstd::StringView key,
                         dstd::StringView dflt = {}) const {
        const dftu_value* v = find(key);
        return (v && v->kind == DFTU_VAL_STR)
                   ? dstd::StringView{v->as.str, v->count}
                   : dflt;
    }
    std::int64_t get_int(dstd::StringView key, std::int64_t dflt = 0) const {
        return dftu_as_i64(find(key), dflt);
    }
    double get_double(dstd::StringView key, double dflt = 0.0) const {
        return dftu_as_f64(find(key), dflt);
    }
    bool get_bool(dstd::StringView key, bool dflt = false) const {
        return dftu_as_bool(find(key), dflt ? 1 : 0) != 0;
    }
    Config child(dstd::StringView key) const {
        const dftu_value* v = find(key);
        return Config{(v && v->kind == DFTU_VAL_OBJECT) ? v : nullptr};
    }

    /// Typed view of the value at `key`, of any kind (including ARRAY/OBJECT);
    /// null if absent. Prefer this over raw() for walking nested config.
    ConfigValue value(dstd::StringView key) const {
        return ConfigValue{find(key)};
    }
    /// Typed view of the ARRAY at `key`; null if absent or not an array.
    /// Iterate elements with ConfigValue::size()/operator[].
    ConfigValue array(dstd::StringView key) const {
        return ConfigValue{raw_array(key)};
    }
    /// The ARRAY at `key` coerced to int64 per element (dftu_as_i64 rules);
    /// empty if the key is absent or not an array.
    dstd::Vector<std::int64_t> get_int_array(dstd::StringView key) const {
        dstd::Vector<std::int64_t> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i)
                out.push_back(dftu_as_i64(&a->as.items[i], 0));
        }
        return out;
    }
    /// The ARRAY at `key` coerced to double per element (dftu_as_f64 rules);
    /// empty if the key is absent or not an array.
    dstd::Vector<double> get_double_array(dstd::StringView key) const {
        dstd::Vector<double> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i)
                out.push_back(dftu_as_f64(&a->as.items[i], 0.0));
        }
        return out;
    }
    /// The ARRAY at `key` as string_views (non-STR elements yield an empty
    /// view); the views borrow the host-owned config tree. Empty if the key is
    /// absent or not an array.
    dstd::Vector<dstd::StringView> get_string_array(
        dstd::StringView key) const {
        dstd::Vector<dstd::StringView> out;
        if (const dftu_value* a = raw_array(key)) {
            out.reserve(a->count);
            for (std::uint32_t i = 0; i < a->count; ++i) {
                const dftu_value& e = a->as.items[i];
                out.push_back(e.kind == DFTU_VAL_STR
                                  ? dstd::StringView{e.as.str, e.count}
                                  : dstd::StringView{});
            }
        }
        return out;
    }

    explicit operator bool() const { return root_ != nullptr; }

   private:
    friend struct detail::ConfigAccess;

    explicit Config(const dftu_value* root) : root_(root) {}

    const dftu_value* find(dstd::StringView key) const {
        if (!root_ || root_->kind != DFTU_VAL_OBJECT) return nullptr;
        for (std::uint32_t i = 0; i < root_->count; ++i) {
            const dftu_member& m = root_->as.members[i];
            if (dstd::StringView{m.key, m.key_len} == key) return m.value;
        }
        return nullptr;
    }

    const dftu_value* raw_array(dstd::StringView key) const {
        const dftu_value* v = find(key);
        return (v && v->kind == DFTU_VAL_ARRAY) ? v : nullptr;
    }

    const dftu_value* root_ = nullptr;
};

namespace detail {

struct ConfigAccess {
    static Config from_raw(const dftu_value* value) noexcept {
        return Config{value};
    }
};

struct ConfigKeyAccess {
    static dftu_config_key raw(const ConfigKey& key) noexcept {
        return key.raw();
    }
};

struct ConfigValueAccess {
    static ConfigValue from_raw(const dftu_value* value) noexcept {
        return ConfigValue{value};
    }
};

struct OpSpecAccess {
    static const dftu_op_desc* raw(const OpSpec& spec) noexcept {
        return spec.raw();
    }
};

inline Config config_from_raw(const dftu_value* value) noexcept {
    return ConfigAccess::from_raw(value);
}

/// The SDK view of one node a host handed over, such as a @ref Doc root.
inline ConfigValue config_value_from_raw(const dftu_value* value) noexcept {
    return ConfigValueAccess::from_raw(value);
}

inline dftu_config_key config_key_raw(const ConfigKey& key) noexcept {
    return ConfigKeyAccess::raw(key);
}

inline const dftu_op_desc* op_spec_raw(const OpSpec& spec) noexcept {
    return OpSpecAccess::raw(spec);
}

}  // namespace detail

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_TYPES_H */
