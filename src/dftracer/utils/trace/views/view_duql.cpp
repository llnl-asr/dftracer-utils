// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/internal/rest_column.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/group_fold.h>
#include <dftracer/utils/duql/overlap.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

// The plugin functions the op registry holds now.
duql::PluginCatalog registry_plugins() {
    duql::PluginCatalog out;
    out.kind = [](dstd::StringView name) {
        const dftu_op_desc* op = dftu_op_find(dstd::String(name).c_str());
        if (!op) return duql::PluginKind::None;
        const dftu_op_kind k = dftu_op_kind_of(op->sig);
        if (k == DFTU_OP_KIND_SERIES &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_SERIES)
            return duql::PluginKind::Column;
        if (k == DFTU_OP_KIND_FRAME &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_FRAME)
            return duql::PluginKind::Table;
        if (k == DFTU_OP_KIND_AGGREGATE &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_SERIES)
            return duql::PluginKind::Aggregate;
        return duql::PluginKind::Other;
    };
    for (std::uint32_t k = 0; k < dftu_op_count(); ++k) {
        const dstd::StringView op = dftu_op_at(k)->name;
        const auto dot = op.find('.');
        if (dot == dstd::StringView::npos || op.starts_with("dftu.")) continue;
        dstd::String ns(op.substr(0, dot));
        if (std::find(out.namespaces.begin(), out.namespaces.end(), ns) ==
            out.namespaces.end())
            out.namespaces.push_back(std::move(ns));
    }
    std::sort(out.namespaces.begin(), out.namespaces.end());
    return out;
}

[[noreturn]] void refuse(dstd::StringView why) {
    throw common::DFTUtilsException::cat(common::ErrorCode::InvalidArgument,
                                         "View::duql ", why);
}

dstd::String std_str(dstd::StringView s) { return dstd::String(s); }

dstd::Vector<dstd::String> std_strs(const dstd::Vector<dstd::String>& v) {
    dstd::Vector<dstd::String> out;
    out.reserve(v.size());
    for (const auto& n : v) out.emplace_back(n);
    return out;
}

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

bool is_integer(dataframe::TypeId t) {
    return t == dataframe::TypeId::Int8 || t == dataframe::TypeId::Int16 ||
           t == dataframe::TypeId::Int32 || t == dataframe::TypeId::Int64 ||
           t == dataframe::TypeId::Uint8 || t == dataframe::TypeId::Uint16 ||
           t == dataframe::TypeId::Uint32 || t == dataframe::TypeId::Uint64;
}

bool is_number(dataframe::TypeId t) {
    return is_integer(t) || t == dataframe::TypeId::Float32 ||
           t == dataframe::TypeId::Float64;
}

// The names of the columns of `f`, a frame a source declaring `declared`
// hands out: its own when it has every declared column, else the declared
// ones by position, as the ops above read it, then its own. Columns past the
// declared ones are those its input returned beyond its schema.
dstd::Vector<std::uint32_t> frame_ids(
    const dataframe::DataFrame& f, const dstd::Vector<dstd::String>& declared,
    dftracer::utils::dstd::StringIntern& intern) {
    const bool by_name =
        std::all_of(declared.begin(), declared.end(), [&](const auto& n) {
            return std::find(f.names.begin(), f.names.end(), n) !=
                   f.names.end();
        });
    dstd::Vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < f.names.size(); ++i)
        ids.push_back(intern.get_or_insert(
            !by_name && i < declared.size() ? declared[i] : f.names[i]));
    return ids;
}

dstd::Vector<dstd::String> field_names(const dataframe::Schema& s) {
    dstd::Vector<dstd::String> out;
    for (const auto& f : s.fields) out.push_back(f.name);
    return out;
}

// Rows of `inner`, collected, then passed through `finish` before any op
// above reads them.
class FinishSource final : public dataframe::Source {
   public:
    using Finish = dstd::Function<dataframe::DataFrame(dataframe::DataFrame)>;

    FinishSource(dataframe::LazyFrame inner, dataframe::Schema schema,
                 Finish finish)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          finish_(dstd::make_shared<Finish>(std::move(finish))) {}

    dataframe::Schema schema() const override { return schema_; }
    // The rows carry the columns the inner scan returned beyond its schema.
    bool undeclared_columns() const override { return true; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<Rows>(
            inner_, finish_, req.projection,
            req.projection.empty() ? field_names(schema_) : req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(dataframe::LazyFrame inner, dstd::SharedPtr<Finish> finish,
             // types
             dstd::Vector<dstd::String> projection,
             // types
             dstd::Vector<dstd::String> declared)
            : inner_(std::move(inner)),
              finish_(std::move(finish)),
              projection_(std::move(projection)),
              declared_(std::move(declared)) {}

        coro::CoroTask<dstd::Optional<dataframe::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                // GCC 12 destroys a temporary built inside co_await twice.
                dataframe::DataFrame collected = co_await inner_.collect();
                dataframe::DataFrame f = (*finish_)(
                    co_await dataframe::join_chunks(std::move(collected)));
                if (!projection_.empty()) f = f.select(projection_);
                intern_ =
                    dstd::make_shared<dftracer::utils::dstd::StringIntern>();
                ids_ = frame_ids(f, declared_, *intern_);
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
            auto m = co_await rows_->next(max_rows);
            if (m) {
                m->dyn_state().name_ids = ids_;
                m->dyn->intern = intern_;
            }
            co_return m;
        }

       private:
        dataframe::LazyFrame inner_;
        dstd::SharedPtr<Finish> finish_;
        dstd::Vector<dstd::String> projection_;
        dstd::Vector<dstd::String> declared_;
        dstd::UniquePtr<dataframe::Cursor> rows_;
        dstd::SharedPtr<dftracer::utils::dstd::StringIntern> intern_;
        dstd::Vector<std::uint32_t> ids_;
    };

    dataframe::LazyFrame inner_;
    dataframe::Schema schema_;
    dstd::SharedPtr<Finish> finish_;
};

// Rows of `inner`, each batch passed through `map` as it streams.
class MapSource final : public dataframe::Source {
   public:
    using Map = dstd::Function<dataframe::DataFrame(dataframe::DataFrame)>;

    MapSource(dataframe::LazyFrame inner, dataframe::Schema schema, Map map)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          map_(dstd::make_shared<Map>(std::move(map))) {}

    dataframe::Schema schema() const override { return schema_; }
    // The rows carry the columns the inner scan returned beyond its schema.
    bool undeclared_columns() const override { return true; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<Rows>(
            inner_, map_, req.projection,
            req.projection.empty() ? field_names(schema_) : req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(const dataframe::LazyFrame& inner, dstd::SharedPtr<Map> map,
             // types
             dstd::Vector<dstd::String> projection,
             // types
             dstd::Vector<dstd::String> declared)
            : batches_(inner.stream()),
              map_(std::move(map)),
              projection_(std::move(projection)),
              declared_(std::move(declared)) {}

        coro::CoroTask<dstd::Optional<dataframe::Morsel>> next(
            std::int64_t max_rows) override {
            for (;;) {
                if (rows_) {
                    auto m = co_await rows_->next(max_rows);
                    if (m) {
                        m->dyn_state().name_ids = ids_;
                        m->dyn->intern = intern_;
                        co_return m;
                    }
                }
                auto batch = co_await batches_.next();
                if (!batch) co_return dstd::nullopt;
                dataframe::DataFrame f = (*map_)(std::move(*batch));
                if (!projection_.empty()) f = f.select(projection_);
                ids_ = frame_ids(f, declared_, *intern_);
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
        }

       private:
        dstd::SharedPtr<dftracer::utils::dstd::StringIntern> intern_ =
            dstd::make_shared<dftracer::utils::dstd::StringIntern>();
        dstd::Vector<std::uint32_t> ids_;
        coro::AsyncGenerator<dataframe::DataFrame> batches_;
        dstd::SharedPtr<Map> map_;
        dstd::Vector<dstd::String> projection_;
        dstd::Vector<dstd::String> declared_;
        dstd::UniquePtr<dataframe::Cursor> rows_;
    };

    dataframe::LazyFrame inner_;
    dataframe::Schema schema_;
    dstd::SharedPtr<Map> map_;
};

// `f` with the columns `in` names, in order; one its rows lack is null.
dataframe::DataFrame aligned(
    dataframe::DataFrame f,
    const dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>>& in) {
    bool same = f.names.size() == in.size();
    for (std::size_t i = 0; same && i < in.size(); ++i)
        same = f.names[i] == in[i].first;
    if (same) return f;
    dataframe::DataFrame out;
    const std::int64_t n = f.num_rows();
    for (const auto& [name, type] : in) {
        out.names.emplace_back(name);
        const auto it = std::find(f.names.begin(), f.names.end(), name);
        if (it != f.names.end()) {
            out.columns.push_back(
                f.columns[static_cast<std::size_t>(it - f.names.begin())]
                    .share());
            continue;
        }
        const bool scalar = type != dataframe::TypeId::Unknown &&
                            type != dataframe::TypeId::List &&
                            type != dataframe::TypeId::Struct;
        out.columns.push_back(dataframe::Series::nulls(
            scalar ? type : dataframe::TypeId::String, n));
    }
    for (std::size_t i = 0; i < f.names.size(); ++i)
        if (std::none_of(in.begin(), in.end(), [&](const auto& c) {
                return c.first == f.names[i];
            })) {
            out.names.emplace_back(f.names[i]);
            out.columns.push_back(f.columns[i].share());
        }
    return out;
}

// One row per group of `inner`'s rows, folded by a GroupFold as they
// stream.
class FoldSource final : public dataframe::Source {
   public:
    FoldSource(dataframe::LazyFrame inner, dataframe::Schema schema,
               dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in,
               dstd::Vector<dataframe::DataType> key_types,
               dstd::Vector<duql::FoldAgg> aggs)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          in_(std::move(in)),
          key_types_(std::move(key_types)),
          aggs_(std::move(aggs)) {}

    dataframe::Schema schema() const override { return schema_; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<Rows>(*this, req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(const FoldSource& src, dstd::Vector<dstd::String> projection)
            : batches_(src.inner_.stream()),
              fold_(src.key_types_, src.aggs_),
              in_(src.in_),
              projection_(std::move(projection)) {
            for (const auto& f : src.schema_.fields) names_.push_back(f.name);
        }

        coro::CoroTask<dstd::Optional<dataframe::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                for (;;) {
                    auto batch = co_await batches_.next();
                    if (!batch) break;
                    fold_.add(aligned(std::move(*batch), in_));
                }
                dataframe::DataFrame f = fold_.finish(std::move(names_));
                if (!projection_.empty()) f = f.select(projection_);
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
            co_return co_await rows_->next(max_rows);
        }

       private:
        coro::AsyncGenerator<dataframe::DataFrame> batches_;
        duql::GroupFold fold_;
        dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in_;
        dstd::Vector<dstd::String> names_;
        dstd::Vector<dstd::String> projection_;
        dstd::UniquePtr<dataframe::Cursor> rows_;
    };

    dataframe::LazyFrame inner_;
    dataframe::Schema schema_;
    dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in_;
    dstd::Vector<dataframe::DataType> key_types_;
    dstd::Vector<duql::FoldAgg> aggs_;
};

std::uint64_t pivot_max_columns() {
    return common::Env::get<std::uint64_t>("DUQL_PIVOT_MAX_COLUMNS")
        .value_or(1024);
}

std::uint64_t fill_max_rows() {
    return common::Env::get<std::uint64_t>("DUQL_FILL_MAX_ROWS")
        .value_or(10'000'000);
}

dstd::String agg_name(duql::AggFn fn) {
    switch (fn) {
        case duql::AggFn::Count:
            return "count";
        case duql::AggFn::CountIf:
            return "count_if";
        case duql::AggFn::Sum:
            return "sum";
        case duql::AggFn::Min:
            return "min";
        case duql::AggFn::Max:
            return "max";
        case duql::AggFn::Mean:
            return "mean";
        case duql::AggFn::Var:
            return "var";
        case duql::AggFn::Std:
            return "std";
        case duql::AggFn::First:
            return "first";
        case duql::AggFn::Last:
            return "last";
        case duql::AggFn::Quantile:
            return "quantile";
        case duql::AggFn::Histogram:
            return "histogram";
        case duql::AggFn::Busy:
            return "busy";
        case duql::AggFn::Concurrency:
            return "concurrency";
        case duql::AggFn::Utilization:
            return "utilization";
        case duql::AggFn::Active:
            return "active";
        case duql::AggFn::CountDistinct:
            return "count_distinct";
        case duql::AggFn::Collect:
            return "collect";
        case duql::AggFn::Argmax:
            return "arg_max";
        case duql::AggFn::Argmin:
            return "arg_min";
        case duql::AggFn::Sketch:
            return "sketch";
        case duql::AggFn::Merge:
            return "merge";
        case duql::AggFn::Plugin:
            return "plugin function";
    }
    return {};
}

dstd::Optional<AggOp> trace_op(const duql::PipelineAgg& a) {
    switch (a.fn) {
        case duql::AggFn::Count:
            if (a.arg) return dstd::nullopt;
            return AggOp::Count;
        case duql::AggFn::Sum:
            return AggOp::Sum;
        case duql::AggFn::Min:
            return AggOp::Min;
        case duql::AggFn::Max:
            return AggOp::Max;
        case duql::AggFn::Mean:
            return AggOp::Mean;
        case duql::AggFn::Var:
            return AggOp::Var;
        case duql::AggFn::Std:
            return AggOp::Std;
        case duql::AggFn::Quantile:
            if (a.merged || !(a.q > 0 && a.q < 1)) return dstd::nullopt;
            return AggOp::Pct;
        case duql::AggFn::Histogram:
            return AggOp::Hist;
        case duql::AggFn::Busy:
            return AggOp::Busy;
        case duql::AggFn::Concurrency:
            return AggOp::Concurrency;
        case duql::AggFn::Utilization:
            return AggOp::Utilization;
        case duql::AggFn::Active:
            return AggOp::Active;
        case duql::AggFn::CountIf:
        case duql::AggFn::First:
        case duql::AggFn::Last:
        case duql::AggFn::CountDistinct:
        case duql::AggFn::Collect:
        case duql::AggFn::Argmax:
        case duql::AggFn::Argmin:
        case duql::AggFn::Sketch:
        case duql::AggFn::Merge:
        case duql::AggFn::Plugin:
            return dstd::nullopt;
    }
    return dstd::nullopt;
}

// The field a term reads when it is nothing but a whole record field.
const duql::TField* whole_field(const duql::Term* t) {
    if (!t) return nullptr;
    const auto* f = std::get_if<duql::TField>(&t->node);
    if (!f || f->root != duql::FieldRoot::Record ||
        f->neg_at != f->steps.size())
        return nullptr;
    for (const auto& s : f->steps)
        if (s.index) return nullptr;
    return f;
}

const duql::TField* whole_field(const duql::TermRef& t) {
    return whole_field(t.get());
}

// The record field a quantifier's subject reads as a whole array: a path
// without a negative index.
const duql::TField* array_field(const duql::Term& t) {
    const auto* f = std::get_if<duql::TField>(&t.node);
    if (!f || f->root != duql::FieldRoot::Record ||
        f->neg_at != f->steps.size())
        return nullptr;
    return f;
}

// `a[0].b` as the dotted path `a.0.b` the index catalog records.
dstd::String dotted(dstd::StringView path) {
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
dstd::String pivot_name(const duql::TConst& c) {
    return std::visit(
        [](const auto& v) -> dstd::String {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, duql::TNull>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, dstd::String>)
                return dstd::String(v);
            else if constexpr (std::is_same_v<T, double>)
                return number_text(v);
            else
                return dstd::to_string(v);
        },
        c.value);
}

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
                        bool carried) {
    Reads out;
    dstd::Vector<dstd::String> defined;
    auto known = [](const dstd::Vector<dstd::String>& v, dstd::StringView n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    auto field = [&](dstd::StringView base, bool array) {
        if (known(defined, base) ||
            known(defined, base.substr(0, base.find_first_of(".["))))
            return;
        if (!known(out.fields, base)) out.fields.emplace_back(base);
        if (array && !known(out.arrays, base)) out.arrays.emplace_back(base);
    };
    auto add = [&](const duql::TermRef& t) {
        if (!t) return;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* f = std::get_if<duql::TField>(&x.node);
                f && f->root != duql::FieldRoot::Element)
                field(f->base, false);
            if (const auto* q = std::get_if<duql::TQuant>(&x.node))
                if (const auto* a = array_field(*q->subject))
                    field(a->base, true);
            if (const auto* i = std::get_if<duql::TIndex>(&x.node))
                if (const auto* a = array_field(*i->array))
                    field(a->base, true);
        });
    };
    auto items = [&](const dstd::Vector<duql::PipelineItem>& v) {
        for (const auto& it : v) add(it.term);
    };
    auto define = [&](const dstd::Vector<duql::PipelineItem>& v) {
        for (const auto& it : v) defined.push_back(dstd::String(it.name));
    };
    auto sort_keys = [&](const dstd::Vector<duql::PipelineSortKey>& v) {
        for (const auto& k : v) add(k.key.term);
    };
    bool whole_rows = false;
    dstd::String bucket_name = "bucket";
    for (const auto& stage : stages)
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, duql::PipelineGroup> ||
                              std::is_same_v<T, duql::PipelineSelect> ||
                              std::is_same_v<T, duql::PipelinePivot>) {
                    if (!whole_rows) out.closed = true;
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineDistinct>) {
                    if (s.items.empty())
                        whole_rows = true;
                    else if (!whole_rows)
                        out.closed = true;
                }
                if constexpr (std::is_same_v<T, duql::PipelineWhere>) {
                    add(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineSelect> ||
                                     std::is_same_v<T, duql::PipelineDerive> ||
                                     std::is_same_v<T,
                                                    duql::PipelineDistinct>) {
                    items(s.items);
                    define(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineRename>) {
                    for (const auto& pair : s.pairs) {
                        if (!carried) field(pair.second, false);
                        defined.push_back(dstd::String(pair.first));
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelinePlugin>) {
                    add(s.arg.term);
                    defined.push_back(dstd::String(s.arg.name));
                    if (s.arg2) {
                        add(s.arg2->term);
                        defined.push_back(dstd::String(s.arg2->name));
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelineSort>) {
                    sort_keys(s.keys);
                } else if constexpr (std::is_same_v<T, duql::PipelineTakeBy>) {
                    items(s.keys);
                    sort_keys(s.order);
                } else if constexpr (std::is_same_v<T, duql::PipelineGroup>) {
                    items(s.keys);
                    for (const auto& a : s.aggs) {
                        add(a.arg);
                        add(a.by);
                    }
                    define(s.keys);
                    for (const auto& a : s.aggs)
                        defined.push_back(dstd::String(a.name));
                    defined.push_back(bucket_name);
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineTimeRange>) {
                    add(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineBucket>) {
                    add(s.key);
                    bucket_name = s.name;
                } else if constexpr (std::is_same_v<T, duql::PipelineSession>) {
                    items(s.keys);
                    add(s.time);
                    add(s.end);
                    defined.push_back(dstd::String(s.name));
                } else if constexpr (std::is_same_v<T, duql::PipelineWindow>) {
                    items(s.keys);
                    sort_keys(s.order);
                    for (const auto& c : s.calls) {
                        add(c.arg);
                        add(c.by);
                        defined.push_back(dstd::String(c.column));
                    }
                    items(s.items);
                    define(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineExpand>) {
                    field(s.path, true);
                    defined.push_back(dstd::String(s.name));
                    if (!s.index.empty())
                        defined.push_back(dstd::String(s.index));
                } else if constexpr (std::is_same_v<T, duql::PipelinePivot>) {
                    add(s.key.term);
                    for (const auto& a : s.aggs) {
                        add(a.arg);
                        add(a.by);
                    }
                    for (const auto& a : s.aggs)
                        for (const auto& v : s.values)
                            defined.push_back(dstd::String(a.name + ".") +
                                              pivot_name(v));
                } else if constexpr (std::is_same_v<T, duql::PipelineUnpivot>) {
                    for (const auto& f : s.fields) field(f, false);
                    defined.push_back(dstd::String(s.key));
                    defined.push_back(dstd::String(s.value));
                } else if constexpr (std::is_same_v<T, duql::PipelineLookup>) {
                    for (const auto& k : s.keys) add(k.first.term);
                    if (const auto* a =
                            std::get_if<duql::PipelineAsof>(&s.mode))
                        add(a->time.term);
                    if (const auto* o =
                            std::get_if<duql::PipelineOverlap>(&s.mode)) {
                        add(o->time.term);
                        add(o->duration.term);
                    }
                    for (auto& n : adds(s)) defined.push_back(std::move(n));
                }
            },
            stage);
    return out;
}

char type_letter(const dstd::String& type) {
    if (type == "int" || type == "uint") return 'i';
    if (type == "float") return 'f';
    if (type == "string") return 's';
    if (type == "bool") return 'b';
    if (type == "null") return 0;
    return 'j';
}

char join_letters(char a, char b) {
    if (!a) return b;
    if (!b || a == b) return a;
    if ((a == 'i' && b == 'f') || (a == 'f' && b == 'i')) return 'f';
    return 'j';
}

dataframe::DataType letter_type(char c) {
    switch (c) {
        case 'i':
            return dataframe::scalar(dataframe::TypeId::Int64);
        case 'f':
            return dataframe::scalar(dataframe::TypeId::Float64);
        case 'b':
            return dataframe::scalar(dataframe::TypeId::Bool);
        case 'a':
            return dataframe::scalar(dataframe::TypeId::Unknown);
        default:
            return dataframe::scalar(dataframe::TypeId::String);
    }
}

bool digits(dstd::StringView s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

// The select token prefix of a list of JSON text elements.
const dstd::String JSON_LIST = detail::list_token("j", "");

// The select token and column type that read the array field `path` as a
// List, typed from the element paths the index observed.
dstd::Pair<dstd::String, dataframe::DataType> list_type(
    const dstd::Vector<SchemaLeaf>& tree, const dstd::String& path,
    bool args_fallback) {
    char scalar = 0;
    bool scalars = false;
    bool objects = false;
    bool nested = false;
    dstd::Map<dstd::String, char> fields;
    dstd::Vector<dstd::String> prefixes = {dotted(path) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(path) + ".");
    for (const auto& leaf : tree) {
        dstd::StringView rest;
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                rest = dstd::StringView(leaf.path).substr(p.size());
                break;
            }
        const std::size_t dot = rest.find('.');
        if (!digits(rest.substr(0, dot))) continue;
        const char c = type_letter(leaf.type);
        if (dot == dstd::StringView::npos) {
            scalars = true;
            scalar = join_letters(scalar, c);
            continue;
        }
        const dstd::StringView f = rest.substr(dot + 1);
        const std::size_t end = f.find('.');
        const dstd::String name(f.substr(0, end));
        if (digits(name)) {
            nested = true;
            continue;
        }
        objects = true;
        char& slot = fields[name];
        slot = end == dstd::StringView::npos ? join_letters(slot, c) : 'j';
    }
    if (objects && !scalars && !nested) {
        dstd::String spec = "{";
        dstd::Vector<dataframe::Field> members;
        for (const auto& [name, c] : fields) {
            const char k = c ? c : 's';
            if (spec.size() > 1) spec += ',';
            spec += name + "=" + k;
            members.push_back(
                dataframe::Field{dstd::String(name), letter_type(k), true});
        }
        spec += '}';
        return {detail::list_token(spec, path),
                dataframe::list_of(dataframe::struct_of(std::move(members)))};
    }
    const char k = nested || objects ? 'j'
                   : scalars         ? (scalar ? scalar : 's')
                                     : 'a';
    return {detail::list_token(dstd::String(1, k), path),
            dataframe::list_of(letter_type(k))};
}

// What a narrowed scan reads for the field `f`: an array (`array`, or a
// field the index saw only elements under) as one list column, added to
// `arrays`; an object as its leaf columns; anything else by name.
dstd::Vector<dstd::String> scan_reads(const dstd::Vector<SchemaLeaf>& tree,
                                      const dstd::String& f, bool array,
                                      dstd::Vector<dstd::String>& arrays,
                                      bool args_fallback) {
    dstd::Vector<dstd::String> prefixes = {dotted(f) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(f) + ".");
    bool elements = false;
    dstd::Vector<dstd::String> leaves;
    for (const auto& leaf : tree)
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                const dstd::StringView rest =
                    dstd::StringView(leaf.path).substr(p.size());
                if (digits(rest.substr(0, rest.find('.'))))
                    elements = true;
                else
                    leaves.push_back(leaf.path);
                break;
            }
    if (array || (elements && leaves.empty())) {
        if (std::find(arrays.begin(), arrays.end(), f) == arrays.end())
            arrays.push_back(f);
        return {list_type(tree, f, args_fallback).first};
    }
    if (!leaves.empty() && !elements) return leaves;
    return {f};
}

dataframe::TypeId observed_type(const dstd::String& type) {
    if (type == "int" || type == "uint") return dataframe::TypeId::Int64;
    if (type == "float") return dataframe::TypeId::Float64;
    if (type == "string") return dataframe::TypeId::String;
    if (type == "bool") return dataframe::TypeId::Bool;
    return dataframe::TypeId::Unknown;
}

// One null row of `type`.
dataframe::Series null_cell(const dataframe::DataType& type) {
    if (type.id == dataframe::TypeId::List && !type.fields.empty())
        return dataframe::Series::list({0, 0},
                                       null_cell(type.fields.front().type))
            .take(dstd::Vector<std::int64_t>{-1});
    const bool scalar = type.id != dataframe::TypeId::Unknown &&
                        type.id != dataframe::TypeId::List &&
                        type.id != dataframe::TypeId::Struct;
    return dataframe::Series::nulls(
        scalar ? type.id : dataframe::TypeId::String, 1);
}

// Keeps a row index plus the seed inside int64.
constexpr std::uint64_t SEED_MASK = (std::uint64_t{1} << 62) - 1;

dstd::String number_text(double v) {
    if (std::trunc(v) == v && std::abs(v) < 9.0e15)
        return dstd::to_string(static_cast<std::int64_t>(v));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool is_count(duql::AggFn fn) {
    return fn == duql::AggFn::Count || fn == duql::AggFn::CountIf ||
           fn == duql::AggFn::CountDistinct;
}

std::uint64_t env_count(dstd::StringView name, std::uint64_t fallback) {
    return common::Env::get<std::uint64_t>(name).value_or(fallback);
}

constexpr const char* HIT_COLUMN = "__duql_hit";
constexpr std::uint64_t LOOKUP_MAX_ROWS = 1'000'000;
constexpr std::int64_t TYPE_PROBE_ROWS = 65'536;
// Keys a side's distinct rows in the lookup cache apart from its rows.
constexpr std::uint64_t DISTINCT_CACHE_SALT = 0x64697374696e6374ULL;
constexpr std::uint64_t LOOKUP_MAX_BYTES = std::uint64_t{256} << 20;

// `resolved.` columns are gone; an arrow into a row set of the source reads
// the same value.
void reject_resolved(const duql::Program& p) {
    auto check = [](dstd::StringView path) {
        if (!path.starts_with("resolved.")) return;
        refuse("'" + dstd::String(path) +
               "': 'resolved.' columns are removed; write an arrow into a "
               "row set of the source, such as 'fhash -> files.path', "
               "'hhash -> hosts.name' or 'run -> runs.app'");
    };
    dstd::Function<void(const duql::QueryNode&)> node =
        [&](const duql::QueryNode& n) {
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, duql::AndNode> ||
                                  std::is_same_v<T, duql::OrNode>) {
                        node(*x.left);
                        node(*x.right);
                    } else if constexpr (std::is_same_v<T, duql::NotNode>) {
                        node(*x.operand);
                    } else if constexpr (std::is_same_v<T, duql::CompareNode> ||
                                         std::is_same_v<T, duql::InNode> ||
                                         std::is_same_v<T, duql::NotInNode> ||
                                         std::is_same_v<T, duql::MatchNode>) {
                        check(x.field.path);
                    }
                },
                n.data);
        };
    auto pipeline = [&](const duql::Pipeline& pl) {
        if (pl.filter) node(*pl.filter);
        duql::for_each_pipeline_term(pl, [&](const duql::Term& t) {
            if (const auto* f = std::get_if<duql::TField>(&t.node))
                check(f->base);
        });
    };
    pipeline(p.main);
    for (const auto& s : p.sides) pipeline(s.pipeline);
}

