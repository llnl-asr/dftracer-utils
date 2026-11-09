// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

namespace {

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
    using Finish = FrameFn;

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
                if (!projection_.empty())
                    f = common::unwrap(f.select(projection_));
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
    using Map = FrameFn;

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
                if (!projection_.empty())
                    f = common::unwrap(f.select(projection_));
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

}  // namespace

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

namespace {

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
                    common::unwrap(fold_.add(aligned(std::move(*batch), in_)));
                }
                dataframe::DataFrame f =
                    common::unwrap(fold_.finish(std::move(names_)));
                if (!projection_.empty())
                    f = common::unwrap(f.select(projection_));
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

}  // namespace

dataframe::LazyFrame finish_source(dataframe::LazyFrame inner,
                                   dataframe::Schema schema, FrameFn finish) {
    return dataframe::LazyFrame::scan(dstd::make_shared<FinishSource>(
        std::move(inner), std::move(schema), std::move(finish)));
}

dataframe::LazyFrame map_source(dataframe::LazyFrame inner,
                                dataframe::Schema schema, FrameFn map) {
    return dataframe::LazyFrame::scan(dstd::make_shared<MapSource>(
        std::move(inner), std::move(schema), std::move(map)));
}

dataframe::LazyFrame fold_source(
    dataframe::LazyFrame inner, dataframe::Schema schema,
    dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in,
    dstd::Vector<dataframe::DataType> key_types,
    dstd::Vector<duql::FoldAgg> aggs) {
    return dataframe::LazyFrame::scan(dstd::make_shared<FoldSource>(
        std::move(inner), std::move(schema), std::move(in),
        std::move(key_types), std::move(aggs)));
}

}  // namespace dftracer::utils::trace::views::duql_detail
