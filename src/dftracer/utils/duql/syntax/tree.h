// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
#define DFTRACER_UTILS_DUQL_SYNTAX_TREE_H

#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/variant.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <variant>

namespace dftracer::utils::duql::syntax {

inline constexpr int DUQL_VERSION_MAJOR = DFTRACER_UTILS_DUQL_VERSION_MAJOR;
inline constexpr int DUQL_VERSION_MINOR = DFTRACER_UTILS_DUQL_VERSION_MINOR;

struct Expr;
using ExprPtr = dstd::UniquePtr<Expr>;
struct Pipeline;
using PipelinePtr = dstd::UniquePtr<Pipeline>;

/// Byte range of a node in the source text.
struct Span {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct Null {
    bool operator==(const Null&) const = default;
};

/// A literal. Integers keep the old parser's typing: non-negative ones are
/// unsigned. `number_text` keeps a number's spelling for printing.
struct Literal {
    dstd::Variant<Null, bool, std::int64_t, std::uint64_t, double, dstd::String>
        value;
    dstd::String number_text;
    bool operator==(const Literal&) const = default;
};

/// A duration literal such as `250ms`; `amount` is the number as written.
struct Duration {
    dstd::String amount;
    dstd::String unit;
    bool operator==(const Duration&) const = default;
};

/// One step of a path: a key (bare or backtick-quoted) or an array index.
struct PathStep {
    dstd::String key;
    bool quoted = false;
    dstd::Optional<std::int64_t> index;
    bool wildcard() const { return key == "*" && !quoted; }
    bool operator==(const PathStep&) const = default;
};

enum class PathRoot : std::uint8_t { Record, Current, Enclosing };

/// `a.b[0]`, `.name` (current row) or `^.name` (enclosing row). A step with
/// key `*` that is not quoted is a wildcard: the path is a pattern.
struct Path {
    PathRoot root = PathRoot::Record;
    dstd::Vector<PathStep> steps;
    bool operator==(const Path&) const = default;
};

struct Param {
    dstd::String name;
    bool operator==(const Param&) const = default;
};

enum class UnaryOp : std::uint8_t { Neg, Not };

struct Unary {
    UnaryOp op;
    ExprPtr operand;
};

enum class BinaryOp : std::uint8_t {
    Or,
    And,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Regex,
    Iregex,
    Nregex,
    Niregex,
    Add,
    Sub,
    Mul,
    Div,
    Idiv,
    Mod,
    Coalesce,
};

struct Binary {
    BinaryOp op;
    ExprPtr left;
    ExprPtr right;
};

/// `x [not] in [..]` or `x [not] in (from ...)`.
/// `x [not] in [..]`, `x [not] in (from ...)` or `x [not] in $p`; binding
/// `$p` fills `list`.
struct In {
    ExprPtr subject;
    bool negated = false;
    dstd::Vector<ExprPtr> list;
    PipelinePtr subquery;
    dstd::String list_param;
};

/// `"text" [not] in path` or `"text" [not] in any(path)`: the legacy
/// case-insensitive substring test.
struct Contains {
    dstd::String text;
    bool negated = false;
    Path path;
    bool any = false;
};

struct Between {
    ExprPtr subject;
    bool negated = false;
    ExprPtr low;
    ExprPtr high;
};

/// `x [not] [i]like "p"` or `x [not] [i]like $p`; binding `$p` fills
/// `pattern`.
struct Like {
    ExprPtr subject;
    bool negated = false;
    bool icase = false;
    dstd::String pattern;
    dstd::Optional<dstd::String> escape;
    dstd::String pattern_param;
};

struct Is {
    ExprPtr subject;
    bool negated = false;
    bool missing = false;
};

/// `key -> rowset(target_key).path`.
struct Arrow {
    ExprPtr key;
    dstd::String rowset;
    dstd::Optional<Path> target_key;
    Path path;
};

struct Arg {
    dstd::String name;
    ExprPtr value;
};

/// `name(args)`; `name` may be namespaced (`myplug.entropy`).
struct Call {
    dstd::String name;
    dstd::Vector<Arg> args;
};

/// `call over N rows` (`rows`) or `call over width` (a duration, number or
/// parameter): the frame of a window call.
struct Over {
    ExprPtr call;
    ExprPtr width;
    bool rows = false;
};

struct List {
    dstd::Vector<ExprPtr> items;
};

struct Tuple {
    dstd::Vector<ExprPtr> items;
};

struct Subquery {
    PipelinePtr pipeline;
};

/// `base[index]`: the element of `base` at a computed index. It ends a path.
struct Index {
    ExprPtr base;
    ExprPtr index;
};

using ExprNode = dstd::Variant<Literal, Duration, Path, Param, Unary, Binary,
                               In, Contains, Between, Like, Is, Arrow, Call,
                               Over, List, Tuple, Subquery, Index>;

struct Expr {
    ExprNode node;
    Span span;
};

struct Assign {
    dstd::String name;
    ExprPtr value;
};

/// A `select` or `group` item: `expr [as name]` or `name = expr`.
struct Item {
    ExprPtr value;
    dstd::String name;
};

enum class NullsOrder : std::uint8_t { Default, First, Last };

struct SortKey {
    ExprPtr value;
    bool descending = false;
    NullsOrder nulls = NullsOrder::Default;
};

struct Where {
    ExprPtr condition;
};
struct Derive {
    dstd::Vector<Assign> fields;
};
struct Parse {
    Path column;
    ExprPtr pattern;
};
struct Select {
    dstd::Vector<Item> items;
};
struct Drop {
    dstd::Vector<Path> paths;
};
struct Rename {
    dstd::Vector<dstd::Pair<dstd::String, Path>> pairs;
};
struct Distinct {
    dstd::Vector<Item> keys;
};
struct Group {
    dstd::Vector<Item> keys;
    dstd::Vector<Assign> aggregates;
};
struct Agg {
    dstd::Vector<Assign> aggregates;
};
struct Window {
    dstd::Vector<Item> partition;
    dstd::Vector<SortKey> order;
    dstd::Vector<Assign> fields;
};
/// `pivot k [in [v [as label], ...]] { ... }`; `labels` pairs with
/// `values`, empty for a value with no label.
struct Pivot {
    ExprPtr key;
    dstd::Vector<ExprPtr> values;
    dstd::Vector<dstd::String> labels;
    dstd::Vector<Assign> aggregates;
};
struct Unpivot {
    dstd::Vector<Path> paths;
    dstd::String key_name;
    dstd::String value_name;
};
struct Sort {
    dstd::Vector<SortKey> keys;
};
/// `take n [by ... [sort ...]]`, or `take a..b` (`last` is `b`): rows `a`
/// to `b`, 1-based and inclusive.
struct Take {
    dstd::String count;
    dstd::Vector<ExprPtr> by;
    dstd::Vector<SortKey> order;
    dstd::String last;
};
struct Skip {
    dstd::String count;
};
struct Sample {
    dstd::String amount;
    bool percent = false;
    dstd::String seed;
};
struct Expand {
    Path path;
    dstd::String as;
    dstd::String with_index;
    bool keep_empty = false;
};
enum class LookupKind : std::uint8_t { Left, Inner, Anti };

/// `lookup side on k [== k2], ... [inner | anti] [into name]`; `side` is the
/// inline `(from ...)` pipeline when `rowset` is empty.
struct Lookup {
    dstd::String rowset;
    PipelinePtr side;
    LookupKind kind = LookupKind::Left;
    dstd::Vector<dstd::Pair<ExprPtr, ExprPtr>> keys;
    dstd::String into;
};
enum class AsofDirection { Backward, Forward, Nearest };

/// `lookup rowset on k [== k2], ... asof t [== t2] [direction] [within d]`;
/// `within` is a number, a duration or a parameter.
struct AsofLookup {
    dstd::String rowset;
    PipelinePtr side;
    dstd::Vector<dstd::Pair<ExprPtr, ExprPtr>> keys;
    dstd::Pair<ExprPtr, ExprPtr> time;
    AsofDirection direction = AsofDirection::Backward;
    ExprPtr within;
};
/// `lookup rowset on k [== k2], ... overlap [into name]`.
struct OverlapLookup {
    dstd::String rowset;
    PipelinePtr side;
    dstd::Vector<dstd::Pair<ExprPtr, ExprPtr>> keys;
    dstd::String into;
};
struct Union {
    PipelinePtr other;
};
struct CallStage {
    Call call;
};
/// `time_range [low] .. [high] [overlap]`: at least one bound.
struct TimeRange {
    ExprPtr low;
    ExprPtr high;
    bool overlap = false;
};
struct CallTree {};
/// `name(args)` at a stage position: a pipeline macro call.
struct Use {
    Call call;
};
enum class FillMode : std::uint8_t { Zero, Forward, Linear };
/// `bucket width [every step] [at origin] [fill [forward | linear] [from low to
/// high]] [as name]`; the key column is `bucket` without a name.
struct Bucket {
    ExprPtr width;
    ExprPtr every;
    ExprPtr at;
    ExprPtr low;
    ExprPtr high;
    FillMode fill_mode = FillMode::Zero;
    bool fill = false;
    dstd::String as;
};
/// `session [k, ...] gap g [max m] [as name]`.
struct Session {
    dstd::Vector<Item> keys;
    ExprPtr gap;
    ExprPtr max;
    dstd::String as;
};

using StageNode =
    dstd::Variant<Where, Derive, Select, Drop, Rename, Distinct, Group, Agg,
                  Window, Pivot, Unpivot, Sort, Take, Skip, Sample, Expand,
                  Lookup, AsofLookup, OverlapLookup, Union, CallStage,
                  TimeRange, CallTree, Bucket, Session, Parse, Use>;

struct Stage {
    StageNode node;
    Span span;
};

/// A `from` source: a quoted file, a row set or a parameter.
struct From {
    dstd::Variant<dstd::String, Param> name;
    bool quoted = false;
};

struct Pipeline {
    dstd::Vector<From> sources;
    dstd::Vector<Stage> stages;
};

struct Let {
    dstd::String name;
    PipelinePtr pipeline;
};

/// `def name[(params)] = expr | stages`.
struct Def {
    dstd::String name;
    dstd::Vector<dstd::String> params;
    dstd::Variant<ExprPtr, PipelinePtr> body;
};

struct RowSet {
    dstd::String name;
    PipelinePtr pipeline;
};

/// `source name { rowset = pipeline ... def ... }`.
struct SourceDecl {
    dstd::String name;
    dstd::Vector<RowSet> rowsets;
    dstd::Vector<Def> defs;
};

using Decl = dstd::Variant<Let, Def, SourceDecl>;

struct Program {
    dstd::Vector<Decl> decls;
    /// Absent when the text holds only declarations.
    PipelinePtr pipeline;
};

/// Structural equality, ignoring spans.
bool equal(const Program& a, const Program& b);

/// Canonical text: the engine's `duql MAJOR.MINOR`, then one declaration or
/// stage per line.
dstd::String to_text(const Program& query);
dstd::String to_text(const Expr& expr);
/// A pipeline on one line, stages joined by ` | `.
dstd::String to_text(const Pipeline& pipeline);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
