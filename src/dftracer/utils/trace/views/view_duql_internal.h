// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_DUQL_INTERNAL_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_DUQL_INTERNAL_H

#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/group_fold.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <variant>

namespace dftracer::utils::trace::views::duql_detail {

using Status = dstd::Result<void>;
using FrameFn =
    dstd::Function<dstd::Result<dataframe::DataFrame>(dataframe::DataFrame)>;

// The plugin functions the op registry holds now.
duql::PluginCatalog registry_plugins();

// The error of a query the View cannot take: an InvalidArgument error whose
// message starts with "View::duql ".
dstd::Unexpected<dstd::Error> refuse(dstd::StringView why);

template <dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> refuse(const A&... a) {
    return refuse(dstd::fmt<S>(a...));
}

inline dstd::String std_str(dstd::StringView s) { return dstd::String(s); }

dstd::Vector<dstd::String> std_strs(const dstd::Vector<dstd::String>& v);

template <class Names>
dstd::Vector<dstd::String> strings_of(const Names& names) {
    dstd::Vector<dstd::String> out;
    out.reserve(names.size());
    for (const auto& n : names) out.emplace_back(n);
    return out;
}

template <class Names>
dstd::String joined(const Names& names) {
    dstd::String out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

inline bool is_integer(dataframe::TypeId t) {
    return t == dataframe::TypeId::Int8 || t == dataframe::TypeId::Int16 ||
           t == dataframe::TypeId::Int32 || t == dataframe::TypeId::Int64 ||
           t == dataframe::TypeId::Uint8 || t == dataframe::TypeId::Uint16 ||
           t == dataframe::TypeId::Uint32 || t == dataframe::TypeId::Uint64;
}

inline bool is_number(dataframe::TypeId t) {
    return is_integer(t) || t == dataframe::TypeId::Float32 ||
           t == dataframe::TypeId::Float64;
}

std::uint64_t pivot_max_columns();
std::uint64_t fill_max_rows();

dstd::String agg_name(duql::AggFn fn);
dstd::Optional<AggOp> trace_op(const duql::PipelineAgg& a);
bool is_count(duql::AggFn fn);

// The field a term reads when it is nothing but a whole record field.
inline const duql::TField* whole_field(const duql::Term* t) {
    if (!t) return nullptr;
    const auto* f = std::get_if<duql::TField>(&t->node);
    if (!f || f->root != duql::FieldRoot::Record ||
        f->neg_at != f->steps.size())
        return nullptr;
    for (const auto& s : f->steps)
        if (s.index) return nullptr;
    return f;
}

inline const duql::TField* whole_field(const duql::TermRef& t) {
    return whole_field(t.get());
}

// The record field a quantifier's subject reads as a whole array: a path
// without a negative index.
const duql::TField* array_field(const duql::Term& t);

// `a[0].b` as the dotted path `a.0.b` the index catalog records.
inline dstd::String dotted(dstd::StringView path) {
    dstd::String out;
    for (const char c : path) {
        if (c == '[')
            out += '.';
        else if (c != ']')
            out += c;
    }
    return out;
}

dstd::String number_text(double v);

// A pivot value as its column name suffix: as a duql literal, with a
// string as it is and null as `null`.
dstd::String pivot_name(const duql::TConst& c);

// The fields a pipeline reads from records, and those it reads as arrays.
// `closed`: a later stage sets the output columns (group, agg, select, pivot,
// distinct keys) and no earlier stage compares whole rows, so the scan need
// read only `fields`.
struct Reads {
    dstd::Vector<dstd::String> fields;
    dstd::Vector<dstd::String> arrays;
    bool closed = false;
};

// The record fields the stages read: every field a term names before an
// earlier stage defines a column of that name. An array is a field `expand`
// or a quantifier reads as a whole.
Reads referenced_fields(const dstd::Vector<duql::PipelineStage>& stages,
                        const dstd::Function<dstd::Vector<dstd::String>(
                            const duql::PipelineLookup&)>& adds,
                        bool carried);

// The select token prefix of a list of JSON text elements.
extern const dstd::String JSON_LIST;

// The select token and column type that read the array field `path` as a
// List, typed from the element paths the index observed.
dstd::Pair<dstd::String, dataframe::DataType> list_type(
    const dstd::Vector<SchemaLeaf>& tree, const dstd::String& path,
    bool args_fallback);

// What a narrowed scan reads for the field `f`: an array (`array`, or a
// field the index saw only elements under) as one list column, added to
// `arrays`; an object as its leaf columns; anything else by name.
dstd::Vector<dstd::String> scan_reads(const dstd::Vector<SchemaLeaf>& tree,
                                      const dstd::String& f, bool array,
                                      dstd::Vector<dstd::String>& arrays,
                                      bool args_fallback);

dataframe::TypeId observed_type(const dstd::String& type);

// One null row of `type`.
dataframe::Series null_cell(const dataframe::DataType& type);

inline constexpr const char* HIT_COLUMN = "__duql_hit";

// `resolved.` columns are gone; an arrow into a row set of the source reads
// the same value.
Status reject_resolved(const duql::Program& p);

dstd::String input_text(const duql::Input& in, const duql::Program& program);

// The sides `p` reads directly: its lookups', its `lookup` stages' and a
// union's.
void side_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out);

// The sides `p`'s as-of and overlap lookups read, a union's included. They
// order matches by row order, so they are collected in file order, before the
// scan.
void asof_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out);