dstd::String input_text(const duql::Input& in, const duql::Program& program) {
    switch (in.kind) {
        case duql::InputKind::Side:
            return dstd::String(program.sides[in.side].name);
        case duql::InputKind::All:
            return "all";
        case duql::InputKind::Data:
            return "data";
        case duql::InputKind::File:
            return dstd::String("\"" + in.path + "\"");
    }
    return {};
}

// The sides `p` reads directly: its lookups', its `lookup` stages' and a
// union's.
void side_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    if (p.input.kind == duql::InputKind::Side) out.push_back(p.input.side);
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node)) {
            out.push_back(l->side);
            if (l->empty) out.push_back(*l->empty);
        }
    });
    for (const auto& s : p.stages) {
        if (const auto* l = std::get_if<duql::PipelineLookup>(&s))
            out.push_back(l->side);
        if (const auto* u = std::get_if<duql::PipelineUnion>(&s)) {
            dstd::Vector<std::size_t> other;
            side_reads(*u->other, other);
            for (const auto i : other)
                if (std::find(out.begin(), out.end(), i) == out.end())
                    out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The sides `p`'s as-of and overlap lookups read, a union's included. They
// order matches by row order, so they are collected in file order, before the
// scan.
void asof_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    for (const auto& s : p.stages) {
        if (const auto* l = std::get_if<duql::PipelineLookup>(&s);
            l && (std::holds_alternative<duql::PipelineAsof>(l->mode) ||
                  std::holds_alternative<duql::PipelineOverlap>(l->mode)))
            out.push_back(l->side);
        if (const auto* u = std::get_if<duql::PipelineUnion>(&s))
            asof_reads(*u->other, out);
    }
}

// The sides `p`'s terms and as-of lookups read, a union's included: the ones
// its build step runs. A joined lookup's side streams into its join.
void term_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node)) {
            if (!duql::joined(*l) && !l->key_set) out.push_back(l->side);
            if (l->empty) out.push_back(*l->empty);
        }
    });
    asof_reads(p, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The sides `p`'s key-set `in`s read: only their distinct rows, before the
// scan.
void set_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node);
            l && l->key_set)
            out.push_back(l->side);
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// Each top-level `and` term of `p`'s scan filter that holds only when its
// key matches a row of a side: a semi-join, or an arrow compared with a
// value. `cond` is the arrow's condition on the value, else null.
void each_pushed(
    const duql::Pipeline& p,
    const dstd::Function<void(const duql::Term&, const duql::TField&,
                              const duql::TLookup&, const duql::Term*)>& fn) {
    dstd::Function<void(const duql::QueryNode&)> walk =
        [&](const duql::QueryNode& n) {
            if (const auto* a = std::get_if<duql::AndNode>(&n.data)) {
                walk(*a->left);
                walk(*a->right);
                return;
            }
            const auto* leaf = std::get_if<duql::ExprLeaf>(&n.data);
            if (!leaf) return;
            const duql::Term* cond = nullptr;
            const duql::TLookup* l =
                std::get_if<duql::TLookup>(&leaf->term->node);
            if (l && (l->kind != duql::LookupKind::In || l->negated))
                l = nullptr;
            if (const auto* q = std::get_if<duql::TQuant>(&leaf->term->node);
                q && !q->all) {
                l = std::get_if<duql::TLookup>(&q->subject->node);
                cond = q->cond.get();
            }
            if (!l || l->keys.size() != 1) return;
            if (const auto* f = whole_field(l->keys.front().get()))
                fn(*leaf->term, *f, *l, cond);
        };
    if (p.filter) walk(*p.filter);
}

dstd::Optional<duql::LiteralNode> literal_of(const dataframe::Series& s,
                                             std::int64_t r) {
    if (s.is_null(r)) return dstd::nullopt;
    const dataframe::TypeId t = s.type();
    if (t == dataframe::TypeId::Uint64)
        return duql::LiteralNode{s.data<std::uint64_t>()[r]};
    if (is_integer(t))
        return duql::LiteralNode{s.slice(r, 1)
                                     .cast(dataframe::TypeId::Int64)
                                     .data<std::int64_t>()[0]};
    if (t == dataframe::TypeId::Float32 || t == dataframe::TypeId::Float64)
        return duql::LiteralNode{
            s.slice(r, 1).cast(dataframe::TypeId::Float64).data<double>()[0]};
    if (t == dataframe::TypeId::Bool)
        return duql::LiteralNode{
            ((s.data<std::uint8_t>()[r >> 3] >> (r & 7)) & 1) != 0};
    if (t == dataframe::TypeId::String || t == dataframe::TypeId::LargeString)
        return duql::LiteralNode{dstd::String(s.string_at(r))};
    return dstd::nullopt;
}

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
                                   dstd::Optional<duql::Query> query) {
    if (!query) return p.extra;
    duql::QueryNodePtr root = duql::clone(query->root());
    dstd::Function<void(duql::QueryNodePtr&)> walk =
        [&](duql::QueryNodePtr& n) {
            if (auto* a = std::get_if<duql::AndNode>(&n->data)) {
                walk(a->left);
                walk(a->right);
                return;
            }
            if (const auto* leaf = std::get_if<duql::ExprLeaf>(&n->data))
                for (const auto& [term, keys] : p.replace)
                    if (leaf->term.get() == term) {
                        n = duql::clone(keys.root());
                        return;
                    }
        };
    walk(root);
    if (p.extra)
        root = duql::make_node(
            duql::AndNode{duql::clone(p.extra->root()), std::move(root)});
    return duql::Query::from_node(std::move(root));
}

// `k in {keys}` for each pushed term of `p`, over its bound tables: for an
// arrow, the keys of the rows whose value meets its condition when that
// condition reads nothing of the record, in place of the term.
Pushdown key_sets(const duql::Pipeline& p) {
    Pushdown out;
    duql::QueryNodePtr extra;
    each_pushed(p, [&](const duql::Term& leaf, const duql::TField& f,
                       const duql::TLookup& l, const duql::Term* cond) {
        const duql::LookupTable& t = *l.slot->table;
        const dataframe::Series& keys = t.frame->columns[t.key_columns.front()];
        dstd::Optional<dataframe::Series> pass;
        bool record = false;
        if (cond)
            duql::for_each_term(*cond, [&](const duql::Term& x) {
                if (const auto* fx = std::get_if<duql::TField>(&x.node);
                    fx && fx->root != duql::FieldRoot::Element)
                    record = true;
                if (std::holds_alternative<duql::TLookup>(x.node) ||
                    std::holds_alternative<duql::TQuant>(x.node))
                    record = true;
            });
        if (cond && !record && t.value) {
            const dataframe::Series& values = t.frame->columns[*t.value];
            if (auto e = duql::vectorize_condition(
                    *cond, {{".", values.data_type()}}, false))
                pass = dataframe::eval(*e, {&values}).materialize();
        }
        auto passes = [&](std::int64_t r) {
            if (!pass) return !cond || !t.value || record;
            return !pass->is_null(r) &&
                   ((pass->data<std::uint8_t>()[r >> 3] >> (r & 7)) & 1);
        };
        duql::ArrayNode arr;
        for (const auto& [key, rows] : t.rows)
            for (const std::int64_t r : rows)
                if (passes(r)) {
                    if (auto lit = literal_of(keys, r))
                        arr.elements.push_back(std::move(*lit));
                    break;
                }
        // Past the cap the list prunes nothing, and the lookup term filters
        // the same rows without a per-record scan of the list.
        if (arr.elements.size() > index::plan::SEMI_JOIN_CAP) return;
        arr.set = duql::make_in_set(arr);
        auto node = duql::make_node(
            duql::InNode{duql::FieldNode{f.base}, std::move(arr)});
        // A condition on the row set's value alone passes the same records
        // as the key set, so the set replaces the term.
        if (!cond || pass) {
            out.replace.emplace_back(&leaf,
                                     duql::Query::from_node(std::move(node)));
            return;
        }
        extra = extra ? duql::make_node(
                            duql::AndNode{std::move(extra), std::move(node)})
                      : std::move(node);
    });
    if (extra) out.extra = duql::Query::from_node(std::move(extra));
    return out;
}

// Where side `plan` caches under `text`, when it can.
struct CacheKey {
    dstd::String path;
    std::uint64_t sig = 0;
};

dstd::Optional<CacheKey> cache_key(const detail::ViewPlan& given,
                                   duql::InputKind input,
                                   dstd::StringView text) {
    // A file opened without an index path reads the index beside it.
    detail::ViewPlan plan = given;
    for (auto& f : plan.files)
        if (f.index_path.empty())
            f.index_path = internal::determine_index_path(f.file_path, "");
    const dstd::String path = index::cache::lookup_cache_path(plan);
    if (path.empty()) return dstd::nullopt;
    detail::plan_record_schema(plan);
    // A `data` side reads the View as given: its filters are part of it.
    dstd::String key(text);
    if (input == duql::InputKind::Data) {
        key +=
            "\n" +
            (plan.query ? dstd::String(plan.query->source()) : dstd::String()) +
            "\n" + dstd::to_string(static_cast<int>(plan.phase)) +
            dstd::to_string(static_cast<int>(plan.all_records));
        if (plan.time_range)
            key += "\n" + number_text(plan.time_range->first) + ".." +
                   number_text(plan.time_range->second);
    }
    const auto sig = index::cache::lookup_signature(plan, key);
    if (!sig) return dstd::nullopt;
    return CacheKey{dstd::String(path.data(), path.size()), *sig};
}

[[noreturn]] void over_rows(const dstd::String& name, std::uint64_t rows,
                            std::uint64_t max) {
    refuse("'" + name + "' gives " + dstd::to_string(rows) +
           " rows or more, over DUQL_LOOKUP_MAX_ROWS (" + dstd::to_string(max) +
           "); a lookup reads a small row set: narrow it with 'where' or "
           "'select'");
}

[[noreturn]] void over_bytes(const dstd::String& name, std::uint64_t bytes,
                             std::uint64_t max) {
    refuse("'" + name + "' holds " + dstd::to_string(bytes) +
           " bytes, over DUQL_LOOKUP_MAX_BYTES (" + dstd::to_string(max) +
           "); a lookup reads a small row set: narrow it with 'select'");
}

// Stores a side's rows in the lookup cache; a store that fails keeps none.
void persist(const CacheKey& key,
             const dstd::Vector<dataframe::DataFrame>& parts) {
    if (parts.empty()) return;
    try {
        const auto db = index::cache::open_lookup_db(
            key.path, index::cache::CacheStore::Mode::ReadWrite);
        if (!db) return;
        dstd::Vector<const dataframe::DataFrame*> ptrs;
        for (const auto& p : parts) ptrs.push_back(&p);
        index::cache::persist_lookup(
            *db, key.sig,
            ptrs.size() == 1 ? dataframe::frame_to_native(parts.front())
                             : dataframe::frame_to_native(dataframe::concat(
                                   ptrs, dataframe::ConcatHow::Diagonal)));
    } catch (const std::exception&) {
    }
}

// The row sets of a compiled program. A side runs once, the first time a
// plan that reads it executes; building plans runs none.
class Sides {
   public:
    explicit Sides(duql::Program p)
        : program(std::move(p)),
          views(program.sides.size()),
          columns(program.sides.size()),
          keys(program.sides.size()),
          files(program.sides.size()),
          stored(program.sides.size(), 0),
          frames_(program.sides.size()),
          once_(program.sides.size()),
          key_frames_(program.sides.size()),
          key_once_(program.sides.size()) {}

    duql::Program program;
    dstd::Vector<dstd::Optional<View>> views;
    dstd::Vector<dstd::Vector<duql::VectorColumn>> columns;
    dstd::Vector<dstd::Optional<CacheKey>> keys;
    // The files of each source row set, and whether their indexes hold it.
    dstd::Vector<dstd::Vector<index::plan::RowSetFile>> files;
    dstd::Vector<char> stored;

    dstd::SharedPtr<const dataframe::DataFrame> frame(std::size_t i) {
        std::call_once(once_[i], [&] { frames_[i] = run(i); });
        return frames_[i];
    }

    // The first TYPE_PROBE_ROWS rows of side `i`, for the types of its
    // columns.
    dstd::SharedPtr<const dataframe::DataFrame> probe(std::size_t i) const {
        const dataframe::LazyFrame plan =
            views[i]->head(TYPE_PROBE_ROWS).lazy();
        dataframe::DataFrame f;
        dftracer::utils::default_runtime().block_on(
            coro::named("duql_side_types", [&]() -> coro::CoroTask<void> {
                f = co_await plan.collect();
                co_return;
            }()));
        return dstd::make_shared<const dataframe::DataFrame>(std::move(f));
    }

    // The distinct rows of side `i` for a key-set `in`.
    dstd::SharedPtr<const dataframe::DataFrame> key_frame(std::size_t i) {
        std::call_once(key_once_[i], [&] { key_frames_[i] = distinct(i); });
        return key_frames_[i];
    }

    // Runs the sides `p` reads, binds its lookups to their rows and gives
    // the key sets for its scan filter.
    Pushdown prepare(const duql::Pipeline& p) {
        dstd::Vector<std::size_t> reads;
        term_reads(p, reads);
        for (const auto i : reads) frame(i);
        dstd::Vector<std::size_t> sets;
        set_reads(p, sets);
        for (const auto i : sets)
            if (std::find(reads.begin(), reads.end(), i) == reads.end())
                key_frame(i);
        {
            const std::lock_guard<std::mutex> lock(bind_);
            duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
                const auto* l = std::get_if<duql::TLookup>(&t.node);
                if (!l || duql::joined(*l) || l->slot->table) return;
                const auto& rows =
                    frames_[l->side] ? frames_[l->side] : key_frames_[l->side];
                l->slot->table = duql::make_lookup_table(*l, rows);
            });
        }
        return key_sets(p);
    }

   private:
    dstd::Vector<dstd::SharedPtr<const dataframe::DataFrame>> frames_;
    dstd::Vector<std::once_flag> once_;
    dstd::Vector<dstd::SharedPtr<const dataframe::DataFrame>> key_frames_;
    dstd::Vector<std::once_flag> key_once_;
    std::mutex bind_;

    // Side `i`'s rows with a value in every column, each once. The rows
    // caps do not apply; the View's memory budget bounds them.
    dstd::SharedPtr<const dataframe::DataFrame> distinct(std::size_t i) {
        const dstd::String& name = dstd::String(program.sides[i].name);
        dstd::Optional<CacheKey> key = keys[i];
        if (key) {
            key->sig = dftracer::utils::dstd::hash::splitmix64(
                key->sig ^ DISTINCT_CACHE_SALT);
            try {
                if (const auto db = index::cache::open_lookup_db(
                        key->path, index::cache::CacheStore::Mode::ReadOnly))
                    if (auto bytes = index::cache::read_lookup(*db, key->sig))
                        if (auto cached = dataframe::frame_from_native(*bytes))
                            return dstd::make_shared<
                                const dataframe::DataFrame>(
                                duql::flat_frame(std::move(*cached)));
            } catch (const std::exception&) {
            }
        }
        const std::uint64_t budget =
            dftracer::utils::memory::resolve_spill_budget(
                views[i]->memory_budget_bytes());
        dftracer::utils::dstd::StringViewSet seen;
        dstd::Vector<dataframe::DataFrame> parts;
        std::uint64_t bytes = 0;
        const dataframe::LazyFrame plan = views[i]->lazy();
        dftracer::utils::default_runtime().block_on(
            coro::named("duql_key_set", [&]() -> coro::CoroTask<void> {
                auto gen = plan.stream();
                dstd::String row_key;
                for (;;) {
                    auto f = co_await gen.next();
                    if (!f) break;
                    dataframe::DataFrame part = duql::flat_frame(std::move(*f));
                    dstd::Vector<std::int64_t> take;
                    for (std::int64_t r = 0; r < part.num_rows(); ++r) {
                        row_key.clear();
                        bool keyed = true;
                        for (const auto& c : part.columns)
                            keyed =
                                keyed && duql::append_cell_key(row_key, c, r);
                        if (!keyed || !seen.insert(row_key).second) continue;
                        take.push_back(r);
                        bytes += row_key.size() + sizeof(dstd::String);
                    }
                    if (take.empty()) continue;
                    dataframe::DataFrame kept;
                    kept.names = part.names;
                    for (const auto& c : part.columns)
                        kept.columns.push_back(c.take(take));
                    bytes += duql::frame_bytes(kept);
                    if (bytes > budget)
                        refuse("'" + name + "' holds more distinct rows than " +
                               "the memory budget (" + dstd::to_string(budget) +
                               " bytes); a top-level 'in' reads them into "
                               "memory: raise the budget, or write the test " +
                               "under 'not' or in a later 'where' to join it");
                    parts.push_back(std::move(kept));
                }
                co_return;
            }()));
        if (parts.empty()) {
            dataframe::DataFrame none;
            for (const auto& c : columns[i]) {
                none.names.emplace_back(c.name);
                none.columns.push_back(dataframe::Series::nulls(
                    c.type.id == dataframe::TypeId::Unknown
                        ? dataframe::TypeId::String
                        : c.type.id,
                    0));
            }
            return dstd::make_shared<const dataframe::DataFrame>(
                std::move(none));
        }
        if (key) persist(*key, parts);
        dstd::Vector<const dataframe::DataFrame*> ptrs;
        for (const auto& part : parts) ptrs.push_back(&part);
        return dstd::make_shared<const dataframe::DataFrame>(
            dataframe::concat(ptrs, dataframe::ConcatHow::Diagonal));
    }

    dstd::SharedPtr<const dataframe::DataFrame> run(std::size_t i) {
        const dstd::String& name = dstd::String(program.sides[i].name);
        const std::uint64_t max_rows =
            env_count("DUQL_LOOKUP_MAX_ROWS", LOOKUP_MAX_ROWS);
        const std::uint64_t max_bytes =
            env_count("DUQL_LOOKUP_MAX_BYTES", LOOKUP_MAX_BYTES);
        const auto limit = static_cast<std::int64_t>(std::min<std::uint64_t>(
            max_rows, std::numeric_limits<std::int64_t>::max() - 1));
        if (stored[i])
            if (auto f = index::plan::stored_rowset(files[i], name)) {
                if (static_cast<std::uint64_t>(f->num_rows()) > max_rows)
                    over_rows(name, static_cast<std::uint64_t>(f->num_rows()),
                              max_rows);
                dataframe::DataFrame flat = duql::flat_frame(std::move(*f));
                if (const std::uint64_t bytes = duql::frame_bytes(flat);
                    bytes > max_bytes)
                    over_bytes(name, bytes, max_bytes);
                return dstd::make_shared<const dataframe::DataFrame>(
                    std::move(flat));
            }
        if (keys[i]) try {
                if (const auto db = index::cache::open_lookup_db(
                        keys[i]->path,
                        index::cache::CacheStore::Mode::ReadOnly))
                    if (auto bytes =
                            index::cache::read_lookup(*db, keys[i]->sig))
                        if (auto cached = dataframe::frame_from_native(*bytes))
                            return dstd::make_shared<
                                const dataframe::DataFrame>(
                                duql::flat_frame(std::move(*cached)));
            } catch (const std::exception&) {
            }
        const dataframe::LazyFrame plan = views[i]->head(limit + 1).lazy();
        dataframe::DataFrame f;
        dftracer::utils::default_runtime().block_on(
            coro::named("duql_lookup_side", [&]() -> coro::CoroTask<void> {
                f = co_await plan.collect();
                co_return;
            }()));
        if (static_cast<std::uint64_t>(f.num_rows()) > max_rows)
            over_rows(name, static_cast<std::uint64_t>(f.num_rows()), max_rows);
        f = duql::flat_frame(std::move(f));
        if (const std::uint64_t bytes = duql::frame_bytes(f); bytes > max_bytes)
            over_bytes(name, bytes, max_bytes);
        if (keys[i]) {
            dstd::Vector<dataframe::DataFrame> parts;
            parts.push_back(f.select(f.names));
            persist(*keys[i], parts);
        }
        return dstd::make_shared<const dataframe::DataFrame>(std::move(f));
    }
};

// The program a plan reads. Only the main plan owns it: side plans, which
// Sides owns, hold it weakly and run only while the main plan does.
dstd::SharedPtr<Sides> owner(const dstd::WeakPtr<Sides>& sides) {
    auto s = sides.lock();
    if (!s) refuse("a row set ran after the query that reads it was released");
    return s;
}

// The rows of side `side`, from memory when a run first pulls them: the copy
// the build step collected, or the cache's.
class SideSource final : public dataframe::Source {
   public:
    SideSource(dstd::SharedPtr<Sides> strong, dstd::WeakPtr<Sides> weak,
               std::size_t side, dataframe::Schema schema)
        : strong_(std::move(strong)),
          weak_(std::move(weak)),
          side_(side),
          schema_(std::move(schema)) {}

    dataframe::Schema schema() const override { return schema_; }

    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r;
        r.cursor = dstd::make_unique<Rows>(strong_ ? strong_ : owner(weak_),
                                           side_, req.projection);
        r.filters.assign(req.filters.size(), dataframe::Pushed::No);
        return r;
    }

   private:
    class Rows final : public dataframe::Cursor {
       public:
        Rows(dstd::SharedPtr<Sides> sides, std::size_t side,
             // types
             dstd::Vector<dstd::String> projection)
            : sides_(std::move(sides)),
              side_(side),
              projection_(std::move(projection)) {}

        coro::CoroTask<dstd::Optional<dataframe::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                const auto frame = sides_->frame(side_);
                dataframe::DataFrame f;
                f.names = frame->names;
                for (const auto& c : frame->columns)
                    f.columns.push_back(c.share());
                if (!projection_.empty()) f = f.select(projection_);
                rows_ = dataframe::InMemorySource(std::move(f)).scan({}).cursor;
            }
            co_return co_await rows_->next(max_rows);
        }

       private:
        dstd::SharedPtr<Sides> sides_;
        std::size_t side_;
        dstd::Vector<dstd::String> projection_;
        dstd::UniquePtr<dataframe::Cursor> rows_;
    };

    dstd::SharedPtr<Sides> strong_;
    dstd::WeakPtr<Sides> weak_;
    std::size_t side_;
    dataframe::Schema schema_;
};

// The cache write of side `side` as its rows stream into a join. A side past
// DUQL_LOOKUP_MAX_ROWS or DUQL_LOOKUP_MAX_BYTES is not cached; the engine's
// join spills it instead of failing. The main plan's tap owns the sides.
dstd::SharedPtr<const dataframe::detail::Tap> side_tap(
    dstd::SharedPtr<Sides> strong, dstd::WeakPtr<Sides> weak,
    std::size_t side) {
    auto tap = dstd::make_shared<dataframe::detail::Tap>();
    tap->open = [strong = std::move(strong), weak = std::move(weak), side] {
        struct State {
            std::uint64_t rows = 0;
            std::uint64_t bytes = 0;
            bool over = false;
            dstd::Vector<dataframe::DataFrame> parts;
        };
        auto state = dstd::make_shared<State>();
        const auto s = strong ? strong : owner(weak);
        const dstd::Optional<CacheKey> key = s->keys[side];
        const std::uint64_t max_rows =
            env_count("DUQL_LOOKUP_MAX_ROWS", LOOKUP_MAX_ROWS);
        const std::uint64_t max_bytes =
            env_count("DUQL_LOOKUP_MAX_BYTES", LOOKUP_MAX_BYTES);
        dataframe::detail::TapRun run;
        run.rows = [state, key, max_rows,
                    max_bytes](const dataframe::DataFrame& f) {
            if (state->over) return;
            state->rows += static_cast<std::uint64_t>(f.num_rows());
            dataframe::DataFrame part;
            part.names = f.names;
            for (const auto& c : f.columns) part.columns.push_back(c.share());
            part = duql::flat_frame(std::move(part));
            state->bytes += duql::frame_bytes(part);
            if (state->rows > max_rows || state->bytes > max_bytes) {
                state->over = true;
                state->parts.clear();
                return;
            }
            if (key) state->parts.push_back(std::move(part));
        };
        run.end = [state, key] {
            if (key && !state->over) persist(*key, state->parts);
        };
        return run;
    };
    return tap;
}

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

Built build(const Ctx& ctx, const duql::Pipeline& p, bool table,
            dstd::Vector<dstd::String>& lines, bool main = false,
            bool ordered = false);

