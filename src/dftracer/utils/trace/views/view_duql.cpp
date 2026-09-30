#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

namespace {

namespace df = dftracer::utils::dataframe;
namespace ix = dftracer::utils::index;

[[noreturn]] void refuse(const std::string& why) {
    throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT, "View::duql ",
                                 why);
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

bool is_integer(df::TypeId t) {
    return t == df::TypeId::Int8 || t == df::TypeId::Int16 ||
           t == df::TypeId::Int32 || t == df::TypeId::Int64 ||
           t == df::TypeId::Uint8 || t == df::TypeId::Uint16 ||
           t == df::TypeId::Uint32 || t == df::TypeId::Uint64;
}

bool is_number(df::TypeId t) {
    return is_integer(t) || t == df::TypeId::Float32 ||
           t == df::TypeId::Float64;
}

// Rows of `inner`, collected, then passed through `finish` before any op
// above reads them.
class FinishSource final : public df::Source {
   public:
    using Finish = std::function<df::DataFrame(df::DataFrame)>;

    FinishSource(df::LazyFrame inner, df::Schema schema, Finish finish)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          finish_(std::move(finish)) {}

    df::Schema schema() const override { return schema_; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        auto rows = std::make_unique<Rows>(inner_, finish_, req.projection);
        rows->named_ = schema_.fields.empty();
        r.cursor = std::move(rows);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    class Rows final : public df::Cursor {
       public:
        Rows(df::LazyFrame inner, Finish finish,
             std::vector<std::string> projection)
            : inner_(std::move(inner)),
              finish_(std::move(finish)),
              projection_(std::move(projection)) {}

        coro::CoroTask<std::optional<df::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                df::DataFrame f = finish_(co_await inner_.collect());
                if (!projection_.empty()) f = f.select(projection_);
                if (named_) {
                    intern_ = std::make_shared<dftracer::utils::StringIntern>();
                    for (const auto& n : f.names)
                        ids_.push_back(intern_->get_or_insert(n));
                }
                rows_ = df::InMemorySource(std::move(f)).scan({}).cursor;
            }
            auto m = co_await rows_->next(max_rows);
            if (m && named_) {
                m->name_ids = ids_;
                m->intern = intern_;
            }
            co_return m;
        }

        // Names each morsel's columns, for a result whose columns only the
        // data gives.
        bool named_ = false;

       private:
        df::LazyFrame inner_;
        Finish finish_;
        std::vector<std::string> projection_;
        std::unique_ptr<df::Cursor> rows_;
        std::shared_ptr<dftracer::utils::StringIntern> intern_;
        std::vector<std::uint32_t> ids_;
    };

    df::LazyFrame inner_;
    df::Schema schema_;
    Finish finish_;
};

// Rows of `inner`, each batch passed through `map` as it streams.
class MapSource final : public df::Source {
   public:
    using Map = std::function<df::DataFrame(df::DataFrame)>;

    MapSource(df::LazyFrame inner, df::Schema schema, Map map)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          map_(std::move(map)) {}

    df::Schema schema() const override { return schema_; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<Rows>(inner_, map_, req.projection);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    class Rows final : public df::Cursor {
       public:
        Rows(const df::LazyFrame& inner, Map map,
             std::vector<std::string> projection)
            : batches_(inner.stream()),
              map_(std::move(map)),
              projection_(std::move(projection)) {}

        coro::CoroTask<std::optional<df::Morsel>> next(
            std::int64_t max_rows) override {
            for (;;) {
                if (rows_)
                    if (auto m = co_await rows_->next(max_rows)) co_return m;
                auto batch = co_await batches_.next();
                if (!batch) co_return std::nullopt;
                df::DataFrame f = map_(std::move(*batch));
                if (!projection_.empty()) f = f.select(projection_);
                rows_ = df::InMemorySource(std::move(f)).scan({}).cursor;
            }
        }

       private:
        coro::AsyncGenerator<df::DataFrame> batches_;
        Map map_;
        std::vector<std::string> projection_;
        std::unique_ptr<df::Cursor> rows_;
    };

    df::LazyFrame inner_;
    df::Schema schema_;
    Map map_;
};

// `f` with the columns `in` names, in order; one its rows lack is null.
df::DataFrame aligned(
    df::DataFrame f,
    const std::vector<std::pair<std::string, df::TypeId>>& in) {
    bool same = f.names.size() == in.size();
    for (std::size_t i = 0; same && i < in.size(); ++i)
        same = f.names[i] == in[i].first;
    if (same) return f;
    df::DataFrame out;
    const std::int64_t n = f.num_rows();
    for (const auto& [name, type] : in) {
        out.names.push_back(name);
        const auto it = std::find(f.names.begin(), f.names.end(), name);
        if (it != f.names.end()) {
            out.columns.push_back(
                f.columns[static_cast<std::size_t>(it - f.names.begin())]
                    .share());
            continue;
        }
        const bool scalar = type != df::TypeId::Unknown &&
                            type != df::TypeId::List &&
                            type != df::TypeId::Struct;
        out.columns.push_back(
            df::Series::nulls(scalar ? type : df::TypeId::String, n));
    }
    return out;
}

// One row per group of `inner`'s rows, folded by a GroupFold as they
// stream.
class FoldSource final : public df::Source {
   public:
    FoldSource(df::LazyFrame inner, df::Schema schema,
               std::vector<std::pair<std::string, df::TypeId>> in,
               std::vector<df::DataType> key_types,
               std::vector<duql::FoldAgg> aggs)
        : inner_(std::move(inner)),
          schema_(std::move(schema)),
          in_(std::move(in)),
          key_types_(std::move(key_types)),
          aggs_(std::move(aggs)) {}

    df::Schema schema() const override { return schema_; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<Rows>(*this, req.projection);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    class Rows final : public df::Cursor {
       public:
        Rows(const FoldSource& src, std::vector<std::string> projection)
            : batches_(src.inner_.stream()),
              fold_(src.key_types_, src.aggs_),
              in_(src.in_),
              projection_(std::move(projection)) {
            for (const auto& f : src.schema_.fields) names_.push_back(f.name);
        }

        coro::CoroTask<std::optional<df::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                while (auto batch = co_await batches_.next())
                    fold_.add(aligned(std::move(*batch), in_));
                df::DataFrame f = fold_.finish(std::move(names_));
                if (!projection_.empty()) f = f.select(projection_);
                rows_ = df::InMemorySource(std::move(f)).scan({}).cursor;
            }
            co_return co_await rows_->next(max_rows);
        }

       private:
        coro::AsyncGenerator<df::DataFrame> batches_;
        duql::GroupFold fold_;
        std::vector<std::pair<std::string, df::TypeId>> in_;
        std::vector<std::string> names_;
        std::vector<std::string> projection_;
        std::unique_ptr<df::Cursor> rows_;
    };

    df::LazyFrame inner_;
    df::Schema schema_;
    std::vector<std::pair<std::string, df::TypeId>> in_;
    std::vector<df::DataType> key_types_;
    std::vector<duql::FoldAgg> aggs_;
};

std::uint64_t pivot_max_columns() {
    if (const char* v = std::getenv("DUQL_PIVOT_MAX_COLUMNS"))
        return std::strtoull(v, nullptr, 10);
    return 1024;
}

std::uint64_t fill_max_rows() {
    if (const char* v = std::getenv("DUQL_FILL_MAX_ROWS"))
        return std::strtoull(v, nullptr, 10);
    return 10'000'000;
}

std::string agg_name(duql::AggFn fn) {
    switch (fn) {
        case duql::AggFn::COUNT:
            return "count";
        case duql::AggFn::COUNT_IF:
            return "count_if";
        case duql::AggFn::SUM:
            return "sum";
        case duql::AggFn::MIN:
            return "min";
        case duql::AggFn::MAX:
            return "max";
        case duql::AggFn::MEAN:
            return "mean";
        case duql::AggFn::VAR:
            return "var";
        case duql::AggFn::STD:
            return "std";
        case duql::AggFn::FIRST:
            return "first";
        case duql::AggFn::LAST:
            return "last";
        case duql::AggFn::QUANTILE:
            return "quantile";
        case duql::AggFn::HISTOGRAM:
            return "histogram";
        case duql::AggFn::BUSY:
            return "busy";
        case duql::AggFn::CONCURRENCY:
            return "concurrency";
        case duql::AggFn::UTILIZATION:
            return "utilization";
        case duql::AggFn::ACTIVE:
            return "active";
        case duql::AggFn::COUNT_DISTINCT:
            return "count_distinct";
        case duql::AggFn::COLLECT:
            return "collect";
        case duql::AggFn::ARGMAX:
            return "arg_max";
        case duql::AggFn::ARGMIN:
            return "arg_min";
        case duql::AggFn::SKETCH:
            return "sketch";
        case duql::AggFn::MERGE:
            return "merge";
    }
    return {};
}

std::optional<AggOp> trace_op(const duql::PipelineAgg& a) {
    switch (a.fn) {
        case duql::AggFn::COUNT:
            if (a.arg) return std::nullopt;
            return AggOp::Count;
        case duql::AggFn::SUM:
            return AggOp::Sum;
        case duql::AggFn::MIN:
            return AggOp::Min;
        case duql::AggFn::MAX:
            return AggOp::Max;
        case duql::AggFn::MEAN:
            return AggOp::Mean;
        case duql::AggFn::VAR:
            return AggOp::Var;
        case duql::AggFn::STD:
            return AggOp::Std;
        case duql::AggFn::QUANTILE:
            if (a.merged || !(a.q > 0 && a.q < 1)) return std::nullopt;
            return AggOp::Pct;
        case duql::AggFn::HISTOGRAM:
            return AggOp::Hist;
        case duql::AggFn::BUSY:
            return AggOp::Busy;
        case duql::AggFn::CONCURRENCY:
            return AggOp::Concurrency;
        case duql::AggFn::UTILIZATION:
            return AggOp::Utilization;
        case duql::AggFn::ACTIVE:
            return AggOp::Active;
        case duql::AggFn::COUNT_IF:
        case duql::AggFn::FIRST:
        case duql::AggFn::LAST:
        case duql::AggFn::COUNT_DISTINCT:
        case duql::AggFn::COLLECT:
        case duql::AggFn::ARGMAX:
        case duql::AggFn::ARGMIN:
        case duql::AggFn::SKETCH:
        case duql::AggFn::MERGE:
            return std::nullopt;
    }
    return std::nullopt;
}

// The field a term reads when it is nothing but a whole record field.
const duql::TField* whole_field(const duql::Term* t) {
    if (!t) return nullptr;
    const auto* f = std::get_if<duql::TField>(&t->node);
    if (!f || f->root != duql::FieldRoot::RECORD ||
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
    if (!f || f->root != duql::FieldRoot::RECORD ||
        f->neg_at != f->steps.size())
        return nullptr;
    return f;
}

// `a[0].b` as the dotted path `a.0.b` the index catalog records.
std::string dotted(std::string_view path) {
    std::string out;
    for (const char c : path) {
        if (c == '[')
            out += '.';
        else if (c != ']')
            out += c;
    }
    return out;
}

std::string number_text(double v);

// A pivot value as its column name suffix: as a duql literal, with a
// string as it is and null as `null`.
std::string pivot_name(const duql::TConst& c) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, duql::TNull>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, double>)
                return number_text(v);
            else
                return std::to_string(v);
        },
        c.value);
}

// The fields a pipeline reads from records, and those it reads as arrays.
// `closed`: a later stage sets the output columns (group, agg, select, pivot,
// distinct keys) and no earlier stage compares whole rows, so the scan need
// read only `fields`.
struct Reads {
    std::vector<std::string> fields;
    std::vector<std::string> arrays;
    bool closed = false;
};