// The sides `p`'s terms and as-of lookups read, a union's included: the ones
// its build step runs. A joined lookup's side streams into its join.
void term_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out);

// The sides `p`'s key-set `in`s read: only their distinct rows, before the
// scan.
void set_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out);

// Each top-level `and` term of `p`'s scan filter that holds only when its
// key matches a row of a side: a semi-join, or an arrow compared with a
// value. `cond` is the arrow's condition on the value, else null.
void each_pushed(
    const duql::Pipeline& p,
    const dstd::Function<void(const duql::Term&, const duql::TField&,
                              const duql::TLookup&, const duql::Term*)>& fn);

// The key sets a scan filter takes from its pushed terms: a semi-join's
// `k in {keys}` replaces its leaf, which it equals at the top of an `and`;
// an arrow's is added beside it.
struct Pushdown {
    dstd::Vector<dstd::Pair<const duql::Term*, duql::Query>> replace;
    dstd::Optional<duql::Query> extra;
};

// `query` with `p`'s key sets: the replaced leaves at the top of its `and`s
// swapped, the others added.
dstd::Optional<duql::Query> pushed(const Pushdown& p,
                                   dstd::Optional<duql::Query> query);

// `k in {keys}` for each pushed term of `p`, over its bound tables: for an
// arrow, the keys of the rows whose value meets its condition when that
// condition reads nothing of the record, in place of the term.
dstd::Result<Pushdown> key_sets(const duql::Pipeline& p);

// Where side `plan` caches under `text`, when it can.
struct CacheKey {
    dstd::String path;
    std::uint64_t sig = 0;
};

dstd::Optional<CacheKey> cache_key(const detail::ViewPlan& given,
                                   duql::InputKind input,
                                   dstd::StringView text);

// Rows of `inner`, collected, then passed through `finish` before any op
// above reads them.
dataframe::LazyFrame finish_source(dataframe::LazyFrame inner,
                                   dataframe::Schema schema, FrameFn finish);

// Rows of `inner`, each batch passed through `map` as it streams.
dataframe::LazyFrame map_source(dataframe::LazyFrame inner,
                                dataframe::Schema schema, FrameFn map);

// `f` with the columns `in` names, in order; one its rows lack is null.
dataframe::DataFrame aligned(
    dataframe::DataFrame f,
    const dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>>& in);

// One row per group of `inner`'s rows, folded by a GroupFold as they
// stream.
dataframe::LazyFrame fold_source(
    dataframe::LazyFrame inner, dataframe::Schema schema,
    dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in,
    dstd::Vector<dataframe::DataType> key_types,
    dstd::Vector<duql::FoldAgg> aggs);

// The row sets of a compiled program. A side runs once, the first time a
// plan that reads it executes; building plans runs none.
class Sides {
   public:
    explicit Sides(duql::Program p);

    duql::Program program;
    dstd::Vector<dstd::Optional<View>> views;
    dstd::Vector<dstd::Vector<duql::VectorColumn>> columns;
    dstd::Vector<dstd::Optional<CacheKey>> keys;
    // The files of each source row set, and whether their indexes hold it.
    dstd::Vector<dstd::Vector<index::plan::RowSetFile>> files;
    dstd::Vector<char> stored;

    // The rows of side `i`, run the first time. A side that fails runs again
    // on the next call.
    dstd::Result<dstd::SharedPtr<const dataframe::DataFrame>> frame(
        std::size_t i);