// A side's cache state for explain: whether the store holds it and, when
// it does, its rows. Reads no trace.
dstd::String cache_state(const dstd::Optional<CacheKey>& key) {
    if (!key) return "not cached";
    const auto db = index::cache::open_lookup_db(
        key->path, index::cache::CacheStore::Mode::ReadOnly);
    if (!db || !index::cache::lookup_exists(*db, key->sig)) return "not cached";
    try {
        if (auto bytes = index::cache::read_lookup(*db, key->sig))
            if (auto f = dataframe::frame_from_native(*bytes))
                return "cached, " + dstd::to_string(f->num_rows()) + " rows";
    } catch (const std::exception&) {
    }
    return "cached";
}

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
            dstd::Optional<dstd::Vector<duql::VectorColumn>> frame = {})
        : collected_(std::move(collected)),
          main_(main),
          v_(std::move(v)),
          schema_(schema),
          roles_(ctx.roles),
          ctx_(ctx),
          step_(std::move(step)),
          fields_(std::move(reads.fields)),
          arrays_(std::move(reads.arrays)),
          closed_(reads.closed) {
        if (!frame) {
            columns_from_view(true);
            return;
        }
        cols_ = std::move(*frame);
        scan_ = false;
        blocker_ = "from";
    }

    void apply(const duql::PipelineStage& stage) {
        std::visit([this](const auto& s) { this->stage(s); }, stage);
        dstd::Vector<dstd::String> rest;
        for (const auto& c : cols_)
            if (!c.quant && !c.lookup && !c.call && !c.index && !c.list &&
                !c.term)
                rest.push_back(c.name);
        if (rest.size() != cols_.size()) keep(std::move(rest));
    }

    // Ends the scan, so the columns are those the stages give.
    void end_scan(const char* stage) { leave_scan(stage); }

    const dstd::Vector<duql::VectorColumn>& columns() const { return cols_; }

    // Drops the fields the scan read only for the stages.
    void finish_columns() {
        dstd::Vector<dstd::String> rest;
        bool dropped = false;
        for (const auto& n : names()) {
            if (std::find(extras_.begin(), extras_.end(), n) != extras_.end())
                dropped = true;
            else
                rest.push_back(n);
        }
        if (dropped) keep(std::move(rest));
    }

    // The scan applied a select, so its columns are fixed.
    void selected_at_scan() { scan_selected_ = true; }

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
    dstd::Optional<dstd::Map<dstd::String, dataframe::TypeId>> index_types_;
    dstd::Optional<dstd::Vector<SchemaLeaf>> tree_;
    std::size_t quants_ = 0;
    std::size_t lookups_ = 0;
    std::size_t calls_ = 0;
    std::size_t typed_ = 0;

    const dstd::Vector<SchemaLeaf>& tree() {
        if (!tree_) tree_ = detail::catalog_leaves(v_);
        return *tree_;
    }

    bool is_array(const dstd::String& field) const {
        return std::find(arrays_.begin(), arrays_.end(), field) !=
               arrays_.end();
    }

    bool is_json(dstd::StringView name) const {
        for (const auto& spec : schema_.fields)
            if (spec.type == index::FieldType::Json &&
                (name == spec.name || name == spec.path ||
                 (schema_.args_fallback && name.starts_with("args.") &&
                  name.substr(5) == spec.name)))
                return true;
        return false;
    }

    // The type the index observed for `name`, Unknown when it saw none or
    // several.
    dataframe::TypeId observed(dstd::StringView name) {
        if (!index_types_) {
            index_types_.emplace();
            for (const auto& l : tree())
                index_types_->emplace(l.path, observed_type(l.type));
        }
        const auto it = index_types_->find(dotted(name));
        return it == index_types_->end() ? dataframe::TypeId::Unknown
                                         : it->second;
    }

    void columns_from_view(bool records = false) {
        cols_.clear();
        for (const auto& f : v_.output_schema().fields) {
            dataframe::DataType type = f.type;
            if (type.id == dataframe::TypeId::Unknown)
                type = dataframe::scalar(observed(f.name));
            cols_.push_back({dstd::String(f.name), std::move(type),
                             is_json(f.name), records});
        }
        if (!records) return;
        for (const auto& a : arrays_) {
            auto i = index(a);
            if (!i) i = arg_index(a);
            if (i && cols_[*i].type.fields.empty() &&
                (cols_[*i].type.id == dataframe::TypeId::List ||
                 cols_[*i].type.id == dataframe::TypeId::Unknown)) {
                auto [token, type] =
                    list_type(tree(), a, schema_.args_fallback);
                cols_[*i].type = std::move(type);
                cols_[*i].json = token.starts_with(JSON_LIST);
            }
        }
    }

    // A field read by the raw scan: a scanned column, or, while the view
    // still reads raw events, any field under the type the index observed.
    dstd::Optional<duql::VectorColumn> scan_column(const dstd::String& base) {
        if (auto i = index(base)) return cols_[*i];
        if (auto i = arg_index(base)) return cols_[*i];
        if (!scan_) return dstd::nullopt;
        dataframe::TypeId t = observed(base);
        if (t == dataframe::TypeId::Unknown && schema_.args_fallback)
            t = observed("args." + base);
        if (t == dataframe::TypeId::Unknown) return dstd::nullopt;
        return duql::VectorColumn{base, dataframe::scalar(t), is_json(base)};
    }

    // Whether the index lists field `f`, or a path under it.
    bool indexed(const dstd::String& f) {
        const dstd::String d = dotted(f);
        for (const auto& leaf : tree()) {
            for (const dstd::String& p :
                 {d, schema_.args_fallback ? "args." + d : dstd::String()})
                if (!p.empty() &&
                    (leaf.path == p ||
                     (leaf.path.size() > p.size() && leaf.path.starts_with(p) &&
                      leaf.path[p.size()] == '.')))
                    return true;
        }
        return false;
    }

    void leave_scan(const char* stage) {
        if (!scan_) return;
        // A closed pipeline reads only its fields; otherwise every column
        // stays and the fields add what the scan does not give yet.
        dstd::Vector<dstd::String> read = names();
        dstd::Vector<dstd::String> only;
        bool narrow = closed_;
        const auto add = [](dstd::Vector<dstd::String>& v, dstd::String f) {
            if (std::find(v.begin(), v.end(), f) == v.end())
                v.push_back(std::move(f));
        };
        for (const auto& f : fields_) {
            if (index(f) || arg_index(f)) {
                add(only, f);
                continue;
            }
            // A field the index does not list may still be in the events.
            if (narrow && !indexed(f)) narrow = false;
            for (auto& r : scan_reads(tree(), f, is_array(f), arrays_,
                                      schema_.args_fallback)) {
                add(only, r);
                add(read, std::move(r));
            }
        }
        if (narrow && !only.empty()) read = std::move(only);
        if (!narrow && read.size() > cols_.size() && name_paths(read)) {
            lines.push_back("scan names: " + joined(read));
        } else if (read.size() > cols_.size() ||
                   (narrow && read.size() < cols_.size())) {
            const dstd::Vector<dstd::String> before = names();
            lines.push_back("scan select: " + joined(read));
            v_ = v_.select(std::move(read));
            columns_from_view(true);
            for (const auto& n : names())
                if (std::find(before.begin(), before.end(), n) == before.end())
                    extras_.push_back(n);
        }
        scan_ = false;
        blocker_ = stage;
        lines.push_back("scan order: file, then line");
        v_ = ctx_.order(v_);
    }

    // An open pipeline keeps every column the scan returns, so the fields it
    // reads beyond the columns become columns of the scan without narrowing
    // it, as a full index would declare them. Only for plain paths, on a view
    // that still reads raw events.
    bool name_paths(const dstd::Vector<dstd::String>& read) {
        if (!v_.filters_events()) return false;
        dstd::Vector<dataframe::Field> named;
        for (std::size_t i = cols_.size(); i < read.size(); ++i) {
            if (read[i] != detail::select_source_field(read[i]) ||
                read[i].starts_with(detail::list_token("", "").substr(0, 2)))
                return false;
            named.push_back(
                dataframe::Field{dstd::String(read[i]),
                                 dataframe::scalar(observed(read[i])), true});
        }
        v_ = detail::with_named_columns(v_, std::move(named));
        columns_from_view(true);
        return true;
    }

    dstd::Vector<dstd::String> names() const {
        dstd::Vector<dstd::String> out;
        out.reserve(cols_.size());
        for (const auto& c : cols_) out.push_back(c.name);
        return out;
    }

    dstd::Vector<dataframe::DataType> types() const {
        dstd::Vector<dataframe::DataType> out;
        out.reserve(cols_.size());
        for (const auto& c : cols_) out.push_back(c.type);
        return out;
    }

    dstd::Optional<std::size_t> index(dstd::StringView name) const {
        for (std::size_t i = 0; i < cols_.size(); ++i)
            if (cols_[i].name == name) return i;
        return dstd::nullopt;
    }

    // The column of the bare name `base` under `args.`, when the source
    // falls back to it.
    dstd::Optional<std::size_t> arg_index(const dstd::String& base) const {
        if (!schema_.args_fallback) return dstd::nullopt;
        return index("args." + base);
    }

    dataframe::Expr col(dstd::StringView name) const {
        return dataframe::expr_col(static_cast<std::int32_t>(*index(name)));
    }

    // Whether a field named `base` reads a column: the column itself, one it
    // lies inside, or one inside it.
    bool has_column_for(const dstd::String& base) const {
        auto under = [](const dstd::String& a, const dstd::String& b) {
            return a.size() > b.size() && a.starts_with(b) &&
                   (a[b.size()] == '.' || a[b.size()] == '[');
        };
        auto reads = [&](const dstd::String& path) {
            for (const auto& c : cols_)
                if (c.name == path || under(path, c.name) ||
                    under(c.name, path))
                    return true;
            return false;
        };
        return reads(base) || (schema_.args_fallback && reads("args." + base));
    }

    dataframe::Expr compile(const duql::TermRef& t, const dstd::String& text,
                            bool condition = false) {
        hidden(t);
        // After the scan the columns are fixed, so a name none of them holds
        // is a mistake, not a missing field.
        if (!scan_)
            duql::for_each_term_field(*t, [&](const duql::TField& f) {
                if (f.root == duql::FieldRoot::Record &&
                    !has_column_for(dstd::String(f.base)))
                    refuse("has no column '" + dstd::String(f.base) +
                           "'; the columns here are " + joined(names()));
            });
        const bool fb = schema_.args_fallback;
        if (untyped(*t)) return per_batch(t, text, condition);
        auto e = condition ? duql::vectorize_condition(*t, cols_, fb)
                           : duql::vectorize(*t, cols_, fb);
        if (!e) refuse(dstd::String(e.error().message + " in '") + text + "'");
        return std::move(*e);
    }

    // Whether `t` reads a column whose type the plan does not know: one a
    // scan without a catalog names, which each batch types on its own.
    bool untyped(const duql::Term& t) const {
        bool out = false;
        duql::for_each_term_field(t, [&](const duql::TField& f) {
            if (f.root != duql::FieldRoot::Record) return;
            for (const auto& c : cols_)
                if (c.type.id == dataframe::TypeId::Unknown && !c.json &&
                    (c.name == dstd::String(f.base) ||
                     (schema_.args_fallback &&
                      c.name == dstd::String("args." + f.base)) ||
                     (f.base.size() > c.name.size() &&
                      f.base.starts_with(c.name) &&
                      f.base[c.name.size()] == '.')))
                    out = true;
        });
        return out;
    }

    // `t` computed per batch into a column: each batch gives the untyped
    // columns its own types, then `t` compiles as it would over a plan that
    // declared them so, once per distinct set of types.
    dataframe::Expr per_batch(const duql::TermRef& t, const dstd::String& text,
                              bool condition) {
        const dstd::String name = "__duql_t_" + dstd::to_string(typed_++);
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back(
            {name, dataframe::scalar(condition ? dataframe::TypeId::Bool
                                               : dataframe::TypeId::Unknown)});
        next.back().term = t.get();
        struct Cache {
            std::mutex mu;
            dstd::Map<dstd::Vector<dataframe::TypeId>, dataframe::Expr> exprs;
        };
        map_rows(
            [t, text, condition, name, cols = cols_, fb = schema_.args_fallback,
             cache = dstd::make_shared<Cache>()](dataframe::DataFrame f) {
                dstd::Vector<duql::VectorColumn> typed = cols;
                dstd::Vector<dataframe::TypeId> types;
                for (std::size_t i = 0; i < typed.size(); ++i) {
                    if (typed[i].type.id == dataframe::TypeId::Unknown)
                        typed[i].type = f.columns[i].data_type();
                    types.push_back(typed[i].type.id);
                }
                dataframe::Expr e;
                {
                    std::lock_guard<std::mutex> lock(cache->mu);
                    auto it = cache->exprs.find(types);
                    if (it == cache->exprs.end()) {
                        auto built =
                            condition ? duql::vectorize_condition(*t, typed, fb)
                                      : duql::vectorize(*t, typed, fb);
                        if (!built)
                            throw std::invalid_argument(
                                (dstd::String(built.error().message) + " in '" +
                                 text + "'")
                                    .c_str());
                        it = cache->exprs.emplace(types, std::move(*built))
                                 .first;
                    }
                    e = it->second;
                }
                // Series*>
                dstd::Vector<const dataframe::Series*> inputs;
                for (const auto& c : f.columns) inputs.push_back(&c);
                dataframe::Series out = dataframe::eval(e, inputs);
                f.names.emplace_back(name);
                f.columns.push_back(std::move(out));
                return f;
            },
            std::move(next), "per batch " + name + " = " + text);
        return dataframe::expr_col(static_cast<std::int32_t>(cols_.size() - 1));
    }

    // `next` as the columns, each batch passed through `fn`.
    // Each batch reaches `fn` with the current columns in order: a scan of
    // records gives a batch only the fields its records hold.
    void map_rows(MapSource::Map fn, dstd::Vector<duql::VectorColumn> next,
                  dstd::String line) {
        lines.push_back(std::move(line));
        dataframe::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in;
        for (const auto& c : cols_) in.emplace_back(c.name, c.type.id);
        v_ = v_.with_lazy(
            dataframe::LazyFrame::scan(dstd::make_shared<MapSource>(
                v_.lazy(), std::move(schema),
                [in = std::move(in),
                 fn = std::move(fn)](dataframe::DataFrame f) {
                    return fn(aligned(std::move(f), in));
                })));
        cols_ = std::move(next);
    }

    // A hidden column for each quantifier and each lookup in `t`, inner ones
    // first, which the vectorizer compiles them to. `t` stays alive with the
    // plan.
    void hidden(const duql::TermRef& t) {
        // A call inside a quantifier's condition reads the element, which
        // only the quantifier's own frame holds.
        dstd::Vector<const duql::Term*> inner;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* q = std::get_if<duql::TQuant>(&x.node))
                duql::for_each_term(*q->cond, [&](const duql::Term& y) {
                    inner.push_back(&y);
                });
        });
        // The vectorizer reads a list literal that contains() tests directly.
        dstd::Vector<const duql::Term*> literal;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* c = std::get_if<duql::TCall>(&x.node);
                c && c->fn == duql::Fn::Contains &&
                std::holds_alternative<duql::TList>(c->args[0]->node))
                literal.push_back(c->args[0].get());
        });
        dstd::Vector<const duql::Term*> found;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            const auto* l = std::get_if<duql::TLookup>(&x.node);
            if (std::holds_alternative<duql::TQuant>(x.node) ||
                (l && !l->all) ||
                (duql::is_column_term(x) &&
                 std::find(inner.begin(), inner.end(), &x) == inner.end() &&
                 std::find(literal.begin(), literal.end(), &x) ==
                     literal.end()))
                found.push_back(&x);
        });
        for (auto it = found.rbegin(); it != found.rend(); ++it) {
            if (std::holds_alternative<duql::TQuant>((*it)->node))
                quantifier(t, *it);
            else if (duql::is_column_term(**it))
                call(t, *it);
            else
                lookup(t, *it);
        }
    }

    // The fields of the object `f` names into `keys`, `args` and `types`:
    // its one struct column, or the columns under its path, one per key; a
    // key over deeper columns reads whether any of them holds a value.
    void object_fields(const duql::TField& f, const dstd::String& text,
                       bool values, dstd::Vector<dstd::String>& keys,
                       dstd::Vector<dataframe::Expr>& args,
                       dstd::Vector<dataframe::DataType>& types) {
        if (f.root != duql::FieldRoot::Record || f.neg_at != f.steps.size())
            return;
        if (auto i = field_index(dstd::String(f.base))) {
            if (cols_[*i].type.id != dataframe::TypeId::Struct) return;
            for (const auto& x : cols_[*i].type.fields)
                keys.emplace_back(x.name);
            args.push_back(col(cols_[*i].name));
            types.push_back(cols_[*i].type);
            return;
        }
        dstd::String prefix = dstd::String(f.base + ".");
        if (schema_.args_fallback &&
            !std::any_of(cols_.begin(), cols_.end(), [&](const auto& c) {
                return c.name.starts_with(prefix);
            }))
            prefix = "args." + prefix;
        dstd::Map<dstd::String, dstd::Vector<std::size_t>> under;
        for (std::size_t i = 0; i < cols_.size(); ++i) {
            if (!cols_[i].name.starts_with(prefix)) continue;
            const dstd::String rest = cols_[i].name.substr(prefix.size());
            under[rest.substr(0, rest.find_first_of(".["))].push_back(i);
        }
        for (const auto& [key, at] : under) {
            const dstd::String direct = prefix + key;
            keys.push_back(key);
            if (at.size() == 1 && cols_[at.front()].name == direct &&
                !cols_[at.front()].json) {
                args.push_back(col(direct));
                types.push_back(cols_[at.front()].type);
                continue;
            }
            if (values)
                refuse("'" + text + "' reads '" + direct +
                       "', which holds an object or list; only a scan "
                       "filter evaluates it");
            dataframe::Expr any;
            for (const std::size_t i : at) {
                const dataframe::Expr has =
                    dataframe::expr_is_null(col(cols_[i].name), false);
                any = any.valid() ? dataframe::expr_logical(
                                        dataframe::LogicalOp::Or, any, has)
                                  : has;
            }
            args.push_back(dataframe::expr_select(
                any, dataframe::expr_lit_bool(true),
                dataframe::expr_lit_null(dataframe::TypeId::Bool)));
            types.push_back(dataframe::scalar(dataframe::TypeId::Bool));
        }
    }

    // The column of a call, computed index or list literal only
    // call_column() computes.
    void call(const duql::TermRef& t, const duql::Term* term) {
        const auto* c = std::get_if<duql::TCall>(&term->node);
        const auto* ix = std::get_if<duql::TIndex>(&term->node);
        const auto* ls = std::get_if<duql::TList>(&term->node);
        if (std::any_of(cols_.begin(), cols_.end(), [&](const auto& x) {
                return (c && x.call == c) || (ix && x.index == ix) ||
                       (ls && x.list == ls);
            }))
            return;
        const dstd::String text = dstd::String(duql::term_text(*term));
        dstd::Vector<const duql::Term*> operands;
        if (c)
            for (const auto& a : c->args) operands.push_back(a.get());
        else if (ix)
            operands = {ix->array.get(), ix->index.get()};
        else
            for (const auto& a : ls->items) operands.push_back(a.get());
        dstd::Vector<dataframe::Expr> args;
        dstd::Vector<dataframe::DataType> arg_types;
        dstd::Vector<dstd::String> keys;
        if (c && (c->fn == duql::Fn::Keys || c->fn == duql::Fn::Values)) {
            if (const auto* f = std::get_if<duql::TField>(&c->args[0]->node))
                object_fields(*f, text, c->fn == duql::Fn::Values, keys, args,
                              arg_types);
        } else {
            for (const auto* a : operands) {
                if (const auto* inner = std::get_if<duql::TCall>(&a->node);
                    inner && inner->fn == duql::Fn::ParseJson)
                    refuse("'" + text +
                           "' reads a parse_json() value, which only a scan "
                           "filter evaluates");
                auto e = duql::vectorize(*a, cols_, schema_.args_fallback);
                if (!e)
                    refuse(dstd::String(e.error().message + " in '") + text +
                           "'");
                args.push_back(std::move(*e));
                arg_types.push_back(
                    ls && duql::is_null_term(*a, cols_, schema_.args_fallback)
                        ? dataframe::scalar(dataframe::TypeId::Unknown)
                        : dataframe::infer_type(args.back(), types()));
            }
        }
        const dstd::String name = "__duql_c_" + dstd::to_string(calls_++);
        auto mark = [&](duql::VectorColumn& col) {
            col.call = c;
            col.index = ix;
            col.list = ls;
        };
        // An operand of no known type: each batch types the call from its
        // own operand columns, as a plan that declared them so would.
        dstd::Vector<bool> per_batch(operands.size(), false);
        for (std::size_t i = 0; i < operands.size() && i < arg_types.size();
             ++i)
            per_batch[i] = arg_types[i].id == dataframe::TypeId::Unknown &&
                           untyped(*operands[i]);
        if (std::find(per_batch.begin(), per_batch.end(), true) !=
            per_batch.end()) {
            dstd::Vector<duql::VectorColumn> next = cols_;
            next.push_back(
                {name, dataframe::scalar(dataframe::TypeId::Unknown)});
            mark(next.back());
            map_rows(
                [term, args, keys, arg_types, per_batch, text,
                 name](dataframe::DataFrame f) {
                    // Series*>
                    dstd::Vector<const dataframe::Series*> inputs;
                    for (const auto& x : f.columns) inputs.push_back(&x);
                    dstd::Vector<dataframe::Series> values;
                    dstd::Vector<dataframe::DataType> types = arg_types;
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        values.push_back(dataframe::eval(args[i], inputs));
                        if (per_batch[i]) types[i] = values.back().data_type();
                    }
                    dstd::Optional<dataframe::DataType> type;
                    try {
                        type = duql::call_type(*term, types);
                    } catch (const std::invalid_argument& e) {
                        throw std::invalid_argument(
                            ("'" + text + "': " + e.what()).c_str());
                    }
                    const std::int64_t rows = f.num_rows();
                    f.names.emplace_back(name);
                    f.columns.push_back(
                        type ? duql::call_column(*term, values, keys, *type,
                                                 rows)
                             : dataframe::Series::nulls(dataframe::TypeId::Bool,
                                                        rows));
                    return f;
                },
                std::move(next), "call " + name + " per batch = " + text);
            return;
        }
        dstd::Optional<dataframe::DataType> type;
        try {
            type = duql::call_type(*term, arg_types);
        } catch (const std::invalid_argument& e) {
            refuse("'" + text + "': " + e.what());
        }
        if (!type) {
            with_column(name, dataframe::expr_lit_null(dataframe::TypeId::Bool),
                        text);
            cols_.back().type = dataframe::scalar(dataframe::TypeId::Unknown);
            mark(cols_.back());
            return;
        }
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({name, *type, c && c->fn == duql::Fn::ParseJson, false});
        mark(next.back());
        map_rows(
            [t, term, args, keys, type = *type, name](dataframe::DataFrame f) {
                // Series*>
                dstd::Vector<const dataframe::Series*> inputs;
                for (const auto& x : f.columns) inputs.push_back(&x);
                dstd::Vector<dataframe::Series> values;
                for (const auto& a : args)
                    values.push_back(dataframe::eval(a, inputs));
                const std::int64_t rows = f.num_rows();
                f.names.emplace_back(name);
                f.columns.push_back(
                    duql::call_column(*term, values, keys, type, rows));
                return f;
            },
            std::move(next), "call " + name + " = " + text);
    }

    // Binds the pipeline's lookups before a batch reads them.
    static void ready(const dstd::SharedPtr<const detail::BuildStep>& step) {
        if (step) step->run(dstd::nullopt);
    }

    void quantifier(const duql::TermRef& t, const duql::Term* term) {
        const auto* q = &std::get<duql::TQuant>(term->node);
        if (std::any_of(cols_.begin(), cols_.end(),
                        [q](const auto& c) { return c.quant == q; }))
            return;
        if (const auto* f = std::get_if<duql::TField>(&q->subject->node);
            f && f->root == duql::FieldRoot::Record &&
            !array_field(*q->subject))
            refuse("'" + duql::term_text(*term) +
                   "' walks a negative index; only a scan filter "
                   "evaluates it");
        if (const auto* f = array_field(*q->subject))
            if (const auto i = field_index(dstd::String(f->base));
                i && cols_[*i].json)
                refuse("'" + duql::term_text(*term) + "' reads '" + f->base +
                       "', whose elements mix types; only a scan "
                       "filter evaluates it");
        const dstd::String name = "__duql_q_" + dstd::to_string(quants_++);
        dstd::Vector<duql::VectorColumn> in = cols_;
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({name, dataframe::scalar(dataframe::TypeId::Bool), false,
                        false, q});
        map_rows(
            [t, q, in = std::move(in), name, step = step_,
             fb = schema_.args_fallback](dataframe::DataFrame f) {
                ready(step);
                // Series*>
                dstd::Vector<const dataframe::Series*> inputs;
                for (const auto& c : f.columns) inputs.push_back(&c);
                auto truth = duql::quantify(*q, inputs, in, fb);
                if (!truth) refuse(truth.error().message);
                f.names.emplace_back(name);
                f.columns.push_back(std::move(*truth));
                return f;
            },
            std::move(next),
            "quantify " + name + " = " + dstd::String(duql::term_text(*term)));
    }

    // The type of the column `name` of side `side`, Unknown when it has none.
    dataframe::DataType side_type(std::size_t side,
                                  dstd::StringView name) const {
        const auto& cols = ctx_.sides->columns[side];
        for (const auto& c : cols)
            if (c.name == name) return c.type;
        if (schema_.args_fallback)
            for (const auto& c : cols)
                if (c.name == "args." + dstd::String(name)) return c.type;
        return dataframe::scalar(dataframe::TypeId::Unknown);
    }

    // The columns of `l`'s keys over the rows, computed apart when a key is
    // not a column; `t` owns `l`.
    dstd::Vector<dstd::String> key_columns(const duql::TermRef& t,
                                           const duql::TLookup& l,
                                           const dstd::String& id) {
        dstd::Vector<dstd::String> out;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const duql::TermRef key(t, l.keys[k].get());
            out.push_back(source({"", key, duql::term_text(*key)},
                                 "__duql_lk" + id + "_" + dstd::to_string(k)));
        }
        return out;
    }

    // The side of a joined `l`, with `marker` a TRUE column of that name;
    // in `right`, the columns it matches on.
    dataframe::LazyFrame joined_side(const duql::TLookup& l,
                                     dstd::Vector<dstd::String>& right,
                                     bool& memory, const dstd::String& marker) {
        const auto& side_cols = ctx_.sides->columns[l.side];
        if (l.kind == duql::LookupKind::In) {
            if (side_cols.size() != l.keys.size())
                refuse(dstd::String("the sub-query '" + l.name + "' gives ") +
                       dstd::to_string(side_cols.size()) +
                       " columns; 'in' compares " +
                       dstd::to_string(l.keys.size() - l.correlated));
            for (const auto& c : side_cols) right.push_back(c.name);
        } else {
            right.clear();
            for (const auto& t : l.target) right.emplace_back(t);
        }
        dataframe::LazyFrame side = side_plan(l.side, memory);
        if (marker.empty()) return side;
        return side.with_column(std_str(marker),
                                dataframe::expr_lit_bool(true));
    }

    // Whether `l`'s keys are columns of one exact type each, the type of
    // the side's column, as a typed semi join matches them.
    bool typed_keys(const duql::TLookup& l) const {
        const auto& side_cols = ctx_.sides->columns[l.side];
        if (side_cols.size() != l.keys.size()) return false;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const auto* f = whole_field(l.keys[k].get());
            const auto i = f ? index(f->base) : dstd::nullopt;
            if (!i) return false;
            const dataframe::DataType& a = cols_[*i].type;
            const dataframe::DataType& b = side_cols[k].type;
            const bool exact = is_integer(a.id) ||
                               a.id == dataframe::TypeId::String ||
                               a.id == dataframe::TypeId::Bool;
            if (!exact || a.id != b.id || a.is_json() || b.is_json())
                return false;
        }
        return true;
    }

    // `k in (...)`, a top-level term of a `where`: the rows whose keys match
    // a row of the side, by a semi join that narrows the scan.
    void semi_join(const duql::TermRef& t, const duql::TLookup& l) {
        const dstd::Vector<dstd::String> before = names();
        const dstd::String id = dstd::to_string(lookups_++);
        const dstd::Vector<dstd::String> left = key_columns(t, l, id);
        dstd::Vector<dstd::String> right;
        bool memory = false;
        dataframe::LazyFrame side = joined_side(l, right, memory, "");
        lines.push_back(dstd::String("semi join " + l.name + " on ") +
                        joined(left) +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        v_ = v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                         std_strs(right),
                                         dataframe::JoinHow::Semi));
        if (names() != before) keep(before);
    }

    // A joined lookup's column: `in` through a lookup join that attaches a
    // marker, a keyed scalar through a nest join that counts the matches.
    void join_lookup(const duql::TermRef& t, const duql::Term* term) {
        const auto* l = &std::get<duql::TLookup>(term->node);
        const dstd::Vector<dstd::String> before = names();
        const dstd::String id = dstd::to_string(lookups_++);
        const dstd::String name = "__duql_l_" + id;
        const dstd::Vector<dstd::String> left = key_columns(t, *l, id);
        dstd::Vector<dstd::String> right;
        bool memory = false;
        const bool in = l->kind == duql::LookupKind::In;
        const dstd::String matched = "__duql_lm_" + id;
        dataframe::LazyFrame side =
            joined_side(*l, right, memory, in ? matched : dstd::String());
        const dstd::String how = in ? "lookup join " : "nest join ";
        lines.push_back(how + dstd::String(l->name) + " on " + joined(left) +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        dstd::Optional<std::size_t> field;
        dataframe::DataType type = dataframe::scalar(dataframe::TypeId::Bool);
        if (in) {
            v_ = v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                             std_strs(right),
                                             dataframe::JoinHow::Lookup));
            cols_.push_back(
                {matched, dataframe::scalar(dataframe::TypeId::Bool)});
        } else {
            dstd::Vector<dataframe::Field> fields;
            const auto& side_cols = ctx_.sides->columns[l->side];
            for (std::size_t c = 0; c < side_cols.size(); ++c) {
                fields.push_back(dataframe::Field{std_str(side_cols[c].name),
                                                  side_cols[c].type, true});
                if (side_cols[c].name == dstd::String(l->column)) field = c;
            }
            if (!field)
                refuse("'" + l->name + "' has no column '" + l->column + "'");
            type = side_cols[*field].type;
            v_ = v_.with_lazy(
                v_.lazy().join(std::move(side), std_strs(left), std_strs(right),
                               dataframe::JoinHow::Nest, std_str(matched)));
            cols_.push_back({matched, dataframe::list_of(dataframe::struct_of(
                                          std::move(fields)))});
        }
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({name, type, false, false, nullptr, l});
        dstd::SharedPtr<Sides> strong = main_ ? ctx_.sides : nullptr;
        dstd::WeakPtr<Sides> weak = ctx_.sides;
        const std::size_t subject = l->keys.size() - l->correlated;
        const bool negated = l->negated;
        const dstd::String sub = dstd::String(l->name);
        const dstd::Optional<std::size_t> empty = l->empty;
        map_rows(
            [=](dataframe::DataFrame f) {
                auto at =
                    [&](const dstd::String& n) -> const dataframe::Series& {
                    for (std::size_t i = 0; i < f.names.size(); ++i)
                        if (dstd::StringView(f.names[i]) == n)
                            return f.columns[i];
                    refuse("the join of '" + sub + "' lost column '" + n + "'");
                };
                dstd::Vector<dataframe::Series> keys;
                for (const auto& k : left) keys.push_back(at(k).materialize());
                // Series*>
                dstd::Vector<const dataframe::Series*> ptrs;
                for (const auto& k : keys) ptrs.push_back(&k);
                dataframe::Series out;
                if (in) {
                    out = duql::in_column(ptrs, subject,
                                          at(matched).materialize(), negated);
                } else {
                    dstd::SharedPtr<const dataframe::DataFrame> none;
                    if (empty)
                        none = (strong ? strong : owner(weak))->frame(*empty);
                    out = duql::scalar_column(dstd::String(sub), ptrs,
                                              at(matched).materialize(), *field,
                                              none.get());
                }
                f.names.emplace_back(name);
                f.columns.push_back(std::move(out));
                return f;
            },
            std::move(next),
            "lookup " + name + " = " + dstd::String(duql::term_text(*term)));
        dstd::Vector<dstd::String> keep_names = before;
        keep_names.push_back(name);
        keep(std::move(keep_names));
    }

    void lookup(const duql::TermRef& t, const duql::Term* term) {
        const auto* l = &std::get<duql::TLookup>(term->node);
        if (std::any_of(cols_.begin(), cols_.end(),
                        [l](const auto& c) { return c.lookup == l; }))
            return;
        if (duql::joined(*l)) {
            join_lookup(t, term);
            return;
        }
        dataframe::DataType type = dataframe::scalar(dataframe::TypeId::Bool);
        if (l->kind == duql::LookupKind::Arrow) {
            type = side_type(l->side, l->column);
        } else if (l->kind == duql::LookupKind::Scalar) {
            const auto& cols = ctx_.sides->columns[l->side];
            if (l->range == duql::RangeRead::Sum) {
                const dataframe::TypeId v = side_type(l->side, l->column).id;
                type = dataframe::scalar(v == dataframe::TypeId::Float32 ||
                                                 v == dataframe::TypeId::Float64
                                             ? dataframe::TypeId::Float64
                                             : dataframe::TypeId::Int64);
            } else if (l->range == duql::RangeRead::Mean) {
                type = dataframe::scalar(dataframe::TypeId::Float64);
            } else if (l->range == duql::RangeRead::Count ||
                       l->range == duql::RangeRead::CountValues ||
                       l->range == duql::RangeRead::CountIf) {
                type = dataframe::scalar(dataframe::TypeId::Int64);
            } else if (!l->keys.empty())
                type = side_type(l->side, l->column);
            else if (!cols.empty())
                type = side_type(l->side, cols.front().name);
        }
        const dstd::String name = "__duql_l_" + dstd::to_string(lookups_++);
        dstd::Vector<duql::VectorColumn> in = cols_;
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({name, type, false, false, nullptr, l});
        map_rows(
            [t, l, in = std::move(in), name, step = step_,
             fb = schema_.args_fallback](dataframe::DataFrame f) {
                ready(step);
                // Series*>
                dstd::Vector<const dataframe::Series*> inputs;
                for (const auto& c : f.columns) inputs.push_back(&c);
                auto values = duql::lookup(*l, inputs, in, fb);
                if (!values) refuse(values.error().message);
                f.names.emplace_back(name);
                f.columns.push_back(std::move(*values));
                return f;
            },
            std::move(next),
            "lookup " + name + " = " + dstd::String(duql::term_text(*term)));
    }

    void with_column(const dstd::String& name, const dataframe::Expr& e,
                     const dstd::String& text) {
        dataframe::DataType type = dataframe::infer_type(e, types());
        v_ = v_.with_column(std_str(name), e);
        if (auto i = index(name))
            cols_[*i] = {name, std::move(type), false};
        else
            cols_.push_back({name, std::move(type), false});
        lines.push_back("with_column " + name + " = " + text);
    }

    // Keeps the `keep` columns. A keep that only drops the stages' own
    // columns keeps the columns the scan returns beyond the schema too.
    void keep(dstd::Vector<dstd::String> keep, bool rest = true) {
        dstd::Vector<duql::VectorColumn> next;
        for (const auto& n : keep) {
            const auto i = index(n);
            if (!i) refuse("has no column '" + n + "'");
            next.push_back(cols_[*i]);
        }
        lines.push_back("select " + joined(keep));
        if (rest) keep.emplace_back(dataframe::REST_COLUMN);
        v_ = v_.with_lazy(v_.lazy().select(std_strs(keep)));
        cols_ = std::move(next);
    }

    void rename(dstd::Vector<dstd::String> to) {
        for (std::size_t i = 0; i < to.size(); ++i) cols_[i].name = to[i];
        lines.push_back("rename " + joined(to));
        v_ = v_.with_lazy(v_.lazy().rename(std_strs(to)));
    }

    // The column `it` names when it is its own column, else a hidden column
    // `tmp` computed from it.
    dstd::String source(const duql::PipelineItem& it, dstd::String tmp) {
        const auto* f = std::get_if<duql::TField>(&it.term->node);
        if (f && f->base == it.name && f->neg_at == f->steps.size() &&
            index(it.name))
            return dstd::String(it.name);
        if (const auto* w = whole_field(it.term)) {
            if (index(w->base)) return dstd::String(w->base);
            if (arg_index(dstd::String(w->base)))
                return dstd::String("args." + w->base);
        }
        with_column(tmp, compile(it.term, dstd::String(it.text)),
                    dstd::String(it.text));
        return tmp;
    }

    // The items as the only columns, in order, under their names. An item
    // that is its own column stays; the others are computed apart first so
    // no item reads another's result.
    void project(const dstd::Vector<duql::PipelineItem>& items) {
        extras_.clear();
        dstd::Vector<dstd::String> sources;
        dstd::Vector<dstd::String> final_names;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const dstd::String tmp = "__duql_" + dstd::to_string(i);
            dstd::String from = source(items[i], tmp);
            if (std::find(sources.begin(), sources.end(), from) !=
                sources.end()) {
                with_column(tmp, col(from), dstd::String(items[i].text));
                from = tmp;
            }
            sources.push_back(std::move(from));
            final_names.push_back(dstd::String(items[i].name));
        }
        keep(std::move(sources), false);
        if (names() != final_names) rename(std::move(final_names));
    }

    void stage(const duql::PipelineWhere& w) {
        leave_scan("where");
        if (const auto* l = std::get_if<duql::TLookup>(&w.condition->node);
            l && l->kind == duql::LookupKind::In && !l->negated &&
            duql::joined(*l) && typed_keys(*l)) {
            semi_join(w.condition, *l);
            return;
        }
        v_ = v_.filter(compile(w.condition, dstd::String(w.text), true));
        lines.push_back(dstd::String("filter after scan: " + w.text));
    }

    void stage(const duql::PipelineSelect& s) {
        leave_scan("select");
        project(s.items);
    }

    void stage(const duql::PipelineDerive& d) {
        leave_scan("derive");
        for (const auto& it : d.items)
            with_column(dstd::String(it.name),
                        compile(it.term, dstd::String(it.text)),
                        dstd::String(it.text));
    }

    void stage(const duql::PipelineDrop& d) {
        leave_scan("drop");
        cols_.erase(std::remove_if(cols_.begin(), cols_.end(),
                                   [&](const duql::VectorColumn& c) {
                                       return std::any_of(
                                           d.names.begin(), d.names.end(),
                                           [&](const dstd::String& n) {
                                               return dstd::StringView(n) ==
                                                      c.name;
                                           });
                                   }),
                    cols_.end());
        lines.push_back("drop " + joined(d.names));
        v_ = v_.with_lazy(v_.lazy().drop(std_strs(strings_of(d.names))));
    }

    void stage(const duql::PipelineRename& r) {
        leave_scan("rename");
        dstd::Vector<dstd::String> from, to;
        for (const auto& [next, old] : r.pairs) {
            from.push_back(dstd::String(old));
            to.push_back(dstd::String(next));
        }
        dstd::Vector<dstd::String> after = names();
        for (std::size_t k = 0; k < from.size(); ++k)
            if (const auto i = index(from[k])) after[*i] = to[k];
        for (std::size_t i = 0; i < cols_.size(); ++i) cols_[i].name = after[i];
        lines.push_back("rename_columns " + joined(from) + " -> " + joined(to));
        v_ = v_.with_lazy(
            v_.lazy().rename_columns(std_strs(from), std_strs(to)));
    }

    void stage(const duql::PipelineDistinct& d) {
        leave_scan("distinct");
        if (!d.items.empty()) project(d.items);
        v_ = v_.with_lazy(v_.unique());
        lines.push_back("unique");
    }

    struct SortColumns {
        dstd::Vector<dstd::String> by;
        dstd::Vector<bool> desc;
        dstd::Vector<dstd::String> keys;  ///< `by` without the null flags.
    };

    // The columns and directions `keys` sort by, computing the ones that are
    // not columns.
    SortColumns sort_columns(const dstd::Vector<duql::PipelineSortKey>& keys) {
        SortColumns out;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto& key = keys[k];
            dstd::String name = dstd::String(key.key.name);
            if (!index(name)) {
                name = "__duql_sort_" + dstd::to_string(k);
                with_column(name,
                            compile(key.key.term, dstd::String(key.key.text)),
                            dstd::String(key.key.text));
            }
            if (key.nulls_first) {
                dstd::String flag = "__duql_nulls_" + dstd::to_string(k);
                with_column(flag, dataframe::expr_is_null(col(name)),
                            dstd::String(key.key.text + " is null"));
                out.by.push_back(std::move(flag));
                out.desc.push_back(true);
            }
            out.by.push_back(name);
            out.desc.push_back(key.descending);
            out.keys.push_back(name);
        }
        return out;
    }

    void sort(const dstd::Vector<duql::PipelineSortKey>& keys) {
        const dstd::Vector<dstd::String> before = names();
        SortColumns c = sort_columns(keys);
        lines.push_back("sort_by_multi " + joined(c.by));
        v_ = v_.sort_by_multi(std_strs(c.by), std::move(c.desc));
        if (names() != before) keep(before);
    }

    void stage(const duql::PipelineSort& s) {
        leave_scan("sort");
        sort(s.keys);
    }

    void stage(const duql::PipelineTake& t) {
        leave_scan("take");
        v_ = v_.head(t.count);
        lines.push_back("head " + dstd::to_string(t.count));
    }

    void stage(const duql::PipelineSkip& s) {
        leave_scan("skip");
        v_ = v_.slice(s.count, std::numeric_limits<std::int64_t>::max());
        lines.push_back("slice " + dstd::to_string(s.count));
    }

    void stage(const duql::PipelineTakeBy& t) {
        leave_scan("take");
        const dstd::Vector<dstd::String> before = names();
        if (!t.order.empty()) sort(t.order);
        dstd::Vector<dstd::String> keys;
        for (std::size_t k = 0; k < t.keys.size(); ++k)
            keys.push_back(
                source(t.keys[k], "__duql_by_" + dstd::to_string(k)));
        lines.push_back("head_by " + joined(keys) + " " +
                        dstd::to_string(t.count));
        v_ = v_.with_lazy(v_.lazy().head_by(std_strs(keys), t.count));
        if (names() != before) keep(before);
    }

    void stage(const duql::PipelineSample& s) {
        leave_scan("sample");
        const dstd::String seed = " seed " + dstd::to_string(s.seed);
        if (!s.percent) {
            const auto n = static_cast<std::int64_t>(s.amount);
            lines.push_back("sample " + dstd::to_string(n) + seed);
            v_ = v_.with_lazy(v_.lazy().sample(n, s.seed));
            return;
        }
        const dstd::Vector<dstd::String> before = names();
        const dstd::String row = "__duql_row";
        v_ = v_.with_lazy(v_.lazy().with_row_index(std_str(row)));
        cols_.insert(cols_.begin(),
                     {row, dataframe::scalar(dataframe::TypeId::Int64), false});
        const dataframe::Expr hash = dataframe::expr_cast(
            dataframe::TypeId::Float64,
            dataframe::expr_prim(
                dataframe::PrimOp::Mix64,
                dataframe::expr_arith(
                    dataframe::ArithOp::Add, col(row),
                    dataframe::expr_lit(
                        static_cast<std::int64_t>(s.seed & SEED_MASK)))));
        // mix64 is signed: uniform over [-2^63, 2^63).
        if (s.amount < 100)
            v_ = v_.filter(dataframe::expr_cmp_expr(
                dataframe::CmpOp::Lt, hash,
                dataframe::expr_lit(-9223372036854775808.0 +
                                    s.amount / 100.0 *
                                        18446744073709551616.0)));
        lines.push_back("sample " + number_text(s.amount) + "%" + seed);
        keep(before);
    }

    // Microseconds per unit of the record schema's time role.
    double us_per_time_unit() const {
        return static_cast<double>(roles_.time_ns_per_unit) / 1000.0;
    }

    void stage(const duql::PipelineTimeRange& t) {
        if (scan_ && !t.overlap && std::isfinite(t.low) &&
            std::isfinite(t.high)) {
            v_ = v_.time_range(t.low * us_per_time_unit(),
                               t.high * us_per_time_unit());
            lines.push_back("scan time_range " + number_text(t.low) + " .. " +
                            number_text(t.high) + " (pushed)");
            return;
        }
        if (scan_) {
            v_ = v_.filter(duql::Query::from_node(
                duql::make_node(duql::ExprLeaf{t.condition, t.text})));
            lines.push_back(
                dstd::String("scan filter: " + t.text + " (pushed)"));
            return;
        }
        v_ = v_.filter(compile(t.condition, dstd::String(t.text), true));
        lines.push_back(dstd::String("filter after scan: " + t.text));
    }

    void stage(const duql::PipelineBucket& b) { bucket_ = b; }

    void stage(const duql::PipelineCallTree&) {
        scan_ = false;
        blocker_ = "call_tree";
        lines.push_back("call_tree pid, tid");
        v_ =
            v_.with_lazy(v_.call_tree({"pid", "tid"}, dstd::String(roles_.time),
                                      dstd::String(roles_.duration), "name"));
        columns_from_view();
        dstd::Vector<dstd::String> to = names();
        for (auto& n : to) {
            if (n == "level") n = "depth";
            if (n == "parent_id") n = "parent";
        }
        rename(std::move(to));
    }

    void row_index(const dstd::String& name) {
        v_ = v_.with_lazy(v_.lazy().with_row_index(std_str(name)));
        cols_.insert(
            cols_.begin(),
            {name, dataframe::scalar(dataframe::TypeId::Int64), false});
    }

    dstd::Optional<std::size_t> field_index(const dstd::String& field) const {
        if (auto i = index(field)) return i;
        return arg_index(field);
    }

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
                   const dstd::Vector<WinSpec>& specs) {
        if (specs.empty()) return;
        dstd::Vector<const char*> keys;
        for (const auto& k : part) keys.push_back(k.c_str());
        const char* by = order.c_str();
        dstd::Vector<dftu_window_spec> raw;
        dstd::Vector<dstd::String> out = names();
        for (const auto& s : specs) {
            dftu_window_spec w{};
            w.func = s.func;
            w.value = s.value.empty() ? nullptr : s.value.c_str();
            w.out = s.out.c_str();
            if ((s.func >= DFTU_WINDOW_FRAME_SUM &&
                 s.func <= DFTU_WINDOW_FRAME_MEAN) ||
                s.func >= DFTU_WINDOW_FRAME_VAR)
                w.param.frame = {
                    s.offset,    s.preceding,
                    s.following, s.mode,
                    s.q,         s.by.empty() ? nullptr : s.by.c_str()};
            else
                w.param.offset = s.offset;
            raw.push_back(w);
            out.push_back(s.out);
            cols_.push_back({s.out, s.type, false});
        }
        dataframe::OpArgs a;
        a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
            .strlist(2, &by, 1)
            .winlist(3, raw);
        v_ = v_.with_lazy(
            v_.lazy().frame_op("dftu.frame.window", a, {}, std_strs(out)));
    }

    // Aggregates over each whole partition, as `group` gives them, on every
    // row: the rows grouped by their partition id `pid` and joined back.
    void partition_aggs(
        const dstd::Vector<
            dstd::Pair<const duql::PipelineWinCall*, dstd::String>>& calls,
        const dstd::String& pid) {
        dstd::Vector<dataframe::GroupAgg> specs;
        dstd::Vector<dstd::String> quantiles;
        dstd::Vector<duql::VectorColumn> next = cols_;
        dstd::String text;
        for (const auto& [c, in] : calls) {
            dataframe::GroupAgg g{dataframe::Agg::Var, std_str(in),
                                  std_str(c->column)};
            dataframe::DataType type =
                dataframe::scalar(dataframe::TypeId::Float64);
            switch (c->fn) {
                case duql::WinFn::Std:
                    g.op = dataframe::Agg::Std;
                    break;
                case duql::WinFn::Quantile:
                    g.op = dataframe::Agg::Pct;
                    g.param = c->q;
                    quantiles.push_back(dstd::String(c->column));
                    break;
                case duql::WinFn::Histogram:
                    g.op = dataframe::Agg::Hist;
                    type = dataframe::list_of(dataframe::struct_of(
                        {dataframe::Field{
                             "lo",
                             dataframe::scalar(dataframe::TypeId::Float64),
                             true},
                         dataframe::Field{
                             "hi",
                             dataframe::scalar(dataframe::TypeId::Float64),
                             true},
                         dataframe::Field{
                             "count",
                             dataframe::scalar(dataframe::TypeId::Uint64),
                             true}}));
                    break;
                default:
                    break;
            }
            specs.push_back(std::move(g));
            next.push_back({dstd::String(c->column), std::move(type)});
            text += (text.empty() ? "" : ", ") + c->text;
        }
        dataframe::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        const dstd::String spid(pid);
        finish(
            [spid, specs](dataframe::DataFrame f) {
                dataframe::DataFrame g =
                    f.group_by(dstd::Vector<dstd::String>{spid}, specs);
                return f.join(g, {spid}, {spid}, dataframe::JoinHow::Lookup);
            },
            "partition aggregates " + text, std::move(schema));
        cols_ = std::move(next);
        for (const auto& q : quantiles)
            v_ = v_.with_column(
                std_str(q),
                dataframe::expr_select(
                    dataframe::expr_unary(dataframe::UnaryOp::IsNan, col(q)),
                    dataframe::expr_lit_null(dataframe::TypeId::Float64),
                    col(q)));
    }

    dataframe::DataType type_of(const dstd::String& column) const {
        return cols_[*index(column)].type;
    }

    dataframe::DataType sum_type(const dstd::String& in,
                                 const dstd::String& text) {
        const dataframe::TypeId t = type_of(in).id;
        if (t != dataframe::TypeId::Unknown && !is_number(t))
            refuse("'" + text + "' in a window needs numbers");
        return dataframe::scalar(is_integer(t) ? dataframe::TypeId::Int64
                                               : dataframe::TypeId::Float64);
    }

    // Row `a` differs from row `b`, null included.
    dataframe::Expr differs(const dstd::String& a, const dstd::String& b) {
        const dataframe::Expr na = dataframe::expr_is_null(col(a));
        const dataframe::Expr nb = dataframe::expr_is_null(col(b));
        return dataframe::expr_select(
            na, dataframe::expr_not(nb),
            dataframe::expr_select(nb, dataframe::expr_lit_bool(true),
                                   dataframe::expr_cmp_expr(
                                       dataframe::CmpOp::Ne, col(a), col(b))));
    }

    void stage(const duql::PipelineWindow& w) {
        leave_scan("window");
        const dstd::Vector<dstd::String> before = names();
        const dstd::String pos = "__duql_pos";
        row_index(pos);
        dstd::Vector<dstd::String> part;
        dstd::String keys_text;
        for (std::size_t k = 0; k < w.keys.size(); ++k) {
            part.push_back(
                source(w.keys[k], "__duql_part_" + dstd::to_string(k)));
            keys_text += (keys_text.empty() ? "" : ", ") + w.keys[k].text;
        }
        dstd::String ord = pos;
        dstd::Vector<dstd::String> sort_keys;
        dstd::String sort_text;
        if (!w.order.empty()) {
            SortColumns c = sort_columns(w.order);
            sort_keys = c.keys;
            lines.push_back("sort_by_multi " + joined(c.by));
            v_ = v_.sort_by_multi(std_strs(c.by), std::move(c.desc));
            ord = "__duql_ord";
            row_index(ord);
            for (const auto& k : w.order)
                sort_text += (sort_text.empty() ? "" : ", ") +
                             dstd::String(k.descending ? "-" : "") +
                             dstd::String(k.key.text);
        }
        // Passes: over the order, over the reverse order, then over the
        // order again for what reads the first two.
        dstd::Vector<WinSpec> forward, reverse, last, ranged, distribution;
        dstd::Vector<const duql::PipelineWinCall*> ranks;
        dstd::Vector<const duql::PipelineWinCall*> dist;
        dstd::Vector<dstd::Pair<dstd::String, const duql::PipelineWinCall*>>
            edges;
        // A sum, mean, min, max or count over its `over` frame, else over
        // the whole partition.
        const auto framed = [&](WinSpec s, const duql::PipelineWinCall& c) {
            if (!c.frame) {
                forward.push_back(std::move(s));
                return;
            }
            s.offset = 1;
            s.following = 0;
            if (c.frame->range) {
                if (sort_keys.size() != 1) refuse("'over' needs one sort key");
                const dataframe::TypeId t = type_of(sort_keys[0]).id;
                if (t != dataframe::TypeId::Unknown && !is_number(t))
                    refuse("'" + c.text + "' needs a numeric sort key");
                s.preceding = static_cast<std::int64_t>(c.frame->width);
                s.mode = DFTU_WINDOW_FRAME_RANGE;
                ranged.push_back(std::move(s));
            } else {
                s.preceding = c.frame->rows - 1;
                forward.push_back(std::move(s));
            }
        };
        dstd::Vector<dstd::Pair<const duql::PipelineWinCall*, dstd::String>>
            partition;
        const dataframe::DataType i64 =
            dataframe::scalar(dataframe::TypeId::Int64);
        for (std::size_t i = 0; i < w.calls.size(); ++i) {
            const auto& c = w.calls[i];
            dstd::String in;
            if (c.fn == duql::WinFn::CountIf) {
                in = "__duql_in_" + dstd::to_string(i);
                with_column(
                    in,
                    dataframe::expr_select(
                        compile(c.arg, dstd::String(c.text), true),
                        dataframe::expr_lit(std::int64_t{1}),
                        dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                    dstd::String(c.text));
            } else if (c.arg) {
                in = source({"", c.arg, c.text},
                            "__duql_in_" + dstd::to_string(i));
            }
            const dstd::String tmp = "__duql_fill_" + dstd::to_string(i);
            switch (c.fn) {
                case duql::WinFn::RowNumber:
                    forward.push_back({DFTU_WINDOW_ROW_NUMBER, "",
                                       dstd::String(c.column), i64});
                    break;
                case duql::WinFn::RunningCount:
                    forward.push_back({DFTU_WINDOW_RUNNING_COUNT, "",
                                       dstd::String(c.column), i64});
                    break;
                case duql::WinFn::Lag:
                case duql::WinFn::Lead:
                    forward.push_back(
                        {c.fn == duql::WinFn::Lag ? DFTU_WINDOW_LAG
                                                  : DFTU_WINDOW_LEAD,
                         in, dstd::String(c.column), type_of(in), c.offset});
                    if (c.fallback) {
                        const dstd::String edge =
                            "__duql_edge_" + dstd::to_string(i);
                        forward.push_back(
                            {forward.back().func, pos, edge, i64, c.offset});
                        edges.emplace_back(edge, &c);
                    }
                    break;
                case duql::WinFn::RunningMin:
                case duql::WinFn::RunningMax:
                    forward.push_back({c.fn == duql::WinFn::RunningMin
                                           ? DFTU_WINDOW_RUNNING_MIN
                                           : DFTU_WINDOW_RUNNING_MAX,
                                       in, dstd::String(c.column),
                                       type_of(in)});
                    break;
                case duql::WinFn::RunningMean:
                    sum_type(in, dstd::String(c.text));
                    forward.push_back(
                        {DFTU_WINDOW_FRAME_MEAN, in, dstd::String(c.column),
                         dataframe::scalar(dataframe::TypeId::Float64), 1, 0});
                    break;
                case duql::WinFn::Ntile:
                    forward.push_back({DFTU_WINDOW_NTILE, "",
                                       dstd::String(c.column), i64, c.offset});
                    break;
                case duql::WinFn::Nth:
                    forward.push_back({DFTU_WINDOW_NTH_VALUE, in,
                                       dstd::String(c.column), type_of(in),
                                       c.offset});
                    break;
                case duql::WinFn::FillForward:
                    forward.push_back({DFTU_WINDOW_FILL_FORWARD, in,
                                       dstd::String(c.column), type_of(in)});
                    break;
                case duql::WinFn::PercentRank:
                case duql::WinFn::CumeDist:
                    dist.push_back(&c);
                    break;
                case duql::WinFn::RunningSum:
                    forward.push_back(
                        {DFTU_WINDOW_FRAME_SUM, in, dstd::String(c.column),
                         sum_type(in, dstd::String(c.text)), 0, 0});
                    break;
                case duql::WinFn::Count:
                    framed({DFTU_WINDOW_FRAME_COUNT, in.empty() ? pos : in,
                            dstd::String(c.column), i64},
                           c);
                    break;
                case duql::WinFn::Sum:
                    framed({DFTU_WINDOW_FRAME_SUM, in, dstd::String(c.column),
                            sum_type(in, dstd::String(c.text))},
                           c);
                    break;
                case duql::WinFn::Mean:
                    sum_type(in, dstd::String(c.text));
                    framed({DFTU_WINDOW_FRAME_MEAN, in, dstd::String(c.column),
                            dataframe::scalar(dataframe::TypeId::Float64)},
                           c);
                    break;
                case duql::WinFn::Min:
                case duql::WinFn::Max:
                    framed({c.fn == duql::WinFn::Min ? DFTU_WINDOW_FRAME_MIN
                                                     : DFTU_WINDOW_FRAME_MAX,
                            in, dstd::String(c.column), type_of(in)},
                           c);
                    break;
                case duql::WinFn::Last:
                    forward.push_back(
                        {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
                    reverse.push_back({DFTU_WINDOW_FIRST_VALUE, tmp,
                                       dstd::String(c.column), type_of(in)});
                    break;
                case duql::WinFn::First:
                    reverse.push_back(
                        {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
                    last.push_back({DFTU_WINDOW_FIRST_VALUE, tmp,
                                    dstd::String(c.column), type_of(in)});
                    break;
                case duql::WinFn::Rank:
                case duql::WinFn::DenseRank:
                    ranks.push_back(&c);
                    break;
                case duql::WinFn::CountIf:
                    framed({DFTU_WINDOW_FRAME_COUNT, in, dstd::String(c.column),
                            i64},
                           c);
                    break;
                case duql::WinFn::CountDistinct:
                    framed({DFTU_WINDOW_FRAME_COUNT_DISTINCT, in,
                            dstd::String(c.column), i64},
                           c);
                    break;
                case duql::WinFn::Collect:
                    framed(
                        {DFTU_WINDOW_FRAME_COLLECT, in, dstd::String(c.column),
                         dataframe::list_of(type_of(in))},
                        c);
                    break;
                case duql::WinFn::Argmax:
                case duql::WinFn::Argmin: {
                    WinSpec s{c.fn == duql::WinFn::Argmax
                                  ? DFTU_WINDOW_FRAME_ARG_MAX
                                  : DFTU_WINDOW_FRAME_ARG_MIN,
                              in, dstd::String(c.column), type_of(in)};
                    s.by = source({"", c.by, c.text},
                                  "__duql_by_" + dstd::to_string(i));
                    framed(std::move(s), c);
                    break;
                }
                case duql::WinFn::Var:
                case duql::WinFn::Std:
                case duql::WinFn::Quantile:
                    if (c.frame) {
                        sum_type(in, dstd::String(c.text));
                        WinSpec s{
                            c.fn == duql::WinFn::Var ? DFTU_WINDOW_FRAME_VAR
                            : c.fn == duql::WinFn::Std
                                ? DFTU_WINDOW_FRAME_STD
                                : DFTU_WINDOW_FRAME_QUANTILE,
                            in, dstd::String(c.column),
                            dataframe::scalar(dataframe::TypeId::Float64)};
                        s.q = c.q;
                        framed(std::move(s), c);
                        break;
                    }
                    [[fallthrough]];
                case duql::WinFn::Histogram:
                    sum_type(in, dstd::String(c.text));
                    partition.emplace_back(&c, in);
                    break;
            }
        }
        const dstd::String pid = "__duql_pid";
        if (!partition.empty())
            forward.push_back({DFTU_WINDOW_FIRST_VALUE, pos, pid, i64});
        // Ranks: a row starts a run of peers when its sort keys differ from
        // the row before it in the partition.
        const bool peers =
            (!ranks.empty() || !dist.empty()) && !sort_keys.empty();
        if (peers) {
            forward.push_back({DFTU_WINDOW_ROW_NUMBER, "", "__duql_rn", i64});
            for (std::size_t k = 0; k < sort_keys.size(); ++k)
                forward.push_back({DFTU_WINDOW_LAG, sort_keys[k],
                                   "__duql_prev_" + dstd::to_string(k),
                                   type_of(sort_keys[k]), 1});
        }
        window_op(part, ord, forward);
        window_op(part, sort_keys.empty() ? ord : sort_keys[0], ranged);
        for (const auto& [edge, c] : edges)
            with_column(dstd::String(c->column),
                        dataframe::expr_select(
                            dataframe::expr_is_null(col(edge)),
                            compile(c->fallback, dstd::String(c->text)),
                            col(c->column)),
                        dstd::String(c->text));
        if (peers) {
            dataframe::Expr peer =
                dataframe::expr_cmp(dataframe::CmpOp::Eq, col("__duql_rn"),
                                    dataframe::detail::expr_scalar_i(1));
            for (std::size_t k = 0; k < sort_keys.size(); ++k)
                peer = dataframe::expr_logical(
                    dataframe::LogicalOp::Or, peer,
                    differs(sort_keys[k], "__duql_prev_" + dstd::to_string(k)));
            with_column("__duql_peer",
                        dataframe::expr_select(
                            peer, dataframe::expr_lit(std::int64_t{1}),
                            dataframe::expr_lit(std::int64_t{0})),
                        "peer start");
            with_column("__duql_start",
                        dataframe::expr_select(
                            peer, col("__duql_rn"),
                            dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                        "peer start row");
            for (const auto* c : ranks)
                last.push_back(
                    c->fn == duql::WinFn::Rank
                        ? WinSpec{DFTU_WINDOW_FILL_FORWARD, "__duql_start",
                                  dstd::String(c->column), i64}
                        : WinSpec{DFTU_WINDOW_FRAME_SUM, "__duql_peer",
                                  dstd::String(c->column), i64, 0, 0});
            if (!dist.empty())
                last.push_back({DFTU_WINDOW_FRAME_SUM, "__duql_peer",
                                "__duql_dr", i64, 0, 0});
        } else {
            for (const auto* c : ranks)
                with_column(dstd::String(c->column),
                            dataframe::expr_lit(std::int64_t{1}),
                            dstd::String(c->text));
            if (!dist.empty())
                with_column("__duql_dr", dataframe::expr_lit(std::int64_t{1}),
                            "peer group");
        }
        if (!reverse.empty()) {
            with_column("__duql_rev", dataframe::expr_neg(col(ord)), "-" + ord);
            window_op(part, "__duql_rev", reverse);
        }
        window_op(part, ord, last);
        for (const auto* c : dist)
            distribution.push_back(
                {c->fn == duql::WinFn::PercentRank ? DFTU_WINDOW_PERCENT_RANK
                                                   : DFTU_WINDOW_CUME_DIST,
                 "", dstd::String(c->column),
                 dataframe::scalar(dataframe::TypeId::Float64)});
        window_op(part, "__duql_dr", distribution);
        if (!partition.empty()) partition_aggs(partition, pid);
        lines.push_back("sort_by_multi " + pos);
        v_ = v_.sort_by_multi({std_str(pos)}, dstd::Vector<bool>{false});
        dstd::Vector<dstd::String> keep_names = before;
        dstd::String items;
        for (const auto& it : w.items) {
            with_column(dstd::String(it.name),
                        compile(it.term, dstd::String(it.text)),
                        dstd::String(it.text));
            if (std::find(keep_names.begin(), keep_names.end(),
                          dstd::String(it.name)) == keep_names.end())
                keep_names.push_back(dstd::String(it.name));
            items += (items.empty() ? "" : ", ") + it.name + " = " + it.text;
        }
        lines.push_back("window keys " + keys_text + "; sort " + sort_text +
                        "; " + items);
        keep(std::move(keep_names));
    }

    static dftu_scalar plugin_scalar(const duql::LiteralValue& v) {
        dftu_scalar s{};
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            s.kind = DFTU_SCALAR_TAG_I64;
            s.value.i = *i;
        } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            s.kind = DFTU_SCALAR_TAG_U64;
            s.value.u = *u;
        } else if (const auto* d = std::get_if<double>(&v)) {
            s.kind = DFTU_SCALAR_TAG_F64;
            s.value.d = *d;
        } else if (const auto* b = std::get_if<bool>(&v)) {
            s.kind = DFTU_SCALAR_TAG_I64;
            s.value.i = *b ? 1 : 0;
        }
        return s;
    }

    [[noreturn]] static void plugin_refusal(const dstd::String& what,
                                            const dftu_op_desc* op,
                                            const char* why) {
        refuse(dstd::String(why) + " for '" + what + "'; it takes " +
               dftu_op_signature(op->sig));
    }

    // `f` with the column `name` replaced by the op run over it.
    static dataframe::DataFrame run_column_op(
        dataframe::DataFrame f, const dstd::String& name,
        const dftu_op_desc* op, const dstd::String& second, dftu_scalar a,
        dftu_scalar b, const dstd::Optional<dstd::String>& str) {
        dftu_dataframe* in = dataframe::dataframe_handle_wrap(std::move(f));
        dftu_dataframe* out =
            dftu_dataframe_column_op(in, name.c_str(), op->name,
                                     second.empty() ? nullptr : second.c_str(),
                                     a, b, str ? str->c_str() : nullptr);
        dftu_dataframe_free(in);
        if (!out) plugin_refusal(op->name, op, "the arguments do not fit");
        return dataframe::dataframe_handle_take(out);
    }

    void stage(const duql::PipelinePlugin& p) {
        leave_scan("plugin function");
        const dftu_op_desc* op = dftu_op_find(p.op.c_str());
        if (!op) refuse("plugin function '" + p.op + "' is not registered");
        with_column(dstd::String(p.arg.name),
                    compile(p.arg.term, dstd::String(p.arg.text)),
                    dstd::String(p.arg.text));
        dstd::String second;
        if (p.arg2) {
            with_column(dstd::String(p.arg2->name),
                        compile(p.arg2->term, dstd::String(p.arg2->text)),
                        dstd::String(p.arg2->text));
            second = p.arg2->name;
        }
        std::size_t takes_scalars = 0, takes_series = 0, takes_str = 0;
        for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i) {
            const dftu_op_tok tok = DFTU_OP_SIG_ARG(op->sig, i);
            if (tok == DFTU_TOK_NONE) break;
            if (tok == DFTU_TOK_SERIES)
                ++takes_series;
            else if (tok == DFTU_TOK_STR || tok == DFTU_TOK_CHAR)
                ++takes_str;
            else
                ++takes_scalars;
        }
        if (p.scalars.size() > takes_scalars ||
            p.arg2.has_value() > takes_series || p.str.has_value() > takes_str)
            plugin_refusal(dstd::String(p.op), op, "the arguments do not fit");
        const dftu_scalar a =
            p.scalars.size() > 0 ? plugin_scalar(p.scalars[0]) : dftu_scalar{};
        const dftu_scalar b =
            p.scalars.size() > 1 ? plugin_scalar(p.scalars[1]) : dftu_scalar{};
        const std::size_t at = *index(p.arg.name);
        dataframe::DataType type =
            dataframe::scalar(dataframe::TypeId::Unknown);
        bool known = cols_[at].type.id != dataframe::TypeId::Unknown;
        if (p.arg2)
            known = known &&
                    cols_[*index(second)].type.id != dataframe::TypeId::Unknown;
        if (known) {
            dataframe::DataFrame probe;
            probe.names.emplace_back(dstd::String(p.arg.name));
            probe.columns.push_back(
                dataframe::Series::nulls(cols_[at].type.id, 0));
            if (p.arg2) {
                probe.names.emplace_back(second);
                probe.columns.push_back(
                    dataframe::Series::nulls(cols_[*index(second)].type.id, 0));
            }
            const auto out =
                run_column_op(std::move(probe), dstd::String(p.arg.name), op,
                              second, a, b, p.str);
            type = dataframe::scalar(out.columns[0].type());
        }
        dstd::Vector<duql::VectorColumn> next = cols_;
        next[at].type = type;
        map_rows(
            [name = p.arg.name, op, second, a, b,
             str = p.str](dataframe::DataFrame f) {
                return run_column_op(std::move(f), dstd::String(name), op,
                                     second, a, b, str);
            },
            std::move(next),
            dstd::String("plugin " + p.op + " over " + p.arg.name));
    }

    void stage(const duql::PipelineCall& c) {
        leave_scan("call");
        const dftu_op_desc* op = dftu_op_find(c.op.c_str());
        if (!op) refuse("'call " + c.op + "' is not registered");
        const dstd::String what = dstd::String("call " + c.op);
        dataframe::OpArgs args;
        std::size_t used = 0;
        for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i) {
            const dftu_op_tok tok = DFTU_OP_SIG_ARG(op->sig, i);
            if (tok == DFTU_TOK_NONE) break;
            if (used >= c.args.size())
                plugin_refusal(what, op, "too few arguments");
            const duql::LiteralValue& v = c.args[used++];
            const auto* s = std::get_if<dstd::String>(&v);
            const auto* d = std::get_if<double>(&v);
            std::int64_t n = 0;
            if (const auto* x = std::get_if<std::int64_t>(&v))
                n = *x;
            else if (const auto* u = std::get_if<std::uint64_t>(&v))
                n = static_cast<std::int64_t>(*u);
            else if (const auto* b = std::get_if<bool>(&v))
                n = *b ? 1 : 0;
            const bool integer = !s && !d;
            switch (tok) {
                case DFTU_TOK_I64:
                    if (!integer)
                        plugin_refusal(what, op, "an integer is needed");
                    args.i64(i, n);
                    break;
                case DFTU_TOK_I32:
                case DFTU_TOK_BOOL:
                    if (!integer)
                        plugin_refusal(what, op, "an integer is needed");
                    args.i32(i, static_cast<std::int32_t>(n));
                    break;
                case DFTU_TOK_F64:
                    if (s) plugin_refusal(what, op, "a number is needed");
                    args.f64(i, d ? *d : static_cast<double>(n));
                    break;
                case DFTU_TOK_STR:
                    if (!s) plugin_refusal(what, op, "a string is needed");
                    args.str(i, *s);
                    break;
                case DFTU_TOK_SCALAR:
                    if (s) plugin_refusal(what, op, "a number is needed");
                    args.scalar(i, plugin_scalar(v));
                    break;
                default:
                    plugin_refusal(what, op, "an argument of this kind");
            }
        }
        if (used != c.args.size())
            plugin_refusal(what, op, "too many arguments");
        lines.push_back(dstd::String("frame_op " + c.op));
        v_ = v_.with_lazy(v_.lazy().frame_op(std_str(c.op), args));
        cols_.clear();
    }

    void stage(const duql::PipelineSession& ss) {
        leave_scan("session");
        if (index(ss.name))
            refuse("'session' gives column '" + ss.name +
                   "', which is already a column; name it with 'as'");
        dstd::Vector<dstd::String> keep_names = names();
        keep_names.push_back(dstd::String(ss.name));
        const dstd::String pos = "__duql_pos";
        row_index(pos);
        dstd::Vector<dstd::String> part;
        dstd::String keys_text;
        for (std::size_t k = 0; k < ss.keys.size(); ++k) {
            part.push_back(
                source(ss.keys[k], "__duql_part_" + dstd::to_string(k)));
            keys_text += (keys_text.empty() ? "" : ", ") + ss.keys[k].text;
        }
        const dstd::String time = "__duql_time";
        const dstd::String end = "__duql_end";
        with_column(time,
                    compile(ss.time, dstd::String(duql::term_text(*ss.time))),
                    dstd::String(duql::term_text(*ss.time)));
        if (ss.end)
            with_column(end,
                        compile(ss.end, dstd::String(duql::term_text(*ss.end))),
                        dstd::String(duql::term_text(*ss.end)));
        // Time order, ties in input order.
        lines.push_back("sort_by_multi " + time + ", " + pos);
        v_ = v_.sort_by_multi({std_str(time), std_str(pos)},
                              dstd::Vector<bool>{false, false});
        const dstd::String ord = "__duql_ord";
        row_index(ord);

        dstd::Vector<const char*> keys;
        for (const auto& k : part) keys.push_back(k.c_str());
        const char* by = ord.c_str();
        dftu_window_spec w{};
        w.func = DFTU_WINDOW_SESSIONIZE;
        w.out = ss.name.c_str();
        w.param.session = {time.c_str(), ss.end ? end.c_str() : nullptr, ss.gap,
                           ss.span};
        dstd::Vector<dstd::String> out = names();
        out.push_back(dstd::String(ss.name));
        cols_.push_back({dstd::String(ss.name),
                         dataframe::scalar(dataframe::TypeId::Int64), false});
        const dstd::Vector<dftu_window_spec> specs{w};
        dataframe::OpArgs a;
        a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
            .strlist(2, &by, 1)
            .winlist(3, specs);
        v_ = v_.with_lazy(
            v_.lazy().frame_op("dftu.frame.window", a, {}, std_strs(out)));

        lines.push_back("sort_by_multi " + pos);
        v_ = v_.sort_by_multi({std_str(pos)}, dstd::Vector<bool>{false});
        lines.push_back(
            "session keys " + keys_text + "; time " + dstd::String(ss.text) +
            "; gap " + number_text(ss.gap) + "; max " +
            (ss.span > 0 ? number_text(ss.span) : dstd::String("none")) +
            "; as " + dstd::String(ss.name));
        keep(std::move(keep_names));
    }

    void stage(const duql::PipelineExpand& e) {
        leave_scan("expand");
        dstd::String line = dstd::String("expand " + e.path + " as " + e.name);
        if (!e.index.empty()) line += " with_index " + e.index;
        if (e.keep_empty) line += " keep_empty";
        const auto li = field_index(dstd::String(e.path));
        const bool list = li && cols_[*li].type.id == dataframe::TypeId::List;
        const dataframe::DataType elem =
            list && !cols_[*li].type.fields.empty()
                ? cols_[*li].type.fields.front().type
                : dataframe::scalar(dataframe::TypeId::String);
        const bool is_struct = elem.id == dataframe::TypeId::Struct;
        const bool json = list && cols_[*li].json;
        auto replaced = [&](const dstd::String& n) {
            return n == dstd::String(e.name) || n == dstd::String(e.index) ||
                   (is_struct && n.starts_with(e.name + "."));
        };
        dstd::Vector<duql::VectorColumn> next;
        dstd::Vector<bool> drop;
        auto element_columns = [&] {
            if (is_struct)
                for (const auto& f : elem.fields)
                    next.push_back(
                        {dstd::String(e.name + ".").append(f.name), f.type});
            else
                next.push_back({dstd::String(e.name), elem, json});
            if (!e.index.empty())
                next.push_back({dstd::String(e.index),
                                dataframe::scalar(dataframe::TypeId::Int64)});
        };
        for (std::size_t j = 0; j < cols_.size(); ++j) {
            const bool here = li && j == *li;
            drop.push_back(!here && replaced(cols_[j].name));
            if (here)
                element_columns();
            else if (!drop.back())
                next.push_back(cols_[j]);
        }
        if (!li) element_columns();
        const std::size_t at = li ? *li : cols_.size();
        const dstd::String name = dstd::String(e.name);
        const dstd::String idx = dstd::String(e.index);
        const bool keep_empty = e.keep_empty;
        map_rows(
            [at, drop, name, idx, keep_empty](dataframe::DataFrame f) {
                const std::int64_t n = f.num_rows();
                dataframe::Series lists;
                const std::int32_t* off = nullptr;
                if (at < f.columns.size() &&
                    f.columns[at].type() == dataframe::TypeId::List) {
                    lists = f.columns[at].materialize();
                    off = lists.offsets();
                }
                dstd::Vector<std::int64_t> parent;
                dstd::Vector<std::int64_t> element;
                dstd::Vector<std::int64_t> position;
                auto row = [&](std::int64_t r, std::int64_t el,
                               std::int64_t p) {
                    parent.push_back(r);
                    element.push_back(el);
                    position.push_back(p);
                };
                for (std::int64_t r = 0; r < n; ++r) {
                    if (!off || lists.is_null(r)) {
                        row(r, -1, -1);
                    } else if (off[r] == off[r + 1]) {
                        if (keep_empty) row(r, -1, -1);
                    } else {
                        for (std::int32_t k = off[r]; k < off[r + 1]; ++k)
                            row(r, k, k - off[r]);
                    }
                }
                const auto m = static_cast<std::int64_t>(parent.size());
                dataframe::DataFrame out;
                auto elements = [&] {
                    if (off &&
                        lists.child(0).type() == dataframe::TypeId::Struct) {
                        const dataframe::Series child = lists.child(0);
                        for (std::int64_t i = 0; i < child.num_children();
                             ++i) {
                            dstd::String joined_name(name);
                            joined_name.append(".").append(child.field_name(i));
                            out.names.push_back(std::move(joined_name));
                            out.columns.push_back(child.child(i).take(element));
                        }
                    } else if (off) {
                        out.names.emplace_back(name);
                        out.columns.push_back(lists.child(0).take(element));
                    } else {
                        out.names.emplace_back(name);
                        out.columns.push_back(dataframe::Series::nulls(
                            dataframe::TypeId::String, m));
                    }
                    if (idx.empty()) return;
                    dstd::Vector<std::uint8_t> valid(
                        static_cast<std::size_t>((m + 7) / 8), 0);
                    for (std::int64_t r = 0; r < m; ++r)
                        if (position[static_cast<std::size_t>(r)] >= 0)
                            valid[static_cast<std::size_t>(r >> 3)] |=
                                static_cast<std::uint8_t>(1u << (r & 7));
                    out.names.emplace_back(idx);
                    out.columns.push_back(dataframe::Series::flat_i64(
                        position.data(), m, valid.data()));
                };
                for (std::size_t j = 0; j < f.columns.size(); ++j) {
                    if (j == at) {
                        elements();
                    } else if (!drop[j]) {
                        out.names.emplace_back(f.names[j]);
                        out.columns.push_back(f.columns[j].take(parent));
                    }
                }
                if (at >= f.columns.size()) elements();
                return out;
            },
            std::move(next), line);
    }

    // A term reading the column `name` as it is.
    static duql::PipelineItem column_item(const dstd::String& name) {
        duql::TField f;
        f.base = dstd::String(name);
        f.steps.push_back({dstd::String(name), false, dstd::nullopt});
        f.neg_at = 1;
        return {dstd::String(name),
                dstd::make_shared<const duql::Term>(duql::Term{std::move(f)}),
                dstd::String(name)};
    }

    void stage(const duql::PipelinePivot& p) {
        leave_scan("pivot");
        dstd::Vector<dstd::String> read;
        auto reads = [&](const duql::TermRef& t) {
            if (!t) return;
            duql::for_each_term_field(*t, [&](const duql::TField& f) {
                if (auto i = field_index(dstd::String(f.base)))
                    read.push_back(cols_[*i].name);
            });
        };
        reads(p.key.term);
        for (const auto& a : p.aggs) {
            reads(a.arg);
            reads(a.by);
        }
        duql::PipelineGroup g;
        for (const auto& c : cols_) {
            const bool skip =
                std::find(read.begin(), read.end(), c.name) != read.end() ||
                std::find(extras_.begin(), extras_.end(), c.name) !=
                    extras_.end() ||
                c.name == dstd::String(p.key.name);
            if (skip) continue;
            duql::PipelineItem it = column_item(c.name);
            if (c.record && c.name.starts_with("args."))
                it.name = c.name.substr(5);
            g.keys.push_back(std::move(it));
        }
        const std::size_t n_keys = g.keys.size();
        duql::PipelineItem key = p.key;
        key.name = "__duql_pivot";
        g.keys.push_back(std::move(key));
        g.aggs = p.aggs;
        dstd::Vector<dstd::String> fixed;
        for (const auto& v : p.values) fixed.push_back(pivot_name(v));
        lines.push_back(dstd::String("pivot " + p.key.text) +
                        (p.fixed ? " in [" + joined(fixed) + "]"
                                 : " (columns after the scan)"));
        stage(g);
        dstd::Vector<dstd::String> aggs;
        dstd::Vector<duql::VectorColumn> next(cols_.begin(),
                                              cols_.begin() + n_keys);
        for (std::size_t a = 0; a < p.aggs.size(); ++a) {
            aggs.push_back(dstd::String(p.aggs[a].name));
            for (const auto& v : fixed)
                next.push_back({dstd::String(p.aggs[a].name + ".") + v,
                                cols_[n_keys + 1 + a].type});
        }
        dstd::Optional<dataframe::Schema> schema{dataframe::Schema{}};
        if (p.fixed)
            for (const auto& c : next)
                schema->fields.push_back(
                    dataframe::Field{std_str(c.name), c.type, true});
        const bool open = !p.fixed;
        const std::uint64_t limit = pivot_max_columns();
        finish(
            [n_keys, aggs, fixed, open, limit](dataframe::DataFrame f) {
                return spread(std::move(f), n_keys, aggs, fixed, open, limit);
            },
            "spread __duql_pivot into " + joined(aggs), std::move(schema));
        cols_ = p.fixed ? std::move(next) : dstd::Vector<duql::VectorColumn>{};
        duql::PipelineRename labels;
        for (std::size_t i = 0; i < p.labels.size() && i < fixed.size(); ++i) {
            if (p.labels[i].empty()) continue;
            for (const auto& a : aggs)
                labels.pairs.emplace_back(
                    aggs.size() == 1 ? dstd::String(p.labels[i])
                                     : a + "_" + dstd::String(p.labels[i]),
                    a + "." + fixed[i]);
        }
        if (!labels.pairs.empty()) stage(labels);
    }

    // Rows sorted by (keys, pivot value) as one row per key tuple, with a
    // column `agg.value` per aggregate and value.
    static dataframe::DataFrame spread(dataframe::DataFrame f,
                                       std::size_t n_keys,
                                       const dstd::Vector<dstd::String>& aggs,
                                       dstd::Vector<dstd::String> values,
                                       bool open, std::uint64_t limit) {
        const std::int64_t n = f.num_rows();
        auto text = [](const dataframe::Series& s,
                       std::int64_t r) -> dstd::String {
            if (s.is_null(r)) return "null";
            if (s.type() == dataframe::TypeId::String)
                return dstd::String(s.string_at(r));
            if (s.type() == dataframe::TypeId::Bool)
                return s.cast(dataframe::TypeId::Int64).data<std::int64_t>()[r]
                           ? "true"
                           : "false";
            if (s.type() == dataframe::TypeId::Uint64)
                return dstd::to_string(
                    s.materialize().data<std::uint64_t>()[r]);
            if (is_integer(s.type()))
                return dstd::to_string(s.slice(r, 1)
                                           .cast(dataframe::TypeId::Int64)
                                           .data<std::int64_t>()[0]);
            return number_text(s.slice(r, 1)
                                   .cast(dataframe::TypeId::Float64)
                                   .data<double>()[0]);
        };
        const dataframe::Series& pk = f.columns[n_keys];
        dstd::Vector<dstd::String> row_value;
        for (std::int64_t r = 0; r < n; ++r) row_value.push_back(text(pk, r));
        if (open) {
            dstd::Vector<std::int64_t> firsts;
            dstd::Set<dstd::String> seen;
            for (std::int64_t r = 0; r < n; ++r)
                if (seen.insert(row_value[static_cast<std::size_t>(r)]).second)
                    firsts.push_back(r);
            if (firsts.size() > limit)
                refuse("'pivot' gives " + dstd::to_string(firsts.size()) +
                       " columns per aggregate, over DUQL_PIVOT_MAX_COLUMNS (" +
                       dstd::to_string(limit) + ")");
            const dataframe::Series order = pk.take(firsts).argsort();
            for (std::int64_t i = 0; i < order.length(); ++i)
                values.push_back(row_value[static_cast<std::size_t>(
                    firsts[static_cast<std::size_t>(
                        order.data<std::int64_t>()[i])])]);
        }
        dstd::Vector<dataframe::Series> key_text;
        for (std::size_t c = 0; c < n_keys; ++c)
            key_text.push_back(f.columns[c].share());
        auto same_tuple = [&](std::int64_t a, std::int64_t b) {
            for (const auto& s : key_text)
                if (text(s, a) != text(s, b)) return false;
            return true;
        };
        dstd::Vector<std::int64_t> starts;
        dstd::Vector<dstd::Vector<std::int64_t>> cells(values.size());
        for (std::int64_t r = 0; r < n; ++r) {
            if (r == 0 || !same_tuple(r, r - 1)) {
                starts.push_back(r);
                for (auto& c : cells) c.push_back(-1);
            }
            for (std::size_t v = 0; v < values.size(); ++v)
                if (values[v] == row_value[static_cast<std::size_t>(r)])
                    cells[v].back() = r;
        }
        dataframe::DataFrame out;
        for (std::size_t c = 0; c < n_keys; ++c) {
            out.names.emplace_back(f.names[c]);
            out.columns.push_back(f.columns[c].take(starts));
        }
        for (std::size_t a = 0; a < aggs.size(); ++a)
            for (std::size_t v = 0; v < values.size(); ++v) {
                out.names.emplace_back(aggs[a] + "." + values[v]);
                out.columns.push_back(f.columns[n_keys + 1 + a].take(cells[v]));
            }
        return out;
    }

    void stage(const duql::PipelineUnpivot& u) {
        leave_scan("unpivot");
        dstd::Vector<std::size_t> at;
        dstd::Vector<dataframe::TypeId> types;
        for (const auto& f : u.fields) {
            const auto i = field_index(dstd::String(f));
            if (!i) refuse("'unpivot' has no field '" + f + "'");
            at.push_back(*i);
            types.push_back(cols_[*i].type.id);
        }
        const bool unknown =
            std::find(types.begin(), types.end(), dataframe::TypeId::Unknown) !=
            types.end();
        const bool numbers = std::all_of(types.begin(), types.end(), is_number);
        const bool same =
            std::all_of(types.begin(), types.end(),
                        [&](dataframe::TypeId t) { return t == types[0]; });
        dataframe::TypeId target =
            types.empty() ? dataframe::TypeId::String : types[0];
        if (unknown) {
            target = dataframe::TypeId::String;
        } else if (!same && numbers) {
            target = std::all_of(types.begin(), types.end(), is_integer)
                         ? dataframe::TypeId::Int64
                         : dataframe::TypeId::Float64;
        } else if (!same) {
            dstd::String what;
            for (std::size_t k = 0; k < u.fields.size(); ++k)
                what += dstd::String((what.empty() ? "" : ", ") + u.fields[k] +
                                     " (") +
                        dstd::String(dataframe::type_name(types[k])) + ")";
            refuse("'unpivot' mixes the types of " + what +
                   "; convert them with int(), float() or string()");
        }
        dstd::Vector<dstd::String> values;
        for (std::size_t k = 0; k < at.size(); ++k) {
            const dstd::String name = "__duql_value_" + dstd::to_string(k);
            dataframe::Expr e = col(cols_[at[k]].name);
            if (types[k] != target)
                e = target == dataframe::TypeId::String
                        ? dataframe::expr_convert(dataframe::ConvertOp::String,
                                                  e)
                        : dataframe::expr_cast(target, e);
            with_column(name, e, dstd::String(u.fields[k]));
            values.push_back(name);
        }
        dstd::Vector<std::size_t> ids;
        dstd::Vector<duql::VectorColumn> next;
        for (std::size_t j = 0; j < cols_.size(); ++j) {
            const bool listed =
                std::find(at.begin(), at.end(), j) != at.end() ||
                cols_[j].name.starts_with("__duql_value_");
            if (listed || cols_[j].name == dstd::String(u.key) ||
                cols_[j].name == dstd::String(u.value))
                continue;
            ids.push_back(j);
            next.push_back(cols_[j]);
        }
        next.push_back({dstd::String(u.key),
                        dataframe::scalar(dataframe::TypeId::String)});
        next.push_back({dstd::String(u.value), dataframe::scalar(target)});
        dstd::Vector<std::size_t> value_at;
        for (const auto& v : values) value_at.push_back(*index(v));
        const dstd::Vector<dstd::String> keys = strings_of(u.fields);
        const dstd::String key = dstd::String(u.key);
        const dstd::String value = dstd::String(u.value);
        map_rows(
            [ids, value_at, keys, key, value](dataframe::DataFrame f) {
                const std::int64_t n = f.num_rows();
                const auto k = static_cast<std::int64_t>(keys.size());
                dstd::Vector<std::int64_t> parent;
                dstd::Vector<std::int64_t> pick;
                dstd::Vector<dstd::String> names;
                for (std::int64_t r = 0; r < n; ++r)
                    for (std::int64_t j = 0; j < k; ++j) {
                        parent.push_back(r);
                        pick.push_back(j * n + r);
                        names.emplace_back(keys[static_cast<std::size_t>(j)]);
                    }
                dstd::Vector<dataframe::DataFrame> parts;
                for (const auto v : value_at) {
                    dataframe::DataFrame one;
                    one.names.emplace_back("v");
                    one.columns.push_back(f.columns[v].share());
                    parts.push_back(std::move(one));
                }
                dstd::Vector<const dataframe::DataFrame*> ptrs;
                for (const auto& p : parts) ptrs.push_back(&p);
                dataframe::DataFrame stacked =
                    ptrs.empty() ? dataframe::DataFrame{}
                                 : dataframe::concat(
                                       ptrs, dataframe::ConcatHow::Vertical);
                dataframe::DataFrame out;
                for (const auto j : ids) {
                    out.names.emplace_back(f.names[j]);
                    out.columns.push_back(f.columns[j].take(parent));
                }
                out.names.emplace_back(key);
                out.columns.push_back(dataframe::Series::strings(names));
                out.names.emplace_back(value);
                out.columns.push_back(stacked.columns.front().take(pick));
                return out;
            },
            std::move(next),
            "unpivot " + joined(u.fields) + " as " + dstd::String(u.key) +
                ", " + dstd::String(u.value));
    }

    // Whether the lookup cache holds side `i`, read without a trace.
    bool cached(std::size_t i) const {
        if (ctx_.sides->stored[i]) return true;
        const auto& key = ctx_.sides->keys[i];
        if (!key) return false;
        const auto db = index::cache::open_lookup_db(
            key->path, index::cache::CacheStore::Mode::ReadOnly);
        return db && index::cache::lookup_exists(*db, key->sig);
    }

    // Side `i` as a join's right plan: from memory when the build step or
    // the cache has it, else its own plan, which shares the scan with the
    // rows when it reads their files, under a tap for its caps and cache.
    dataframe::LazyFrame side_plan(std::size_t i, bool& memory) const {
        memory = std::find(collected_.begin(), collected_.end(), i) !=
                     collected_.end() ||
                 cached(i);
        if (!memory)
            return dataframe::detail::tap(
                ctx_.sides->views[i]->lazy(),
                side_tap(main_ ? ctx_.sides : nullptr, ctx_.sides, i));
        dataframe::Schema schema;
        for (const auto& c : ctx_.sides->columns[i])
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        return dataframe::LazyFrame::scan(dstd::make_shared<SideSource>(
            main_ ? ctx_.sides : nullptr, ctx_.sides, i, std::move(schema)));
    }

    void stage(const duql::PipelineLookup& l) {
        leave_scan("lookup");
        const dstd::Vector<dstd::String> before = names();
        const auto& side_cols = ctx_.sides->columns[l.side];
        auto side_index =
            [&](dstd::StringView name) -> dstd::Optional<std::size_t> {
            for (std::size_t i = 0; i < side_cols.size(); ++i)
                if (side_cols[i].name == name) return i;
            if (schema_.args_fallback)
                for (std::size_t i = 0; i < side_cols.size(); ++i)
                    if (side_cols[i].name == "args." + dstd::String(name))
                        return i;
            return dstd::nullopt;
        };
        dstd::Vector<dstd::String> left;
        dstd::Vector<dstd::String> right;
        dstd::String keys_text;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const auto& [item, column] = l.keys[k];
            const auto r = side_index(column);
            if (!r)
                refuse("'lookup " + l.name + "': '" + l.name +
                       "' has no column '" + column + "'");
            right.push_back(side_cols[*r].name);
            left.push_back(source(item, "__duql_lk_" + dstd::to_string(k)));
            keys_text +=
                (keys_text.empty() ? "" : ", ") + item.text + " == " + column;
        }
        if (const auto* a = std::get_if<duql::PipelineAsof>(&l.mode)) {
            asof_lookup(l, *a, left, right, keys_text, side_index);
            return;
        }
        if (const auto* o = std::get_if<duql::PipelineOverlap>(&l.mode)) {
            overlap_lookup(l, *o, left, right, keys_text, side_index);
            return;
        }
        if (l.kind == duql::syntax::LookupKind::Anti) {
            bool memory = false;
            dataframe::LazyFrame side = side_plan(l.side, memory);
            lines.push_back(dstd::String("lookup " + l.name + " on ") +
                            keys_text + " anti" +
                            (memory ? " (side from memory)"
                                    : " (side joined, sharing the scan)"));
            v_ = v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                             std_strs(right),
                                             dataframe::JoinHow::Anti));
            if (names() != before) keep(before);
            return;
        }
        const bool inner = l.kind == duql::syntax::LookupKind::Inner;
        const auto* into = std::get_if<duql::PipelineNest>(&l.mode);
        const bool nest = into != nullptr;
        dstd::Vector<duql::VectorColumn> next = cols_;
        dstd::Vector<dstd::String> added;
        auto shown = [&](const dstd::String& n) {
            return index(n) && std::find(extras_.begin(), extras_.end(), n) ==
                                   extras_.end();
        };
        if (nest) {
            if (index(into->name))
                refuse("'lookup " + l.name + " into " + into->name +
                       "' names a column the rows have; pick another name");
            dstd::Vector<dataframe::Field> fields;
            for (const auto& c : side_cols)
                fields.push_back(
                    dataframe::Field{std_str(c.name), c.type, true});
            next.push_back(
                {dstd::String(into->name),
                 dataframe::list_of(dataframe::struct_of(std::move(fields)))});
            added.push_back(dstd::String(into->name));
        } else {
            for (const auto& c : side_cols) {
                if (std::find(right.begin(), right.end(), c.name) !=
                    right.end())
                    continue;
                const auto at = index(c.name);
                // A record field the rows lack stays null there, and the
                // join fills it; any other clash is the query's error.
                if (at && shown(c.name) && !cols_[*at].record)
                    refuse(dstd::String("'lookup " + l.name + "' adds '") +
                           c.name +
                           "', which the rows already have; rename one with "
                           "'rename' or read the matches with 'into'");
                if (at) {
                    next[*at] = {c.name, c.type};
                } else {
                    next.push_back({c.name, c.type});
                    added.push_back(c.name);
                }
            }
        }
        bool memory = false;
        dataframe::LazyFrame side = side_plan(l.side, memory);
        dstd::Vector<dstd::String> right_on = right;
        if (!nest) {
            // The key columns name the row set in the join's errors.
            // dstd::Vector<dstd::String>
            dstd::Vector<dstd::String> renamed = side.schema();
            for (auto& n : renamed)
                if (std::find(right.begin(), right.end(),
                              dstd::StringView(n)) != right.end()) {
                    dstd::String prefixed(l.name);
                    prefixed += '.';
                    prefixed += n;
                    n = std::move(prefixed);
                }
            side = side.rename(std::move(renamed));
            for (auto& k : right_on) k = dstd::String(l.name + ".") + k;
            if (inner) {
                side = side.with_column(dstd::String(HIT_COLUMN),
                                        dataframe::expr_lit_bool(true));
                next.push_back(
                    {HIT_COLUMN, dataframe::scalar(dataframe::TypeId::Bool)});
            }
        }
        lines.push_back(dstd::String("lookup " + l.name + " on ") + keys_text +
                        (inner ? " inner" : "") +
                        dstd::String((nest ? " into " + into->name : "")) +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        v_ = v_.with_lazy(v_.lazy().join(
            std::move(side), std_strs(left), std_strs(right_on),
            nest ? dataframe::JoinHow::Nest : dataframe::JoinHow::Lookup,
            nest ? std_str(into->name) : dstd::String()));
        cols_ = std::move(next);
        if (inner) {
            if (nest)
                v_ = v_.with_lazy(
                    v_.lazy().filter(dataframe::expr_list_len(col(into->name)) >
                                     std::int64_t{0}));
            else
                v_ = v_.with_lazy(v_.lazy().filter(dataframe::expr_not(
                    dataframe::expr_is_null(col(HIT_COLUMN)))));
        }
        dstd::Vector<dstd::String> keep_names = before;
        for (const auto& n : added) keep_names.push_back(n);
        if (names() != keep_names) keep(std::move(keep_names));
    }

    // The type an as-of pair shares: the type itself, else the number type
    // both convert to.
    static dataframe::DataType asof_type(const dstd::String& what,
                                         const dstd::String& side,
                                         const dataframe::DataType& rows,
                                         const dataframe::DataType& other) {
        if (rows.id == dataframe::TypeId::Unknown &&
            other.id == dataframe::TypeId::Unknown)
            refuse("'lookup " + side + " ... asof': " + what +
                   " has no known type; convert it with int(), float() or "
                   "string()");
        if (rows == other) return rows;
        if (rows.id == dataframe::TypeId::Unknown) return other;
        if (other.id == dataframe::TypeId::Unknown) return rows;
        if (is_number(rows.id) && is_number(other.id))
            return dataframe::scalar(is_integer(rows.id) && is_integer(other.id)
                                         ? dataframe::TypeId::Int64
                                         : dataframe::TypeId::Float64);
        refuse("'lookup " + side + " ... asof': " + what + " is " +
               dstd::String(dataframe::type_name(rows.id)) +
               " in the rows and " +
               dstd::String(dataframe::type_name(other.id)) + " in '" + side +
               "'");
    }

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

    SideValues side_values(const duql::PipelineLookup& l, const char* clause,
                           const dstd::Vector<std::size_t>& keys,
                           std::size_t consumed,
                           std::size_t consumed2 = SIZE_MAX) const {
        const auto& side_cols = ctx_.sides->columns[l.side];
        SideValues out;
        out.names.resize(side_cols.size());
        auto shown = [&](const dstd::String& n) {
            return index(n) && std::find(extras_.begin(), extras_.end(), n) ==
                                   extras_.end();
        };
        for (std::size_t c = 0; c < side_cols.size(); ++c) {
            if (c == consumed || c == consumed2 ||
                std::find(keys.begin(), keys.end(), c) != keys.end())
                continue;
            const dstd::String& own_name = side_cols[c].name;
            const auto at = index(own_name);
            if (at && shown(own_name) && !cols_[*at].record)
                refuse(dstd::String("'lookup " + l.name + " ... " + clause +
                                    "' adds '") +
                       own_name +
                       "', which the rows already have; rename one with "
                       "'rename'");
            out.names[c] = own_name;
            if (at) {
                const dataframe::DataType& own = cols_[*at].type;
                const dataframe::DataType& theirs = side_cols[c].type;
                if (own.id != dataframe::TypeId::Unknown &&
                    theirs.id != dataframe::TypeId::Unknown && own != theirs)
                    refuse(dstd::String("'lookup " + l.name + " ... " + clause +
                                        "': '") +
                           own_name + "' is " +
                           dstd::String(dataframe::type_name(own.id)) +
                           " in the rows and " +
                           dstd::String(dataframe::type_name(theirs.id)) +
                           " in '" + dstd::String(l.name) + "'");
                out.names[c] = "__duql_av_" + own_name;
                out.fills.emplace_back(own_name, out.names[c]);
            } else {
                out.added.push_back(own_name);
            }
            out.columns.push_back({out.names[c], side_cols[c].type});
        }
        return out;
    }

    // `lookup s on k asof t`: the engine's as-of join on names both frames
    // share, then the input order back.
    void asof_lookup(
        const duql::PipelineLookup& l, const duql::PipelineAsof& a,
        const dstd::Vector<dstd::String>& left,
        const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
        const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
            side_index) {
        const auto& side_cols = ctx_.sides->columns[l.side];
        const auto t = side_index(a.column);
        if (!t)
            refuse("'lookup " + l.name + " ... asof': '" + l.name +
                   "' has no column '" + a.column + "'");
        const dstd::Vector<dstd::String> before = names();
        dstd::Vector<std::size_t> side_key;
        for (const auto& r : right) side_key.push_back(*side_index(r));
        if (std::find(side_key.begin(), side_key.end(), *t) != side_key.end())
            refuse("'lookup " + l.name + " ... asof': '" + a.column +
                   "' is both a key and the time");

        dstd::Vector<dstd::String> equi;
        dstd::Vector<dstd::String> side_names;
        for (const auto& c : side_cols) side_names.push_back(c.name);
        bool memory = false;
        dataframe::LazyFrame side = side_plan(l.side, memory);
        for (std::size_t k = 0; k < left.size(); ++k) {
            const dataframe::DataType lt = cols_[*index(left[k])].type;
            const dataframe::DataType st = side_cols[side_key[k]].type;
            const dataframe::DataType to =
                asof_type(dstd::String("key '" + l.keys[k].second + "'"),
                          dstd::String(l.name), lt, st);
            const dstd::String from = "__duql_lk_" + dstd::to_string(k);
            with_column(from, dataframe::expr_cast(to.id, col(left[k])),
                        dstd::String(l.keys[k].first.text));
            side = side.with_column(
                std_str(side_names[side_key[k]]),
                dataframe::expr_cast(
                    to.id, dataframe::expr_col(
                               static_cast<std::int32_t>(side_key[k]))));
            equi.push_back(from);
        }
        const dstd::String time_src = source(a.time, "__duql_at");
        const dataframe::DataType time_type = asof_type(
            dstd::String("time '" + a.column + "'"), dstd::String(l.name),
            cols_[*index(time_src)].type, side_cols[*t].type);
        if (!is_number(time_type.id))
            refuse("'lookup " + l.name + " ... asof': time '" + a.time.text +
                   "' is " + dstd::String(dataframe::type_name(time_type.id)) +
                   ", not a number");
        const dstd::String time = "__duql_at";
        with_column(time, dataframe::expr_cast(time_type.id, col(time_src)),
                    dstd::String(a.time.text));
        side = side.with_column(
            std_str(side_names[*t]),
            dataframe::expr_cast(
                time_type.id,
                dataframe::expr_col(static_cast<std::int32_t>(*t))));

        SideValues values = side_values(l, "asof", side_key, *t);
        // A null key has no key: the op would pair null with null.
        for (const auto k : side_key)
            side = side.filter(dataframe::expr_is_null(
                dataframe::expr_col(static_cast<std::int32_t>(k)), false));
        dstd::Vector<dstd::String> renamed = side_names;
        renamed[*t] = time;
        for (std::size_t k = 0; k < side_key.size(); ++k)
            renamed[side_key[k]] = equi[k];
        for (std::size_t c = 0; c < renamed.size(); ++c)
            if (!values.names[c].empty()) renamed[c] = values.names[c];
        side = side.rename(std_strs(renamed));

        const dstd::String pos = "__duql_pos";
        row_index(pos);
        dstd::Vector<dstd::String> out = names();
        for (const auto& n : values.names)
            if (!n.empty()) out.push_back(n);
        dstd::Vector<const char*> by;
        for (const auto& e : equi) by.push_back(e.c_str());
        dataframe::OpArgs args;
        args.str(2, time.c_str())
            .strlist(3, by.data(), static_cast<std::int32_t>(by.size()))
            .i32(4, static_cast<std::int32_t>(a.direction))
            .f64(5, a.tolerance.value_or(-1));
        static constexpr const char* DIRECTIONS[] = {"backward", "forward",
                                                     "nearest"};
        static_assert(static_cast<int>(duql::syntax::AsofDirection::Forward) ==
                      DFTU_ASOF_FORWARD);
        static_assert(static_cast<int>(duql::syntax::AsofDirection::Nearest) ==
                      DFTU_ASOF_NEAREST);
        lines.push_back(dstd::String("lookup " + l.name + " on ") + keys_text +
                        " asof " + dstd::String(a.time.text) + " " +
                        DIRECTIONS[static_cast<std::size_t>(a.direction)] +
                        (a.tolerance ? " within " + number_text(*a.tolerance)
                                     : dstd::String()) +
                        " (op dftu.frame.asof)");
        v_ = v_.with_lazy(v_.lazy().frame_op("dftu.frame.asof", args,
                                             {std::move(side)}, std_strs(out)));
        for (auto& c : values.columns) cols_.push_back(std::move(c));
        for (const auto& [name, tmp] : values.fills)
            with_column(name, dataframe::expr_coalesce({col(name), col(tmp)}),
                        name + " ?? " + tmp);
        lines.push_back("sort_by " + pos);
        v_ = v_.with_lazy(v_.lazy().sort_by(std_str(pos)));
        dstd::Vector<dstd::String> keep_names = before;
        for (const auto& n : values.added) keep_names.push_back(n);
        keep(std::move(keep_names));
    }

    // `lookup s on k overlap [into m]`: the sweep over `s` (already in
    // memory, in file order) on the rows once the scan has ended.
    void overlap_lookup(
        const duql::PipelineLookup& l, const duql::PipelineOverlap& o,
        const dstd::Vector<dstd::String>& left,
        const dstd::Vector<dstd::String>& right, const dstd::String& keys_text,
        const dstd::Function<dstd::Optional<std::size_t>(dstd::StringView)>&
            side_index) {
        const auto& side_cols = ctx_.sides->columns[l.side];
        const auto t = side_index(o.time_column);
        const auto d = side_index(o.duration_column);
        if (!t || !d)
            refuse("'lookup " + l.name + " ... overlap': '" + l.name +
                   "' needs the columns '" + o.time_column + "' and '" +
                   o.duration_column +
                   "', the record schema's time and "
                   "duration");
        const dstd::Vector<dstd::String> before = names();
        dstd::Vector<std::size_t> side_key;
        for (const auto& r : right) side_key.push_back(*side_index(r));
        const dstd::String time = source(o.time, "__duql_ot");
        const dstd::String duration = source(o.duration, "__duql_od");

        const bool nest = !o.into.empty();
        SideValues values;
        dstd::Vector<duql::VectorColumn> next = cols_;
        if (nest) {
            if (index(o.into))
                refuse("'lookup " + l.name + " overlap into " + o.into +
                       "' names a column the rows have; pick another name");
            dstd::Vector<dataframe::Field> fields;
            for (const auto& c : side_cols)
                fields.push_back(
                    dataframe::Field{std_str(c.name), c.type, true});
            next.push_back(
                {dstd::String(o.into),
                 dataframe::list_of(dataframe::struct_of(std::move(fields)))});
            values.added.push_back(dstd::String(o.into));
        } else {
            values = side_values(l, "overlap", side_key, *t, *d);
            for (const auto& c : values.columns) next.push_back(c);
        }

        dataframe::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        const dstd::Vector<dstd::String> side_names = [&] {
            dstd::Vector<dstd::String> out;
            for (const auto& c : side_cols) out.push_back(c.name);
            return out;
        }();
        const std::size_t side_id = l.side;
        const dstd::String into = dstd::String(o.into);
        const double scale = o.scale;
        const dstd::Vector<std::size_t> consumed = [&] {
            dstd::Vector<std::size_t> out = side_key;
            out.push_back(*t);
            out.push_back(*d);
            return out;
        }();
        const dstd::Vector<dstd::String> value_names = values.names;
        dstd::SharedPtr<Sides> strong = main_ ? ctx_.sides : nullptr;
        dstd::WeakPtr<Sides> weak = ctx_.sides;
        finish(
            [=](dataframe::DataFrame rows) {
                const dstd::SharedPtr<Sides> sides =
                    strong ? strong : owner(weak);
                const auto held = sides->frame(side_id);
                dataframe::DataFrame side;
                side.names = held->names;
                for (const auto& c : held->columns)
                    side.columns.push_back(c.share());
                side = duql::flat_frame(std::move(side));
                rows = duql::flat_frame(std::move(rows));
                auto find = [](const dataframe::DataFrame& f,
                               const dstd::String& n) {
                    for (std::size_t i = 0; i < f.names.size(); ++i)
                        if (dstd::StringView(f.names[i]) == n)
                            return f.columns[i].share();
                    throw std::invalid_argument(
                        ("overlap: no column " + n).c_str());
                };
                duql::OverlapColumns mine;
                duql::OverlapColumns theirs;
                for (const auto& n : left) mine.keys.push_back(find(rows, n));
                mine.start = find(rows, time);
                mine.duration = find(rows, duration);
                mine.scale = scale;
                for (const auto k : side_key)
                    theirs.keys.push_back(find(side, side_names[k]));
                theirs.start = find(side, side_names[*t]);
                theirs.duration = find(side, side_names[*d]);
                theirs.scale = scale;
                const duql::OverlapMatches m =
                    duql::overlap_matches(mine, theirs);

                dataframe::DataFrame out;
                out.names = rows.names;
                if (!into.empty()) {
                    for (const auto& c : rows.columns)
                        out.columns.push_back(c.share());
                    dstd::Vector<std::int32_t> offsets;
                    for (const auto x : m.offsets)
                        offsets.push_back(static_cast<std::int32_t>(x));
                    dstd::Vector<dataframe::Series> parts;
                    for (const auto& c : side.columns)
                        parts.push_back(c.take(m.rows));
                    out.names.emplace_back(into);
                    out.columns.push_back(dataframe::Series::list(
                        offsets, dataframe::Series::structs(side.names,
                                                            std::move(parts))));
                    return out;
                }
                dstd::Vector<std::int64_t> from_rows;
                dstd::Vector<std::int64_t> from_side;
                for (std::size_t i = 0; i + 1 < m.offsets.size(); ++i) {
                    if (m.offsets[i] == m.offsets[i + 1]) {
                        from_rows.push_back(static_cast<std::int64_t>(i));
                        from_side.push_back(-1);
                    }
                    for (auto k = m.offsets[i]; k < m.offsets[i + 1]; ++k) {
                        from_rows.push_back(static_cast<std::int64_t>(i));
                        from_side.push_back(
                            m.rows[static_cast<std::size_t>(k)]);
                    }
                }
                for (const auto& c : rows.columns)
                    out.columns.push_back(c.take(from_rows));
                for (std::size_t c = 0; c < value_names.size(); ++c) {
                    if (value_names[c].empty()) continue;
                    out.names.emplace_back(value_names[c]);
                    out.columns.push_back(side.columns[c].take(from_side));
                }
                return out;
            },
            dstd::String("lookup " + l.name + " on ") + keys_text + " overlap" +
                (nest ? dstd::String(" into " + o.into) : dstd::String()) +
                " (sweep over " + dstd::String(l.name) + ")",
            std::move(schema));
        cols_ = std::move(next);
        for (const auto& [name, tmp] : values.fills)
            with_column(name, dataframe::expr_coalesce({col(name), col(tmp)}),
                        name + " ?? " + tmp);
        dstd::Vector<dstd::String> keep_names = before;
        for (const auto& n : values.added) keep_names.push_back(n);
        keep(std::move(keep_names));
    }

    // The type both sides of a union give `name`, or a refusal naming it.
    static dataframe::DataType unify(const dstd::String& name,
                                     const dataframe::DataType& a,
                                     const dataframe::DataType& b) {
        if (a.id == dataframe::TypeId::Unknown) return b;
        if (b.id == dataframe::TypeId::Unknown) return a;
        if (a == b) return a;
        if (is_number(a.id) && is_number(b.id))
            return dataframe::scalar(is_integer(a.id) && is_integer(b.id)
                                         ? dataframe::TypeId::Int64
                                         : dataframe::TypeId::Float64);
        refuse("'union' gives column '" + name + "' as " +
               dstd::String(dataframe::type_name(a.id)) + " and " +
               dstd::String(dataframe::type_name(b.id)) +
               "; convert one with int(), float() or string()");
    }

    // `v` with the columns `cols` as `want`: missing ones null, others cast.
    static View align(View v, const dstd::Vector<duql::VectorColumn>& cols,
                      const dstd::Vector<duql::VectorColumn>& want) {
        auto at = [&](const dstd::String& n) -> dstd::Optional<std::size_t> {
            for (std::size_t i = 0; i < cols.size(); ++i)
                if (cols[i].name == n) return i;
            return dstd::nullopt;
        };
        dstd::Vector<dstd::String> order;
        for (const auto& w : want) {
            order.push_back(w.name);
            const auto i = at(w.name);
            const dataframe::TypeId to = w.type.id == dataframe::TypeId::Unknown
                                             ? dataframe::TypeId::String
                                             : w.type.id;
            if (!i) {
                v = v.with_column(std_str(w.name),
                                  dataframe::expr_lit_null(to));
                continue;
            }
            if (cols[*i].type == w.type) continue;
            const dataframe::Expr c =
                dataframe::expr_col(static_cast<std::int32_t>(*i));
            v = v.with_column(
                std_str(w.name),
                to == dataframe::TypeId::String
                    ? dataframe::expr_convert(dataframe::ConvertOp::String, c)
                    : dataframe::expr_cast(to, c));
        }
        return v.with_lazy(v.lazy().select(std_strs(order)));
    }

    void stage(const duql::PipelineUnion& u) {
        leave_scan("union");
        const std::size_t first_line = lines.size();
        Built other = build(ctx_, *u.other, true, lines);
        for (std::size_t i = first_line; i < lines.size(); ++i)
            lines[i] = "  " + lines[i];
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(first_line),
                     "union from " +
                         input_text(u.other->input, ctx_.sides->program) + ":");
        dstd::Vector<duql::VectorColumn> want;
        for (const auto& c : cols_) want.push_back({c.name, c.type});
        for (const auto& c : other.cols) {
            auto it =
                std::find_if(want.begin(), want.end(),
                             [&](const auto& w) { return w.name == c.name; });
            if (it == want.end())
                want.push_back({c.name, c.type});
            else
                it->type = unify(c.name, it->type, c.type);
        }
        for (auto& w : want)
            if (w.type.id == dataframe::TypeId::Unknown)
                w.type = dataframe::scalar(dataframe::TypeId::String);
        View mine = align(v_, cols_, want);
        View theirs = align(other.view, other.cols, want);
        v_ = v_.with_lazy(mine.lazy().concat(theirs.lazy()));
        cols_ = std::move(want);
    }

    // The column type an aggregate gives on either plan, or nullopt to keep
    // what the plan gives.
    dstd::Optional<dataframe::TypeId> agg_type(
        const duql::PipelineAgg& a,
        dstd::Optional<dataframe::TypeId> in) const {
        switch (a.fn) {
            case duql::AggFn::Count:
            case duql::AggFn::CountIf:
            case duql::AggFn::CountDistinct:
                return dataframe::TypeId::Int64;
            case duql::AggFn::Sketch:
            case duql::AggFn::Merge:
                return dataframe::TypeId::String;
            case duql::AggFn::Collect:
            case duql::AggFn::Plugin:
                return dstd::nullopt;
            case duql::AggFn::Sum:
                if (!in) return dstd::nullopt;
                return is_integer(*in) ? dataframe::TypeId::Int64
                                       : dataframe::TypeId::Float64;
            case duql::AggFn::Min:
            case duql::AggFn::Max:
            case duql::AggFn::First:
            case duql::AggFn::Last:
            case duql::AggFn::Argmax:
            case duql::AggFn::Argmin:
                if (in && (is_number(*in) || *in == dataframe::TypeId::String ||
                           *in == dataframe::TypeId::Bool))
                    return in;
                return dstd::nullopt;
            case duql::AggFn::Mean:
            case duql::AggFn::Var:
            case duql::AggFn::Std:
            case duql::AggFn::Quantile:
                return dataframe::TypeId::Float64;
            case duql::AggFn::Histogram:
            case duql::AggFn::Busy:
            case duql::AggFn::Concurrency:
            case duql::AggFn::Utilization:
            case duql::AggFn::Active:
                return dstd::nullopt;
        }
        return dstd::nullopt;
    }

    dstd::Optional<dataframe::TypeId> term_type(const duql::TermRef& t,
                                                const dstd::String& text) {
        if (!t) return dstd::nullopt;
        if (const auto* f = whole_field(t))
            if (const auto c = scan_column(dstd::String(f->base)))
                return c->type.id == dataframe::TypeId::Unknown
                           ? dstd::nullopt
                           : dstd::Optional<dataframe::TypeId>(c->type.id);
        const dataframe::TypeId id =
            dataframe::infer_type(compile(t, text), types()).id;
        if (id == dataframe::TypeId::Unknown) return dstd::nullopt;
        return id;
    }

    // A whole-field term's column, usable by the trace aggregation.
    dstd::Optional<duql::VectorColumn> trace_column(const duql::TermRef& t) {
        const auto* f = whole_field(t);
        if (!f) return dstd::nullopt;
        auto c = scan_column(dstd::String(f->base));
        if (!c || c->json) return dstd::nullopt;
        return c;
    }

    dstd::String trace_refusal(
        const duql::PipelineGroup& g,
        const dstd::Optional<duql::PipelineBucket>& pending) {
        if (!scan_)
            return "'" + blocker_ +
                   "' comes before it; only where, time_range, bucket and "
                   "a field select may";
        if (pending) {
            if (pending->every)
                return "a hopping bucket assigns a record to several windows";
            const double us = pending->width * us_per_time_unit();
            if (std::trunc(us) != us)
                return "its bucket width is not a whole number of "
                       "microseconds";
            const double origin_us = pending->origin * us_per_time_unit();
            if (std::trunc(origin_us) != origin_us || origin_us < 0)
                return "its bucket origin is not a whole, non-negative number "
                       "of microseconds";
        }
        // A select at the scan fixes the columns: a field it left out is no
        // column, whatever the files hold.
        if (scan_selected_) {
            dstd::String missing;
            for (const auto& k : g.keys)
                duql::for_each_term_field(*k.term, [&](const duql::TField& f) {
                    if (!has_column_for(dstd::String(f.base))) missing = f.base;
                });
            for (const auto& a : g.aggs)
                if (a.arg)
                    duql::for_each_term_field(
                        *a.arg, [&](const duql::TField& f) {
                            if (!has_column_for(dstd::String(f.base)))
                                missing = f.base;
                        });
            if (!missing.empty())
                refuse("has no column '" + missing +
                       "'; the columns here are " + joined(names()));
        }
        dstd::Vector<dstd::String> seen;
        for (const auto& k : g.keys) {
            const auto c = trace_column(k.term);
            if (!c || (c->type.id != dataframe::TypeId::String &&
                       c->type.id != dataframe::TypeId::Int64 &&
                       c->type.id != dataframe::TypeId::Uint64))
                return dstd::String("key '" + k.text +
                                    "' is not a string or integer field");
            for (const auto& s : seen)
                if (s == c->name) return "a field is a key twice";
            seen.push_back(c->name);
        }
        for (const auto& a : g.aggs) {
            if (!trace_op(a))
                return dstd::String("'" + a.text +
                                    "' runs only on the scanned columns");
            if (!a.arg) continue;
            const auto c = trace_column(a.arg);
            if (!c || !is_number(c->type.id))
                return dstd::String("'" + a.text +
                                    "' does not read a numeric field");
        }
        return {};
    }

    void stage(const duql::PipelineGroup& group) {
        dstd::Optional<duql::PipelineBucket> bucket = std::move(bucket_);
        bucket_.reset();
        // A sum over no values is null: a hidden mean tells which groups
        // had one.
        duql::PipelineGroup g = group;
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> sums;
        for (const auto& a : group.aggs)
            if (a.fn == duql::AggFn::Sum) {
                dstd::String valid =
                    "__duql_valid_" + dstd::to_string(sums.size());
                duql::PipelineAgg hidden;
                hidden.name = valid;
                hidden.fn = duql::AggFn::Mean;
                hidden.arg = a.arg;
                hidden.text = "mean(" + duql::term_text(*a.arg) + ")";
                g.aggs.push_back(std::move(hidden));
                sums.emplace_back(a.name, std::move(valid));
            }
        const dstd::String why = trace_refusal(g, bucket);
        if (!why.empty()) {
            for (const auto& a : g.aggs)
                if (duql::is_occupancy(a.fn)) {
                    if (bucket && bucket->every)
                        refuse("'" + agg_name(a.fn) +
                               "' does not run with a hopping bucket");
                    refuse("'" + agg_name(a.fn) +
                           "' needs the trace aggregation, which this group "
                           "cannot take: " +
                           why);
                }
            leave_scan("group");
        }
        dstd::Vector<dstd::Optional<dataframe::TypeId>> key_types;
        if (bucket)
            key_types.push_back(
                term_type(bucket->key, dstd::String(bucket->text))
                    .value_or(dataframe::TypeId::Int64));
        for (const auto& k : g.keys)
            key_types.push_back(term_type(k.term, dstd::String(k.text)));
        dstd::Vector<dstd::Optional<dataframe::TypeId>> agg_types;
        for (const auto& a : g.aggs)
            agg_types.push_back(
                agg_type(a, term_type(a.arg, dstd::String(a.text))));

        dstd::Vector<dstd::String> keys;
        if (bucket) keys.push_back(dstd::String(bucket->name));
        for (const auto& k : g.keys) keys.push_back(dstd::String(k.name));
        dstd::Vector<dstd::String> out = keys;
        for (const auto& a : group.aggs) out.push_back(dstd::String(a.name));
        const dstd::Vector<dstd::String> visible = out;
        for (std::size_t i = 0; i < out.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (out[i] == out[j])
                    refuse("group gives two columns named '" + out[i] + "'");

        dstd::String text;
        for (const auto& k : g.keys)
            text += (text.empty() ? "" : ", ") + k.text;
        for (const auto& [sum, valid] : sums) out.push_back(valid);
        dstd::String aggs;
        for (const auto& a : group.aggs)
            aggs += (aggs.empty() ? "" : ", ") + a.name + " = " + a.text;
        if (why.empty()) {
            lines.push_back("group (trace plan): keys " + text + "; aggs " +
                            aggs);
            trace_group(g, bucket);
        } else {
            lines.push_back("group (frame plan): keys " + text + "; aggs " +
                            aggs);
            frame_group(g, bucket);
        }
        rename(out);
        cols_.clear();
        for (const auto& f : v_.output_schema().fields)
            cols_.push_back({dstd::String(f.name), f.type, false});
        for (std::size_t i = 0; i < out.size(); ++i) {
            const dstd::Optional<dataframe::TypeId> want =
                i < key_types.size() ? key_types[i]
                                     : agg_types[i - key_types.size()];
            if (!want || cols_[i].type.id == *want ||
                (*want == dataframe::TypeId::String &&
                 cols_[i].type.id != dataframe::TypeId::Unknown))
                continue;
            dataframe::Expr e = col(out[i]);
            const bool is_text = cols_[i].type.id == dataframe::TypeId::String;
            if ((is_text && is_number(*want)) ||
                (cols_[i].type.id == dataframe::TypeId::Uint64 &&
                 *want == dataframe::TypeId::Int64))
                e = dataframe::expr_convert(is_integer(*want)
                                                ? dataframe::ConvertOp::Int
                                                : dataframe::ConvertOp::Float,
                                            e);
            v_ =
                v_.with_column(std_str(out[i]), dataframe::expr_cast(*want, e));
            cols_[i].type = dataframe::scalar(*want);
        }
        for (const auto& [sum, valid] : sums)
            v_ = v_.with_column(
                std_str(sum),
                dataframe::expr_arith(
                    dataframe::ArithOp::Mul, col(sum),
                    dataframe::expr_select(
                        dataframe::expr_is_null(col(valid), false),
                        dataframe::expr_lit(std::int64_t{1}),
                        dataframe::expr_lit_null(dataframe::TypeId::Int64))));
        for (const auto& a : group.aggs)
            if (a.fn == duql::AggFn::Quantile)
                v_ = v_.with_column(
                    std_str(dstd::String(a.name)),
                    dataframe::expr_select(
                        dataframe::expr_unary(dataframe::UnaryOp::IsNan,
                                              col(a.name)),
                        dataframe::expr_lit_null(dataframe::TypeId::Float64),
                        col(a.name)));
        if (out != visible) keep(visible, false);
        if (!keys.empty()) {
            lines.push_back("sort_by_multi " + joined(keys));
            v_ = v_.sort_by_multi(std_strs(keys),
                                  dstd::Vector<bool>(keys.size(), false));
        }
        if (bucket && bucket->fill)
            finish_fill(g, *bucket);
        else if (keys.empty())
            finish_empty(g);
        extras_.clear();
    }

    void trace_group(const duql::PipelineGroup& g,
                     const dstd::Optional<duql::PipelineBucket>& bucket) {
        // A select before it would project the aggregation's output; the
        // keys and arguments are among the selected columns already.
        View v = v_.select(dstd::Vector<dstd::String>{});
        dstd::Vector<dstd::String> keep_cols;
        if (bucket) {
            const auto width = static_cast<std::uint64_t>(
                std::llround(bucket->width * us_per_time_unit()));
            const auto origin = static_cast<std::uint64_t>(
                std::llround(bucket->origin * us_per_time_unit()));
            if (origin == 0) {
                lines.push_back("time_bucket " + dstd::to_string(width) +
                                " us");
                v = v.time_bucket(width);
            } else {
                lines.push_back("time_bucket " + dstd::to_string(width) +
                                " us at " + dstd::to_string(origin) + " us");
                v = v.time_bucket(width, origin);
            }
            keep_cols.push_back("time_bucket");
        }
        dstd::Vector<GroupKey> keys;
        for (const auto& k : g.keys) {
            keys.push_back(
                GroupKey::field(dstd::String(whole_field(k.term)->base)));
            keep_cols.push_back(dstd::String(whole_field(k.term)->base));
        }
        dstd::Vector<AggSpec> specs;
        for (std::size_t i = 0; i < g.aggs.size(); ++i) {
            const auto& a = g.aggs[i];
            dstd::String out = "__duql_agg_" + dstd::to_string(i);
            specs.emplace_back(
                *trace_op(a),
                a.arg ? dstd::String(whole_field(a.arg)->base) : dstd::String(),
                out, "", a.q);
            keep_cols.push_back(std::move(out));
        }
        if (!keys.empty()) v = v.group_by(std::move(keys));
        v_ = v.agg(std::move(specs));
        scan_ = false;
        blocker_ = "group";
        columns_from_view();
        keep(std::move(keep_cols), false);
        // busy() and active() come back in microseconds; the duration role
        // may count in another unit.
        const auto dur_ns = roles_.fields.find(roles_.duration);
        const double us_per_dur =
            dur_ns == roles_.fields.end()
                ? 1.0
                : static_cast<double>(dur_ns->second) / 1000.0;
        for (std::size_t i = 0; i < g.aggs.size() && us_per_dur != 1.0; ++i) {
            if (g.aggs[i].fn != duql::AggFn::Busy &&
                g.aggs[i].fn != duql::AggFn::Active)
                continue;
            const dstd::String out = "__duql_agg_" + dstd::to_string(i);
            with_column(
                out,
                dataframe::expr_binary(
                    dataframe::BinaryOp::Div,
                    dataframe::expr_col(static_cast<std::int32_t>(*index(out))),
                    dataframe::expr_lit(us_per_dur)),
                out + " in duration units");
        }
        // The trace aggregation keys buckets in microseconds; the time role
        // may count in another unit.
        if (bucket && us_per_time_unit() != 1.0) {
            const auto i = index("time_bucket");
            dataframe::Expr in_unit = dataframe::expr_binary(
                dataframe::BinaryOp::Div,
                dataframe::expr_col(static_cast<std::int32_t>(*i)),
                dataframe::expr_lit(us_per_time_unit()));
            const auto* field = schema_.field_at(roles_.time);
            if (field && field->type == index::FieldType::Int)
                in_unit =
                    dataframe::expr_cast(dataframe::TypeId::Int64, in_unit);
            with_column("time_bucket", in_unit, "time_bucket in time units");
        }
    }

    // The doubles that fill the reducer's operands after its column, or a
    // refusal when they do not fit its signature.
    static dstd::Vector<double> plugin_params(const duql::PipelineAgg& a) {
        const dftu_op_desc* op = dftu_op_find(a.plugin.c_str());
        if (!op) refuse("plugin function '" + a.plugin + "' is not registered");
        if (!a.arg)
            plugin_refusal(dstd::String(a.plugin), op, "the column is missing");
        std::size_t takes = 0;
        for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i)
            if (DFTU_OP_SIG_ARG(op->sig, i) != DFTU_TOK_NONE) ++takes;
        if (a.params.size() != takes)
            plugin_refusal(dstd::String(a.plugin), op,
                           "the arguments do not fit");
        dstd::Vector<double> out;
        for (std::size_t i = 0; i < takes; ++i) {
            const dftu_op_tok tok =
                DFTU_OP_SIG_ARG(op->sig, static_cast<std::uint32_t>(i + 1));
            const dftu_scalar v = plugin_scalar(a.params[i]);
            const bool whole = v.kind != DFTU_SCALAR_TAG_F64;
            const bool real = tok == DFTU_TOK_F64 || tok == DFTU_TOK_SCALAR;
            const bool count = tok == DFTU_TOK_I32 || tok == DFTU_TOK_REDUCE ||
                               tok == DFTU_TOK_BOOL;
            if (v.kind == DFTU_SCALAR_TAG_STR || !(real || (count && whole)))
                plugin_refusal(dstd::String(a.plugin), op,
                               "the arguments do not fit");
            out.push_back(v.kind == DFTU_SCALAR_TAG_F64 ? v.value.d
                          : v.kind == DFTU_SCALAR_TAG_U64
                              ? static_cast<double>(v.value.u)
                              : static_cast<double>(v.value.i));
        }
        return out;
    }

    static dataframe::DataType plugin_type(const dstd::String& name) {
        switch (DFTU_OP_SIG_RET(dftu_op_find(name.c_str())->sig)) {
            case DFTU_TOK_I64:
                return dataframe::scalar(dataframe::TypeId::Int64);
            case DFTU_TOK_BOOL:
                return dataframe::scalar(dataframe::TypeId::Bool);
            default:
                return dataframe::scalar(dataframe::TypeId::Float64);
        }
    }

    static std::int64_t floor_div(std::int64_t a, std::int64_t b) {
        std::int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }

    // Copies each row into every window `[s, s + W)` that holds its time,
    // with `s = T + k * E`, and writes `s` to `__duql_bucket`. A null time
    // drops the row.
    void hop_rows(const duql::PipelineBucket& b) {
        const dstd::String time = "__duql_time";
        with_column(time,
                    compile(column_item(dstd::String(roles_.time)).term,
                            dstd::String(roles_.time)),
                    dstd::String(roles_.time));
        const std::size_t at = *index(time);
        const dataframe::TypeId tid = cols_[at].type.id;
        const auto whole = [](double x) { return std::trunc(x) == x; };
        const double step = *b.every;
        const bool exact = (tid == dataframe::TypeId::Int64 ||
                            tid == dataframe::TypeId::Uint64) &&
                           whole(b.width) && whole(step) && whole(b.origin);
        const dataframe::TypeId out_type =
            exact ? dataframe::TypeId::Int64 : dataframe::TypeId::Float64;
        const double width = b.width;
        const double origin = b.origin;
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({"__duql_bucket", dataframe::scalar(out_type)});
        const auto windows =
            static_cast<std::size_t>(std::max(1.0, std::ceil(width / step)));
        map_rows(
            [at, exact, width, step, origin, windows](dataframe::DataFrame f) {
                const std::int64_t n = f.num_rows();
                dstd::Vector<std::int64_t> parent;
                parent.reserve(static_cast<std::size_t>(n) * windows);
                dstd::Vector<std::int64_t> starts_i;
                dstd::Vector<double> starts_f;
                if (exact) {
                    const dataframe::Series t =
                        f.columns[at]
                            .cast(dataframe::TypeId::Int64)
                            .materialize();
                    const std::int64_t* v = t.data<std::int64_t>();
                    const auto w = static_cast<std::int64_t>(width);
                    const auto e = static_cast<std::int64_t>(step);
                    const auto o = static_cast<std::int64_t>(origin);
                    starts_i.reserve(parent.capacity());
                    for (std::int64_t r = 0; r < n; ++r) {
                        if (t.is_null(r)) continue;
                        const std::int64_t u = v[r] - o;
                        const std::int64_t hi = floor_div(u, e);
                        for (std::int64_t k = floor_div(u - w, e) + 1; k <= hi;
                             ++k) {
                            parent.push_back(r);
                            starts_i.push_back(o + k * e);
                        }
                    }
                } else {
                    const dataframe::Series t =
                        f.columns[at]
                            .cast(dataframe::TypeId::Float64)
                            .materialize();
                    const double* v = t.data<double>();
                    starts_f.reserve(parent.capacity());
                    for (std::int64_t r = 0; r < n; ++r) {
                        if (t.is_null(r)) continue;
                        const double u = v[r] - origin;
                        const auto hi =
                            static_cast<std::int64_t>(std::floor(u / step));
                        for (auto k = static_cast<std::int64_t>(
                                          std::floor((u - width) / step)) +
                                      1;
                             k <= hi; ++k) {
                            parent.push_back(r);
                            starts_f.push_back(origin +
                                               static_cast<double>(k) * step);
                        }
                    }
                }
                const auto m = static_cast<std::int64_t>(parent.size());
                dataframe::DataFrame out;
                for (std::size_t j = 0; j < f.columns.size(); ++j) {
                    out.names.emplace_back(f.names[j]);
                    out.columns.push_back(f.columns[j].take(parent));
                }
                out.names.emplace_back("__duql_bucket");
                out.columns.push_back(
                    exact ? dataframe::Series::flat_i64(starts_i.data(), m)
                          : dataframe::Series::flat_f64(starts_f.data(), m));
                return out;
            },
            std::move(next),
            "hop buckets " + number_text(width) + " every " +
                number_text(step) +
                (origin != 0 ? " at " + number_text(origin) : ""));
    }

    void frame_group(const duql::PipelineGroup& g,
                     const dstd::Optional<duql::PipelineBucket>& bucket) {
        dstd::Vector<dstd::String> keys;
        if (bucket) {
            if (bucket->every)
                hop_rows(*bucket);
            else
                with_column("__duql_bucket",
                            compile(bucket->key, dstd::String(bucket->text)),
                            dstd::String(bucket->text));
            keys.push_back("__duql_bucket");
        }
        for (std::size_t k = 0; k < g.keys.size(); ++k)
            keys.push_back(
                source(g.keys[k], "__duql_key_" + dstd::to_string(k)));
        dstd::Vector<dataframe::GroupAgg> specs;
        dstd::Vector<duql::FoldAgg> fold(g.aggs.size());
        dstd::Vector<dstd::String> by(g.aggs.size());
        bool folded = false;
        for (std::size_t i = 0; i < g.aggs.size(); ++i) {
            const auto& a = g.aggs[i];
            const dstd::String out = "__duql_agg_" + dstd::to_string(i);
            dstd::String in;
            if (a.by)
                by[i] = source({"", a.by, duql::term_text(*a.by)},
                               "__duql_by_" + dstd::to_string(i));
            if (a.fn == duql::AggFn::CountIf) {
                in = "__duql_in_" + dstd::to_string(i);
                with_column(
                    in,
                    dataframe::expr_select(
                        compile(a.arg, dstd::String(a.text), true),
                        dataframe::expr_lit(std::int64_t{1}),
                        dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                    dstd::String(a.text));
            } else if (a.arg) {
                in = source({"", a.arg, a.text},
                            "__duql_in_" + dstd::to_string(i));
            }
            dataframe::GroupAgg spec{dataframe::Agg::Count, std_str(in),
                                     std_str(out)};
            switch (a.fn) {
                case duql::AggFn::Count:
                    spec.op = dataframe::Agg::CountValid;
                    if (!a.arg) {
                        // A key-less Count with no input gives null.
                        spec.column = "__duql_one";
                        if (!index(spec.column))
                            with_column(dstd::String(spec.column),
                                        dataframe::expr_lit(std::int64_t{1}),
                                        "1");
                    }
                    break;
                case duql::AggFn::CountIf:
                    spec.op = dataframe::Agg::CountValid;
                    break;
                case duql::AggFn::Sum:
                    spec.op = dataframe::Agg::Sum;
                    break;
                case duql::AggFn::Min:
                    spec.op = dataframe::Agg::Min;
                    break;
                case duql::AggFn::Max:
                    spec.op = dataframe::Agg::Max;
                    break;
                case duql::AggFn::Mean:
                    spec.op = dataframe::Agg::Mean;
                    break;
                case duql::AggFn::Var:
                    spec.op = dataframe::Agg::Var;
                    break;
                case duql::AggFn::Std:
                    spec.op = dataframe::Agg::Std;
                    break;
                case duql::AggFn::First:
                    spec.op = dataframe::Agg::First;
                    break;
                case duql::AggFn::Last:
                    spec.op = dataframe::Agg::Last;
                    break;
                case duql::AggFn::Quantile:
                    spec.op = dataframe::Agg::Pct;
                    spec.param = a.q;
                    if (a.merged) fold[i].op = duql::FoldOp::MergeQuantile;
                    break;
                case duql::AggFn::Histogram:
                    spec.op = dataframe::Agg::Hist;
                    break;
                case duql::AggFn::Busy:
                case duql::AggFn::Concurrency:
                case duql::AggFn::Utilization:
                case duql::AggFn::Active:
                    refuse("'" + agg_name(a.fn) +
                           "' needs the trace aggregation");
                case duql::AggFn::CountDistinct:
                    fold[i].op = duql::FoldOp::CountDistinct;
                    break;
                case duql::AggFn::Collect:
                    fold[i].op = duql::FoldOp::Collect;
                    break;
                case duql::AggFn::Argmax:
                    fold[i].op = duql::FoldOp::Argmax;
                    break;
                case duql::AggFn::Argmin:
                    fold[i].op = duql::FoldOp::Argmin;
                    break;
                case duql::AggFn::Sketch:
                    fold[i].op = duql::FoldOp::Sketch;
                    break;
                case duql::AggFn::Merge:
                    fold[i].op = duql::FoldOp::Merge;
                    break;
                case duql::AggFn::Plugin:
                    fold[i].op = duql::FoldOp::Plugin;
                    fold[i].plugin = a.plugin;
                    fold[i].params = plugin_params(a);
                    break;
            }
            folded = folded || fold[i].op != duql::FoldOp::Engine;
            fold[i].engine = dataframe::to_agg_op(spec.op);
            fold[i].param = spec.param;
            specs.push_back(std::move(spec));
        }
        if (!folded) {
            v_ = v_.group_by(std_strs(keys), specs);
            columns_from_view();
            return;
        }
        fold_group(keys, specs, by, std::move(fold));
    }

    // A group with an aggregate the engine lacks: one streaming pass, in
    // input order, that folds the engine's aggregates and duql's.
    void fold_group(const dstd::Vector<dstd::String>& keys,
                    const dstd::Vector<dataframe::GroupAgg>& specs,
                    const dstd::Vector<dstd::String>& by,
                    dstd::Vector<duql::FoldAgg> fold) {
        dstd::Vector<dstd::String> read = keys;
        auto at = [&](dstd::StringView name) {
            const auto it = std::find(read.begin(), read.end(), name);
            if (it != read.end())
                return static_cast<std::int32_t>(it - read.begin());
            read.emplace_back(name);
            return static_cast<std::int32_t>(read.size() - 1);
        };
        for (std::size_t i = 0; i < fold.size(); ++i) {
            if (!specs[i].column.empty()) fold[i].in = at(specs[i].column);
            if (!by[i].empty()) fold[i].by = at(by[i]);
        }
        dataframe::Schema schema;
        dstd::Vector<dataframe::DataType> key_types;
        for (const auto& k : keys) {
            key_types.push_back(type_of(k));
            schema.fields.push_back(
                dataframe::Field{std_str(k), type_of(k), true});
        }
        dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in;
        for (const auto& n : read) in.emplace_back(n, type_of(n).id);
        for (std::size_t i = 0; i < fold.size(); ++i) {
            auto& f = fold[i];
            const dataframe::DataType t =
                f.in < 0 ? dataframe::scalar(dataframe::TypeId::Int64)
                         : type_of(read[static_cast<std::size_t>(f.in)]);
            switch (f.op) {
                case duql::FoldOp::Engine:
                    f.type = dataframe::agg_output_type(f.engine, t.id);
                    break;
                case duql::FoldOp::CountDistinct:
                    f.type = dataframe::scalar(dataframe::TypeId::Int64);
                    break;
                case duql::FoldOp::Collect:
                    f.type = dataframe::list_of(t);
                    break;
                case duql::FoldOp::Argmax:
                case duql::FoldOp::Argmin:
                    f.type = t;
                    break;
                case duql::FoldOp::Plugin:
                    f.type = plugin_type(dstd::String(f.plugin));
                    break;
                case duql::FoldOp::Sketch:
                case duql::FoldOp::Merge:
                    f.type = dataframe::scalar(dataframe::TypeId::String);
                    break;
                case duql::FoldOp::MergeQuantile:
                    f.type = dataframe::scalar(dataframe::TypeId::Float64);
                    break;
            }
            schema.fields.push_back(dataframe::Field{
                std_str("__duql_agg_" + dstd::to_string(i)), f.type, true});
        }
        lines.push_back("group fold over " + joined(read) +
                        " (one pass, in input order)");
        v_ = v_.with_lazy(
            dataframe::LazyFrame::scan(dstd::make_shared<FoldSource>(
                v_.lazy().select(std_strs(read)), std::move(schema),
                std::move(in), std::move(key_types), std::move(fold))));
        columns_from_view();
    }

    void finish(FinishSource::Finish fn, const dstd::String& line,
                dstd::Optional<dataframe::Schema> schema = dstd::nullopt) {
        lines.push_back(line);
        if (!schema) schema = v_.output_schema();
        v_ = v_.with_lazy(
            dataframe::LazyFrame::scan(dstd::make_shared<FinishSource>(
                v_.lazy(), std::move(*schema), std::move(fn))));
    }

    // `agg` over no rows: one row, counts 0 and the rest null.
    void finish_empty(const duql::PipelineGroup& g) {
        dstd::Vector<dstd::String> counts;
        for (const auto& a : g.aggs)
            if (is_count(a.fn)) counts.push_back(dstd::String(a.name));
        dstd::Vector<dstd::Pair<dstd::String, dataframe::DataType>> cols;
        for (const auto& c : cols_) cols.emplace_back(c.name, c.type);
        finish(
            [counts, cols](dataframe::DataFrame f) {
                if (f.num_rows() > 0) return f;
                dataframe::DataFrame row;
                const std::int64_t zero = 0;
                for (const auto& [name, type] : cols) {
                    row.names.emplace_back(name);
                    if (std::find(counts.begin(), counts.end(), name) !=
                        counts.end())
                        row.columns.push_back(
                            dataframe::Series::flat_i64(&zero, 1));
                    else
                        row.columns.push_back(null_cell(type));
                }
                return row;
            },
            "one row when empty");
    }

    // `bucket ... fill`: a row for every bucket between the first and the
    // last, or over the `from .. to` range, for each group. `forward` and
    // `linear` then fill the nulls of the added rows from the group's real
    // rows.
    void finish_fill(const duql::PipelineGroup& g,
                     const duql::PipelineBucket& b) {
        dstd::Vector<dstd::String> counts;
        for (const auto& a : g.aggs)
            if (is_count(a.fn)) counts.push_back(dstd::String(a.name));
        const std::size_t n_keys = g.keys.size();
        const double width = b.every.value_or(b.width);
        const double origin = b.origin;
        const std::uint64_t limit = fill_max_rows();
        const duql::syntax::FillMode mode = b.fill_mode;
        const dstd::Optional<double> range_lo = b.low;
        const dstd::Optional<double> range_hi = b.high;
        finish(
            [counts, n_keys, width, origin, limit, mode, range_lo,
             range_hi](dataframe::DataFrame f) {
                const std::int64_t n = f.num_rows();
                if (n == 0) return f;
                const dataframe::Series bucket =
                    f.columns[0].cast(dataframe::TypeId::Float64);
                const double* at = bucket.data<double>();
                double lo = std::numeric_limits<double>::infinity();
                double hi = -lo;
                for (std::int64_t r = 0; r < n; ++r) {
                    if (bucket.is_null(r)) continue;
                    lo = std::min(lo, at[r]);
                    hi = std::max(hi, at[r]);
                }
                if (range_lo) {
                    lo = origin +
                         std::floor((*range_lo - origin) / width) * width;
                    hi = origin +
                         std::floor((*range_hi - origin) / width) * width;
                }
                if (lo > hi) return f;
                const auto steps = static_cast<std::uint64_t>(
                                       std::llround((hi - lo) / width)) +
                                   1;
                dstd::Map<dstd::String, std::int64_t> groups;
                dstd::Vector<std::int64_t> reps;
                dstd::Map<dstd::Pair<dstd::String, std::int64_t>, bool> present;
                const auto texts = [n_keys](const dataframe::DataFrame& fr) {
                    dstd::Vector<dataframe::Series> text;
                    for (std::size_t c = 1; c <= n_keys; ++c)
                        text.push_back(fr.columns[c].type() ==
                                               dataframe::TypeId::String
                                           ? fr.columns[c].share()
                                           : fr.columns[c].cast(
                                                 dataframe::TypeId::String));
                    return text;
                };
                const auto key_in =
                    [](const dstd::Vector<dataframe::Series>& text,
                       std::int64_t r) {
                        dstd::String k;
                        for (const dataframe::Series& s : text) {
                            if (s.is_null(r)) {
                                k += '\x01';
                            } else {
                                k += '\x02';
                                k += s.string_at(r);
                            }
                            k += '\0';
                        }
                        return k;
                    };
                const dstd::Vector<dataframe::Series> text = texts(f);
                auto key_of = [&](std::int64_t r) { return key_in(text, r); };
                for (std::int64_t r = 0; r < n; ++r) {
                    dstd::String k = key_of(r);
                    if (groups
                            .emplace(k, static_cast<std::int64_t>(reps.size()))
                            .second)
                        reps.push_back(r);
                    if (!bucket.is_null(r))
                        present[{k, std::llround((at[r] - lo) / width)}] = true;
                }
                if (steps * reps.size() > limit)
                    refuse("'bucket ... fill' would make " +
                           dstd::to_string(steps) + " buckets x " +
                           dstd::to_string(reps.size()) +
                           " groups, over DUQL_FILL_MAX_ROWS (" +
                           dstd::to_string(limit) + ")");
                dstd::Vector<std::int64_t> take;
                dstd::Vector<double> starts;
                for (std::uint64_t s = 0; s < steps; ++s)
                    for (const auto& [k, at_rep] : groups)
                        if (!present.count({k, static_cast<std::int64_t>(s)})) {
                            take.push_back(
                                reps[static_cast<std::size_t>(at_rep)]);
                            starts.push_back(lo +
                                             static_cast<double>(s) * width);
                        }
                if (take.empty()) return f;
                dstd::Vector<dstd::String> key_names(
                    f.names.begin(), f.names.begin() + 1 + n_keys);
                dataframe::DataFrame add =
                    f.select(std_strs(key_names)).take(take);
                const auto m = static_cast<std::int64_t>(take.size());
                add.columns[0] = dataframe::Series::flat_f64(starts.data(), m)
                                     .cast(f.columns[0].type());
                const dstd::Vector<std::int64_t> zeros(
                    static_cast<std::size_t>(m), 0);
                for (const auto& c : counts) {
                    add.names.emplace_back(c);
                    add.columns.push_back(
                        dataframe::Series::flat_i64(zeros.data(), m));
                }
                const bool carry = mode != duql::syntax::FillMode::Zero;
                dataframe::DataFrame flagged;
                const dataframe::DataFrame* base = &f;
                if (carry) {
                    const dstd::Vector<std::int64_t> none(
                        static_cast<std::size_t>(n), 0);
                    const dstd::Vector<std::int64_t> ones(
                        static_cast<std::size_t>(m), 1);
                    flagged = f.with_column(
                        "__duql_added",
                        dataframe::Series::flat_i64(none.data(), n));
                    add.names.emplace_back("__duql_added");
                    add.columns.push_back(
                        dataframe::Series::flat_i64(ones.data(), m));
                }
                dataframe::DataFrame out = dataframe::concat(
                    {base, &add}, dataframe::ConcatHow::Diagonal);
                dstd::Vector<dstd::String> keep = strings_of(f.names);
                if (carry) keep.push_back("__duql_added");
                out = out.select(std_strs(keep))
                          .sort_by_multi(
                              std_strs(key_names),
                              dstd::Vector<bool>(key_names.size(), false));
                if (!carry) return out;
                const std::int64_t total = out.num_rows();
                const dataframe::Series added_col =
                    out.columns.back().materialize();
                const std::int64_t* added = added_col.data<std::int64_t>();
                const dataframe::Series pos =
                    out.columns[0].cast(dataframe::TypeId::Float64);
                const double* position = pos.data<double>();
                const dstd::Vector<dataframe::Series> sorted_text = texts(out);
                dstd::Map<dstd::String, dstd::Vector<std::int64_t>> members;
                for (std::int64_t r = 0; r < total; ++r)
                    if (!pos.is_null(r))
                        members[key_in(sorted_text, r)].push_back(r);
                const bool linear = mode == duql::syntax::FillMode::Linear;
                for (std::size_t c = 1 + n_keys; c < f.names.size(); ++c) {
                    if (std::find(counts.begin(), counts.end(),
                                  dstd::StringView(f.names[c])) != counts.end())
                        continue;
                    const dataframe::Series& col = out.columns[c];
                    const bool numeric =
                        dataframe::is_numeric_dispatchable(col.type());
                    if (linear && !numeric) continue;
                    if (!linear) {
                        dstd::Vector<std::int64_t> pick(
                            static_cast<std::size_t>(total));
                        for (std::int64_t r = 0; r < total; ++r) pick[r] = r;
                        for (const auto& [k, rows] : members) {
                            std::int64_t last = -1;
                            for (const std::int64_t r : rows) {
                                if (added[r] == 0) {
                                    if (!col.is_null(r)) last = r;
                                } else if (last >= 0) {
                                    pick[r] = last;
                                }
                            }
                        }
                        out.columns[c] = col.take(pick);
                        continue;
                    }
                    const dataframe::Series num =
                        col.cast(dataframe::TypeId::Float64);
                    const double* v = num.data<double>();
                    dstd::Vector<double> value(v, v + total);
                    dstd::Vector<std::uint8_t> valid(
                        static_cast<std::size_t>((total + 7) / 8), 0);
                    for (std::int64_t r = 0; r < total; ++r)
                        if (!col.is_null(r))
                            valid[static_cast<std::size_t>(r / 8)] |=
                                static_cast<std::uint8_t>(1U << (r % 8));
                    for (const auto& [k, rows] : members) {
                        dstd::Vector<std::int64_t> before(rows.size(), -1);
                        std::int64_t last = -1;
                        for (std::size_t i = 0; i < rows.size(); ++i) {
                            before[i] = last;
                            const std::int64_t r = rows[i];
                            if (added[r] == 0 && !col.is_null(r)) last = r;
                        }
                        last = -1;
                        for (std::size_t i = rows.size(); i-- > 0;) {
                            const std::int64_t r = rows[i];
                            if (added[r] == 0) {
                                if (!col.is_null(r)) last = r;
                            } else if (last >= 0 && before[i] >= 0) {
                                const std::int64_t p = before[i];
                                const double t = (position[r] - position[p]) /
                                                 (position[last] - position[p]);
                                value[static_cast<std::size_t>(r)] =
                                    v[p] + (v[last] - v[p]) * t;
                                valid[static_cast<std::size_t>(r / 8)] |=
                                    static_cast<std::uint8_t>(1U << (r % 8));
                            }
                        }
                    }
                    out.columns[c] = dataframe::Series::flat(
                                         dataframe::TypeId::Float64,
                                         value.data(), total, valid.data())
                                         .cast(col.type());
                }
                return out.select(f.names);
            },
            "fill buckets " + number_text(width) +
                (origin != 0 ? " at " + number_text(origin) : "") +
                (mode == duql::syntax::FillMode::Forward  ? " forward"
                 : mode == duql::syntax::FillMode::Linear ? " linear"
                                                          : "") +
                (b.low ? " from " + number_text(*b.low) + " to " +
                             number_text(*b.high)
                       : ""));
    }
};

