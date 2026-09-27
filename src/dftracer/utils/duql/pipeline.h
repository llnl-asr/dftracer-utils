#ifndef DFTRACER_UTILS_DUQL_PIPELINE_H
#define DFTRACER_UTILS_DUQL_PIPELINE_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/term.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::duql {

/// The fields of a record schema with a time or duration role, each as
/// nanoseconds per unit of its value, the unit of the time role, and the
/// paths bound to the time and duration roles (empty when unbound).
struct Roles {
    dftracer::utils::StringViewMap<std::int64_t> fields;
    std::int64_t time_ns_per_unit = 1000;
    std::string time;
    std::string duration;
    /// The record schema's id, for errors.
    std::string schema;
    /// The time is ISO-8601 text in the records and microseconds once
    /// decoded, so a condition on it runs after the scan.
    bool time_text = false;
};

using TermRef = std::shared_ptr<const Term>;

/// A named expression: a derived column, a select item or a distinct key.
struct PipelineItem {
    std::string name;
    TermRef term;
    std::string text;  ///< The term's duql text, for explain.
};

struct PipelineWhere {
    TermRef condition;
    std::string text;
};
struct PipelineSelect {
    std::vector<PipelineItem> items;
};
struct PipelineDrop {
    std::vector<std::string> names;
};
/// Each pair is (new name, old name).
struct PipelineRename {
    std::vector<std::pair<std::string, std::string>> pairs;
};
struct PipelineDerive {
    std::vector<PipelineItem> items;
};
struct PipelineDistinct {
    std::vector<PipelineItem> items;
};
struct PipelineSortKey {
    PipelineItem key;
    bool descending = false;
    bool nulls_first = false;
};
struct PipelineSort {
    std::vector<PipelineSortKey> keys;
};
struct PipelineTake {
    std::int64_t count = 0;
};
struct PipelineSkip {
    std::int64_t count = 0;
};

/// `take n by k1, k2 [sort s]`.
struct PipelineTakeBy {
    std::int64_t count = 0;
    std::vector<PipelineItem> keys;
    std::vector<PipelineSortKey> order;
};

/// `sample n [seed s]`, or `sample p% [seed s]` when `percent`.
struct PipelineSample {
    double amount = 0;
    bool percent = false;
    std::uint64_t seed = 0;
};

enum class AggFn : std::uint8_t {
    COUNT,
    COUNT_IF,
    SUM,
    MIN,
    MAX,
    MEAN,
    VAR,
    STD,
    FIRST,
    LAST,
    QUANTILE,
    HISTOGRAM,
    BUSY,
    CONCURRENCY,
    UTILIZATION,
    ACTIVE,
    COUNT_DISTINCT,
    COLLECT,
    ARGMAX,
    ARGMIN,
    SKETCH,
    MERGE,
};

inline bool is_occupancy(AggFn fn) {
    return fn == AggFn::BUSY || fn == AggFn::CONCURRENCY ||
           fn == AggFn::UTILIZATION || fn == AggFn::ACTIVE;
}

/// One `name = fn(arg)` of a `group` or `agg` block. `arg` is null for
/// `count()` and the occupancy aggregates; `by` is the second argument of
/// `arg_max` and `arg_min`; `q` is the quantile level. `merged` marks
/// `quantile(merge(arg), q)`, a quantile of the merged stored sketches.
struct PipelineAgg {
    std::string name;
    AggFn fn = AggFn::COUNT;
    TermRef arg;
    double q = 0;
    std::string text;
    TermRef by;
    bool merged = false;
};

/// `group k { ... }`, or `agg { ... }` with no keys.
struct PipelineGroup {
    std::vector<PipelineItem> keys;
    std::vector<PipelineAgg> aggs;
};

/// `time_range low .. high [overlap]`, in the unit of the time role, and
/// the same window as a condition on the time and duration fields.
struct PipelineTimeRange {
    double low = 0;
    double high = 0;
    bool overlap = false;
    TermRef condition;
    std::string text;
};

/// `bucket width [fill]`, in the unit of the time role; keys the next
/// `group` or `agg`. `key` is the bucket start of a record.
struct PipelineBucket {
    double width = 0;
    bool fill = false;
    TermRef key;
    std::string text;
};

struct PipelineCallTree {};

/// `session [keys] gap g [max m] [as name]`: `time` reads the time role and
/// `end` the time plus the duration in the time's unit (null without a
/// duration role); `gap` and `span` are in the time role's unit (`span` 0 = no
/// limit).
struct PipelineSession {
    std::vector<PipelineItem> keys;
    TermRef time;
    TermRef end;
    double gap = 0;
    double span = 0;
    std::string name;
    std::string text;
};

enum class WinFn : std::uint8_t {
    ROW_NUMBER,
    RANK,
    DENSE_RANK,
    LAG,
    LEAD,
    RUNNING_SUM,
    RUNNING_COUNT,
    COUNT,
    SUM,
    MIN,
    MAX,
    MEAN,
    FIRST,
    LAST,
    VAR,
    STD,
    QUANTILE,
    HISTOGRAM,
};

/// One window function call of a `window` block, computed into the hidden
/// column `column`. `arg` is null for the functions that take none;
/// `offset` is the lag or lead distance, `q` the quantile level.
struct PipelineWinCall {
    std::string column;
    WinFn fn = WinFn::ROW_NUMBER;
    TermRef arg;
    std::int64_t offset = 1;
    double q = 0;
    std::string text;
};