    // The first rows of side `i`, for the types of its columns.
    dstd::Result<dstd::SharedPtr<const dataframe::DataFrame>> probe(
        std::size_t i) const;

    // The distinct rows of side `i` for a key-set `in`.
    dstd::Result<dstd::SharedPtr<const dataframe::DataFrame>> key_frame(
        std::size_t i);

    // Runs the sides `p` reads, binds its lookups to their rows and gives
    // the key sets for its scan filter.
    dstd::Result<Pushdown> prepare(const duql::Pipeline& p);

   private:
    dstd::Vector<dstd::SharedPtr<const dataframe::DataFrame>> frames_;
    dstd::Vector<std::mutex> frame_mu_;
    dstd::Vector<dstd::SharedPtr<const dataframe::DataFrame>> key_frames_;
    dstd::Vector<std::mutex> key_frame_mu_;
    std::mutex bind_;

    // Side `i`'s rows with a value in every column, each once. The rows
    // caps do not apply; the View's memory budget bounds them.
    dstd::Result<dstd::SharedPtr<const dataframe::DataFrame>> distinct(
        std::size_t i);

    dstd::Result<dstd::SharedPtr<const dataframe::DataFrame>> run(
        std::size_t i);
};

// The program a plan reads. Only the main plan owns it: side plans, which
// Sides owns, hold it weakly and run only while the main plan does. The error
// is that the query that reads it was released.
dstd::Result<dstd::SharedPtr<Sides>> owner(const dstd::WeakPtr<Sides>& sides);

// The rows of side `side`, from memory when a run first pulls them: the copy
// the build step collected, or the cache's.
dataframe::LazyFrame side_source(dstd::SharedPtr<Sides> strong,
                                 dstd::WeakPtr<Sides> weak, std::size_t side,
                                 dataframe::Schema schema);

// The cache write of side `side` as its rows stream into a join. A side past
// DUQL_LOOKUP_MAX_ROWS or DUQL_LOOKUP_MAX_BYTES is not cached; the engine's
// join spills it instead of failing. The main plan's tap owns the sides.
dstd::SharedPtr<const dataframe::detail::Tap> side_tap(
    dstd::SharedPtr<Sides> strong, dstd::WeakPtr<Sides> weak, std::size_t side);

// A side's cache state for explain: whether the store holds it and, when
// it does, its rows. Reads no trace.
dstd::Result<dstd::String> cache_state(const dstd::Optional<CacheKey>& key);

struct Built {
    View view;
    dstd::Vector<duql::VectorColumn> cols;
};

// What building a pipeline needs of the View it starts from.
struct Ctx {
    dstd::SharedPtr<Sides> sides;
    duql::Roles roles;
    dstd::Function<View(const View&)> order;
    dstd::Function<View(const View&, dstd::SharedPtr<const detail::BuildStep>)>
        with_step;
    dstd::Function<View(const duql::Input&)> input;
    dstd::Function<const index::RecordSchema&(const View&)> schema;
};

// `p` as a View over its input: its build step, when it reads lookups, then
// its scan filter and projection, then its stages. With `table`, the scan
// ends, so the columns are the stages' own. The `main` plan owns the sides.
dstd::Result<Built> build(const Ctx& ctx, const duql::Pipeline& p, bool table,
                          dstd::Vector<dstd::String>& lines, bool main = false,
                          bool ordered = false);

// Applies the stages after the scan to a View, keeping the column list each
// stage's expressions compile against. While only scan stages have run, the
// View still reads raw events and a `group` may run in its trace
// aggregation; the first other stage asks for the ordered scan.
class Applier {
   public:
    // With `frame`, `v` holds rows with those columns rather than a scan.
    Applier(View v, const index::RecordSchema& schema, const Ctx& ctx,
            Reads reads, dstd::SharedPtr<const detail::BuildStep> step,
            dstd::Vector<std::size_t> collected, bool main,
            dstd::Optional<dstd::Vector<duql::VectorColumn>> frame = {});

    Status apply(const duql::PipelineStage& stage);

    // Ends the scan, so the columns are those the stages give.
    void end_scan(const char* stage) { leave_scan(stage); }

    const dstd::Vector<duql::VectorColumn>& columns() const { return cols_; }
    // Drops the fields the scan read only for the stages.
    Status finish_columns();

    // The scan applied a select, so its columns are fixed.
    void selected_at_scan() { scan_selected_ = true; }