// The record fields the stages read: every field a term names before an
// earlier stage defines a column of that name. An array is a field `expand`
// or a quantifier reads as a whole.
Reads referenced_fields(
    const std::vector<duql::PipelineStage>& stages,
    const std::function<std::vector<std::string>(const duql::PipelineLookup&)>&
        adds) {
    Reads out;
    std::vector<std::string> defined;
    auto known = [](const std::vector<std::string>& v, const std::string& n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    auto field = [&](const std::string& base, bool array) {
        if (known(defined, base)) return;
        if (!known(out.fields, base)) out.fields.push_back(base);
        if (array && !known(out.arrays, base)) out.arrays.push_back(base);
    };
    auto add = [&](const duql::TermRef& t) {
        if (!t) return;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* f = std::get_if<duql::TField>(&x.node);
                f && f->root != duql::FieldRoot::ELEMENT)
                field(f->base, false);
            if (const auto* q = std::get_if<duql::TQuant>(&x.node))
                if (const auto* a = array_field(*q->subject))
                    field(a->base, true);
        });
    };
    auto items = [&](const std::vector<duql::PipelineItem>& v) {
        for (const auto& it : v) add(it.term);
    };
    auto define = [&](const std::vector<duql::PipelineItem>& v) {
        for (const auto& it : v) defined.push_back(it.name);
    };
    auto sort_keys = [&](const std::vector<duql::PipelineSortKey>& v) {
        for (const auto& k : v) add(k.key.term);
    };
    bool whole_rows = false;
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
                        field(pair.second, false);
                        defined.push_back(pair.first);
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
                    for (const auto& a : s.aggs) defined.push_back(a.name);
                    defined.push_back("bucket");
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineTimeRange>) {
                    add(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineBucket>) {
                    add(s.key);
                } else if constexpr (std::is_same_v<T, duql::PipelineSession>) {
                    items(s.keys);
                    add(s.time);
                    add(s.end);
                    defined.push_back(s.name);
                } else if constexpr (std::is_same_v<T, duql::PipelineWindow>) {
                    items(s.keys);
                    sort_keys(s.order);
                    for (const auto& c : s.calls) {
                        add(c.arg);
                        defined.push_back(c.column);
                    }
                    items(s.items);
                    define(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineExpand>) {
                    field(s.path, true);
                    defined.push_back(s.name);
                    if (!s.index.empty()) defined.push_back(s.index);
                } else if constexpr (std::is_same_v<T, duql::PipelinePivot>) {
                    add(s.key.term);
                    for (const auto& a : s.aggs) {
                        add(a.arg);
                        add(a.by);
                    }
                    for (const auto& a : s.aggs)
                        for (const auto& v : s.values)
                            defined.push_back(a.name + "." + pivot_name(v));
                } else if constexpr (std::is_same_v<T, duql::PipelineUnpivot>) {
                    for (const auto& f : s.fields) field(f, false);
                    defined.push_back(s.key);
                    defined.push_back(s.value);
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

char type_letter(const std::string& type) {
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

df::DataType letter_type(char c) {
    switch (c) {
        case 'i':
            return df::scalar(df::TypeId::Int64);
        case 'f':
            return df::scalar(df::TypeId::Float64);
        case 'b':
            return df::scalar(df::TypeId::Bool);
        case 'a':
            return df::scalar(df::TypeId::Unknown);
        default:
            return df::scalar(df::TypeId::String);
    }
}

bool digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

// The select token prefix of a list of JSON text elements.
const std::string JSON_LIST = detail::list_token("j", "");

// The select token and column type that read the array field `path` as a
// List, typed from the element paths the index observed.
std::pair<std::string, df::DataType> list_type(
    const std::vector<SchemaLeaf>& tree, const std::string& path,
    bool args_fallback) {
    char scalar = 0;
    bool scalars = false;
    bool objects = false;
    bool nested = false;
    std::map<std::string, char> fields;
    std::vector<std::string> prefixes = {dotted(path) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(path) + ".");
    for (const auto& leaf : tree) {
        std::string_view rest;
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                rest = std::string_view(leaf.path).substr(p.size());
                break;
            }
        const std::size_t dot = rest.find('.');
        if (!digits(rest.substr(0, dot))) continue;
        const char c = type_letter(leaf.type);
        if (dot == std::string_view::npos) {
            scalars = true;
            scalar = join_letters(scalar, c);
            continue;
        }
        const std::string_view f = rest.substr(dot + 1);
        const std::size_t end = f.find('.');
        const std::string name(f.substr(0, end));
        if (digits(name)) {
            nested = true;
            continue;
        }
        objects = true;
        char& slot = fields[name];
        slot = end == std::string_view::npos ? join_letters(slot, c) : 'j';
    }
    if (objects && !scalars && !nested) {
        std::string spec = "{";
        std::vector<df::Field> members;
        for (const auto& [name, c] : fields) {
            const char k = c ? c : 's';
            if (spec.size() > 1) spec += ',';
            spec += name + "=" + k;
            members.push_back(df::Field{name, letter_type(k), true});
        }
        spec += '}';
        return {detail::list_token(spec, path),
                df::list_of(df::struct_of(std::move(members)))};
    }
    const char k = nested || objects ? 'j'
                   : scalars         ? (scalar ? scalar : 's')
                                     : 'a';
    return {detail::list_token(std::string(1, k), path),
            df::list_of(letter_type(k))};
}

// What a narrowed scan reads for the field `f`: an array (`array`, or a
// field the index saw only elements under) as one list column, added to
// `arrays`; an object as its leaf columns; anything else by name.
std::vector<std::string> scan_reads(const std::vector<SchemaLeaf>& tree,
                                    const std::string& f, bool array,
                                    std::vector<std::string>& arrays,
                                    bool args_fallback) {
    std::vector<std::string> prefixes = {dotted(f) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(f) + ".");
    bool elements = false;
    std::vector<std::string> leaves;
    for (const auto& leaf : tree)
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                const std::string_view rest =
                    std::string_view(leaf.path).substr(p.size());
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

df::TypeId observed_type(const std::string& type) {
    if (type == "int" || type == "uint") return df::TypeId::Int64;
    if (type == "float") return df::TypeId::Float64;
    if (type == "string") return df::TypeId::String;
    if (type == "bool") return df::TypeId::Bool;
    return df::TypeId::Unknown;
}

// One null row of `type`.
df::Series null_cell(const df::DataType& type) {
    if (type.id == df::TypeId::List && !type.fields.empty())
        return df::Series::list({0, 0}, null_cell(type.fields.front().type))
            .take(std::vector<std::int64_t>{-1});
    const bool scalar = type.id != df::TypeId::Unknown &&
                        type.id != df::TypeId::List &&
                        type.id != df::TypeId::Struct;
    return df::Series::nulls(scalar ? type.id : df::TypeId::String, 1);
}

// Keeps a row index plus the seed inside int64.
constexpr std::uint64_t SEED_MASK = (std::uint64_t{1} << 62) - 1;

std::string number_text(double v) {
    if (std::trunc(v) == v && std::abs(v) < 9.0e15)
        return std::to_string(static_cast<std::int64_t>(v));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool is_count(duql::AggFn fn) {
    return fn == duql::AggFn::COUNT || fn == duql::AggFn::COUNT_IF ||
           fn == duql::AggFn::COUNT_DISTINCT;
}

std::uint64_t env_count(const char* name, std::uint64_t fallback) {
    if (const char* v = std::getenv(name)) return std::strtoull(v, nullptr, 10);
    return fallback;
}

constexpr std::uint64_t LOOKUP_MAX_ROWS = 1'000'000;
constexpr std::int64_t TYPE_PROBE_ROWS = 65'536;
// Keys a side's distinct rows in the lookup cache apart from its rows.
constexpr std::uint64_t DISTINCT_CACHE_SALT = 0x64697374696e6374ULL;
constexpr std::uint64_t LOOKUP_MAX_BYTES = std::uint64_t{256} << 20;

// `resolved.` columns are gone; an arrow into a row set of the source reads
// the same value.
void reject_resolved(const duql::Program& p) {
    auto check = [](std::string_view path) {
        if (!path.starts_with("resolved.")) return;
        refuse("'" + std::string(path) +
               "': 'resolved.' columns are removed; write an arrow into a "
               "row set of the source, such as 'fhash -> files.path', "
               "'hhash -> hosts.name' or 'run -> runs.app'");
    };
    std::function<void(const duql::QueryNode&)> node =
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

std::string input_text(const duql::Input& in, const duql::Program& program) {
    switch (in.kind) {
        case duql::InputKind::SIDE:
            return program.sides[in.side].name;
        case duql::InputKind::ALL:
            return "all";
        case duql::InputKind::DATA:
            return "data";
        case duql::InputKind::FILE:
            return "\"" + in.path + "\"";
    }
    return {};
}

// The sides `p` reads directly: its lookups', its `lookup` stages' and a
// union's.
void side_reads(const duql::Pipeline& p, std::vector<std::size_t>& out) {
    if (p.input.kind == duql::InputKind::SIDE) out.push_back(p.input.side);
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
            std::vector<std::size_t> other;
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
void asof_reads(const duql::Pipeline& p, std::vector<std::size_t>& out) {
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
void term_reads(const duql::Pipeline& p, std::vector<std::size_t>& out) {
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
void set_reads(const duql::Pipeline& p, std::vector<std::size_t>& out) {
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
    const std::function<void(const duql::Term&, const duql::TField&,
                             const duql::TLookup&, const duql::Term*)>& fn) {
    std::function<void(const duql::QueryNode&)> walk =
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
            if (l && (l->kind != duql::LookupKind::IN || l->negated))
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

std::optional<duql::LiteralNode> literal_of(const df::Series& s,
                                            std::int64_t r) {
    if (s.is_null(r)) return std::nullopt;
    const df::TypeId t = s.type();
    if (t == df::TypeId::Uint64)
        return duql::LiteralNode{s.data<std::uint64_t>()[r]};
    if (is_integer(t))
        return duql::LiteralNode{
            s.slice(r, 1).cast(df::TypeId::Int64).data<std::int64_t>()[0]};
    if (t == df::TypeId::Float32 || t == df::TypeId::Float64)
        return duql::LiteralNode{
            s.slice(r, 1).cast(df::TypeId::Float64).data<double>()[0]};
    if (t == df::TypeId::Bool)
        return duql::LiteralNode{
            ((s.data<std::uint8_t>()[r >> 3] >> (r & 7)) & 1) != 0};
    if (t == df::TypeId::String || t == df::TypeId::LargeString)
        return duql::LiteralNode{std::string(s.string_at(r))};
    return std::nullopt;
}

// The key sets a scan filter takes from its pushed terms: a semi-join's
// `k in {keys}` replaces its leaf, which it equals at the top of an `and`;
// an arrow's is added beside it.
struct Pushdown {
    std::vector<std::pair<const duql::Term*, duql::Query>> replace;
    std::optional<duql::Query> extra;
};

// `query` with `p`'s key sets: the replaced leaves at the top of its `and`s
// swapped, the others added.
std::optional<duql::Query> pushed(const Pushdown& p,
                                  std::optional<duql::Query> query) {
    if (!query) return p.extra;
    duql::QueryNodePtr root = duql::clone(query->root());
    std::function<void(duql::QueryNodePtr&)> walk = [&](duql::QueryNodePtr& n) {
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
        const df::Series& keys = t.frame->columns[t.key_columns.front()];
        std::optional<df::Series> pass;
        bool record = false;
        if (cond)
            duql::for_each_term(*cond, [&](const duql::Term& x) {
                if (const auto* fx = std::get_if<duql::TField>(&x.node);
                    fx && fx->root != duql::FieldRoot::ELEMENT)
                    record = true;
                if (std::holds_alternative<duql::TLookup>(x.node) ||
                    std::holds_alternative<duql::TQuant>(x.node))
                    record = true;
            });
        if (cond && !record && t.value) {
            const df::Series& values = t.frame->columns[*t.value];
            if (auto e = duql::vectorize_condition(
                    *cond, {{".", values.data_type()}}, false))
                pass = df::eval(*e, {&values}).materialize();
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
        if (arr.elements.size() > ix::plan::SEMI_JOIN_CAP) return;
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
    std::string path;
    std::uint64_t sig = 0;
};

std::optional<CacheKey> cache_key(const detail::ViewPlan& given,
                                  duql::InputKind input,
                                  const std::string& text) {
    // A file opened without an index path reads the index beside it.
    detail::ViewPlan plan = given;
    for (auto& f : plan.files)
        if (f.index_path.empty())
            f.index_path = internal::determine_index_path(f.file_path, "");
    const std::string path = ix::cache::lookup_cache_path(plan);
    if (path.empty()) return std::nullopt;
    detail::plan_record_schema(plan);
    // A `data` side reads the View as given: its filters are part of it.
    std::string key = text;
    if (input == duql::InputKind::DATA) {
        key += "\n" + (plan.query ? plan.query->source() : std::string()) +
               "\n" + std::to_string(static_cast<int>(plan.phase)) +
               std::to_string(plan.all_records);
        if (plan.time_range)
            key += "\n" + number_text(plan.time_range->first) + ".." +
                   number_text(plan.time_range->second);
    }
    const auto sig = ix::cache::lookup_signature(plan, key);
    if (!sig) return std::nullopt;
    return CacheKey{path, *sig};
}

[[noreturn]] void over_rows(const std::string& name, std::uint64_t rows,
                            std::uint64_t max) {
    refuse("'" + name + "' gives " + std::to_string(rows) +
           " rows or more, over DUQL_LOOKUP_MAX_ROWS (" + std::to_string(max) +
           "); a lookup reads a small row set: narrow it with 'where' or "
           "'select'");
}

[[noreturn]] void over_bytes(const std::string& name, std::uint64_t bytes,
                             std::uint64_t max) {
    refuse("'" + name + "' holds " + std::to_string(bytes) +
           " bytes, over DUQL_LOOKUP_MAX_BYTES (" + std::to_string(max) +
           "); a lookup reads a small row set: narrow it with 'select'");
}

// Stores a side's rows in the lookup cache; a store that fails keeps none.
void persist(const CacheKey& key, const std::vector<df::DataFrame>& parts) {
    if (parts.empty()) return;
    try {
        const auto db = ix::cache::open_lookup_db(
            key.path, ix::store::RocksDatabase::OpenMode::ReadWrite);
        if (!db) return;
        std::vector<const df::DataFrame*> ptrs;
        for (const auto& p : parts) ptrs.push_back(&p);
        ix::cache::persist_lookup(*db, key.sig,
                                  ptrs.size() == 1
                                      ? df::frame_to_native(parts.front())
                                      : df::frame_to_native(df::concat(
                                            ptrs, df::ConcatHow::Diagonal)));
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
    std::vector<std::optional<View>> views;
    std::vector<std::vector<duql::VectorColumn>> columns;
    std::vector<std::optional<CacheKey>> keys;
    // The files of each source row set, and whether their indexes hold it.
    std::vector<std::vector<ix::plan::RowSetFile>> files;
    std::vector<char> stored;

    std::shared_ptr<const df::DataFrame> frame(std::size_t i) {
        std::call_once(once_[i], [&] { frames_[i] = run(i); });
        return frames_[i];
    }

    // The first TYPE_PROBE_ROWS rows of side `i`, for the types of its
    // columns.
    std::shared_ptr<const df::DataFrame> probe(std::size_t i) const {
        const df::LazyFrame plan = views[i]->head(TYPE_PROBE_ROWS).lazy();
        df::DataFrame f;
        dftracer::utils::default_runtime().run_blocking(
            "duql_side_types",
            [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
                f = co_await plan.collect();
                co_return;
            });
        return std::make_shared<const df::DataFrame>(std::move(f));
    }

    // The distinct rows of side `i` for a key-set `in`.
    std::shared_ptr<const df::DataFrame> key_frame(std::size_t i) {
        std::call_once(key_once_[i], [&] { key_frames_[i] = distinct(i); });
        return key_frames_[i];
    }

    // Runs the sides `p` reads, binds its lookups to their rows and gives
    // the key sets for its scan filter.
    Pushdown prepare(const duql::Pipeline& p) {
        std::vector<std::size_t> reads;
        term_reads(p, reads);
        for (const auto i : reads) frame(i);
        std::vector<std::size_t> sets;
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
    std::vector<std::shared_ptr<const df::DataFrame>> frames_;
    std::vector<std::once_flag> once_;
    std::vector<std::shared_ptr<const df::DataFrame>> key_frames_;
    std::vector<std::once_flag> key_once_;
    std::mutex bind_;

    // Side `i`'s rows with a value in every column, each once. The rows
    // caps do not apply; the View's memory budget bounds them.
    std::shared_ptr<const df::DataFrame> distinct(std::size_t i) {
        const std::string& name = program.sides[i].name;
        std::optional<CacheKey> key = keys[i];
        if (key) {
            key->sig = dftracer::utils::hash::splitmix64(key->sig ^
                                                         DISTINCT_CACHE_SALT);
            try {
                if (const auto db = ix::cache::open_lookup_db(
                        key->path,
                        ix::store::RocksDatabase::OpenMode::ReadOnly))
                    if (auto bytes = ix::cache::read_lookup(*db, key->sig))
                        if (auto cached = df::frame_from_native(*bytes))
                            return std::make_shared<const df::DataFrame>(
                                duql::flat_frame(std::move(*cached)));
            } catch (const std::exception&) {
            }
        }
        const std::uint64_t budget = dftracer::utils::resolve_spill_budget(
            views[i]->memory_budget_bytes());
        dftracer::utils::StringViewSet seen;
        std::vector<df::DataFrame> parts;
        std::uint64_t bytes = 0;
        const df::LazyFrame plan = views[i]->lazy();
        dftracer::utils::default_runtime().run_blocking(
            "duql_key_set",
            [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
                auto gen = plan.stream();
                std::string row_key;
                while (auto f = co_await gen.next()) {
                    df::DataFrame part = duql::flat_frame(std::move(*f));
                    std::vector<std::int64_t> take;
                    for (std::int64_t r = 0; r < part.num_rows(); ++r) {
                        row_key.clear();
                        bool keyed = true;
                        for (const auto& c : part.columns)
                            keyed =
                                keyed && duql::append_cell_key(row_key, c, r);
                        if (!keyed || !seen.insert(row_key).second) continue;
                        take.push_back(r);
                        bytes += row_key.size() + sizeof(std::string);
                    }
                    if (take.empty()) continue;
                    df::DataFrame kept;
                    kept.names = part.names;
                    for (const auto& c : part.columns)
                        kept.columns.push_back(c.take(take));
                    bytes += duql::frame_bytes(kept);
                    if (bytes > budget)
                        refuse("'" + name + "' holds more distinct rows than " +
                               "the memory budget (" + std::to_string(budget) +
                               " bytes); a top-level 'in' reads them into "
                               "memory: raise the budget, or write the test " +
                               "under 'not' or in a later 'where' to join it");
                    parts.push_back(std::move(kept));
                }
                co_return;
            });
        if (parts.empty()) {
            df::DataFrame none;
            for (const auto& c : columns[i]) {
                none.names.push_back(c.name);
                none.columns.push_back(df::Series::nulls(
                    c.type.id == df::TypeId::Unknown ? df::TypeId::String
                                                     : c.type.id,
                    0));
            }
            return std::make_shared<const df::DataFrame>(std::move(none));
        }
        if (key) persist(*key, parts);
        std::vector<const df::DataFrame*> ptrs;
        for (const auto& part : parts) ptrs.push_back(&part);
        return std::make_shared<const df::DataFrame>(
            df::concat(ptrs, df::ConcatHow::Diagonal));
    }

    std::shared_ptr<const df::DataFrame> run(std::size_t i) {
        const std::string& name = program.sides[i].name;
        const std::uint64_t max_rows =
            env_count("DUQL_LOOKUP_MAX_ROWS", LOOKUP_MAX_ROWS);
        const std::uint64_t max_bytes =
            env_count("DUQL_LOOKUP_MAX_BYTES", LOOKUP_MAX_BYTES);
        const auto limit = static_cast<std::int64_t>(std::min<std::uint64_t>(
            max_rows, std::numeric_limits<std::int64_t>::max() - 1));
        if (stored[i])
            if (auto f = ix::plan::stored_rowset(files[i], name)) {
                if (static_cast<std::uint64_t>(f->num_rows()) > max_rows)
                    over_rows(name, static_cast<std::uint64_t>(f->num_rows()),
                              max_rows);
                df::DataFrame flat = duql::flat_frame(std::move(*f));
                if (const std::uint64_t bytes = duql::frame_bytes(flat);
                    bytes > max_bytes)
                    over_bytes(name, bytes, max_bytes);
                return std::make_shared<const df::DataFrame>(std::move(flat));
            }
        if (keys[i]) try {
                if (const auto db = ix::cache::open_lookup_db(
                        keys[i]->path,
                        ix::store::RocksDatabase::OpenMode::ReadWrite))
                    if (auto bytes = ix::cache::read_lookup(*db, keys[i]->sig))
                        if (auto cached = df::frame_from_native(*bytes))
                            return std::make_shared<const df::DataFrame>(
                                duql::flat_frame(std::move(*cached)));
            } catch (const std::exception&) {
            }
        const df::LazyFrame plan = views[i]->head(limit + 1).lazy();
        df::DataFrame f;
        dftracer::utils::default_runtime().run_blocking(
            "duql_lookup_side",
            [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
                f = co_await plan.collect();
                co_return;
            });
        if (static_cast<std::uint64_t>(f.num_rows()) > max_rows)
            over_rows(name, static_cast<std::uint64_t>(f.num_rows()), max_rows);
        f = duql::flat_frame(std::move(f));
        if (const std::uint64_t bytes = duql::frame_bytes(f); bytes > max_bytes)
            over_bytes(name, bytes, max_bytes);
        if (keys[i]) {
            std::vector<df::DataFrame> parts;
            parts.push_back(f.select(f.names));
            persist(*keys[i], parts);
        }
        return std::make_shared<const df::DataFrame>(std::move(f));
    }
};

// The program a plan reads. Only the main plan owns it: side plans, which
// Sides owns, hold it weakly and run only while the main plan does.
std::shared_ptr<Sides> owner(const std::weak_ptr<Sides>& sides) {
    auto s = sides.lock();
    if (!s) refuse("a row set ran after the query that reads it was released");
    return s;
}

// The rows of side `side`, from memory when a run first pulls them: the copy
// the build step collected, or the cache's.
class SideSource final : public df::Source {
   public:
    SideSource(std::shared_ptr<Sides> strong, std::weak_ptr<Sides> weak,
               std::size_t side, df::Schema schema)
        : strong_(std::move(strong)),
          weak_(std::move(weak)),
          side_(side),
          schema_(std::move(schema)) {}

    df::Schema schema() const override { return schema_; }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        df::ScanResult r;
        r.cursor = std::make_unique<Rows>(strong_ ? strong_ : owner(weak_),
                                          side_, req.projection);
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    class Rows final : public df::Cursor {
       public:
        Rows(std::shared_ptr<Sides> sides, std::size_t side,
             std::vector<std::string> projection)
            : sides_(std::move(sides)),
              side_(side),
              projection_(std::move(projection)) {}

        coro::CoroTask<std::optional<df::Morsel>> next(
            std::int64_t max_rows) override {
            if (!rows_) {
                const auto frame = sides_->frame(side_);
                df::DataFrame f;
                f.names = frame->names;
                for (const auto& c : frame->columns)
                    f.columns.push_back(c.share());
                if (!projection_.empty()) f = f.select(projection_);
                rows_ = df::InMemorySource(std::move(f)).scan({}).cursor;
            }
            co_return co_await rows_->next(max_rows);
        }

       private:
        std::shared_ptr<Sides> sides_;
        std::size_t side_;
        std::vector<std::string> projection_;
        std::unique_ptr<df::Cursor> rows_;
    };

    std::shared_ptr<Sides> strong_;
    std::weak_ptr<Sides> weak_;
    std::size_t side_;
    df::Schema schema_;
};

// The cache write of side `side` as its rows stream into a join. A side past
// DUQL_LOOKUP_MAX_ROWS or DUQL_LOOKUP_MAX_BYTES is not cached; the engine's
// join spills it instead of failing. The main plan's tap owns the sides.
std::shared_ptr<const df::detail::Tap> side_tap(std::shared_ptr<Sides> strong,
                                                std::weak_ptr<Sides> weak,
                                                std::size_t side) {
    auto tap = std::make_shared<df::detail::Tap>();
    tap->open = [strong = std::move(strong), weak = std::move(weak), side] {
        struct State {
            std::uint64_t rows = 0;
            std::uint64_t bytes = 0;
            bool over = false;
            std::vector<df::DataFrame> parts;
        };
        auto state = std::make_shared<State>();
        const auto s = strong ? strong : owner(weak);
        const std::optional<CacheKey> key = s->keys[side];
        const std::uint64_t max_rows =
            env_count("DUQL_LOOKUP_MAX_ROWS", LOOKUP_MAX_ROWS);
        const std::uint64_t max_bytes =
            env_count("DUQL_LOOKUP_MAX_BYTES", LOOKUP_MAX_BYTES);
        df::detail::TapRun run;
        run.rows = [state, key, max_rows, max_bytes](const df::DataFrame& f) {
            if (state->over) return;
            state->rows += static_cast<std::uint64_t>(f.num_rows());
            df::DataFrame part;
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
    std::vector<duql::VectorColumn> cols;
};

// What building a pipeline needs of the View it starts from.
struct Ctx {
    std::shared_ptr<Sides> sides;
    duql::Roles roles;
    std::function<View(const View&)> order;
    std::function<View(const View&, std::shared_ptr<const detail::BuildStep>)>
        with_step;
    std::function<View(const duql::Input&)> input;
    std::function<const ix::RecordSchema&(const View&)> schema;
};

Built build(const Ctx& ctx, const duql::Pipeline& p, bool table,
            std::vector<std::string>& lines, bool main = false,
            bool ordered = false);

// A side's cache state for explain: whether the store holds it and, when
// it does, its rows. Reads no trace.
std::string cache_state(const std::optional<CacheKey>& key) {
    if (!key) return "not cached";
    const auto db = ix::cache::open_lookup_db(
        key->path, ix::store::RocksDatabase::OpenMode::ReadOnly);
    if (!db || !ix::cache::lookup_exists(*db, key->sig)) return "not cached";
    try {
        if (auto bytes = ix::cache::read_lookup(*db, key->sig))
            if (auto f = df::frame_from_native(*bytes))
                return "cached, " + std::to_string(f->num_rows()) + " rows";
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
    Applier(View v, const ix::RecordSchema& schema, const Ctx& ctx, Reads reads,
            std::shared_ptr<const detail::BuildStep> step,
            std::vector<std::size_t> collected, bool main,
            std::optional<std::vector<duql::VectorColumn>> frame = {})
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
        std::vector<std::string> rest;
        for (const auto& c : cols_)
            if (!c.quant && !c.lookup && !c.call) rest.push_back(c.name);
        if (rest.size() != cols_.size()) keep(std::move(rest));
    }

    // Ends the scan, so the columns are those the stages give.
    void end_scan(const char* stage) { leave_scan(stage); }

    const std::vector<duql::VectorColumn>& columns() const { return cols_; }

    // Drops the fields the scan read only for the stages.
    void finish_columns() {
        std::vector<std::string> rest;
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
    std::vector<std::string> lines;

   private:
    // The sides the build step collects, and whether this is the main plan,
    // which owns the sides.
    std::vector<std::size_t> collected_;
    bool main_;
    View v_;
    const ix::RecordSchema& schema_;
    const duql::Roles& roles_;
    const Ctx& ctx_;
    std::shared_ptr<const detail::BuildStep> step_;
    std::vector<duql::VectorColumn> cols_;
    bool scan_ = true;
    bool scan_selected_ = false;
    std::string blocker_;
    std::optional<duql::PipelineBucket> bucket_;
    std::vector<std::string> fields_;
    std::vector<std::string> arrays_;
    bool closed_ = false;
    std::vector<std::string> extras_;
    std::optional<std::map<std::string, df::TypeId>> index_types_;
    std::optional<std::vector<SchemaLeaf>> tree_;
    std::size_t quants_ = 0;
    std::size_t lookups_ = 0;
    std::size_t calls_ = 0;

    const std::vector<SchemaLeaf>& tree() {
        if (!tree_) tree_ = v_.schema_tree();
        return *tree_;
    }

    bool is_array(const std::string& field) const {
        return std::find(arrays_.begin(), arrays_.end(), field) !=
               arrays_.end();
    }

    bool is_json(const std::string& name) const {
        for (const auto& spec : schema_.fields)
            if (spec.type == ix::FieldType::JSON &&
                (name == spec.name || name == spec.path ||
                 (schema_.args_fallback && name == "args." + spec.name)))
                return true;
        return false;
    }

    // The type the index observed for `name`, Unknown when it saw none or
    // several.
    df::TypeId observed(const std::string& name) {
        if (!index_types_) {
            index_types_.emplace();
            for (const auto& l : tree())
                index_types_->emplace(l.path, observed_type(l.type));
        }
        const auto it = index_types_->find(dotted(name));
        return it == index_types_->end() ? df::TypeId::Unknown : it->second;
    }

    void columns_from_view(bool records = false) {
        cols_.clear();
        for (const auto& f : v_.output_schema().fields) {
            df::DataType type = f.type;
            if (type.id == df::TypeId::Unknown)
                type = df::scalar(observed(f.name));
            cols_.push_back(
                {f.name, std::move(type), is_json(f.name), records});
        }
        if (!records) return;
        for (const auto& a : arrays_) {
            auto i = index(a);
            if (!i) i = arg_index(a);
            if (i && cols_[*i].type.fields.empty() &&
                (cols_[*i].type.id == df::TypeId::List ||
                 cols_[*i].type.id == df::TypeId::Unknown)) {
                auto [token, type] =
                    list_type(tree(), a, schema_.args_fallback);
                cols_[*i].type = std::move(type);
                cols_[*i].json = token.starts_with(JSON_LIST);
            }
        }
    }

    // A field read by the raw scan: a scanned column, or, while the view
    // still reads raw events, any field under the type the index observed.
    std::optional<duql::VectorColumn> scan_column(const std::string& base) {
        if (auto i = index(base)) return cols_[*i];
        if (auto i = arg_index(base)) return cols_[*i];
        if (!scan_) return std::nullopt;
        df::TypeId t = observed(base);
        if (t == df::TypeId::Unknown && schema_.args_fallback)
            t = observed("args." + base);
        if (t == df::TypeId::Unknown) return std::nullopt;
        return duql::VectorColumn{base, df::scalar(t), is_json(base)};
    }

    // Whether the index lists field `f`, or a path under it.
    bool indexed(const std::string& f) {
        const std::string d = dotted(f);
        for (const auto& leaf : tree()) {
            for (const std::string& p :
                 {d, schema_.args_fallback ? "args." + d : std::string()})
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
        std::vector<std::string> read = names();
        std::vector<std::string> only;
        bool narrow = closed_;
        const auto add = [](std::vector<std::string>& v, std::string f) {
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
        if (read.size() > cols_.size() ||
            (narrow && read.size() < cols_.size())) {
            const std::vector<std::string> before = names();
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

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(cols_.size());
        for (const auto& c : cols_) out.push_back(c.name);
        return out;
    }

    std::vector<df::DataType> types() const {
        std::vector<df::DataType> out;
        out.reserve(cols_.size());
        for (const auto& c : cols_) out.push_back(c.type);
        return out;
    }

    std::optional<std::size_t> index(std::string_view name) const {
        for (std::size_t i = 0; i < cols_.size(); ++i)
            if (cols_[i].name == name) return i;
        return std::nullopt;
    }

    // The column of the bare name `base` under `args.`, when the source
    // falls back to it.
    std::optional<std::size_t> arg_index(const std::string& base) const {
        if (!schema_.args_fallback) return std::nullopt;
        return index("args." + base);
    }

    df::Expr col(std::string_view name) const {
        return df::expr_col(static_cast<std::int32_t>(*index(name)));
    }

    // Whether a field named `base` reads a column: the column itself, one it
    // lies inside, or one inside it.
    bool has_column_for(const std::string& base) const {
        auto under = [](const std::string& a, const std::string& b) {
            return a.size() > b.size() && a.starts_with(b) &&
                   (a[b.size()] == '.' || a[b.size()] == '[');
        };
        auto reads = [&](const std::string& path) {
            for (const auto& c : cols_)
                if (c.name == path || under(path, c.name) ||
                    under(c.name, path))
                    return true;
            return false;
        };
        return reads(base) || (schema_.args_fallback && reads("args." + base));
    }

    df::Expr compile(const duql::TermRef& t, const std::string& text,
                     bool condition = false) {
        hidden(t);
        // After the scan the columns are fixed, so a name none of them holds
        // is a mistake, not a missing field.
        if (!scan_)
            duql::for_each_term_field(*t, [&](const duql::TField& f) {
                if (f.root == duql::FieldRoot::RECORD &&
                    !has_column_for(f.base))
                    refuse("has no column '" + f.base +
                           "'; the columns here are " + joined(names()));
            });
        const bool fb = schema_.args_fallback;
        auto e = condition ? duql::vectorize_condition(*t, cols_, fb)
                           : duql::vectorize(*t, cols_, fb);
        if (!e) refuse(e.error().message + " in '" + text + "'");
        return std::move(*e);
    }

    // `next` as the columns, each batch passed through `fn`.
    // Each batch reaches `fn` with the current columns in order: a scan of
    // records gives a batch only the fields its records hold.
    void map_rows(MapSource::Map fn, std::vector<duql::VectorColumn> next,
                  std::string line) {
        lines.push_back(std::move(line));
        df::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(df::Field{c.name, c.type, true});
        std::vector<std::pair<std::string, df::TypeId>> in;
        for (const auto& c : cols_) in.emplace_back(c.name, c.type.id);
        v_ = v_.with_lazy(df::LazyFrame::scan(std::make_shared<MapSource>(
            v_.lazy(), std::move(schema),
            [in = std::move(in), fn = std::move(fn)](df::DataFrame f) {
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
        std::vector<const duql::Term*> inner;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* q = std::get_if<duql::TQuant>(&x.node))
                duql::for_each_term(*q->cond, [&](const duql::Term& y) {
                    inner.push_back(&y);
                });
        });
        std::vector<const duql::Term*> found;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            const auto* l = std::get_if<duql::TLookup>(&x.node);
            const auto* c = std::get_if<duql::TCall>(&x.node);
            if (std::holds_alternative<duql::TQuant>(x.node) ||
                (l && !l->all) ||
                (c && duql::is_column_call(c->fn) &&
                 std::find(inner.begin(), inner.end(), &x) == inner.end()))
                found.push_back(&x);
        });
        for (auto it = found.rbegin(); it != found.rend(); ++it) {
            if (std::holds_alternative<duql::TQuant>((*it)->node))
                quantifier(t, *it);
            else if (std::holds_alternative<duql::TCall>((*it)->node))
                call(t, *it);
            else
                lookup(t, *it);
        }
    }

    // The fields of the object `f` names into `keys`, `args` and `types`:
    // its one struct column, or the columns under its path, one per key; a
    // key over deeper columns reads whether any of them holds a value.
    void object_fields(const duql::TField& f, const std::string& text,
                       bool values, std::vector<std::string>& keys,
                       std::vector<df::Expr>& args,
                       std::vector<df::DataType>& types) {
        if (f.root != duql::FieldRoot::RECORD || f.neg_at != f.steps.size())
            return;
        if (auto i = field_index(f.base)) {
            if (cols_[*i].type.id != df::TypeId::Struct) return;
            for (const auto& x : cols_[*i].type.fields) keys.push_back(x.name);
            args.push_back(col(cols_[*i].name));
            types.push_back(cols_[*i].type);
            return;
        }
        std::string prefix = f.base + ".";
        if (schema_.args_fallback &&
            !std::any_of(cols_.begin(), cols_.end(), [&](const auto& c) {
                return c.name.starts_with(prefix);
            }))
            prefix = "args." + prefix;
        std::map<std::string, std::vector<std::size_t>> under;
        for (std::size_t i = 0; i < cols_.size(); ++i) {
            if (!cols_[i].name.starts_with(prefix)) continue;
            const std::string rest = cols_[i].name.substr(prefix.size());
            under[rest.substr(0, rest.find_first_of(".["))].push_back(i);
        }
        for (const auto& [key, at] : under) {
            const std::string direct = prefix + key;
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
            df::Expr any;
            for (const std::size_t i : at) {
                const df::Expr has =
                    df::expr_is_null(col(cols_[i].name), false);
                any = any.valid()
                          ? df::expr_logical(df::LogicalOp::Or, any, has)
                          : has;
            }
            args.push_back(
                df::expr_select(any, df::expr_lit_bool(true),
                                df::expr_lit_null(df::TypeId::Bool)));
            types.push_back(df::scalar(df::TypeId::Bool));
        }
    }

    // The column of a call only call_column() computes.
    void call(const duql::TermRef& t, const duql::Term* term) {
        const auto* c = &std::get<duql::TCall>(term->node);
        if (std::any_of(cols_.begin(), cols_.end(),
                        [c](const auto& x) { return x.call == c; }))
            return;
        const std::string text = duql::term_text(*term);
        std::vector<df::Expr> args;
        std::vector<df::DataType> arg_types;
        std::vector<std::string> keys;
        if (c->fn == duql::Fn::KEYS || c->fn == duql::Fn::VALUES) {
            if (const auto* f = std::get_if<duql::TField>(&c->args[0]->node))
                object_fields(*f, text, c->fn == duql::Fn::VALUES, keys, args,
                              arg_types);
        } else {
            for (const auto& a : c->args) {
                if (const auto* inner = std::get_if<duql::TCall>(&a->node);
                    inner && inner->fn == duql::Fn::PARSE_JSON)
                    refuse("'" + text +
                           "' reads a parse_json() value, which only a scan "
                           "filter evaluates");
                auto e = duql::vectorize(*a, cols_, schema_.args_fallback);
                if (!e) refuse(e.error().message + " in '" + text + "'");
                args.push_back(std::move(*e));
                arg_types.push_back(df::infer_type(args.back(), types()));
            }
        }
        std::optional<df::DataType> type;
        try {
            type = duql::call_type(*c, arg_types);
        } catch (const std::invalid_argument& e) {
            refuse("'" + text + "': " + e.what());
        }
        const std::string name = "__duql_c_" + std::to_string(calls_++);
        if (!type) {
            with_column(name, df::expr_lit_null(df::TypeId::Bool), text);
            cols_.back().type = df::scalar(df::TypeId::Unknown);
            cols_.back().call = c;
            return;
        }
        std::vector<duql::VectorColumn> next = cols_;
        next.push_back({name, *type, c->fn == duql::Fn::PARSE_JSON, false,
                        nullptr, nullptr, c});
        map_rows(
            [t, c, args, keys, type = *type, name](df::DataFrame f) {
                std::vector<const df::Series*> inputs;
                for (const auto& x : f.columns) inputs.push_back(&x);
                std::vector<df::Series> values;
                for (const auto& a : args)
                    values.push_back(df::eval(a, inputs));
                f.names.push_back(name);
                f.columns.push_back(duql::call_column(*c, values, keys, type));
                return f;
            },
            std::move(next), "call " + name + " = " + text);
    }

    // Binds the pipeline's lookups before a batch reads them.
    static void ready(const std::shared_ptr<const detail::BuildStep>& step) {
        if (step) step->run(std::nullopt);
    }

    void quantifier(const duql::TermRef& t, const duql::Term* term) {
        const auto* q = &std::get<duql::TQuant>(term->node);
        if (std::any_of(cols_.begin(), cols_.end(),
                        [q](const auto& c) { return c.quant == q; }))
            return;
        if (const auto* f = std::get_if<duql::TField>(&q->subject->node);
            f && f->root == duql::FieldRoot::RECORD &&
            !array_field(*q->subject))
            refuse("'" + duql::term_text(*term) +
                   "' walks a negative index; only a scan filter "
                   "evaluates it");
        if (const auto* f = array_field(*q->subject))
            if (const auto i = field_index(f->base); i && cols_[*i].json)
                refuse("'" + duql::term_text(*term) + "' reads '" + f->base +
                       "', whose elements mix types; only a scan "
                       "filter evaluates it");
        const std::string name = "__duql_q_" + std::to_string(quants_++);
        std::vector<duql::VectorColumn> in = cols_;
        std::vector<duql::VectorColumn> next = cols_;
        next.push_back({name, df::scalar(df::TypeId::Bool), false, false, q});
        map_rows(
            [t, q, in = std::move(in), name, step = step_,
             fb = schema_.args_fallback](df::DataFrame f) {
                ready(step);
                std::vector<const df::Series*> inputs;
                for (const auto& c : f.columns) inputs.push_back(&c);
                auto truth = duql::quantify(*q, inputs, in, fb);
                if (!truth) refuse(truth.error().message);
                f.names.push_back(name);
                f.columns.push_back(std::move(*truth));
                return f;
            },
            std::move(next),
            "quantify " + name + " = " + duql::term_text(*term));
    }

    // The type of the column `name` of side `side`, Unknown when it has none.
    df::DataType side_type(std::size_t side, std::string_view name) const {
        const auto& cols = ctx_.sides->columns[side];
        for (const auto& c : cols)
            if (c.name == name) return c.type;
        if (schema_.args_fallback)
            for (const auto& c : cols)
                if (c.name == "args." + std::string(name)) return c.type;
        return df::scalar(df::TypeId::Unknown);
    }

    // The columns of `l`'s keys over the rows, computed apart when a key is
    // not a column; `t` owns `l`.
    std::vector<std::string> key_columns(const duql::TermRef& t,
                                         const duql::TLookup& l,
                                         const std::string& id) {
        std::vector<std::string> out;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const duql::TermRef key(t, l.keys[k].get());
            out.push_back(source({"", key, duql::term_text(*key)},
                                 "__duql_lk" + id + "_" + std::to_string(k)));
        }
        return out;
    }

    // The side of a joined `l`, with `marker` a TRUE column of that name;
    // in `right`, the columns it matches on.
    df::LazyFrame joined_side(const duql::TLookup& l,
                              std::vector<std::string>& right, bool& memory,
                              const std::string& marker) {
        const auto& side_cols = ctx_.sides->columns[l.side];
        if (l.kind == duql::LookupKind::IN) {
            if (side_cols.size() != l.keys.size())
                refuse("the sub-query '" + l.name + "' gives " +
                       std::to_string(side_cols.size()) +
                       " columns; 'in' compares " +
                       std::to_string(l.keys.size() - l.correlated));
            for (const auto& c : side_cols) right.push_back(c.name);
        } else {
            right = l.target;
        }
        df::LazyFrame side = side_plan(l.side, memory);
        if (marker.empty()) return side;
        return side.with_column(marker, df::expr_lit_bool(true));
    }

    // Whether `l`'s keys are columns of one exact type each, the type of
    // the side's column, as a typed semi join matches them.
    bool typed_keys(const duql::TLookup& l) const {
        const auto& side_cols = ctx_.sides->columns[l.side];
        if (side_cols.size() != l.keys.size()) return false;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const auto* f = whole_field(l.keys[k].get());
            const auto i = f ? index(f->base) : std::nullopt;
            if (!i) return false;
            const df::DataType& a = cols_[*i].type;
            const df::DataType& b = side_cols[k].type;
            const bool exact = is_integer(a.id) || a.id == df::TypeId::String ||
                               a.id == df::TypeId::Bool;
            if (!exact || a.id != b.id || a.json || b.json) return false;
        }
        return true;
    }

    // `k in (...)`, a top-level term of a `where`: the rows whose keys match
    // a row of the side, by a semi join that narrows the scan.
    void semi_join(const duql::TermRef& t, const duql::TLookup& l) {
        const std::vector<std::string> before = names();
        const std::string id = std::to_string(lookups_++);
        const std::vector<std::string> left = key_columns(t, l, id);
        std::vector<std::string> right;
        bool memory = false;
        df::LazyFrame side = joined_side(l, right, memory, "");
        lines.push_back("semi join " + l.name + " on " + joined(left) +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        v_ = v_.with_lazy(
            v_.lazy().join(std::move(side), left, right, df::JoinHow::Semi));
        if (names() != before) keep(before);
    }

    // A joined lookup's column: `in` through a lookup join that attaches a
    // marker, a keyed scalar through a nest join that counts the matches.
    void join_lookup(const duql::TermRef& t, const duql::Term* term) {
        const auto* l = &std::get<duql::TLookup>(term->node);
        const std::vector<std::string> before = names();
        const std::string id = std::to_string(lookups_++);
        const std::string name = "__duql_l_" + id;
        const std::vector<std::string> left = key_columns(t, *l, id);
        std::vector<std::string> right;
        bool memory = false;
        const bool in = l->kind == duql::LookupKind::IN;
        const std::string matched = "__duql_lm_" + id;
        df::LazyFrame side =
            joined_side(*l, right, memory, in ? matched : std::string());
        const std::string how = in ? "lookup join " : "nest join ";
        lines.push_back(how + l->name + " on " + joined(left) +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        std::optional<std::size_t> field;
        df::DataType type = df::scalar(df::TypeId::Bool);
        if (in) {
            v_ = v_.with_lazy(v_.lazy().join(std::move(side), left, right,
                                             df::JoinHow::Lookup));
            cols_.push_back({matched, df::scalar(df::TypeId::Bool)});
        } else {
            std::vector<df::Field> fields;
            const auto& side_cols = ctx_.sides->columns[l->side];
            for (std::size_t c = 0; c < side_cols.size(); ++c) {
                fields.push_back(
                    df::Field{side_cols[c].name, side_cols[c].type, true});
                if (side_cols[c].name == l->column) field = c;
            }
            if (!field)
                refuse("'" + l->name + "' has no column '" + l->column + "'");
            type = side_cols[*field].type;
            v_ = v_.with_lazy(v_.lazy().join(std::move(side), left, right,
                                             df::JoinHow::Nest, matched));
            cols_.push_back(
                {matched, df::list_of(df::struct_of(std::move(fields)))});
        }
        std::vector<duql::VectorColumn> next = cols_;
        next.push_back({name, type, false, false, nullptr, l});
        std::shared_ptr<Sides> strong = main_ ? ctx_.sides : nullptr;
        std::weak_ptr<Sides> weak = ctx_.sides;
        const std::size_t subject = l->keys.size() - l->correlated;
        const bool negated = l->negated;
        const std::string sub = l->name;
        const std::optional<std::size_t> empty = l->empty;
        map_rows(
            [=](df::DataFrame f) {
                auto at = [&](const std::string& n) -> const df::Series& {
                    for (std::size_t i = 0; i < f.names.size(); ++i)
                        if (f.names[i] == n) return f.columns[i];
                    refuse("the join of '" + sub + "' lost column '" + n + "'");
                };
                std::vector<df::Series> keys;
                for (const auto& k : left) keys.push_back(at(k).materialize());
                std::vector<const df::Series*> ptrs;
                for (const auto& k : keys) ptrs.push_back(&k);
                df::Series out;
                if (in) {
                    out = duql::in_column(ptrs, subject,
                                          at(matched).materialize(), negated);
                } else {
                    std::shared_ptr<const df::DataFrame> none;
                    if (empty)
                        none = (strong ? strong : owner(weak))->frame(*empty);
                    out = duql::scalar_column(sub, ptrs,
                                              at(matched).materialize(), *field,
                                              none.get());
                }
                f.names.push_back(name);
                f.columns.push_back(std::move(out));
                return f;
            },
            std::move(next), "lookup " + name + " = " + duql::term_text(*term));
        std::vector<std::string> keep_names = before;
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
        df::DataType type = df::scalar(df::TypeId::Bool);
        if (l->kind == duql::LookupKind::ARROW) {
            type = side_type(l->side, l->column);
        } else if (l->kind == duql::LookupKind::SCALAR) {
            const auto& cols = ctx_.sides->columns[l->side];
            if (l->range == duql::RangeRead::SUM) {
                const df::TypeId v = side_type(l->side, l->column).id;
                type = df::scalar(v == df::TypeId::Float32 ||
                                          v == df::TypeId::Float64
                                      ? df::TypeId::Float64
                                      : df::TypeId::Int64);
            } else if (l->range == duql::RangeRead::MEAN) {
                type = df::scalar(df::TypeId::Float64);
            } else if (l->range == duql::RangeRead::COUNT ||
                       l->range == duql::RangeRead::COUNT_VALUES ||
                       l->range == duql::RangeRead::COUNT_IF) {
                type = df::scalar(df::TypeId::Int64);
            } else if (!l->keys.empty())
                type = side_type(l->side, l->column);
            else if (!cols.empty())
                type = side_type(l->side, cols.front().name);
        }
        const std::string name = "__duql_l_" + std::to_string(lookups_++);
        std::vector<duql::VectorColumn> in = cols_;
        std::vector<duql::VectorColumn> next = cols_;
        next.push_back({name, type, false, false, nullptr, l});
        map_rows(
            [t, l, in = std::move(in), name, step = step_,
             fb = schema_.args_fallback](df::DataFrame f) {
                ready(step);
                std::vector<const df::Series*> inputs;
                for (const auto& c : f.columns) inputs.push_back(&c);
                auto values = duql::lookup(*l, inputs, in, fb);
                if (!values) refuse(values.error().message);
                f.names.push_back(name);
                f.columns.push_back(std::move(*values));
                return f;
            },
            std::move(next), "lookup " + name + " = " + duql::term_text(*term));
    }

    void with_column(const std::string& name, const df::Expr& e,
                     const std::string& text) {
        df::DataType type = df::infer_type(e, types());
        v_ = v_.with_column(name, e);
        if (auto i = index(name))
            cols_[*i] = {name, std::move(type), false};
        else
            cols_.push_back({name, std::move(type), false});
        lines.push_back("with_column " + name + " = " + text);
    }

    void keep(std::vector<std::string> keep) {
        std::vector<duql::VectorColumn> next;
        for (const auto& n : keep) {
            const auto i = index(n);
            if (!i) refuse("has no column '" + n + "'");
            next.push_back(cols_[*i]);
        }
        lines.push_back("select " + joined(keep));
        v_ = v_.with_lazy(v_.lazy().select(std::move(keep)));
        cols_ = std::move(next);
    }

    void rename(std::vector<std::string> to) {
        for (std::size_t i = 0; i < to.size(); ++i) cols_[i].name = to[i];
        lines.push_back("rename " + joined(to));
        v_ = v_.with_lazy(v_.lazy().rename(std::move(to)));
    }

    // The column `it` names when it is its own column, else a hidden column
    // `tmp` computed from it.
    std::string source(const duql::PipelineItem& it, std::string tmp) {
        const auto* f = std::get_if<duql::TField>(&it.term->node);
        if (f && f->base == it.name && f->neg_at == f->steps.size() &&
            index(it.name))
            return it.name;
        if (const auto* w = whole_field(it.term)) {
            if (index(w->base)) return w->base;
            if (arg_index(w->base)) return "args." + w->base;
        }
        with_column(tmp, compile(it.term, it.text), it.text);
        return tmp;
    }

    // The items as the only columns, in order, under their names. An item
    // that is its own column stays; the others are computed apart first so
    // no item reads another's result.
    void project(const std::vector<duql::PipelineItem>& items) {
        extras_.clear();
        std::vector<std::string> sources;
        std::vector<std::string> final_names;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string tmp = "__duql_" + std::to_string(i);
            std::string from = source(items[i], tmp);
            if (std::find(sources.begin(), sources.end(), from) !=
                sources.end()) {
                with_column(tmp, col(from), items[i].text);
                from = tmp;
            }
            sources.push_back(std::move(from));
            final_names.push_back(items[i].name);
        }
        keep(std::move(sources));
        if (names() != final_names) rename(std::move(final_names));
    }

    void stage(const duql::PipelineWhere& w) {
        leave_scan("where");
        if (const auto* l = std::get_if<duql::TLookup>(&w.condition->node);
            l && l->kind == duql::LookupKind::IN && !l->negated &&
            duql::joined(*l) && typed_keys(*l)) {
            semi_join(w.condition, *l);
            return;
        }
        v_ = v_.filter(compile(w.condition, w.text, true));
        lines.push_back("filter after scan: " + w.text);
    }

    void stage(const duql::PipelineSelect& s) {
        leave_scan("select");
        project(s.items);
    }

    void stage(const duql::PipelineDerive& d) {
        leave_scan("derive");
        for (const auto& it : d.items)
            with_column(it.name, compile(it.term, it.text), it.text);
    }

    void stage(const duql::PipelineDrop& d) {
        leave_scan("drop");
        std::vector<std::string> rest;
        for (const auto& n : names()) {
            bool dropped = false;
            for (const auto& x : d.names) dropped = dropped || x == n;
            if (!dropped) rest.push_back(n);
        }
        keep(std::move(rest));
    }

    void stage(const duql::PipelineRename& r) {
        leave_scan("rename");
        std::vector<std::string> to = names();
        for (const auto& [next, old] : r.pairs) {
            const auto i = index(old);
            if (!i) refuse("cannot rename '" + old + "': no such column");
            to[*i] = next;
        }
        rename(std::move(to));
    }

    void stage(const duql::PipelineDistinct& d) {
        leave_scan("distinct");
        if (!d.items.empty()) project(d.items);
        v_ = v_.with_lazy(v_.unique());
        lines.push_back("unique");
    }

    struct SortColumns {
        std::vector<std::string> by;
        std::vector<bool> desc;
        std::vector<std::string> keys;  ///< `by` without the null flags.
    };

    // The columns and directions `keys` sort by, computing the ones that are
    // not columns.
    SortColumns sort_columns(const std::vector<duql::PipelineSortKey>& keys) {
        SortColumns out;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto& key = keys[k];
            std::string name = key.key.name;
            if (!index(name)) {
                name = "__duql_sort_" + std::to_string(k);
                with_column(name, compile(key.key.term, key.key.text),
                            key.key.text);
            }
            if (key.nulls_first) {
                std::string flag = "__duql_nulls_" + std::to_string(k);
                with_column(flag, df::expr_is_null(col(name)),
                            key.key.text + " is null");
                out.by.push_back(std::move(flag));
                out.desc.push_back(true);
            }
            out.by.push_back(name);
            out.desc.push_back(key.descending);
            out.keys.push_back(name);
        }
        return out;
    }

    void sort(const std::vector<duql::PipelineSortKey>& keys) {
        const std::vector<std::string> before = names();
        SortColumns c = sort_columns(keys);
        lines.push_back("sort_by_multi " + joined(c.by));
        v_ = v_.sort_by_multi(std::move(c.by), std::move(c.desc));
        if (names() != before) keep(before);
    }

    void stage(const duql::PipelineSort& s) {
        leave_scan("sort");
        sort(s.keys);
    }

    void stage(const duql::PipelineTake& t) {
        leave_scan("take");
        v_ = v_.head(t.count);
        lines.push_back("head " + std::to_string(t.count));
    }

    void stage(const duql::PipelineSkip& s) {
        leave_scan("skip");
        v_ = v_.slice(s.count, std::numeric_limits<std::int64_t>::max());
        lines.push_back("slice " + std::to_string(s.count));
    }

    void stage(const duql::PipelineTakeBy& t) {
        leave_scan("take");
        const std::vector<std::string> before = names();
        if (!t.order.empty()) sort(t.order);
        std::vector<std::string> keys;
        for (std::size_t k = 0; k < t.keys.size(); ++k)
            keys.push_back(source(t.keys[k], "__duql_by_" + std::to_string(k)));
        lines.push_back("head_by " + joined(keys) + " " +
                        std::to_string(t.count));
        v_ = v_.with_lazy(v_.lazy().head_by(std::move(keys), t.count));
        if (names() != before) keep(before);
    }

    void stage(const duql::PipelineSample& s) {
        leave_scan("sample");
        const std::string seed = " seed " + std::to_string(s.seed);
        if (!s.percent) {
            const auto n = static_cast<std::int64_t>(s.amount);
            lines.push_back("sample " + std::to_string(n) + seed);
            v_ = v_.with_lazy(v_.lazy().sample(n, s.seed));
            return;
        }
        const std::vector<std::string> before = names();
        const std::string row = "__duql_row";
        v_ = v_.with_lazy(v_.lazy().with_row_index(row));
        cols_.insert(cols_.begin(),
                     {row, df::scalar(df::TypeId::Int64), false});
        const df::Expr hash = df::expr_cast(
            df::TypeId::Float64,
            df::expr_prim(df::PrimOp::Mix64,
                          df::expr_arith(df::ArithOp::Add, col(row),
                                         df::expr_lit(static_cast<std::int64_t>(
                                             s.seed & SEED_MASK)))));
        // mix64 is signed: uniform over [-2^63, 2^63).
        if (s.amount < 100)
            v_ = v_.filter(df::expr_cmp_expr(
                df::CmpOp::Lt, hash,
                df::expr_lit(-9223372036854775808.0 +
                             s.amount / 100.0 * 18446744073709551616.0)));
        lines.push_back("sample " + number_text(s.amount) + "%" + seed);
        keep(before);
    }

    // Microseconds per unit of the record schema's time role.
    double us_per_time_unit() const {
        return static_cast<double>(roles_.time_ns_per_unit) / 1000.0;
    }

    void stage(const duql::PipelineTimeRange& t) {
        if (scan_ && !t.overlap) {
            v_ = v_.time_range(t.low * us_per_time_unit(),
                               t.high * us_per_time_unit());
            lines.push_back("scan time_range " + number_text(t.low) + " .. " +
                            number_text(t.high) + " (pushed)");
            return;
        }
        if (scan_) {
            v_ = v_.filter(duql::Query::from_node(
                duql::make_node(duql::ExprLeaf{t.condition, t.text})));
            lines.push_back("scan filter: " + t.text + " (pushed)");
            return;
        }
        v_ = v_.filter(compile(t.condition, t.text, true));
        lines.push_back("filter after scan: " + t.text);
    }

    void stage(const duql::PipelineBucket& b) { bucket_ = b; }

    void stage(const duql::PipelineCallTree&) {
        scan_ = false;
        blocker_ = "call_tree";
        lines.push_back("call_tree pid, tid");
        v_ = v_.with_lazy(
            v_.call_tree({"pid", "tid"}, roles_.time, roles_.duration, "name"));
        columns_from_view();
        std::vector<std::string> to = names();
        for (auto& n : to) {
            if (n == "level") n = "depth";
            if (n == "parent_id") n = "parent";
        }
        rename(std::move(to));
    }

    void row_index(const std::string& name) {
        v_ = v_.with_lazy(v_.lazy().with_row_index(name));
        cols_.insert(cols_.begin(),
                     {name, df::scalar(df::TypeId::Int64), false});
    }

    std::optional<std::size_t> field_index(const std::string& field) const {
        if (auto i = index(field)) return i;
        return arg_index(field);
    }

    struct WinSpec {
        dftu_window_func func;
        std::string value;
        std::string out;
        df::DataType type;
        std::int64_t offset = 0;
        std::int64_t following = DFTU_WINDOW_UNBOUNDED;
    };

    // One dftu.frame.window pass over `order`, each spec's column appended.
    void window_op(const std::vector<std::string>& part,
                   const std::string& order,
                   const std::vector<WinSpec>& specs) {
        if (specs.empty()) return;
        std::vector<const char*> keys;
        for (const auto& k : part) keys.push_back(k.c_str());
        const char* by = order.c_str();
        std::vector<dftu_window_spec> raw;
        std::vector<std::string> out = names();
        for (const auto& s : specs) {
            dftu_window_spec w{};
            w.func = s.func;
            w.value = s.value.empty() ? nullptr : s.value.c_str();
            w.out = s.out.c_str();
            if (s.func >= DFTU_WINDOW_FRAME_SUM &&
                s.func <= DFTU_WINDOW_FRAME_MEAN)
                w.param.frame = {s.offset, DFTU_WINDOW_UNBOUNDED, s.following};
            else
                w.param.offset = s.offset;
            raw.push_back(w);
            out.push_back(s.out);
            cols_.push_back({s.out, s.type, false});
        }
        df::OpArgs a;
        a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
            .strlist(2, &by, 1)
            .winlist(3, raw);
        v_ = v_.with_lazy(
            v_.lazy().frame_op("dftu.frame.window", a, {}, std::move(out)));
    }

    // Aggregates over each whole partition, as `group` gives them, on every
    // row: the rows grouped by their partition id `pid` and joined back.
    void partition_aggs(
        const std::vector<std::pair<const duql::PipelineWinCall*, std::string>>&
            calls,
        const std::string& pid) {
        std::vector<df::GroupAgg> specs;
        std::vector<std::string> quantiles;
        std::vector<duql::VectorColumn> next = cols_;
        std::string text;
        for (const auto& [c, in] : calls) {
            df::GroupAgg g{df::Agg::Var, in, c->column};
            df::DataType type = df::scalar(df::TypeId::Float64);
            switch (c->fn) {
                case duql::WinFn::STD:
                    g.op = df::Agg::Std;
                    break;
                case duql::WinFn::QUANTILE:
                    g.op = df::Agg::Pct;
                    g.param = c->q;
                    quantiles.push_back(c->column);
                    break;
                case duql::WinFn::HISTOGRAM:
                    g.op = df::Agg::Hist;
                    type = df::list_of(df::struct_of(
                        {df::Field{"lo", df::scalar(df::TypeId::Float64), true},
                         df::Field{"hi", df::scalar(df::TypeId::Float64), true},
                         df::Field{"count", df::scalar(df::TypeId::Uint64),
                                   true}}));
                    break;
                default:
                    break;
            }
            specs.push_back(std::move(g));
            next.push_back({c->column, std::move(type)});
            text += (text.empty() ? "" : ", ") + c->text;
        }
        df::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(df::Field{c.name, c.type, true});
        finish(
            [pid, specs](df::DataFrame f) {
                df::DataFrame g =
                    f.group_by(std::vector<std::string>{pid}, specs);
                return f.join(g, {pid}, {pid}, df::JoinHow::Lookup);
            },
            "partition aggregates " + text, std::move(schema));
        cols_ = std::move(next);
        for (const auto& q : quantiles)
            v_ = v_.with_column(
                q, df::expr_select(df::expr_unary(df::UnaryOp::IsNan, col(q)),
                                   df::expr_lit_null(df::TypeId::Float64),
                                   col(q)));
    }

    df::DataType type_of(const std::string& column) const {
        return cols_[*index(column)].type;
    }

    df::DataType sum_type(const std::string& in, const std::string& text) {
        const df::TypeId t = type_of(in).id;
        if (t != df::TypeId::Unknown && !is_number(t))
            refuse("'" + text + "' in a window needs numbers");
        return df::scalar(is_integer(t) ? df::TypeId::Int64
                                        : df::TypeId::Float64);
    }

    // Row `a` differs from row `b`, null included.
    df::Expr differs(const std::string& a, const std::string& b) {
        const df::Expr na = df::expr_is_null(col(a));
        const df::Expr nb = df::expr_is_null(col(b));
        return df::expr_select(
            na, df::expr_not(nb),
            df::expr_select(nb, df::expr_lit_bool(true),
                            df::expr_cmp_expr(df::CmpOp::Ne, col(a), col(b))));
    }

    void stage(const duql::PipelineWindow& w) {
        leave_scan("window");
        const std::vector<std::string> before = names();
        const std::string pos = "__duql_pos";
        row_index(pos);
        std::vector<std::string> part;
        std::string keys_text;
        for (std::size_t k = 0; k < w.keys.size(); ++k) {
            part.push_back(
                source(w.keys[k], "__duql_part_" + std::to_string(k)));
            keys_text += (keys_text.empty() ? "" : ", ") + w.keys[k].text;
        }
        std::string ord = pos;
        std::vector<std::string> sort_keys;
        std::string sort_text;
        if (!w.order.empty()) {
            SortColumns c = sort_columns(w.order);
            sort_keys = c.keys;
            lines.push_back("sort_by_multi " + joined(c.by));
            v_ = v_.sort_by_multi(std::move(c.by), std::move(c.desc));
            ord = "__duql_ord";
            row_index(ord);
            for (const auto& k : w.order)
                sort_text += (sort_text.empty() ? "" : ", ") +
                             std::string(k.descending ? "-" : "") + k.key.text;
        }
        // Passes: over the order, over the reverse order, then over the
        // order again for what reads the first two.
        std::vector<WinSpec> forward, reverse, last;
        std::vector<const duql::PipelineWinCall*> ranks;
        std::vector<std::pair<const duql::PipelineWinCall*, std::string>>
            partition;
        const df::DataType i64 = df::scalar(df::TypeId::Int64);
        for (std::size_t i = 0; i < w.calls.size(); ++i) {
            const auto& c = w.calls[i];
            std::string in;
            if (c.arg)
                in = source({"", c.arg, c.text},
                            "__duql_in_" + std::to_string(i));
            const std::string tmp = "__duql_fill_" + std::to_string(i);
            switch (c.fn) {
                case duql::WinFn::ROW_NUMBER:
                    forward.push_back(
                        {DFTU_WINDOW_ROW_NUMBER, "", c.column, i64});
                    break;
                case duql::WinFn::RUNNING_COUNT:
                    forward.push_back(
                        {DFTU_WINDOW_RUNNING_COUNT, "", c.column, i64});
                    break;
                case duql::WinFn::LAG:
                case duql::WinFn::LEAD:
                    forward.push_back({c.fn == duql::WinFn::LAG
                                           ? DFTU_WINDOW_LAG
                                           : DFTU_WINDOW_LEAD,
                                       in, c.column, type_of(in), c.offset});
                    break;
                case duql::WinFn::RUNNING_SUM:
                    forward.push_back({DFTU_WINDOW_FRAME_SUM, in, c.column,
                                       sum_type(in, c.text), 0, 0});
                    break;
                case duql::WinFn::COUNT:
                    forward.push_back({DFTU_WINDOW_FRAME_COUNT,
                                       in.empty() ? pos : in, c.column, i64});
                    break;
                case duql::WinFn::SUM:
                    forward.push_back({DFTU_WINDOW_FRAME_SUM, in, c.column,
                                       sum_type(in, c.text)});
                    break;
                case duql::WinFn::MEAN:
                    sum_type(in, c.text);
                    forward.push_back({DFTU_WINDOW_FRAME_MEAN, in, c.column,
                                       df::scalar(df::TypeId::Float64)});
                    break;
                case duql::WinFn::MIN:
                case duql::WinFn::MAX:
                    forward.push_back({c.fn == duql::WinFn::MIN
                                           ? DFTU_WINDOW_FRAME_MIN
                                           : DFTU_WINDOW_FRAME_MAX,
                                       in, c.column, type_of(in)});
                    break;
                case duql::WinFn::LAST:
                    forward.push_back(
                        {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
                    reverse.push_back(
                        {DFTU_WINDOW_FIRST_VALUE, tmp, c.column, type_of(in)});
                    break;
                case duql::WinFn::FIRST:
                    reverse.push_back(
                        {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
                    last.push_back(
                        {DFTU_WINDOW_FIRST_VALUE, tmp, c.column, type_of(in)});
                    break;
                case duql::WinFn::RANK:
                case duql::WinFn::DENSE_RANK:
                    ranks.push_back(&c);
                    break;
                case duql::WinFn::VAR:
                case duql::WinFn::STD:
                case duql::WinFn::QUANTILE:
                case duql::WinFn::HISTOGRAM:
                    sum_type(in, c.text);
                    partition.emplace_back(&c, in);
                    break;
            }
        }
        const std::string pid = "__duql_pid";
        if (!partition.empty())
            forward.push_back({DFTU_WINDOW_FIRST_VALUE, pos, pid, i64});
        // Ranks: a row starts a run of peers when its sort keys differ from
        // the row before it in the partition.
        if (!ranks.empty() && !sort_keys.empty()) {
            forward.push_back({DFTU_WINDOW_ROW_NUMBER, "", "__duql_rn", i64});
            for (std::size_t k = 0; k < sort_keys.size(); ++k)
                forward.push_back({DFTU_WINDOW_LAG, sort_keys[k],
                                   "__duql_prev_" + std::to_string(k),
                                   type_of(sort_keys[k]), 1});
        }
        window_op(part, ord, forward);
        if (!ranks.empty() && !sort_keys.empty()) {
            df::Expr peer = df::expr_cmp(df::CmpOp::Eq, col("__duql_rn"),
                                         df::detail::expr_scalar_i(1));
            for (std::size_t k = 0; k < sort_keys.size(); ++k)
                peer = df::expr_logical(
                    df::LogicalOp::Or, peer,
                    differs(sort_keys[k], "__duql_prev_" + std::to_string(k)));
            with_column("__duql_peer",
                        df::expr_select(peer, df::expr_lit(std::int64_t{1}),
                                        df::expr_lit(std::int64_t{0})),
                        "peer start");
            with_column("__duql_start",
                        df::expr_select(peer, col("__duql_rn"),
                                        df::expr_lit_null(df::TypeId::Int64)),
                        "peer start row");
            for (const auto* c : ranks)
                last.push_back(c->fn == duql::WinFn::RANK
                                   ? WinSpec{DFTU_WINDOW_FILL_FORWARD,
                                             "__duql_start", c->column, i64}
                                   : WinSpec{DFTU_WINDOW_FRAME_SUM,
                                             "__duql_peer", c->column, i64, 0,
                                             0});
        } else {
            for (const auto* c : ranks)
                with_column(c->column, df::expr_lit(std::int64_t{1}), c->text);
        }
        if (!reverse.empty()) {
            with_column("__duql_rev", df::expr_neg(col(ord)), "-" + ord);
            window_op(part, "__duql_rev", reverse);
        }
        window_op(part, ord, last);
        if (!partition.empty()) partition_aggs(partition, pid);
        lines.push_back("sort_by_multi " + pos);
        v_ = v_.sort_by_multi({pos}, std::vector<bool>{false});
        std::vector<std::string> keep_names = before;
        std::string items;
        for (const auto& it : w.items) {
            with_column(it.name, compile(it.term, it.text), it.text);
            if (std::find(keep_names.begin(), keep_names.end(), it.name) ==
                keep_names.end())
                keep_names.push_back(it.name);
            items += (items.empty() ? "" : ", ") + it.name + " = " + it.text;
        }
        lines.push_back("window keys " + keys_text + "; sort " + sort_text +
                        "; " + items);
        keep(std::move(keep_names));
    }

    void stage(const duql::PipelineSession& ss) {
        leave_scan("session");
        if (index(ss.name))
            refuse("'session' gives column '" + ss.name +
                   "', which is already a column; name it with 'as'");
        std::vector<std::string> keep_names = names();
        keep_names.push_back(ss.name);
        const std::string pos = "__duql_pos";
        row_index(pos);
        std::vector<std::string> part;
        std::string keys_text;
        for (std::size_t k = 0; k < ss.keys.size(); ++k) {
            part.push_back(
                source(ss.keys[k], "__duql_part_" + std::to_string(k)));
            keys_text += (keys_text.empty() ? "" : ", ") + ss.keys[k].text;
        }
        const std::string time = "__duql_time";
        const std::string end = "__duql_end";
        with_column(time, compile(ss.time, duql::term_text(*ss.time)),
                    duql::term_text(*ss.time));
        if (ss.end)
            with_column(end, compile(ss.end, duql::term_text(*ss.end)),
                        duql::term_text(*ss.end));
        // Time order, ties in input order.
        lines.push_back("sort_by_multi " + time + ", " + pos);
        v_ = v_.sort_by_multi({time, pos}, std::vector<bool>{false, false});
        const std::string ord = "__duql_ord";
        row_index(ord);

        std::vector<const char*> keys;
        for (const auto& k : part) keys.push_back(k.c_str());
        const char* by = ord.c_str();
        dftu_window_spec w{};
        w.func = DFTU_WINDOW_SESSIONIZE;
        w.out = ss.name.c_str();
        w.param.session = {time.c_str(), ss.end ? end.c_str() : nullptr, ss.gap,
                           ss.span};
        std::vector<std::string> out = names();
        out.push_back(ss.name);
        cols_.push_back({ss.name, df::scalar(df::TypeId::Int64), false});
        const std::vector<dftu_window_spec> specs{w};
        df::OpArgs a;
        a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
            .strlist(2, &by, 1)
            .winlist(3, specs);
        v_ = v_.with_lazy(
            v_.lazy().frame_op("dftu.frame.window", a, {}, std::move(out)));

        lines.push_back("sort_by_multi " + pos);
        v_ = v_.sort_by_multi({pos}, std::vector<bool>{false});
        lines.push_back(
            "session keys " + keys_text + "; time " + ss.text + "; gap " +
            number_text(ss.gap) + "; max " +
            (ss.span > 0 ? number_text(ss.span) : std::string("none")) +
            "; as " + ss.name);
        keep(std::move(keep_names));
    }

    void stage(const duql::PipelineExpand& e) {
        leave_scan("expand");
        std::string line = "expand " + e.path + " as " + e.name;
        if (!e.index.empty()) line += " with_index " + e.index;
        if (e.keep_empty) line += " keep_empty";
        const auto li = field_index(e.path);
        const bool list = li && cols_[*li].type.id == df::TypeId::List;
        const df::DataType elem = list && !cols_[*li].type.fields.empty()
                                      ? cols_[*li].type.fields.front().type
                                      : df::scalar(df::TypeId::String);
        const bool is_struct = elem.id == df::TypeId::Struct;
        const bool json = list && cols_[*li].json;
        auto replaced = [&](const std::string& n) {
            return n == e.name || n == e.index ||
                   (is_struct && n.starts_with(e.name + "."));
        };
        std::vector<duql::VectorColumn> next;
        std::vector<bool> drop;
        auto element_columns = [&] {
            if (is_struct)
                for (const auto& f : elem.fields)
                    next.push_back({e.name + "." + f.name, f.type});
            else
                next.push_back({e.name, elem, json});
            if (!e.index.empty())
                next.push_back({e.index, df::scalar(df::TypeId::Int64)});
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
        const std::string name = e.name;
        const std::string idx = e.index;
        const bool keep_empty = e.keep_empty;
        map_rows(
            [at, drop, name, idx, keep_empty](df::DataFrame f) {
                const std::int64_t n = f.num_rows();
                df::Series lists;
                const std::int32_t* off = nullptr;
                if (at < f.columns.size() &&
                    f.columns[at].type() == df::TypeId::List) {
                    lists = f.columns[at].materialize();
                    off = lists.offsets();
                }
                std::vector<std::int64_t> parent;
                std::vector<std::int64_t> element;
                std::vector<std::int64_t> position;
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
                df::DataFrame out;
                auto elements = [&] {
                    if (off && lists.child(0).type() == df::TypeId::Struct) {
                        const df::Series child = lists.child(0);
                        for (std::int64_t i = 0; i < child.num_children();
                             ++i) {
                            out.names.push_back(name + "." +
                                                child.field_name(i));
                            out.columns.push_back(child.child(i).take(element));
                        }
                    } else if (off) {
                        out.names.push_back(name);
                        out.columns.push_back(lists.child(0).take(element));
                    } else {
                        out.names.push_back(name);
                        out.columns.push_back(
                            df::Series::nulls(df::TypeId::String, m));
                    }
                    if (idx.empty()) return;
                    std::vector<std::uint8_t> valid(
                        static_cast<std::size_t>((m + 7) / 8), 0);
                    for (std::int64_t r = 0; r < m; ++r)
                        if (position[static_cast<std::size_t>(r)] >= 0)
                            valid[static_cast<std::size_t>(r >> 3)] |=
                                static_cast<std::uint8_t>(1u << (r & 7));
                    out.names.push_back(idx);
                    out.columns.push_back(
                        df::Series::flat_i64(position.data(), m, valid.data()));
                };
                for (std::size_t j = 0; j < f.columns.size(); ++j) {
                    if (j == at) {
                        elements();
                    } else if (!drop[j]) {
                        out.names.push_back(f.names[j]);
                        out.columns.push_back(f.columns[j].take(parent));
                    }
                }
                if (at >= f.columns.size()) elements();
                return out;
            },
            std::move(next), line);
    }

    // A term reading the column `name` as it is.
    static duql::PipelineItem column_item(const std::string& name) {
        duql::TField f;
        f.base = name;
        f.steps.push_back({name, false, std::nullopt});
        f.neg_at = 1;
        return {name,
                std::make_shared<const duql::Term>(duql::Term{std::move(f)}),
                name};
    }

    void stage(const duql::PipelinePivot& p) {
        leave_scan("pivot");
        std::vector<std::string> read;
        auto reads = [&](const duql::TermRef& t) {
            if (!t) return;
            duql::for_each_term_field(*t, [&](const duql::TField& f) {
                if (auto i = field_index(f.base))
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
                c.name == p.key.name;
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
        std::vector<std::string> fixed;
        for (const auto& v : p.values) fixed.push_back(pivot_name(v));
        lines.push_back("pivot " + p.key.text +
                        (p.fixed ? " in [" + joined(fixed) + "]"
                                 : " (columns after the scan)"));
        stage(g);
        std::vector<std::string> aggs;
        std::vector<duql::VectorColumn> next(cols_.begin(),
                                             cols_.begin() + n_keys);
        for (std::size_t a = 0; a < p.aggs.size(); ++a) {
            aggs.push_back(p.aggs[a].name);
            for (const auto& v : fixed)
                next.push_back(
                    {p.aggs[a].name + "." + v, cols_[n_keys + 1 + a].type});
        }
        std::optional<df::Schema> schema{df::Schema{}};
        if (p.fixed)
            for (const auto& c : next)
                schema->fields.push_back(df::Field{c.name, c.type, true});
        const bool open = !p.fixed;
        const std::uint64_t limit = pivot_max_columns();
        finish(
            [n_keys, aggs, fixed, open, limit](df::DataFrame f) {
                return spread(std::move(f), n_keys, aggs, fixed, open, limit);
            },
            "spread __duql_pivot into " + joined(aggs), std::move(schema));
        cols_ = p.fixed ? std::move(next) : std::vector<duql::VectorColumn>{};
    }

    // Rows sorted by (keys, pivot value) as one row per key tuple, with a
    // column `agg.value` per aggregate and value.
    static df::DataFrame spread(df::DataFrame f, std::size_t n_keys,
                                const std::vector<std::string>& aggs,
                                std::vector<std::string> values, bool open,
                                std::uint64_t limit) {
        const std::int64_t n = f.num_rows();
        auto text = [](const df::Series& s, std::int64_t r) -> std::string {
            if (s.is_null(r)) return "null";
            if (s.type() == df::TypeId::String)
                return std::string(s.string_at(r));
            if (s.type() == df::TypeId::Bool)
                return s.cast(df::TypeId::Int64).data<std::int64_t>()[r]
                           ? "true"
                           : "false";
            if (s.type() == df::TypeId::Uint64)
                return std::to_string(s.materialize().data<std::uint64_t>()[r]);
            if (is_integer(s.type()))
                return std::to_string(s.slice(r, 1)
                                          .cast(df::TypeId::Int64)
                                          .data<std::int64_t>()[0]);
            return number_text(
                s.slice(r, 1).cast(df::TypeId::Float64).data<double>()[0]);
        };
        const df::Series& pk = f.columns[n_keys];
        std::vector<std::string> row_value;
        for (std::int64_t r = 0; r < n; ++r) row_value.push_back(text(pk, r));
        if (open) {
            std::vector<std::int64_t> firsts;
            std::set<std::string> seen;
            for (std::int64_t r = 0; r < n; ++r)
                if (seen.insert(row_value[static_cast<std::size_t>(r)]).second)
                    firsts.push_back(r);
            if (firsts.size() > limit)
                refuse("'pivot' gives " + std::to_string(firsts.size()) +
                       " columns per aggregate, over DUQL_PIVOT_MAX_COLUMNS (" +
                       std::to_string(limit) + ")");
            const df::Series order = pk.take(firsts).argsort();
            for (std::int64_t i = 0; i < order.length(); ++i)
                values.push_back(row_value[static_cast<std::size_t>(
                    firsts[static_cast<std::size_t>(
                        order.data<std::int64_t>()[i])])]);
        }
        std::vector<df::Series> key_text;
        for (std::size_t c = 0; c < n_keys; ++c)
            key_text.push_back(f.columns[c].share());
        auto same_tuple = [&](std::int64_t a, std::int64_t b) {
            for (const auto& s : key_text)
                if (text(s, a) != text(s, b)) return false;
            return true;
        };
        std::vector<std::int64_t> starts;
        std::vector<std::vector<std::int64_t>> cells(values.size());
        for (std::int64_t r = 0; r < n; ++r) {
            if (r == 0 || !same_tuple(r, r - 1)) {
                starts.push_back(r);
                for (auto& c : cells) c.push_back(-1);
            }
            for (std::size_t v = 0; v < values.size(); ++v)
                if (values[v] == row_value[static_cast<std::size_t>(r)])
                    cells[v].back() = r;
        }
        df::DataFrame out;
        for (std::size_t c = 0; c < n_keys; ++c) {
            out.names.push_back(f.names[c]);
            out.columns.push_back(f.columns[c].take(starts));
        }
        for (std::size_t a = 0; a < aggs.size(); ++a)
            for (std::size_t v = 0; v < values.size(); ++v) {
                out.names.push_back(aggs[a] + "." + values[v]);
                out.columns.push_back(f.columns[n_keys + 1 + a].take(cells[v]));
            }
        return out;
    }

    void stage(const duql::PipelineUnpivot& u) {
        leave_scan("unpivot");
        std::vector<std::size_t> at;
        std::vector<df::TypeId> types;
        for (const auto& f : u.fields) {
            const auto i = field_index(f);
            if (!i) refuse("'unpivot' has no field '" + f + "'");
            at.push_back(*i);
            types.push_back(cols_[*i].type.id);
        }
        const bool unknown = std::find(types.begin(), types.end(),
                                       df::TypeId::Unknown) != types.end();
        const bool numbers = std::all_of(types.begin(), types.end(), is_number);
        const bool same =
            std::all_of(types.begin(), types.end(),
                        [&](df::TypeId t) { return t == types[0]; });
        df::TypeId target = types.empty() ? df::TypeId::String : types[0];
        if (unknown) {
            target = df::TypeId::String;
        } else if (!same && numbers) {
            target = std::all_of(types.begin(), types.end(), is_integer)
                         ? df::TypeId::Int64
                         : df::TypeId::Float64;
        } else if (!same) {
            std::string what;
            for (std::size_t k = 0; k < u.fields.size(); ++k)
                what += (what.empty() ? "" : ", ") + u.fields[k] + " (" +
                        std::string(df::type_name(types[k])) + ")";
            refuse("'unpivot' mixes the types of " + what +
                   "; convert them with int(), float() or string()");
        }
        std::vector<std::string> values;
        for (std::size_t k = 0; k < at.size(); ++k) {
            const std::string name = "__duql_value_" + std::to_string(k);
            df::Expr e = col(cols_[at[k]].name);
            if (types[k] != target)
                e = target == df::TypeId::String
                        ? df::expr_convert(df::ConvertOp::String, e)
                        : df::expr_cast(target, e);
            with_column(name, e, u.fields[k]);
            values.push_back(name);
        }
        std::vector<std::size_t> ids;
        std::vector<duql::VectorColumn> next;
        for (std::size_t j = 0; j < cols_.size(); ++j) {
            const bool listed =
                std::find(at.begin(), at.end(), j) != at.end() ||
                cols_[j].name.starts_with("__duql_value_");
            if (listed || cols_[j].name == u.key || cols_[j].name == u.value)
                continue;
            ids.push_back(j);
            next.push_back(cols_[j]);
        }
        next.push_back({u.key, df::scalar(df::TypeId::String)});
        next.push_back({u.value, df::scalar(target)});
        std::vector<std::size_t> value_at;
        for (const auto& v : values) value_at.push_back(*index(v));
        const std::vector<std::string> keys = u.fields;
        const std::string key = u.key;
        const std::string value = u.value;
        map_rows(
            [ids, value_at, keys, key, value](df::DataFrame f) {
                const std::int64_t n = f.num_rows();
                const auto k = static_cast<std::int64_t>(keys.size());
                std::vector<std::int64_t> parent;
                std::vector<std::int64_t> pick;
                std::vector<std::string> names;
                for (std::int64_t r = 0; r < n; ++r)
                    for (std::int64_t j = 0; j < k; ++j) {
                        parent.push_back(r);
                        pick.push_back(j * n + r);
                        names.push_back(keys[static_cast<std::size_t>(j)]);
                    }
                std::vector<df::DataFrame> parts;
                for (const auto v : value_at) {
                    df::DataFrame one;
                    one.names.push_back("v");
                    one.columns.push_back(f.columns[v].share());
                    parts.push_back(std::move(one));
                }
                std::vector<const df::DataFrame*> ptrs;
                for (const auto& p : parts) ptrs.push_back(&p);
                df::DataFrame stacked =
                    ptrs.empty() ? df::DataFrame{}
                                 : df::concat(ptrs, df::ConcatHow::Vertical);
                df::DataFrame out;
                for (const auto j : ids) {
                    out.names.push_back(f.names[j]);
                    out.columns.push_back(f.columns[j].take(parent));
                }
                out.names.push_back(key);
                out.columns.push_back(df::Series::strings(names));
                out.names.push_back(value);
                out.columns.push_back(stacked.columns.front().take(pick));
                return out;
            },
            std::move(next),
            "unpivot " + joined(u.fields) + " as " + u.key + ", " + u.value);
    }

    // Whether the lookup cache holds side `i`, read without a trace.
    bool cached(std::size_t i) const {
        if (ctx_.sides->stored[i]) return true;
        const auto& key = ctx_.sides->keys[i];
        if (!key) return false;
        const auto db = ix::cache::open_lookup_db(
            key->path, ix::store::RocksDatabase::OpenMode::ReadOnly);
        return db && ix::cache::lookup_exists(*db, key->sig);
    }

    // Side `i` as a join's right plan: from memory when the build step or
    // the cache has it, else its own plan, which shares the scan with the
    // rows when it reads their files, under a tap for its caps and cache.
    df::LazyFrame side_plan(std::size_t i, bool& memory) const {
        memory = std::find(collected_.begin(), collected_.end(), i) !=
                     collected_.end() ||
                 cached(i);
        if (!memory)
            return df::detail::tap(
                ctx_.sides->views[i]->lazy(),
                side_tap(main_ ? ctx_.sides : nullptr, ctx_.sides, i));
        df::Schema schema;
        for (const auto& c : ctx_.sides->columns[i])
            schema.fields.push_back(df::Field{c.name, c.type, true});
        return df::LazyFrame::scan(std::make_shared<SideSource>(
            main_ ? ctx_.sides : nullptr, ctx_.sides, i, std::move(schema)));
    }

    void stage(const duql::PipelineLookup& l) {
        leave_scan("lookup");
        const std::vector<std::string> before = names();
        const auto& side_cols = ctx_.sides->columns[l.side];
        auto side_index =
            [&](const std::string& name) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < side_cols.size(); ++i)
                if (side_cols[i].name == name) return i;
            if (schema_.args_fallback)
                for (std::size_t i = 0; i < side_cols.size(); ++i)
                    if (side_cols[i].name == "args." + name) return i;
            return std::nullopt;
        };
        std::vector<std::string> left;
        std::vector<std::string> right;
        std::string keys_text;
        for (std::size_t k = 0; k < l.keys.size(); ++k) {
            const auto& [item, column] = l.keys[k];
            const auto r = side_index(column);
            if (!r)
                refuse("'lookup " + l.name + "': '" + l.name +
                       "' has no column '" + column + "'");
            right.push_back(side_cols[*r].name);
            left.push_back(source(item, "__duql_lk_" + std::to_string(k)));
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
        const auto* into = std::get_if<duql::PipelineNest>(&l.mode);
        const bool nest = into != nullptr;
        std::vector<duql::VectorColumn> next = cols_;
        std::vector<std::string> added;
        auto shown = [&](const std::string& n) {
            return index(n) && std::find(extras_.begin(), extras_.end(), n) ==
                                   extras_.end();
        };
        if (nest) {
            if (index(into->name))
                refuse("'lookup " + l.name + " into " + into->name +
                       "' names a column the rows have; pick another name");
            std::vector<df::Field> fields;
            for (const auto& c : side_cols)
                fields.push_back(df::Field{c.name, c.type, true});
            next.push_back(
                {into->name, df::list_of(df::struct_of(std::move(fields)))});
            added.push_back(into->name);
        } else {
            for (const auto& c : side_cols) {
                if (std::find(right.begin(), right.end(), c.name) !=
                    right.end())
                    continue;
                const auto at = index(c.name);
                // A record field the rows lack stays null there, and the
                // join fills it; any other clash is the query's error.
                if (at && shown(c.name) && !cols_[*at].record)
                    refuse("'lookup " + l.name + "' adds '" + c.name +
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
        df::LazyFrame side = side_plan(l.side, memory);
        std::vector<std::string> right_on = right;
        if (!nest) {
            // The key columns name the row set in the join's errors.
            std::vector<std::string> renamed = side.schema();
            for (auto& n : renamed)
                if (std::find(right.begin(), right.end(), n) != right.end())
                    n = l.name + "." + n;
            side = side.rename(std::move(renamed));
            for (auto& k : right_on) k = l.name + "." + k;
        }
        lines.push_back("lookup " + l.name + " on " + keys_text +
                        (nest ? " into " + into->name : "") +
                        (memory ? " (side from memory)"
                                : " (side joined, sharing the scan)"));
        v_ = v_.with_lazy(
            v_.lazy().join(std::move(side), left, right_on,
                           nest ? df::JoinHow::Nest : df::JoinHow::Lookup,
                           nest ? into->name : std::string()));
        cols_ = std::move(next);
        std::vector<std::string> keep_names = before;
        for (const auto& n : added) keep_names.push_back(n);
        if (names() != keep_names) keep(std::move(keep_names));
    }

    // The type an as-of pair shares: the type itself, else the number type
    // both convert to.
    static df::DataType asof_type(const std::string& what,
                                  const std::string& side,
                                  const df::DataType& rows,
                                  const df::DataType& other) {
        if (rows.id == df::TypeId::Unknown && other.id == df::TypeId::Unknown)
            refuse("'lookup " + side + " ... asof': " + what +
                   " has no known type; convert it with int(), float() or "
                   "string()");
        if (rows == other) return rows;
        if (rows.id == df::TypeId::Unknown) return other;
        if (other.id == df::TypeId::Unknown) return rows;
        if (is_number(rows.id) && is_number(other.id))
            return df::scalar(is_integer(rows.id) && is_integer(other.id)
                                  ? df::TypeId::Int64
                                  : df::TypeId::Float64);
        refuse("'lookup " + side + " ... asof': " + what + " is " +
               std::string(df::type_name(rows.id)) + " in the rows and " +
               std::string(df::type_name(other.id)) + " in '" + side + "'");
    }

    // The side's columns a lookup adds, but its keys and the `consumed`
    // ones: `names` gives each side column its name in the join's output (""
    // for a consumed one). A record field the rows lack is renamed apart and
    // filled into the rows' column by `fills`; any other clash is an error.
    struct SideValues {
        std::vector<std::string> names;
        std::vector<std::string> added;
        std::vector<std::pair<std::string, std::string>> fills;
        std::vector<duql::VectorColumn> columns;
    };

    SideValues side_values(const duql::PipelineLookup& l, const char* clause,
                           const std::vector<std::size_t>& keys,
                           std::size_t consumed,
                           std::size_t consumed2 = SIZE_MAX) const {
        const auto& side_cols = ctx_.sides->columns[l.side];
        SideValues out;
        out.names.resize(side_cols.size());
        auto shown = [&](const std::string& n) {
            return index(n) && std::find(extras_.begin(), extras_.end(), n) ==
                                   extras_.end();
        };
        for (std::size_t c = 0; c < side_cols.size(); ++c) {
            if (c == consumed || c == consumed2 ||
                std::find(keys.begin(), keys.end(), c) != keys.end())
                continue;
            const std::string& own_name = side_cols[c].name;
            const auto at = index(own_name);
            if (at && shown(own_name) && !cols_[*at].record)
                refuse("'lookup " + l.name + " ... " + clause + "' adds '" +
                       own_name +
                       "', which the rows already have; rename one with "
                       "'rename'");
            out.names[c] = own_name;
            if (at) {
                const df::DataType& own = cols_[*at].type;
                const df::DataType& theirs = side_cols[c].type;
                if (own.id != df::TypeId::Unknown &&
                    theirs.id != df::TypeId::Unknown && own != theirs)
                    refuse("'lookup " + l.name + " ... " + clause + "': '" +
                           own_name + "' is " +
                           std::string(df::type_name(own.id)) +
                           " in the rows and " +
                           std::string(df::type_name(theirs.id)) + " in '" +
                           l.name + "'");
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
        const std::vector<std::string>& left,
        const std::vector<std::string>& right, const std::string& keys_text,
        const std::function<std::optional<std::size_t>(const std::string&)>&
            side_index) {
        const auto& side_cols = ctx_.sides->columns[l.side];
        const auto t = side_index(a.column);
        if (!t)
            refuse("'lookup " + l.name + " ... asof': '" + l.name +
                   "' has no column '" + a.column + "'");
        const std::vector<std::string> before = names();
        std::vector<std::size_t> side_key;
        for (const auto& r : right) side_key.push_back(*side_index(r));
        if (std::find(side_key.begin(), side_key.end(), *t) != side_key.end())
            refuse("'lookup " + l.name + " ... asof': '" + a.column +
                   "' is both a key and the time");

        std::vector<std::string> equi;
        std::vector<std::string> side_names;
        for (const auto& c : side_cols) side_names.push_back(c.name);
        bool memory = false;
        df::LazyFrame side = side_plan(l.side, memory);
        for (std::size_t k = 0; k < left.size(); ++k) {
            const df::DataType lt = cols_[*index(left[k])].type;
            const df::DataType st = side_cols[side_key[k]].type;
            const df::DataType to =
                asof_type("key '" + l.keys[k].second + "'", l.name, lt, st);
            const std::string from = "__duql_lk_" + std::to_string(k);
            with_column(from, df::expr_cast(to.id, col(left[k])),
                        l.keys[k].first.text);
            side = side.with_column(
                side_names[side_key[k]],
                df::expr_cast(to.id, df::expr_col(static_cast<std::int32_t>(
                                         side_key[k]))));
            equi.push_back(from);
        }
        const std::string time_src = source(a.time, "__duql_at");
        const df::DataType time_type =
            asof_type("time '" + a.column + "'", l.name,
                      cols_[*index(time_src)].type, side_cols[*t].type);
        if (!is_number(time_type.id))
            refuse("'lookup " + l.name + " ... asof': time '" + a.time.text +
                   "' is " + std::string(df::type_name(time_type.id)) +
                   ", not a number");
        const std::string time = "__duql_at";
        with_column(time, df::expr_cast(time_type.id, col(time_src)),
                    a.time.text);
        side = side.with_column(
            side_names[*t],
            df::expr_cast(time_type.id,
                          df::expr_col(static_cast<std::int32_t>(*t))));

        SideValues values = side_values(l, "asof", side_key, *t);
        // A null key has no key: the op would pair null with null.
        for (const auto k : side_key)
            side = side.filter(df::expr_is_null(
                df::expr_col(static_cast<std::int32_t>(k)), false));
        std::vector<std::string> renamed = side_names;
        renamed[*t] = time;
        for (std::size_t k = 0; k < side_key.size(); ++k)
            renamed[side_key[k]] = equi[k];
        for (std::size_t c = 0; c < renamed.size(); ++c)
            if (!values.names[c].empty()) renamed[c] = values.names[c];
        side = side.rename(std::move(renamed));

        const std::string pos = "__duql_pos";
        row_index(pos);
        std::vector<std::string> out = names();
        for (const auto& n : values.names)
            if (!n.empty()) out.push_back(n);
        std::vector<const char*> by;
        for (const auto& e : equi) by.push_back(e.c_str());
        df::OpArgs args;
        args.str(2, time.c_str())
            .strlist(3, by.data(), static_cast<std::int32_t>(by.size()))
            .i32(4, static_cast<std::int32_t>(a.direction))
            .i64(5, a.tolerance.value_or(-1));
        static constexpr const char* DIRECTIONS[] = {"backward", "forward",
                                                     "nearest"};
        static_assert(static_cast<int>(duql::syntax::AsofDirection::FORWARD) ==
                      DFTU_ASOF_FORWARD);
        static_assert(static_cast<int>(duql::syntax::AsofDirection::NEAREST) ==
                      DFTU_ASOF_NEAREST);
        lines.push_back("lookup " + l.name + " on " + keys_text + " asof " +
                        a.time.text + " " +
                        DIRECTIONS[static_cast<std::size_t>(a.direction)] +
                        (a.tolerance ? " within " + std::to_string(*a.tolerance)
                                     : std::string()) +
                        " (op dftu.frame.asof)");
        v_ = v_.with_lazy(v_.lazy().frame_op("dftu.frame.asof", args,
                                             {std::move(side)}, out));
        for (auto& c : values.columns) cols_.push_back(std::move(c));
        for (const auto& [name, tmp] : values.fills)
            with_column(name, df::expr_coalesce({col(name), col(tmp)}),
                        name + " ?? " + tmp);
        lines.push_back("sort_by " + pos);
        v_ = v_.with_lazy(v_.lazy().sort_by(pos));
        std::vector<std::string> keep_names = before;
        for (const auto& n : values.added) keep_names.push_back(n);
        keep(std::move(keep_names));
    }

    // `lookup s on k overlap [into m]`: the sweep over `s` (already in
    // memory, in file order) on the rows once the scan has ended.
    void overlap_lookup(
        const duql::PipelineLookup& l, const duql::PipelineOverlap& o,
        const std::vector<std::string>& left,
        const std::vector<std::string>& right, const std::string& keys_text,
        const std::function<std::optional<std::size_t>(const std::string&)>&
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
        const std::vector<std::string> before = names();
        std::vector<std::size_t> side_key;
        for (const auto& r : right) side_key.push_back(*side_index(r));
        const std::string time = source(o.time, "__duql_ot");
        const std::string duration = source(o.duration, "__duql_od");

        const bool nest = !o.into.empty();
        SideValues values;
        std::vector<duql::VectorColumn> next = cols_;
        if (nest) {
            if (index(o.into))
                refuse("'lookup " + l.name + " overlap into " + o.into +
                       "' names a column the rows have; pick another name");
            std::vector<df::Field> fields;
            for (const auto& c : side_cols)
                fields.push_back(df::Field{c.name, c.type, true});
            next.push_back(
                {o.into, df::list_of(df::struct_of(std::move(fields)))});
            values.added.push_back(o.into);
        } else {
            values = side_values(l, "overlap", side_key, *t, *d);
            for (const auto& c : values.columns) next.push_back(c);
        }

        df::Schema schema;
        for (const auto& c : next)
            schema.fields.push_back(df::Field{c.name, c.type, true});
        const std::vector<std::string> side_names = [&] {
            std::vector<std::string> out;
            for (const auto& c : side_cols) out.push_back(c.name);
            return out;
        }();
        const std::size_t side_id = l.side;
        const std::string into = o.into;
        const double scale = o.scale;
        const std::vector<std::size_t> consumed = [&] {
            std::vector<std::size_t> out = side_key;
            out.push_back(*t);
            out.push_back(*d);
            return out;
        }();
        const std::vector<std::string> value_names = values.names;
        std::shared_ptr<Sides> strong = main_ ? ctx_.sides : nullptr;
        std::weak_ptr<Sides> weak = ctx_.sides;
        finish(
            [=](df::DataFrame rows) {
                const std::shared_ptr<Sides> sides =
                    strong ? strong : owner(weak);
                const auto held = sides->frame(side_id);
                df::DataFrame side;
                side.names = held->names;
                for (const auto& c : held->columns)
                    side.columns.push_back(c.share());
                side = duql::flat_frame(std::move(side));
                rows = duql::flat_frame(std::move(rows));
                auto find = [](const df::DataFrame& f, const std::string& n) {
                    for (std::size_t i = 0; i < f.names.size(); ++i)
                        if (f.names[i] == n) return f.columns[i].share();
                    throw std::invalid_argument("overlap: no column " + n);
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

                df::DataFrame out;
                out.names = rows.names;
                if (!into.empty()) {
                    for (const auto& c : rows.columns)
                        out.columns.push_back(c.share());
                    std::vector<std::int32_t> offsets;
                    for (const auto x : m.offsets)
                        offsets.push_back(static_cast<std::int32_t>(x));
                    std::vector<df::Series> parts;
                    for (const auto& c : side.columns)
                        parts.push_back(c.take(m.rows));
                    out.names.push_back(into);
                    out.columns.push_back(df::Series::list(
                        offsets,
                        df::Series::structs(side.names, std::move(parts))));
                    return out;
                }
                std::vector<std::int64_t> from_rows;
                std::vector<std::int64_t> from_side;
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
                    out.names.push_back(value_names[c]);
                    out.columns.push_back(side.columns[c].take(from_side));
                }
                return out;
            },
            "lookup " + l.name + " on " + keys_text + " overlap" +
                (nest ? " into " + o.into : std::string()) + " (sweep over " +
                l.name + ")",
            std::move(schema));
        cols_ = std::move(next);
        for (const auto& [name, tmp] : values.fills)
            with_column(name, df::expr_coalesce({col(name), col(tmp)}),
                        name + " ?? " + tmp);
        std::vector<std::string> keep_names = before;
        for (const auto& n : values.added) keep_names.push_back(n);
        keep(std::move(keep_names));
    }

    // The type both sides of a union give `name`, or a refusal naming it.
    static df::DataType unify(const std::string& name, const df::DataType& a,
                              const df::DataType& b) {
        if (a.id == df::TypeId::Unknown) return b;
        if (b.id == df::TypeId::Unknown) return a;
        if (a == b) return a;
        if (is_number(a.id) && is_number(b.id))
            return df::scalar(is_integer(a.id) && is_integer(b.id)
                                  ? df::TypeId::Int64
                                  : df::TypeId::Float64);
        refuse("'union' gives column '" + name + "' as " +
               std::string(df::type_name(a.id)) + " and " +
               std::string(df::type_name(b.id)) +
               "; convert one with int(), float() or string()");
    }

    // `v` with the columns `cols` as `want`: missing ones null, others cast.
    static View align(View v, const std::vector<duql::VectorColumn>& cols,
                      const std::vector<duql::VectorColumn>& want) {
        auto at = [&](const std::string& n) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < cols.size(); ++i)
                if (cols[i].name == n) return i;
            return std::nullopt;
        };
        std::vector<std::string> order;
        for (const auto& w : want) {
            order.push_back(w.name);
            const auto i = at(w.name);
            const df::TypeId to = w.type.id == df::TypeId::Unknown
                                      ? df::TypeId::String
                                      : w.type.id;
            if (!i) {
                v = v.with_column(w.name, df::expr_lit_null(to));
                continue;
            }
            if (cols[*i].type == w.type) continue;
            const df::Expr c = df::expr_col(static_cast<std::int32_t>(*i));
            v = v.with_column(w.name,
                              to == df::TypeId::String
                                  ? df::expr_convert(df::ConvertOp::String, c)
                                  : df::expr_cast(to, c));
        }
        return v.with_lazy(v.lazy().select(std::move(order)));
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
        std::vector<duql::VectorColumn> want;
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
            if (w.type.id == df::TypeId::Unknown)
                w.type = df::scalar(df::TypeId::String);
        View mine = align(v_, cols_, want);
        View theirs = align(other.view, other.cols, want);
        v_ = v_.with_lazy(mine.lazy().concat(theirs.lazy()));
        cols_ = std::move(want);
    }

    // The column type an aggregate gives on either plan, or nullopt to keep
    // what the plan gives.
    std::optional<df::TypeId> agg_type(const duql::PipelineAgg& a,
                                       std::optional<df::TypeId> in) const {
        switch (a.fn) {
            case duql::AggFn::COUNT:
            case duql::AggFn::COUNT_IF:
            case duql::AggFn::COUNT_DISTINCT:
                return df::TypeId::Int64;
            case duql::AggFn::SKETCH:
            case duql::AggFn::MERGE:
                return df::TypeId::String;
            case duql::AggFn::COLLECT:
                return std::nullopt;
            case duql::AggFn::SUM:
                if (!in) return std::nullopt;
                return is_integer(*in) ? df::TypeId::Int64
                                       : df::TypeId::Float64;
            case duql::AggFn::MIN:
            case duql::AggFn::MAX:
            case duql::AggFn::FIRST:
            case duql::AggFn::LAST:
            case duql::AggFn::ARGMAX:
            case duql::AggFn::ARGMIN:
                if (in && (is_number(*in) || *in == df::TypeId::String ||
                           *in == df::TypeId::Bool))
                    return in;
                return std::nullopt;
            case duql::AggFn::MEAN:
            case duql::AggFn::VAR:
            case duql::AggFn::STD:
            case duql::AggFn::QUANTILE:
                return df::TypeId::Float64;
            case duql::AggFn::HISTOGRAM:
            case duql::AggFn::BUSY:
            case duql::AggFn::CONCURRENCY:
            case duql::AggFn::UTILIZATION:
            case duql::AggFn::ACTIVE:
                return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<df::TypeId> term_type(const duql::TermRef& t,
                                        const std::string& text) {
        if (!t) return std::nullopt;
        if (const auto* f = whole_field(t))
            if (const auto c = scan_column(f->base))
                return c->type.id == df::TypeId::Unknown
                           ? std::nullopt
                           : std::optional<df::TypeId>(c->type.id);
        const df::TypeId id = df::infer_type(compile(t, text), types()).id;
        if (id == df::TypeId::Unknown) return std::nullopt;
        return id;
    }

    // A whole-field term's column, usable by the trace aggregation.
    std::optional<duql::VectorColumn> trace_column(const duql::TermRef& t) {
        const auto* f = whole_field(t);
        if (!f) return std::nullopt;
        auto c = scan_column(f->base);
        if (!c || c->json) return std::nullopt;
        return c;
    }

    std::string trace_refusal(const duql::PipelineGroup& g) {
        if (!scan_)
            return "'" + blocker_ +
                   "' comes before it; only where, time_range, bucket and "
                   "a field select may";
        if (bucket_) {
            const double us = bucket_->width * us_per_time_unit();
            if (std::trunc(us) != us)
                return "its bucket width is not a whole number of "
                       "microseconds";
        }
        // A select at the scan fixes the columns: a field it left out is no
        // column, whatever the files hold.
        if (scan_selected_) {
            std::string missing;
            for (const auto& k : g.keys)
                duql::for_each_term_field(*k.term, [&](const duql::TField& f) {
                    if (!has_column_for(f.base)) missing = f.base;
                });
            for (const auto& a : g.aggs)
                if (a.arg)
                    duql::for_each_term_field(*a.arg,
                                              [&](const duql::TField& f) {
                                                  if (!has_column_for(f.base))
                                                      missing = f.base;
                                              });
            if (!missing.empty())
                refuse("has no column '" + missing +
                       "'; the columns here are " + joined(names()));
        }
        std::vector<std::string> seen;
        for (const auto& k : g.keys) {
            const auto c = trace_column(k.term);
            if (!c || (c->type.id != df::TypeId::String &&
                       c->type.id != df::TypeId::Int64 &&
                       c->type.id != df::TypeId::Uint64))
                return "key '" + k.text + "' is not a string or integer field";
            for (const auto& s : seen)
                if (s == c->name) return "a field is a key twice";
            seen.push_back(c->name);
        }
        for (const auto& a : g.aggs) {
            if (!trace_op(a))
                return "'" + a.text + "' runs only on the scanned columns";
            if (!a.arg) continue;
            const auto c = trace_column(a.arg);
            if (!c || !is_number(c->type.id))
                return "'" + a.text + "' does not read a numeric field";
        }
        return {};
    }

    void stage(const duql::PipelineGroup& group) {
        std::optional<duql::PipelineBucket> bucket = std::move(bucket_);
        bucket_.reset();
        // A sum over no values is null: a hidden mean tells which groups
        // had one.
        duql::PipelineGroup g = group;
        std::vector<std::pair<std::string, std::string>> sums;
        for (const auto& a : group.aggs)
            if (a.fn == duql::AggFn::SUM) {
                std::string valid =
                    "__duql_valid_" + std::to_string(sums.size());
                g.aggs.push_back({valid, duql::AggFn::MEAN, a.arg, 0,
                                  "mean(" + duql::term_text(*a.arg) + ")",
                                  nullptr, false});
                sums.emplace_back(a.name, std::move(valid));
            }
        const std::string why = trace_refusal(g);
        if (!why.empty()) {
            for (const auto& a : g.aggs)
                if (duql::is_occupancy(a.fn))
                    refuse("'" + agg_name(a.fn) +
                           "' needs the trace aggregation, which this group "
                           "cannot take: " +
                           why);
            leave_scan("group");
        }
        std::vector<std::optional<df::TypeId>> key_types;
        if (bucket)
            key_types.push_back(term_type(bucket->key, bucket->text)
                                    .value_or(df::TypeId::Int64));
        for (const auto& k : g.keys)
            key_types.push_back(term_type(k.term, k.text));
        std::vector<std::optional<df::TypeId>> agg_types;
        for (const auto& a : g.aggs)
            agg_types.push_back(agg_type(a, term_type(a.arg, a.text)));

        std::vector<std::string> keys;
        if (bucket) keys.push_back("bucket");
        for (const auto& k : g.keys) keys.push_back(k.name);
        std::vector<std::string> out = keys;
        for (const auto& a : group.aggs) out.push_back(a.name);
        const std::vector<std::string> visible = out;
        for (std::size_t i = 0; i < out.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (out[i] == out[j])
                    refuse("group gives two columns named '" + out[i] + "'");

        std::string text;
        for (const auto& k : g.keys)
            text += (text.empty() ? "" : ", ") + k.text;
        for (const auto& [sum, valid] : sums) out.push_back(valid);
        std::string aggs;
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
            cols_.push_back({f.name, f.type, false});
        for (std::size_t i = 0; i < out.size(); ++i) {
            const std::optional<df::TypeId> want =
                i < key_types.size() ? key_types[i]
                                     : agg_types[i - key_types.size()];
            if (!want || cols_[i].type.id == *want ||
                (*want == df::TypeId::String &&
                 cols_[i].type.id != df::TypeId::Unknown))
                continue;
            df::Expr e = col(out[i]);
            const bool is_text = cols_[i].type.id == df::TypeId::String;
            if ((is_text && is_number(*want)) ||
                (cols_[i].type.id == df::TypeId::Uint64 &&
                 *want == df::TypeId::Int64))
                e = df::expr_convert(is_integer(*want) ? df::ConvertOp::Int
                                                       : df::ConvertOp::Float,
                                     e);
            v_ = v_.with_column(out[i], df::expr_cast(*want, e));
            cols_[i].type = df::scalar(*want);
        }
        for (const auto& [sum, valid] : sums)
            v_ = v_.with_column(
                sum, df::expr_arith(df::ArithOp::Mul, col(sum),
                                    df::expr_select(
                                        df::expr_is_null(col(valid), false),
                                        df::expr_lit(std::int64_t{1}),
                                        df::expr_lit_null(df::TypeId::Int64))));
        for (const auto& a : group.aggs)
            if (a.fn == duql::AggFn::QUANTILE)
                v_ = v_.with_column(
                    a.name,
                    df::expr_select(
                        df::expr_unary(df::UnaryOp::IsNan, col(a.name)),
                        df::expr_lit_null(df::TypeId::Float64), col(a.name)));
        if (out != visible) keep(visible);
        if (!keys.empty()) {
            lines.push_back("sort_by_multi " + joined(keys));
            v_ = v_.sort_by_multi(keys, std::vector<bool>(keys.size(), false));
        }
        if (bucket && bucket->fill)
            finish_fill(g, *bucket);
        else if (keys.empty())
            finish_empty(g);
        extras_.clear();
    }

    void trace_group(const duql::PipelineGroup& g,
                     const std::optional<duql::PipelineBucket>& bucket) {
        // A select before it would project the aggregation's output; the
        // keys and arguments are among the selected columns already.
        View v = v_.select({});
        std::vector<std::string> keep_cols;
        if (bucket) {
            const auto width = static_cast<std::uint64_t>(
                std::llround(bucket->width * us_per_time_unit()));
            lines.push_back("time_bucket " + std::to_string(width) + " us");
            v = v.time_bucket(width);
            keep_cols.push_back("time_bucket");
        }
        std::vector<GroupKey> keys;
        for (const auto& k : g.keys) {
            keys.push_back(GroupKey::field(whole_field(k.term)->base));
            keep_cols.push_back(whole_field(k.term)->base);
        }
        std::vector<AggSpec> specs;
        for (std::size_t i = 0; i < g.aggs.size(); ++i) {
            const auto& a = g.aggs[i];
            std::string out = "__duql_agg_" + std::to_string(i);
            specs.emplace_back(*trace_op(a),
                               a.arg ? whole_field(a.arg)->base : "", out, "",
                               a.q);
            keep_cols.push_back(std::move(out));
        }
        if (!keys.empty()) v = v.group_by(std::move(keys));
        v_ = v.agg(std::move(specs));
        scan_ = false;
        blocker_ = "group";
        columns_from_view();
        keep(std::move(keep_cols));
        // busy() and active() come back in microseconds; the duration role
        // may count in another unit.
        const auto dur_ns = roles_.fields.find(roles_.duration);
        const double us_per_dur =
            dur_ns == roles_.fields.end()
                ? 1.0
                : static_cast<double>(dur_ns->second) / 1000.0;
        for (std::size_t i = 0; i < g.aggs.size() && us_per_dur != 1.0; ++i) {
            if (g.aggs[i].fn != duql::AggFn::BUSY &&
                g.aggs[i].fn != duql::AggFn::ACTIVE)
                continue;
            const std::string out = "__duql_agg_" + std::to_string(i);
            with_column(
                out,
                df::expr_binary(
                    df::BinaryOp::Div,
                    df::expr_col(static_cast<std::int32_t>(*index(out))),
                    df::expr_lit(us_per_dur)),
                out + " in duration units");
        }
        // The trace aggregation keys buckets in microseconds; the time role
        // may count in another unit.
        if (bucket && us_per_time_unit() != 1.0) {
            const auto i = index("time_bucket");
            df::Expr in_unit = df::expr_binary(
                df::BinaryOp::Div, df::expr_col(static_cast<std::int32_t>(*i)),
                df::expr_lit(us_per_time_unit()));
            const auto* field = schema_.field_at(roles_.time);
            if (field && field->type == ix::FieldType::INT)
                in_unit = df::expr_cast(df::TypeId::Int64, in_unit);
            with_column("time_bucket", in_unit, "time_bucket in time units");
        }
    }

    void frame_group(const duql::PipelineGroup& g,
                     const std::optional<duql::PipelineBucket>& bucket) {
        std::vector<std::string> keys;
        if (bucket) {
            with_column("__duql_bucket", compile(bucket->key, bucket->text),
                        bucket->text);
            keys.push_back("__duql_bucket");
        }
        for (std::size_t k = 0; k < g.keys.size(); ++k)
            keys.push_back(
                source(g.keys[k], "__duql_key_" + std::to_string(k)));
        std::vector<df::GroupAgg> specs;
        std::vector<duql::FoldAgg> fold(g.aggs.size());
        std::vector<std::string> by(g.aggs.size());
        bool folded = false;
        for (std::size_t i = 0; i < g.aggs.size(); ++i) {
            const auto& a = g.aggs[i];
            const std::string out = "__duql_agg_" + std::to_string(i);
            std::string in;
            if (a.by)
                by[i] = source({"", a.by, duql::term_text(*a.by)},
                               "__duql_by_" + std::to_string(i));
            if (a.fn == duql::AggFn::COUNT_IF) {
                in = "__duql_in_" + std::to_string(i);
                with_column(
                    in,
                    df::expr_select(compile(a.arg, a.text, true),
                                    df::expr_lit(std::int64_t{1}),
                                    df::expr_lit_null(df::TypeId::Int64)),
                    a.text);
            } else if (a.arg) {
                in = source({"", a.arg, a.text},
                            "__duql_in_" + std::to_string(i));
            }
            df::GroupAgg spec{df::Agg::Count, in, out};
            switch (a.fn) {
                case duql::AggFn::COUNT:
                    spec.op = df::Agg::CountValid;
                    if (!a.arg) {
                        // A key-less Count with no input gives null.
                        spec.column = "__duql_one";
                        if (!index(spec.column))
                            with_column(spec.column,
                                        df::expr_lit(std::int64_t{1}), "1");
                    }
                    break;
                case duql::AggFn::COUNT_IF:
                    spec.op = df::Agg::CountValid;
                    break;
                case duql::AggFn::SUM:
                    spec.op = df::Agg::Sum;
                    break;
                case duql::AggFn::MIN:
                    spec.op = df::Agg::Min;
                    break;
                case duql::AggFn::MAX:
                    spec.op = df::Agg::Max;
                    break;
                case duql::AggFn::MEAN:
                    spec.op = df::Agg::Mean;
                    break;
                case duql::AggFn::VAR:
                    spec.op = df::Agg::Var;
                    break;
                case duql::AggFn::STD:
                    spec.op = df::Agg::Std;
                    break;
                case duql::AggFn::FIRST:
                    spec.op = df::Agg::First;
                    break;
                case duql::AggFn::LAST:
                    spec.op = df::Agg::Last;
                    break;
                case duql::AggFn::QUANTILE:
                    spec.op = df::Agg::Pct;
                    spec.param = a.q;
                    if (a.merged) fold[i].op = duql::FoldOp::MERGE_QUANTILE;
                    break;
                case duql::AggFn::HISTOGRAM:
                    spec.op = df::Agg::Hist;
                    break;
                case duql::AggFn::BUSY:
                case duql::AggFn::CONCURRENCY:
                case duql::AggFn::UTILIZATION:
                case duql::AggFn::ACTIVE:
                    refuse("'" + agg_name(a.fn) +
                           "' needs the trace aggregation");
                case duql::AggFn::COUNT_DISTINCT:
                    fold[i].op = duql::FoldOp::COUNT_DISTINCT;
                    break;
                case duql::AggFn::COLLECT:
                    fold[i].op = duql::FoldOp::COLLECT;
                    break;
                case duql::AggFn::ARGMAX:
                    fold[i].op = duql::FoldOp::ARGMAX;
                    break;
                case duql::AggFn::ARGMIN:
                    fold[i].op = duql::FoldOp::ARGMIN;
                    break;
                case duql::AggFn::SKETCH:
                    fold[i].op = duql::FoldOp::SKETCH;
                    break;
                case duql::AggFn::MERGE:
                    fold[i].op = duql::FoldOp::MERGE;
                    break;
            }
            folded = folded || fold[i].op != duql::FoldOp::ENGINE;
            fold[i].engine = df::to_agg_op(spec.op);
            fold[i].param = spec.param;
            specs.push_back(std::move(spec));
        }
        if (!folded) {
            v_ = v_.group_by(keys, specs);
            columns_from_view();
            return;
        }
        fold_group(keys, specs, by, std::move(fold));
    }

    // A group with an aggregate the engine lacks: one streaming pass, in
    // input order, that folds the engine's aggregates and duql's.
    void fold_group(const std::vector<std::string>& keys,
                    const std::vector<df::GroupAgg>& specs,
                    const std::vector<std::string>& by,
                    std::vector<duql::FoldAgg> fold) {
        std::vector<std::string> read = keys;
        auto at = [&](const std::string& name) {
            const auto it = std::find(read.begin(), read.end(), name);
            if (it != read.end())
                return static_cast<std::int32_t>(it - read.begin());
            read.push_back(name);
            return static_cast<std::int32_t>(read.size() - 1);
        };
        for (std::size_t i = 0; i < fold.size(); ++i) {
            if (!specs[i].column.empty()) fold[i].in = at(specs[i].column);
            if (!by[i].empty()) fold[i].by = at(by[i]);
        }
        df::Schema schema;
        std::vector<df::DataType> key_types;
        for (const auto& k : keys) {
            key_types.push_back(type_of(k));
            schema.fields.push_back(df::Field{k, type_of(k), true});
        }
        std::vector<std::pair<std::string, df::TypeId>> in;
        for (const auto& n : read) in.emplace_back(n, type_of(n).id);
        for (std::size_t i = 0; i < fold.size(); ++i) {
            auto& f = fold[i];
            const df::DataType t =
                f.in < 0 ? df::scalar(df::TypeId::Int64)
                         : type_of(read[static_cast<std::size_t>(f.in)]);
            switch (f.op) {
                case duql::FoldOp::ENGINE:
                    f.type = df::agg_output_type(f.engine, t.id);
                    break;
                case duql::FoldOp::COUNT_DISTINCT:
                    f.type = df::scalar(df::TypeId::Int64);
                    break;
                case duql::FoldOp::COLLECT:
                    f.type = df::list_of(t);
                    break;
                case duql::FoldOp::ARGMAX:
                case duql::FoldOp::ARGMIN:
                    f.type = t;
                    break;
                case duql::FoldOp::SKETCH:
                case duql::FoldOp::MERGE:
                    f.type = df::scalar(df::TypeId::String);
                    break;
                case duql::FoldOp::MERGE_QUANTILE:
                    f.type = df::scalar(df::TypeId::Float64);
                    break;
            }
            schema.fields.push_back(
                df::Field{"__duql_agg_" + std::to_string(i), f.type, true});
        }
        lines.push_back("group fold over " + joined(read) +
                        " (one pass, in input order)");
        v_ = v_.with_lazy(df::LazyFrame::scan(std::make_shared<FoldSource>(
            v_.lazy().select(read), std::move(schema), std::move(in),
            std::move(key_types), std::move(fold))));
        columns_from_view();
    }

    void finish(FinishSource::Finish fn, const std::string& line,
                std::optional<df::Schema> schema = std::nullopt) {
        lines.push_back(line);
        if (!schema) schema = v_.output_schema();
        v_ = v_.with_lazy(df::LazyFrame::scan(std::make_shared<FinishSource>(
            v_.lazy(), std::move(*schema), std::move(fn))));
    }

    // `agg` over no rows: one row, counts 0 and the rest null.
    void finish_empty(const duql::PipelineGroup& g) {
        std::vector<std::string> counts;
        for (const auto& a : g.aggs)
            if (is_count(a.fn)) counts.push_back(a.name);
        std::vector<std::pair<std::string, df::DataType>> cols;
        for (const auto& c : cols_) cols.emplace_back(c.name, c.type);
        finish(
            [counts, cols](df::DataFrame f) {
                if (f.num_rows() > 0) return f;
                df::DataFrame row;
                const std::int64_t zero = 0;
                for (const auto& [name, type] : cols) {
                    row.names.push_back(name);
                    if (std::find(counts.begin(), counts.end(), name) !=
                        counts.end())
                        row.columns.push_back(df::Series::flat_i64(&zero, 1));
                    else
                        row.columns.push_back(null_cell(type));
                }
                return row;
            },
            "one row when empty");
    }

    // `bucket ... fill`: a row for every bucket between the first and the
    // last, for each group.
    void finish_fill(const duql::PipelineGroup& g,
                     const duql::PipelineBucket& b) {
        std::vector<std::string> counts;
        for (const auto& a : g.aggs)
            if (is_count(a.fn)) counts.push_back(a.name);
        const std::size_t n_keys = g.keys.size();
        const double width = b.width;
        const std::uint64_t limit = fill_max_rows();
        finish(
            [counts, n_keys, width, limit](df::DataFrame f) {
                const std::int64_t n = f.num_rows();
                if (n == 0) return f;
                const df::Series bucket =
                    f.columns[0].cast(df::TypeId::Float64);
                const double* at = bucket.data<double>();
                double lo = std::numeric_limits<double>::infinity();
                double hi = -lo;
                for (std::int64_t r = 0; r < n; ++r) {
                    if (bucket.is_null(r)) continue;
                    lo = std::min(lo, at[r]);
                    hi = std::max(hi, at[r]);
                }
                if (lo > hi) return f;
                const auto steps = static_cast<std::uint64_t>(
                                       std::llround((hi - lo) / width)) +
                                   1;
                std::map<std::string, std::int64_t> groups;
                std::vector<std::int64_t> reps;
                std::map<std::pair<std::string, std::int64_t>, bool> present;
                std::vector<df::Series> text;
                for (std::size_t c = 1; c <= n_keys; ++c)
                    text.push_back(f.columns[c].type() == df::TypeId::String
                                       ? f.columns[c].share()
                                       : f.columns[c].cast(df::TypeId::String));
                auto key_of = [&](std::int64_t r) {
                    std::string k;
                    for (const df::Series& s : text) {
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
                for (std::int64_t r = 0; r < n; ++r) {
                    std::string k = key_of(r);
                    if (groups
                            .emplace(k, static_cast<std::int64_t>(reps.size()))
                            .second)
                        reps.push_back(r);
                    if (!bucket.is_null(r))
                        present[{k, std::llround((at[r] - lo) / width)}] = true;
                }
                if (steps * reps.size() > limit)
                    refuse("'bucket ... fill' would make " +
                           std::to_string(steps) + " buckets x " +
                           std::to_string(reps.size()) +
                           " groups, over DUQL_FILL_MAX_ROWS (" +
                           std::to_string(limit) + ")");
                std::vector<std::int64_t> take;
                std::vector<double> starts;
                for (std::uint64_t s = 0; s < steps; ++s)
                    for (const auto& [k, at_rep] : groups)
                        if (!present.count({k, static_cast<std::int64_t>(s)})) {
                            take.push_back(
                                reps[static_cast<std::size_t>(at_rep)]);
                            starts.push_back(lo +
                                             static_cast<double>(s) * width);
                        }
                if (take.empty()) return f;
                std::vector<std::string> key_names(
                    f.names.begin(), f.names.begin() + 1 + n_keys);
                df::DataFrame add = f.select(key_names).take(take);
                const auto m = static_cast<std::int64_t>(take.size());
                add.columns[0] = df::Series::flat_f64(starts.data(), m)
                                     .cast(f.columns[0].type());
                const std::vector<std::int64_t> zeros(
                    static_cast<std::size_t>(m), 0);
                for (const auto& c : counts) {
                    add.names.push_back(c);
                    add.columns.push_back(
                        df::Series::flat_i64(zeros.data(), m));
                }
                df::DataFrame out =
                    df::concat({&f, &add}, df::ConcatHow::Diagonal);
                return out.select(f.names).sort_by_multi(
                    key_names, std::vector<bool>(key_names.size(), false));
            },
            "fill buckets " + number_text(width));
    }
};

// `p` as a View over its input: its build step, when it reads lookups, then
// its scan filter and projection, then its stages. With `table`, the scan
// ends, so the columns are the stages' own. The `main` plan owns the sides.
Built build(const Ctx& ctx, const duql::Pipeline& p, bool table,
            std::vector<std::string>& lines, bool main, bool ordered) {
    View v = ctx.input(p.input);
    if (ordered && p.input.kind != duql::InputKind::SIDE) v = ctx.order(v);
    std::optional<std::vector<duql::VectorColumn>> side_cols;
    if (p.input.kind == duql::InputKind::SIDE) {
        const std::size_t i = p.input.side;
        side_cols = ctx.sides->columns[i];
        df::Schema schema;
        for (const auto& c : *side_cols)
            schema.fields.push_back(df::Field{c.name, c.type, true});
        v = v.with_lazy(df::LazyFrame::scan(std::make_shared<SideSource>(
            main ? ctx.sides : nullptr, ctx.sides, i, std::move(schema))));
        lines.push_back("from " + ctx.sides->program.sides[i].name +
                        (ctx.sides->stored[i]
                             ? " (rows stored in the index)"
                             : " (its side runs when the query executes)"));
    }
    std::vector<std::size_t> reads;
    term_reads(p, reads);
    std::vector<std::size_t> sets;
    set_reads(p, sets);
    std::shared_ptr<detail::BuildStep> step;
    if (!reads.empty() || !sets.empty()) {
        struct Once {
            std::once_flag flag;
            Pushdown keys;
        };
        auto once = std::make_shared<Once>();
        step = std::make_shared<detail::BuildStep>();
        step->run = [once, strong = main ? ctx.sides : nullptr,
                     weak = std::weak_ptr<Sides>(ctx.sides),
                     pipeline = &p](std::optional<duql::Query> query) {
            std::call_once(once->flag, [&] {
                once->keys =
                    (strong ? strong : owner(weak))->prepare(*pipeline);
            });
            return pushed(once->keys, std::move(query));
        };
        v = ctx.with_step(v, step);
        for (const auto i : reads)
            lines.push_back("reads " + ctx.sides->program.sides[i].name +
                            " before the scan");
        for (const auto i : sets)
            if (std::find(reads.begin(), reads.end(), i) == reads.end())
                lines.push_back("reads the distinct rows of " +
                                ctx.sides->program.sides[i].name +
                                " before the scan");
    }
    if (p.filter) {
        lines.push_back("scan filter: " + p.filter_text + " (pushed)");
        v = v.filter(duql::Query::from_node(duql::clone(*p.filter)));
    }
    each_pushed(p, [&](const duql::Term&, const duql::TField& f,
                       const duql::TLookup& l, const duql::Term*) {
        lines.push_back("scan filter: " + f.base + " in (keys of " + l.name +
                        ") (pushed)");
    });
    auto adds = [&](const duql::PipelineLookup& l) {
        std::vector<std::string> out;
        if (const auto* into = std::get_if<duql::PipelineNest>(&l.mode)) {
            out.push_back(into->name);
            return out;
        }
        for (const auto& c : ctx.sides->columns[l.side]) out.push_back(c.name);
        return out;
    };
    Reads fields = referenced_fields(p.stages, adds);
    if (!p.scan_select.empty()) {
        lines.push_back("scan select: " + joined(p.scan_select));
        std::vector<std::string> select;
        std::optional<std::vector<SchemaLeaf>> tree;
        for (const auto& f : p.scan_select) {
            const bool array =
                std::find(fields.arrays.begin(), fields.arrays.end(), f) !=
                fields.arrays.end();
            if (!array && p.stages.empty()) {
                select.push_back(f);
                continue;
            }
            if (!tree) tree = v.schema_tree();
            for (auto& r : scan_reads(*tree, f, array, fields.arrays,
                                      ctx.schema(v).args_fallback))
                select.push_back(std::move(r));
        }
        v = v.select(std::move(select));
        fields.fields.clear();
    }
    if (p.stages.empty() && !table) {
        std::vector<duql::VectorColumn> cols;
        for (const auto& f : v.output_schema().fields)
            cols.push_back({f.name, f.type});
        return {std::move(v), std::move(cols)};
    }
    const ix::RecordSchema& schema = ctx.schema(v);
    Applier a(std::move(v), schema, ctx, std::move(fields), step, reads, main,
              std::move(side_cols));
    if (!p.scan_select.empty()) a.selected_at_scan();
    for (const auto& s : p.stages) a.apply(s);
    if (table) a.end_scan("union");
    a.finish_columns();
    for (auto& l : a.lines) lines.push_back(std::move(l));
    std::vector<duql::VectorColumn> cols = a.columns();
    return {std::move(a).take(), std::move(cols)};
}

}  // namespace

View detail::view_of(detail::ScanPlan plan) { return View(std::move(plan)); }

std::pair<View, std::vector<std::string>> View::duql_plan(
    const std::string& text, const duql::Params& params) const {
    const ix::RecordSchema& schema = detail::plan_record_schema(*plan_);
    Ctx ctx;
    ctx.roles = ix::duql_roles(schema);
    auto p = duql::compile_program(text, params, &ctx.roles, schema.source);
    if (!p) refuse(p.error().format());
    reject_resolved(*p);
    ctx.sides = std::make_shared<Sides>(std::move(*p));
    ctx.order = [](const View& in) {
        return in.reshape("duql", [](const detail::ScanPlan& s) {
            return detail::scan::ordered(s);
        });
    };
    ctx.with_step = [](const View& in,
                       std::shared_ptr<const detail::BuildStep> step) {
        return in.reshape("duql", [&](const detail::ScanPlan& s) {
            auto next = std::make_shared<detail::ViewPlan>(*s);
            next->build_step = step;
            return detail::ScanPlan(std::move(next));
        });
    };
    ctx.input = [this](const duql::Input& in) -> View {
        switch (in.kind) {
            case duql::InputKind::DATA:
                break;
            case duql::InputKind::ALL:
            case duql::InputKind::SIDE: {
                auto all = std::make_shared<detail::ViewPlan>();
                all->files = plan_->files;
                all->record_schema = plan_->record_schema;
                all->rollup_root = plan_->rollup_root;
                all->views_root = plan_->views_root;
                all->memory_budget = plan_->memory_budget;
                all->cancelled = plan_->cancelled;
                all->all_records = true;
                return detail::view_of(std::move(all));
            }
            case duql::InputKind::FILE:
                return View::from_file(
                    in.path, internal::determine_index_path(in.path, ""));
        }
        return *this;
    };
    ctx.schema = [](const View& v) -> const ix::RecordSchema& {
        return detail::plan_record_schema(*v.plan_);
    };
    Sides& sides = *ctx.sides;
    std::vector<bool> needed(sides.program.sides.size(), false);
    std::vector<std::size_t> reads;
    side_reads(sides.program.main, reads);
    for (const auto i : reads) needed[i] = true;
    for (std::size_t i = needed.size(); i-- > 0;) {
        if (!needed[i]) continue;
        std::vector<std::size_t> more;
        side_reads(sides.program.sides[i].pipeline, more);
        for (const auto j : more) needed[j] = true;
    }
    std::vector<std::size_t> asof;
    asof_reads(sides.program.main, asof);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) asof_reads(sides.program.sides[i].pipeline, asof);
    std::vector<std::size_t> tables;
    std::vector<std::size_t> sets;
    term_reads(sides.program.main, tables);
    set_reads(sides.program.main, sets);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) {
            term_reads(sides.program.sides[i].pipeline, tables);
            set_reads(sides.program.sides[i].pipeline, sets);
        }
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < needed.size(); ++i) {
        if (!needed[i]) continue;
        const duql::Side& side = sides.program.sides[i];
        std::vector<std::string> side_lines;
        Built b = build(ctx, side.pipeline, false, side_lines, false,
                        std::find(asof.begin(), asof.end(), i) != asof.end());
        sides.keys[i] =
            cache_key(*b.view.plan_, side.pipeline.input.kind, side.key);
        if (side.rowset) {
            for (const auto& f : plan_->files)
                sides.files[i].push_back(
                    {f.file_path,
                     f.index_path.empty()
                         ? internal::determine_index_path(f.file_path, "")
                         : f.index_path});
            sides.stored[i] = ix::plan::has_stored_rowsets(sides.files[i]);
        }
        sides.views[i] = std::move(b.view);
        sides.columns[i] = std::move(b.cols);
        // A column whose type only the data gives (a dftracer arg) takes it
        // from the side's rows: all of them when a table reads them anyway,
        // its distinct rows for a key set, else the first TYPE_PROBE_ROWS, so
        // a joined side stays uncapped.
        if (std::any_of(sides.columns[i].begin(), sides.columns[i].end(),
                        [](const duql::VectorColumn& c) {
                            return c.type.id == df::TypeId::Unknown;
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
                    if (c.type.id == df::TypeId::Unknown &&
                        frame->names[k] == c.name)
                        c.type = frame->columns[k].data_type();
        }
        lines.push_back("side " + side.name + ": from " +
                        input_text(side.pipeline.input, sides.program) + "; " +
                        (sides.stored[i] ? std::string("stored in the index")
                                         : cache_state(sides.keys[i])));
        if (!sides.stored[i])
            for (auto& l : side_lines) lines.push_back("  " + std::move(l));
    }
    Built main = build(ctx, sides.program.main, false, lines, true);
    return {std::move(main.view), std::move(lines)};
}

View View::duql(const std::string& text, const duql::Params& params) const {
    return duql_plan(text, params).first;
}

std::string View::explain_duql(const std::string& text,
                               const duql::Params& params) const {
    std::string out;
    for (const auto& l : duql_plan(text, params).second) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace dftracer::utils::trace::views