// `p` as a View over its input: its build step, when it reads lookups, then
// its scan filter and projection, then its stages. With `table`, the scan
// ends, so the columns are the stages' own. The `main` plan owns the sides.
Built build(const Ctx& ctx, const duql::Pipeline& p, bool table,
            dstd::Vector<dstd::String>& lines, bool main, bool ordered) {
    View v = ctx.input(p.input);
    if (ordered && p.input.kind != duql::InputKind::Side) v = ctx.order(v);
    dstd::Optional<dstd::Vector<duql::VectorColumn>> side_cols;
    if (p.input.kind == duql::InputKind::Side) {
        const std::size_t i = p.input.side;
        side_cols = ctx.sides->columns[i];
        dataframe::Schema schema;
        for (const auto& c : *side_cols)
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        v = v.with_lazy(
            dataframe::LazyFrame::scan(dstd::make_shared<SideSource>(
                main ? ctx.sides : nullptr, ctx.sides, i, std::move(schema))));
        lines.push_back(
            dstd::String("from " + ctx.sides->program.sides[i].name +
                         (ctx.sides->stored[i]
                              ? " (rows stored in the index)"
                              : " (its side runs when the query executes)")));
    }
    dstd::Vector<std::size_t> reads;
    term_reads(p, reads);
    dstd::Vector<std::size_t> sets;
    set_reads(p, sets);
    dstd::SharedPtr<detail::BuildStep> step;
    if (!reads.empty() || !sets.empty()) {
        struct Once {
            std::once_flag flag;
            Pushdown keys;
        };
        auto once = dstd::make_shared<Once>();
        step = dstd::make_shared<detail::BuildStep>();
        step->run = [once, strong = main ? ctx.sides : nullptr,
                     weak = dstd::WeakPtr<Sides>(ctx.sides),
                     pipeline = &p](dstd::Optional<duql::Query> query) {
            std::call_once(once->flag, [&] {
                once->keys =
                    (strong ? strong : owner(weak))->prepare(*pipeline);
            });
            return pushed(once->keys, std::move(query));
        };
        v = ctx.with_step(v, step);
        for (const auto i : reads)
            lines.push_back(dstd::String("reads " +
                                         ctx.sides->program.sides[i].name +
                                         " before the scan"));
        for (const auto i : sets)
            if (std::find(reads.begin(), reads.end(), i) == reads.end())
                lines.push_back(dstd::String("reads the distinct rows of " +
                                             ctx.sides->program.sides[i].name +
                                             " before the scan"));
    }
    if (p.filter) {
        lines.push_back(
            dstd::String("scan filter: " + p.filter_text + " (pushed)"));
        v = v.filter(duql::Query::from_node(duql::clone(*p.filter)));
    }
    each_pushed(p, [&](const duql::Term&, const duql::TField& f,
                       const duql::TLookup& l, const duql::Term*) {
        lines.push_back(dstd::String("scan filter: " + f.base +
                                     " in (keys of " + l.name + ") (pushed)"));
    });
    auto adds = [&](const duql::PipelineLookup& l) {
        dstd::Vector<dstd::String> out;
        if (const auto* into = std::get_if<duql::PipelineNest>(&l.mode)) {
            out.push_back(dstd::String(into->name));
            return out;
        }
        if (const auto* o = std::get_if<duql::PipelineOverlap>(&l.mode);
            o && !o->into.empty()) {
            out.push_back(dstd::String(o->into));
            return out;
        }
        if (l.kind == duql::syntax::LookupKind::Anti) return out;
        for (const auto& c : ctx.sides->columns[l.side]) out.push_back(c.name);
        return out;
    };
    Reads fields = referenced_fields(
        p.stages, adds,
        ctx.schema(v).decoder == dftracer::utils::index::Decoder::Path);
    if (!p.scan_select.empty()) {
        lines.push_back("scan select: " + joined(p.scan_select));
        dstd::Vector<dstd::String> select;
        dstd::Optional<dstd::Vector<SchemaLeaf>> tree;
        for (const auto& f : p.scan_select) {
            const bool array =
                std::find(fields.arrays.begin(), fields.arrays.end(),
                          dstd::String(f)) != fields.arrays.end();
            if (!array && p.stages.empty()) {
                select.push_back(dstd::String(f));
                continue;
            }
            if (!tree) tree = detail::catalog_leaves(v);
            for (auto& r :
                 scan_reads(*tree, dstd::String(f), array, fields.arrays,
                            ctx.schema(v).args_fallback))
                select.push_back(std::move(r));
        }
        v = v.select(std::move(select));
        fields.fields.clear();
    }
    if (p.stages.empty() && !table) {
        dstd::Vector<duql::VectorColumn> cols;
        for (const auto& f : v.output_schema().fields)
            cols.push_back({dstd::String(f.name), f.type});
        return {std::move(v), std::move(cols)};
    }
    const index::RecordSchema& schema = ctx.schema(v);
    Applier a(std::move(v), schema, ctx, std::move(fields), step, reads, main,
              std::move(side_cols));
    if (!p.scan_select.empty()) a.selected_at_scan();
    for (const auto& s : p.stages) a.apply(s);
    if (table) a.end_scan("union");
    a.finish_columns();
    for (auto& l : a.lines) lines.push_back(std::move(l));
    dstd::Vector<duql::VectorColumn> cols = a.columns();
    return {std::move(a).take(), std::move(cols)};
}

}  // namespace