    // The first error the catalog read met, else the error of the View the
    // stages built.
    Status status() const;

    View take() && { return std::move(v_); }
    dstd::Vector<dstd::String> lines;

   private:
    // The sides the build step collects, and whether this is the main plan,
    // which owns the sides.
    dstd::Vector<std::size_t> collected_;
    bool main_;
    View v_;
    const index::RecordSchema& schema_;
    const duql::Roles& roles_;
    const Ctx& ctx_;
    dstd::SharedPtr<const detail::BuildStep> step_;
    dstd::Vector<duql::VectorColumn> cols_;
    bool scan_ = true;
    bool scan_selected_ = false;
    dstd::String blocker_;
    dstd::Optional<duql::PipelineBucket> bucket_;
    dstd::Vector<dstd::String> fields_;
    dstd::Vector<dstd::String> arrays_;
    bool closed_ = false;
    dstd::Vector<dstd::String> extras_;
    dstd::Optional<dstd::HashMap<dstd::String, dataframe::TypeId>> index_types_;
    dstd::Optional<dstd::Vector<SchemaLeaf>> tree_;
    dstd::Optional<dstd::Error> tree_error_;
    std::size_t quants_ = 0;
    std::size_t lookups_ = 0;
    std::size_t calls_ = 0;
    std::size_t typed_ = 0;
    const dstd::Vector<SchemaLeaf>& tree();

    bool is_array(const dstd::String& field) const;

    bool is_json(dstd::StringView name) const;

    // The type the index observed for `name`, Unknown when it saw none or
    // several.
    dataframe::TypeId observed(dstd::StringView name);

    void columns_from_view(bool records = false);

    // A field read by the raw scan: a scanned column, or, while the view
    // still reads raw events, any field under the type the index observed.
    dstd::Optional<duql::VectorColumn> scan_column(const dstd::String& base);

    // Whether the index lists field `f`, or a path under it.
    bool indexed(const dstd::String& f);

    void leave_scan(const char* stage);

    // An open pipeline keeps every column the scan returns, so the fields it
    // reads beyond the columns become columns of the scan without narrowing
    // it, as a full index would declare them. Only for plain paths, on a view
    // that still reads raw events.
    bool name_paths(const dstd::Vector<dstd::String>& read);

    dstd::Vector<dstd::String> names() const;

    dstd::Vector<dataframe::DataType> types() const;

    dstd::Optional<std::size_t> index(dstd::StringView name) const;

    // The column of the bare name `base` under `args.`, when the source
    // falls back to it.
    dstd::Optional<std::size_t> arg_index(const dstd::String& base) const;

    dataframe::Expr col(dstd::StringView name) const;

    // Whether a field named `base` reads a column: the column itself, one it
    // lies inside, or one inside it.
    bool has_column_for(const dstd::String& base) const;

    dstd::Result<dataframe::Expr> compile(const duql::TermRef& t,
                                          const dstd::String& text,
                                          bool condition = false);

    // Whether `t` reads a column whose type the plan does not know: one a
    // scan without a catalog names, which each batch types on its own.
    bool untyped(const duql::Term& t) const;

    // `t` computed per batch into a column: each batch gives the untyped
    // columns its own types, then `t` compiles as it would over a plan that
    // declared them so, once per distinct set of types.
    dstd::Result<dataframe::Expr> per_batch(const duql::TermRef& t,
                                            const dstd::String& text,
                                            bool condition);

    // `next` as the columns, each batch passed through `fn`.
    // Each batch reaches `fn` with the current columns in order: a scan of
    // records gives a batch only the fields its records hold.
    void map_rows(FrameFn fn, dstd::Vector<duql::VectorColumn> next,
                  dstd::String line);

    // A hidden column for each quantifier and each lookup in `t`, inner ones
    // first, which the vectorizer compiles them to. `t` stays alive with the
    // plan.
    Status hidden(const duql::TermRef& t);

    // The fields of the object `f` names into `keys`, `args` and `types`:
    // its one struct column, or the columns under its path, one per key; a
    // key over deeper columns reads whether any of them holds a value.
    Status object_fields(const duql::TField& f, const dstd::String& text,
                         bool values, dstd::Vector<dstd::String>& keys,
                         dstd::Vector<dataframe::Expr>& args,
                         dstd::Vector<dataframe::DataType>& types);

    // The column of a call, computed index or list literal only
    // call_column() computes.
    Status call(const duql::TermRef& t, const duql::Term* term);

