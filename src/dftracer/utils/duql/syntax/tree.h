#ifndef DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
#define DFTRACER_UTILS_DUQL_SYNTAX_TREE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::duql::syntax {

inline constexpr int DUQL_VERSION = 1;

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
    bool operator==(const PathStep&) const = default;
};

enum class PathRoot : std::uint8_t { RECORD, CURRENT, ENCLOSING };

/// `a.b[0]`, `.name` (current row) or `^.name` (enclosing row).
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
struct In {
    ExprPtr subject;
    bool negated = false;
    std::vector<ExprPtr> list;
    PipelinePtr subquery;
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

struct Like {
    ExprPtr subject;
    bool negated = false;
    bool icase = false;
    std::string pattern;
    std::optional<std::string> escape;
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

struct List {
    std::vector<ExprPtr> items;
};

struct Tuple {
    std::vector<ExprPtr> items;
};

struct Subquery {
    PipelinePtr pipeline;
};

using ExprNode =
    std::variant<Literal, Duration, Path, Param, Unary, Binary, In, Contains,
                 Between, Like, Is, Arrow, Call, List, Tuple, Subquery>;

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
    std::vector<ExprPtr> keys;
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
struct Pivot {
    ExprPtr key;
    std::vector<ExprPtr> values;
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
struct Take {
    std::string count;
    std::vector<ExprPtr> by;
    std::vector<SortKey> order;
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
/// `lookup rowset on k [== k2], ... [into name]`.
struct Lookup {
    std::string rowset;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::string into;
};
enum class AsofDirection { BACKWARD, FORWARD, NEAREST };

/// `lookup rowset on k [== k2], ... asof t [== t2] [direction] [within n]`.
struct AsofLookup {
    std::string rowset;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::pair<ExprPtr, ExprPtr> time;
    AsofDirection direction = AsofDirection::BACKWARD;
    std::string within;
};
/// `lookup rowset on k [== k2], ... overlap [into name]`.
struct OverlapLookup {
    std::string rowset;
    std::vector<std::pair<ExprPtr, ExprPtr>> keys;
    std::string into;
};
struct Union {
    PipelinePtr other;
};
struct CallStage {
    Call call;
};
struct TimeRange {
    ExprPtr low;
    ExprPtr high;
    bool overlap = false;
};
struct CallTree {};
struct Bucket {
    ExprPtr width;
    bool fill = false;
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
                 CallTree, Bucket, Session>;

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

/// `def name[(params)] = expr`.
struct Def {
    std::string name;
    std::vector<std::string> params;
    ExprPtr body;
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
    int version = DUQL_VERSION;
    std::vector<Decl> decls;
    /// Absent when the text holds only declarations.
    PipelinePtr pipeline;
};

/// Structural equality, ignoring spans.
bool equal(const Program& a, const Program& b);

/// Canonical text: `duql 1`, then one declaration or stage per line.
std::string to_text(const Program& query);
std::string to_text(const Expr& expr);
/// A pipeline on one line, stages joined by ` | `.
std::string to_text(const Pipeline& pipeline);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_TREE_H