View detail::view_of(detail::ScanPlan plan) { return View(std::move(plan)); }

View detail::with_named_columns(const View& v,
                                dstd::Vector<dataframe::Field> columns) {
    return v.reshape("select", [&](const detail::ScanPlan& p) {
        return scan::name_columns(p, std::move(columns));
    });
}

dstd::Pair<View, dstd::Vector<dstd::String>> View::duql_plan(
    dstd::StringView text, const duql::Params& params) const {
    const index::RecordSchema& schema = detail::plan_record_schema(*plan_);
    Ctx ctx;
    ctx.roles = index::duql_roles(schema);
    const duql::PluginCatalog plugins = registry_plugins();
    const duql::LeafPaths leaves = [this] {
        detail::scan::ensure_catalog(plan_);
        if (const auto file = detail::scan::file_without_catalog(plan_))
            refuse(
                "wildcard paths expand against the index's path catalog, "
                "which the index of " +
                *file +
                " does not hold; build the full index (dftracer_index or "
                "BatchIndexer) first");
        dstd::Vector<dstd::String> out;
        for (const auto& l : schema_tree()) out.emplace_back(l.path);
        return out;
    };
    auto p = duql::compile_program(text, params, &ctx.roles, schema.source,
                                   &plugins, &leaves);
    if (!p) refuse(p.error().format());
    reject_resolved(*p);
    ctx.sides = dstd::make_shared<Sides>(std::move(*p));
    ctx.order = [](const View& in) {
        return in.reshape("duql", [](const detail::ScanPlan& s) {
            return detail::scan::ordered(s);
        });
    };
    ctx.with_step = [](const View& in,
                       dstd::SharedPtr<const detail::BuildStep> step) {
        return in.reshape("duql", [&](const detail::ScanPlan& s) {
            auto next = dstd::make_shared<detail::ViewPlan>(*s);
            next->build_step = step;
            return detail::ScanPlan(std::move(next));
        });
    };
    ctx.input = [this](const duql::Input& in) -> View {
        switch (in.kind) {
            case duql::InputKind::Data:
                break;
            case duql::InputKind::All:
            case duql::InputKind::Side: {
                auto all = dstd::make_shared<detail::ViewPlan>();
                all->files = plan_->files;
                all->record_schema = plan_->record_schema;
                all->rollup_root = plan_->rollup_root;
                all->views_root = plan_->views_root;
                all->memory_budget = plan_->memory_budget;
                all->cancelled = plan_->cancelled;
                all->scan_counter = plan_->scan_counter;
                all->all_records = true;
                return detail::view_of(std::move(all));
            }
            case duql::InputKind::File: {
                const dstd::String index =
                    internal::determine_index_path(dstd::String(in.path), "");
                return View::from_file(
                    dstd::String(in.path),
                    dstd::String(index.data(), index.size()));
            }
        }
        return *this;
    };
    ctx.schema = [](const View& v) -> const index::RecordSchema& {
        return detail::plan_record_schema(*v.plan_);
    };
    Sides& sides = *ctx.sides;
    dstd::Vector<bool> needed(sides.program.sides.size(), false);
    dstd::Vector<std::size_t> reads;
    side_reads(sides.program.main, reads);
    for (const auto i : reads) needed[i] = true;
    for (std::size_t i = needed.size(); i-- > 0;) {
        if (!needed[i]) continue;
        dstd::Vector<std::size_t> more;
        side_reads(sides.program.sides[i].pipeline, more);
        for (const auto j : more) needed[j] = true;
    }
    dstd::Vector<std::size_t> asof;
    asof_reads(sides.program.main, asof);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) asof_reads(sides.program.sides[i].pipeline, asof);
    dstd::Vector<std::size_t> tables;
    dstd::Vector<std::size_t> sets;
    term_reads(sides.program.main, tables);
    set_reads(sides.program.main, sets);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) {
            term_reads(sides.program.sides[i].pipeline, tables);
            set_reads(sides.program.sides[i].pipeline, sets);
        }
    dstd::Vector<dstd::String> lines;
    for (std::size_t i = 0; i < needed.size(); ++i) {
        if (!needed[i]) continue;
        const duql::Side& side = sides.program.sides[i];
        dstd::Vector<dstd::String> side_lines;
        Built b = build(ctx, side.pipeline, false, side_lines, false,
                        std::find(asof.begin(), asof.end(), i) != asof.end());
        sides.keys[i] =
            cache_key(*b.view.plan_, side.pipeline.input.kind, side.key);
        if (side.rowset) {
            for (const auto& f : plan_->files)
                sides.files[i].push_back(
                    {dstd::String(dstd::StringView(f.file_path)),
                     f.index_path.empty()
                         ? internal::determine_index_path(f.file_path, "")
                         : dstd::String(dstd::StringView(f.index_path))});
            sides.stored[i] = index::plan::has_stored_rowsets(sides.files[i]);
        }
        sides.views[i] = std::move(b.view);
        sides.columns[i] = std::move(b.cols);
        // A column whose type only the data gives (a dftracer arg) takes it
        // from the side's rows: all of them when a table reads them anyway,
        // its distinct rows for a key set, else the first TYPE_PROBE_ROWS, so
        // a joined side stays uncapped.
        if (std::any_of(sides.columns[i].begin(), sides.columns[i].end(),
                        [](const duql::VectorColumn& c) {
                            return c.type.id == dataframe::TypeId::Unknown;
                        })) {
            const bool table =
                std::find(tables.begin(), tables.end(), i) != tables.end();
            const bool set =
                std::find(sets.begin(), sets.end(), i) != sets.end();
            const auto frame = table ? sides.frame(i)
                               : set ? sides.key_frame(i)
                                     : sides.probe(i);
            for (auto& c : sides.columns[i])
                for (std::size_t k = 0; k < frame->names.size(); ++k)
                    if (c.type.id == dataframe::TypeId::Unknown &&
                        dstd::StringView(frame->names[k]) == c.name)
                        c.type = frame->columns[k].data_type();
        }
        lines.push_back(dstd::String("side " + side.name + ": from ") +
                        input_text(side.pipeline.input, sides.program) + "; " +
                        (sides.stored[i] ? dstd::String("stored in the index")
                                         : cache_state(sides.keys[i])));
        if (!sides.stored[i])
            for (auto& l : side_lines) lines.push_back("  " + std::move(l));
    }
    Built main = build(ctx, sides.program.main, false, lines, true);
    return {std::move(main.view), std::move(lines)};
}

View View::duql(dstd::StringView text, const duql::Params& params) const {
    return duql_plan(text, params).first;
}

dstd::String View::explain_duql(dstd::StringView text,
                                const duql::Params& params) const {
    dstd::String out;
    for (const auto& l : duql_plan(text, params).second) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace dftracer::utils::trace::views