    // Binds the pipeline's lookups before a batch reads them.
    static Status ready(const dstd::SharedPtr<const detail::BuildStep>& step);

    Status quantifier(const duql::TermRef& t, const duql::Term* term);

    // The type of the column `name` of side `side`, Unknown when it has none.
    dataframe::DataType side_type(std::size_t side,
                                  dstd::StringView name) const;

    // The columns of `l`'s keys over the rows, computed apart when a key is
    // not a column; `t` owns `l`.
    dstd::Result<dstd::Vector<dstd::String>> key_columns(
        const duql::TermRef& t, const duql::TLookup& l, const dstd::String& id);

    // The side of a joined `l`, with `marker` a TRUE column of that name;
    // in `right`, the columns it matches on.
    dstd::Result<dataframe::LazyFrame> joined_side(
        const duql::TLookup& l, dstd::Vector<dstd::String>& right, bool& memory,
        const dstd::String& marker);

    // Whether `l`'s keys are columns of one exact type each, the type of
    // the side's column, as a typed semi join matches them.
    bool typed_keys(const duql::TLookup& l) const;

    // `k in (...)`, a top-level term of a `where`: the rows whose keys match
    // a row of the side, by a semi join that narrows the scan.
    Status semi_join(const duql::TermRef& t, const duql::TLookup& l);

    // A joined lookup's column: `in` through a lookup join that attaches a
    // marker, a keyed scalar through a nest join that counts the matches.
    Status join_lookup(const duql::TermRef& t, const duql::Term* term);

    Status lookup(const duql::TermRef& t, const duql::Term* term);

    Status with_column(const dstd::String& name, const dataframe::Expr& e,
                       const dstd::String& text);

    // Keeps the `keep` columns. A keep that only drops the stages' own
    // columns keeps the columns the scan returns beyond the schema too.
    Status keep(dstd::Vector<dstd::String> keep, bool rest = true);

    void rename(dstd::Vector<dstd::String> to);

    // The column `it` names when it is its own column, else a hidden column
    // `tmp` computed from it.
    dstd::Result<dstd::String> source(const duql::PipelineItem& it,
                                      dstd::String tmp);

    // The items as the only columns, in order, under their names. An item
    // that is its own column stays; the others are computed apart first so
    // no item reads another's result.
    Status project(const dstd::Vector<duql::PipelineItem>& items);

    Status stage(const duql::PipelineWhere& w);

    Status stage(const duql::PipelineSelect& s);

    Status stage(const duql::PipelineDerive& d);

    Status stage(const duql::PipelineDrop& d);

    Status stage(const duql::PipelineRename& r);

    Status stage(const duql::PipelineDistinct& d);

    struct SortColumns {
        dstd::Vector<dstd::String> by;
        dstd::Vector<bool> desc;
        dstd::Vector<dstd::String> keys;  ///< `by` without the null flags.
    };
    // The columns and directions `keys` sort by, computing the ones that are
    // not columns.
    dstd::Result<SortColumns> sort_columns(
        const dstd::Vector<duql::PipelineSortKey>& keys);

    Status sort(const dstd::Vector<duql::PipelineSortKey>& keys);

    Status stage(const duql::PipelineSort& s);

    Status stage(const duql::PipelineTake& t);

    Status stage(const duql::PipelineSkip& s);

    Status stage(const duql::PipelineTakeBy& t);

    Status stage(const duql::PipelineSample& s);

    // Microseconds per unit of the record schema's time role.
    double us_per_time_unit() const;

    Status stage(const duql::PipelineTimeRange& t);

    Status stage(const duql::PipelineBucket& b) {
        bucket_ = b;
        return {};
    }
    Status stage(const duql::PipelineCallTree&);

    void row_index(const dstd::String& name);

    dstd::Optional<std::size_t> field_index(const dstd::String& field) const;

    struct WinSpec {
        dftu_window_func func;
        dstd::String value;
        dstd::String out;
        dataframe::DataType type;
        std::int64_t offset = 0;
        std::int64_t following = DFTU_WINDOW_UNBOUNDED;
        std::int64_t preceding = DFTU_WINDOW_UNBOUNDED;
        dftu_window_frame_mode mode = DFTU_WINDOW_FRAME_ROWS;
        double q = 0;
        dstd::String by{};
    };
    // One dftu.frame.window pass over `order`, each spec's column appended.
    void window_op(const dstd::Vector<dstd::String>& part,
                   const dstd::String& order,
                   const dstd::Vector<WinSpec>& specs);

