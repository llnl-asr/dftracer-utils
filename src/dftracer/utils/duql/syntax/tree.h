#ifndef DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
#define DFTRACER_UTILS_DUQL_SYNTAX_TREE_H

#include <dftracer/utils/core/common/versions.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::duql::syntax {

inline constexpr int DUQL_VERSION_MAJOR = DFTRACER_UTILS_DUQL_VERSION_MAJOR;
inline constexpr int DUQL_VERSION_MINOR = DFTRACER_UTILS_DUQL_VERSION_MINOR;

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;
struct Pipeline;
using PipelinePtr = std::unique_ptr<Pipeline>;

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
    std::variant<Null, bool, std::int64_t, std::uint64_t, double, std::string>
        value;
    std::string number_text;
    bool operator==(const Literal&) const = default;
};

/// A duration literal such as `250ms`; `amount` is the number as written.
struct Duration {
    std::string amount;
    std::string unit;
    bool operator==(const Duration&) const = default;
};

/// One step of a path: a key (bare or backtick-quoted) or an array index.
struct PathStep {
    std::string key;
    bool quoted = false;
    std::optional<std::int64_t> index;
    bool wildcard() const { return key == "*" && !quoted; }
    bool operator==(const PathStep&) const = default;
};

enum class PathRoot : std::uint8_t { RECORD, CURRENT, ENCLOSING };

/// `a.b[0]`, `.name` (current row) or `^.name` (enclosing row). A step with
/// key `*` that is not quoted is a wildcard: the path is a pattern.
struct Path {
    PathRoot root = PathRoot::RECORD;
    std::vector<PathStep> steps;
    bool operator==(const Path&) const = default;
};

struct Param {
    std::string name;
    bool operator==(const Param&) const = default;
};

enum class UnaryOp : std::uint8_t { NEG, NOT };

struct Unary {
    UnaryOp op;
    ExprPtr operand;
};

enum class BinaryOp : std::uint8_t {
    OR,
    AND,
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    REGEX,
    IREGEX,
    NREGEX,
    NIREGEX,
    ADD,
    SUB,
    MUL,
    DIV,
    IDIV,
    MOD,
    COALESCE,
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
    std::vector<ExprPtr> list;
    PipelinePtr subquery;
    std::string list_param;
};

/// `"text" [not] in path` or `"text" [not] in any(path)`: the legacy
/// case-insensitive substring test.
struct Contains {
    std::string text;
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
    std::string pattern;
    std::optional<std::string> escape;
    std::string pattern_param;
};

struct Is {
    ExprPtr subject;
    bool negated = false;
    bool missing = false;
};

/// `key -> rowset(target_key).path`.
struct Arrow {
    ExprPtr key;
    std::string rowset;
    std::optional<Path> target_key;
    Path path;
};

struct Arg {
    std::string name;
    ExprPtr value;
};

/// `name(args)`; `name` may be namespaced (`myplug.entropy`).
struct Call {
    std::string name;
    std::vector<Arg> args;
};

/// `call over N rows` (`rows`) or `call over width` (a duration, number or
/// parameter): the frame of a window call.
struct Over {
    ExprPtr call;
    ExprPtr width;
    bool rows = false;
};

struct List {
    std::vector<ExprPtr> items;
};

struct Tuple {
    std::vector<ExprPtr> items;
};

struct Subquery {
    PipelinePtr pipeline;
};

/// `base[index]`: the element of `base` at a computed index. It ends a path.
struct Index {
    ExprPtr base;
    ExprPtr index;
};

using ExprNode = std::variant<Literal, Duration, Path, Param, Unary, Binary, In,
                              Contains, Between, Like, Is, Arrow, Call, Over,
                              List, Tuple, Subquery, Index>;

struct Expr {
    ExprNode node;
    Span span;
};

struct Assign {
    std::string name;
    ExprPtr value;
};

/// A `select` or `group` item: `expr [as name]` or `name = expr`.
struct Item {
    ExprPtr value;
    std::string name;
};

enum class NullsOrder : std::uint8_t { DEFAULT, FIRST, LAST };

struct SortKey {
    ExprPtr value;
    bool descending = false;
    NullsOrder nulls = NullsOrder::DEFAULT;
};