/// `window k1, k2 [sort s] { ... }`: each item reads the calls' columns.
struct PipelineWindow {
    std::vector<PipelineItem> keys;
    std::vector<PipelineSortKey> order;
    std::vector<PipelineWinCall> calls;
    std::vector<PipelineItem> items;
};

/// `expand path [as name] [with_index index] [keep_empty]`.
struct PipelineExpand {
    std::string path;
    std::string name;
    std::string index;
    bool keep_empty = false;
};

/// `pivot key [in [values]] { aggs }`; `fixed` when `in` lists the values.
struct PipelinePivot {
    PipelineItem key;
    bool fixed = false;
    std::vector<TConst> values;
    std::vector<PipelineAgg> aggs;
};

/// `unpivot fields as key, value`.
struct PipelineUnpivot {
    std::vector<std::string> fields;
    std::string key;
    std::string value;
};

/// `into name`: every match as a list column.
struct PipelineNest {
    std::string name;
};

/// `asof t == column [direction] [within n]`: the row of the side nearest in
/// time. `tolerance` is in the time column's units.
struct PipelineAsof {
    PipelineItem time;
    std::string column;
    syntax::AsofDirection direction = syntax::AsofDirection::BACKWARD;
    std::optional<std::int64_t> tolerance;
};

/// `overlap [into name]`: the rows of the side whose interval
/// `[time, time + duration)` overlaps the row's. The side carries the
/// `time_column` and `duration_column` fields; `scale` converts a duration
/// to the time unit (1 when equal).
struct PipelineOverlap {
    PipelineItem time;
    PipelineItem duration;
    std::string time_column;
    std::string duration_column;
    double scale = 1;
    std::string into;
};

/// `lookup side on k == k2, ... [into name | asof ... | overlap ...]`: each key
/// pairs this row's term with the side's column. Monostate is the plain form.
struct PipelineLookup {
    std::size_t side = 0;
    std::string name;
    std::vector<std::pair<PipelineItem, std::string>> keys;
    std::variant<std::monostate, PipelineNest, PipelineAsof, PipelineOverlap>
        mode;
};

struct Pipeline;

/// `union (from ...)`: the rows so far, then the rows of `other`.
struct PipelineUnion {
    std::shared_ptr<const Pipeline> other;
};

using PipelineStage =
    std::variant<PipelineWhere, PipelineSelect, PipelineDrop, PipelineRename,
                 PipelineDerive, PipelineDistinct, PipelineSort, PipelineTake,
                 PipelineSkip, PipelineTakeBy, PipelineSample, PipelineGroup,
                 PipelineTimeRange, PipelineBucket, PipelineCallTree,
                 PipelineWindow, PipelineExpand, PipelinePivot, PipelineUnpivot,
                 PipelineLookup, PipelineUnion, PipelineSession>;

/// The records a pipeline reads: every record of the View's files (`all`),
/// the records the View reads (`data`), the records of the file `path`, or
/// the rows of the source row set `side`.
enum class InputKind : std::uint8_t { ALL, DATA, FILE, SIDE };

struct Input {
    InputKind kind = InputKind::DATA;
    std::string path;
    std::size_t side = 0;
};

/// A compiled pipeline over `input`: the leading `where` stages as one scan
/// filter, a leading field-only `select` as the scan projection, and every
/// later stage in order.
struct Pipeline {
    Input input;
    QueryNodePtr filter;  ///< Null when no stage leads with `where`.
    std::string filter_text;
    std::vector<std::string> scan_select;
    std::vector<PipelineStage> stages;
};

/// A row set a lookup reads: a `let`, or a sub-query named `__sub_<n>`.
/// `key` is its canonical text with parameters bound, for the lookup cache.
struct Side {
    std::string name;
    Pipeline pipeline;
    std::string key;
    /// A row set the record schema's source declares.
    bool rowset = false;
};

/// A compiled query: the row sets its lookups read, each reading only
/// earlier ones, and the main pipeline. `args_fallback` is the source's:
/// a bare name absent from a record resolves at `args.<name>`.
struct Program {
    std::vector<Side> sides;
    Pipeline main;
    bool args_fallback = false;
};

/// Compile `text` with `params` bound. With `roles`, durations and the time
/// functions convert to the units of the schema's fields; without, they fail.
/// `source` is the members of the record schema's source (`name = pipeline`
/// and `def ...`, separated by `;`): its row sets are names for `from`,
/// arrows and `lookup`, and its macros expand as the query's do, after the
/// query's own and before those on `$DFTRACER_DUQL_PATH`.
dftracer::utils::expected<Program, DuqlError> compile_program(
    std::string_view text, const Params& params, const Roles* roles,
    std::string_view source = {});

/// Calls `fn` with every term of `p`'s filter and stages, a union's other
/// pipeline included.
void for_each_pipeline_term(const Pipeline& p,
                            const std::function<void(const Term&)>& fn);

/// The parameter value `text` spells as a duql literal: a number (with an
/// optional `-`), a string, `true` or `false`.
dftracer::utils::expected<LiteralValue, DuqlError> parse_literal(
    std::string_view text);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PIPELINE_H