    // Aggregates over each whole partition, as `group` gives them, on every
    // row: the rows grouped by their partition id `pid` and joined back.
    void partition_aggs(
        const dstd::Vector<
            dstd::Pair<const duql::PipelineWinCall*, dstd::String>>& calls,
        const dstd::String& pid);

    dataframe::DataType type_of(const dstd::String& column) const;

    dstd::Result<dataframe::DataType> sum_type(const dstd::String& in,
                                               const dstd::String& text);

    // Row `a` differs from row `b`, null included.
    dataframe::Expr differs(const dstd::String& a, const dstd::String& b);

    // The window passes a `window` stage collects: over the order, over the
    // reverse order, then over the order again for what reads the first two.
    struct WindowPasses {
        dstd::Vector<WinSpec> forward, reverse, last, ranged, distribution;
        dstd::Vector<const duql::PipelineWinCall*> ranks;
        dstd::Vector<const duql::PipelineWinCall*> dist;
        dstd::Vector<dstd::Pair<dstd::String, const duql::PipelineWinCall*>>
            edges;
        dstd::Vector<dstd::Pair<const duql::PipelineWinCall*, dstd::String>>
            partition;
        dstd::Vector<dstd::String> sort_keys;
        dstd::String pos;
    };
    // A sum, mean, min, max or count over its `over` frame, else over the
    // whole partition.
    Status window_framed(WindowPasses& p, WinSpec s,
                         const duql::PipelineWinCall& c);
    // The column call number `i` reads, computed when it is not a column.
    dstd::Result<dstd::String> window_input(const duql::PipelineWinCall& c,
                                            std::size_t i);
    // An argmax or argmin over its frame, which also reads the `by` column.
    Status window_arg(WindowPasses& p, const duql::PipelineWinCall& c,
                      std::size_t i, const dstd::String& in);
    Status window_call(WindowPasses& p, const duql::PipelineWinCall& c,
                       std::size_t i);
    // Ranks: a row starts a run of peers when its sort keys differ from the
    // row before it in the partition.
    Status window_peers(WindowPasses& p, bool peers);

    Status stage(const duql::PipelineWindow& w);

    static dftu_scalar plugin_scalar(const duql::LiteralValue& v);

    static dstd::Unexpected<dstd::Error> plugin_refusal(
        const dstd::String& what, const dftu_op_desc* op, const char* why);

    // `f` with the column `name` replaced by the op run over it.
    static dstd::Result<dataframe::DataFrame> run_column_op(
        dataframe::DataFrame f, const dstd::String& name,
        const dftu_op_desc* op, const dstd::String& second, dftu_scalar a,
        dftu_scalar b, const dstd::Optional<dstd::String>& str);

    Status stage(const duql::PipelinePlugin& p);

    Status stage(const duql::PipelineCall& c);

    Status stage(const duql::PipelineSession& ss);

    Status stage(const duql::PipelineExpand& e);

    // A term reading the column `name` as it is.
    static duql::PipelineItem column_item(const dstd::String& name);

    Status stage(const duql::PipelinePivot& p);

    // Rows sorted by (keys, pivot value) as one row per key tuple, with a
    // column `agg.value` per aggregate and value.
    static dstd::Result<dataframe::DataFrame> spread(
        dataframe::DataFrame f, std::size_t n_keys,
        const dstd::Vector<dstd::String>& aggs,
        dstd::Vector<dstd::String> values, bool open, std::uint64_t limit);

    Status stage(const duql::PipelineUnpivot& u);

    // Whether the lookup cache holds side `i`, read without a trace.
    dstd::Result<bool> cached(std::size_t i) const;

    // Side `i` as a join's right plan: from memory when the build step or
    // the cache has it, else its own plan, which shares the scan with the
    // rows when it reads their files, under a tap for its caps and cache.
    dstd::Result<dataframe::LazyFrame> side_plan(std::size_t i,
                                                 bool& memory) const;

    Status stage(const duql::PipelineLookup& l);

    // The type an as-of pair shares: the type itself, else the number type
    // both convert to.
    static dstd::Result<dataframe::DataType> asof_type(
        const dstd::String& what, const dstd::String& side,
        const dataframe::DataType& rows, const dataframe::DataType& other);