struct Where {
    ExprPtr condition;
};
struct Derive {
    std::vector<Assign> fields;
};
struct Parse {
    Path column;
    ExprPtr pattern;
};
struct Select {
    std::vector<Item> items;
};
struct Drop {
    std::vector<Path> paths;
};
struct Rename {
    std::vector<std::pair<std::string, Path>> pairs;
};
struct Distinct {
    std::vector<Item> keys;
};
struct Group {
    std::vector<Item> keys;
    std::vector<Assign> aggregates;
};
struct Agg {
    std::vector<Assign> aggregates;
};
struct Window {
    std::vector<Item> partition;
    std::vector<SortKey> order;
    std::vector<Assign> fields;
};
/// `pivot k [in [v [as label], ...]] { ... }`; `labels` pairs with
/// `values`, empty for a value with no label.
struct Pivot {
    ExprPtr key;
    std::vector<ExprPtr> values;
    std::vector<std::string> labels;
    std::vector<Assign> aggregates;
};
struct Unpivot {
    std::vector<Path> paths;
    std::string key_name;
    std::string value_name;
};
struct Sort {
    std::vector<SortKey> keys;
};
/// `take n [by ... [sort ...]]`, or `take a..b` (`last` is `b`): rows `a`
/// to `b`, 1-based and inclusive.
struct Take {
    std::string count;
    std::vector<ExprPtr> by;
    std::vector<SortKey> order;
    std::string last;
};
struct Skip {
    std::string count;
};
struct Sample {
    std::string amount;
    bool percent = false;
    std::string seed;
};
struct Expand {
    Path path;
    std::string as;
    std::string with_index;
    bool keep_empty = false;
};
enum class LookupKind : std::uint8_t { LEFT, INNER, ANTI };

/// `lookup side on k [== k2], ... [inner | anti] [into name]`; `side` is the
/// inline `(from ...)` pipeline when `rowset` is empty.
struct Lookup {
    std::string rowset;
    PipelinePtr side;
    LookupKind kind = LookupKind::LEFT;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::string into;
};
enum class AsofDirection { BACKWARD, FORWARD, NEAREST };

/// `lookup rowset on k [== k2], ... asof t [== t2] [direction] [within d]`;
/// `within` is a number, a duration or a parameter.
struct AsofLookup {
    std::string rowset;
    PipelinePtr side;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::pair<ExprPtr, ExprPtr> time;
    AsofDirection direction = AsofDirection::BACKWARD;
    ExprPtr within;
};
/// `lookup rowset on k [== k2], ... overlap [into name]`.
struct OverlapLookup {
    std::string rowset;
    PipelinePtr side;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::string into;
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
enum class FillMode : std::uint8_t { ZERO, FORWARD, LINEAR };
/// `bucket width [every step] [at origin] [fill [forward | linear] [from low to
/// high]] [as name]`; the key column is `bucket` without a name.
struct Bucket {
    ExprPtr width;
    ExprPtr every;
    ExprPtr at;
    ExprPtr low;
    ExprPtr high;
    FillMode fill_mode = FillMode::ZERO;
    bool fill = false;
    std::string as;
};
/// `session [k, ...] gap g [max m] [as name]`.
struct Session {
    std::vector<Item> keys;
    ExprPtr gap;
    ExprPtr max;
    std::string as;
};

using StageNode =
    std::variant<Where, Derive, Select, Drop, Rename, Distinct, Group, Agg,
                 Window, Pivot, Unpivot, Sort, Take, Skip, Sample, Expand,
                 Lookup, AsofLookup, OverlapLookup, Union, CallStage, TimeRange,
                 CallTree, Bucket, Session, Parse, Use>;

struct Stage {
    StageNode node;
    Span span;
};

/// A `from` source: a quoted file, a row set or a parameter.
struct From {
    std::variant<std::string, Param> name;
    bool quoted = false;
};

struct Pipeline {
    std::vector<From> sources;
    std::vector<Stage> stages;
};

struct Let {
    std::string name;
    PipelinePtr pipeline;
};

/// `def name[(params)] = expr | stages`.
struct Def {
    std::string name;
    std::vector<std::string> params;
    std::variant<ExprPtr, PipelinePtr> body;
};

struct RowSet {
    std::string name;
    PipelinePtr pipeline;
};

/// `source name { rowset = pipeline ... def ... }`.
struct SourceDecl {
    std::string name;
    std::vector<RowSet> rowsets;
    std::vector<Def> defs;
};

using Decl = std::variant<Let, Def, SourceDecl>;

struct Program {
    std::vector<Decl> decls;
    /// Absent when the text holds only declarations.
    PipelinePtr pipeline;
};

/// Structural equality, ignoring spans.
bool equal(const Program& a, const Program& b);

/// Canonical text: the engine's `duql MAJOR.MINOR`, then one declaration or
/// stage per line.
std::string to_text(const Program& query);
std::string to_text(const Expr& expr);
/// A pipeline on one line, stages joined by ` | `.
std::string to_text(const Pipeline& pipeline);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