    // The side's columns a lookup adds, but its keys and the `consumed`
    // ones: `names` gives each side column its name in the join's output (""
    // for a consumed one). A record field the rows lack is renamed apart and
    // filled into the rows' column by `fills`; any other clash is an error.
    struct SideValues {
        dstd::Vector<dstd::String> names;
        dstd::Vector<dstd::String> added;
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> fills;
        dstd::Vector<duql::VectorColumn> columns;
    };
    dstd::Result<SideValues> side_values(
        const duql::PipelineLookup& l, const char* clause,
        const dstd::Vector<std::size_t>& keys, std::size_t consumed,
        std::size_t consumed2 = SIZE_MAX) const;

    // `lookup s on k asof t`: the engine's as-of join on names both frames
    // share, then the input order back.
    Status asof_lookup(
        const duql::PipelineLookup& l, const duql::PipelineAsof& a,
        const dstd::Vector<dstd::String>& left,
        const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
        const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
            side_index);

    // `lookup s on k overlap [into m]`: the sweep over `s` (already in
    // memory, in file order) on the rows once the scan has ended.
    Status overlap_lookup(
        const duql::PipelineLookup& l, const duql::PipelineOverlap& o,
        const dstd::Vector<dstd::String>& left,
        const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
        const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
            side_index);

    // The type both sides of a union give `name`, or a refusal naming it.
    static dstd::Result<dataframe::DataType> unify(
        const dstd::String& name, const dataframe::DataType& a,
        const dataframe::DataType& b);

    // `v` with the columns `cols` as `want`: missing ones null, others cast.
    static View align(View v, const dstd::Vector<duql::VectorColumn>& cols,
                      const dstd::Vector<duql::VectorColumn>& want);

    Status stage(const duql::PipelineUnion& u);

    // The column type an aggregate gives on either plan, or nullopt to keep
    // what the plan gives.
    dstd::Optional<dataframe::TypeId> agg_type(
        const duql::PipelineAgg& a, dstd::Optional<dataframe::TypeId> in) const;

    dstd::Result<dstd::Optional<dataframe::TypeId>> term_type(
        const duql::TermRef& t, const dstd::String& text);

    // A whole-field term's column, usable by the trace aggregation.
    dstd::Optional<duql::VectorColumn> trace_column(const duql::TermRef& t);

    // The reason the trace aggregation cannot take `g`, empty when it can.
    dstd::Result<dstd::String> trace_refusal(
        const duql::PipelineGroup& g,
        const dstd::Optional<duql::PipelineBucket>& pending);

    Status stage(const duql::PipelineGroup& group);

    Status trace_group(const duql::PipelineGroup& g,
                       const dstd::Optional<duql::PipelineBucket>& bucket);

    // The doubles that fill the reducer's operands after its column, or a
    // refusal when they do not fit its signature.
    static dstd::Result<dstd::Vector<double>> plugin_params(
        const duql::PipelineAgg& a);

    static dataframe::DataType plugin_type(const dstd::String& name);

    static std::int64_t floor_div(std::int64_t a, std::int64_t b) {
        std::int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }
    // Copies each row into every window `[s, s + W)` that holds its time,
    // with `s = T + k * E`, and writes `s` to `__duql_bucket`. A null time
    // drops the row.
    Status hop_rows(const duql::PipelineBucket& b);

    Status frame_group(const duql::PipelineGroup& g,
                       const dstd::Optional<duql::PipelineBucket>& bucket);

    // A group with an aggregate the engine lacks: one streaming pass, in
    // input order, that folds the engine's aggregates and duql's.
    void fold_group(const dstd::Vector<dstd::String>& keys,
                    const dstd::Vector<dataframe::GroupAgg>& specs,
                    const dstd::Vector<dstd::String>& by,
                    dstd::Vector<duql::FoldAgg> fold);

    void finish(FrameFn fn, const dstd::String& line,
                dstd::Optional<dataframe::Schema> schema = dstd::nullopt);

    // `agg` over no rows: one row, counts 0 and the rest null.
    void finish_empty(const duql::PipelineGroup& g);

    // `bucket ... fill`: a row for every bucket between the first and the
    // last, or over the `from .. to` range, for each group. `forward` and
    // `linear` then fill the nulls of the added rows from the group's real
    // rows.
    void finish_fill(const duql::PipelineGroup& g,
                     const duql::PipelineBucket& b);
};

}  // namespace dftracer::utils::trace::views::duql_detail

#endif
