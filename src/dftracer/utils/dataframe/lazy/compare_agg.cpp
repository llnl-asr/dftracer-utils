// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/internal/governor.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/budget.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace lazy_internal {

// compare_agg as a plan: the metrics of each side are prefixed `l_` / `r_`,
// the sides are outer joined on the key columns and sorted by them, and each
// metric both sides carry and that is numeric gets `delta_` and `pct_` columns.
dstd::Result<LazyFrame> compose_compare_agg(const LazyFrame& base,
                                            const LazyFrame& variant,
                                            const dstd::Vector<Field>& lhs,
                                            const dstd::Vector<Field>& rhs,
                                            std::size_t nk) {
    if (nk > lhs.size() || nk > rhs.size())
        return invalid_argument<
            "compare_agg: n_key exceeds a frame's column count">();
    dstd::Vector<dstd::String> keys;
    for (std::size_t i = 0; i < nk; ++i) {
        if (lhs[i].name != rhs[i].name)
            return invalid_argument<
                "compare_agg: key column {} is '{}' vs '{}'">(i, lhs[i].name,
                                                              rhs[i].name);
        keys.push_back(lhs[i].name);
    }
    dstd::Vector<dstd::String> from_l, to_l, from_r, to_r;
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        from_l.push_back(lhs[i].name);
        to_l.push_back(dstd::fmt<"l_{}">(lhs[i].name));
    }
    for (std::size_t i = nk; i < rhs.size(); ++i) {
        from_r.push_back(rhs[i].name);
        to_r.push_back(dstd::fmt<"r_{}">(rhs[i].name));
    }
    LazyFrame out = base.rename_columns(from_l, to_l)
                        .join(variant.rename_columns(from_r, to_r), keys, keys,
                              JoinHow::Outer, "_right")
                        .sort_by_multi(keys, false);
    const Schema joined = out.output_schema();
    auto index_of = [&](const dstd::String& name) {
        for (std::size_t i = 0; i < joined.fields.size(); ++i)
            if (joined.fields[i].name == name)
                return static_cast<std::int32_t>(i);
        return std::int32_t{-1};
    };
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        const dstd::String& m = lhs[i].name;
        const std::int32_t li = index_of(dstd::fmt<"l_{}">(m)),
                           ri = index_of(dstd::fmt<"r_{}">(m));
        if (ri < 0 || !is_numeric_dispatchable(joined.fields[li].type.id) ||
            !is_numeric_dispatchable(joined.fields[ri].type.id))
            continue;
        const Expr delta = expr_col(ri) - expr_col(li);
        out = out.with_column(dstd::fmt<"delta_{}">(m), delta)
                  .with_column(dstd::fmt<"pct_{}">(m),
                               (delta * lit(1.0)) / (expr_col(li) * lit(1.0)) *
                                   lit(100.0));
    }
    if (auto built = out.status(); !built) return built.error();
    return out;
}

// A plan source over a spool: the rows a cursor produced, with the schema
// read off its first morsel, replayable and spilled past the budget.

}  // namespace lazy_internal

namespace {

class SpoolSource : public Source {
   public:
    SpoolSource(dstd::SharedPtr<spill::Spool> spool, Schema schema)
        : spool_(std::move(spool)), schema_(std::move(schema)) {}

    Schema schema() const override { return schema_; }

    dstd::Result<ScanResult> scan(const ScanRequest& req) const override {
        ScanResult r;
        auto opened = spool_->reader();
        if (!opened) return opened.error();
        dstd::UniquePtr<Cursor> reader = std::move(*opened);
        if (req.projection.empty()) {
            r.cursor = std::move(reader);
        } else {
            dstd::Vector<std::size_t> pick;
            for (const dstd::String& name : req.projection) {
                const auto it = std::find_if(
                    schema_.fields.begin(), schema_.fields.end(),
                    [&](const Field& f) { return f.name == name; });
                if (it == schema_.fields.end())
                    return not_found<"scan: no column named {}">(name).error();
                pick.push_back(
                    static_cast<std::size_t>(it - schema_.fields.begin()));
            }
            r.cursor = dstd::make_unique<ProjectCursor>(std::move(reader),
                                                        std::move(pick));
        }
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    dstd::SharedPtr<spill::Spool> spool_;
    Schema schema_;
};

// compare_agg over two plans whose columns are only known once they run: both
// sides are spooled (and spill past the budget), their names and types are read
// off the spooled morsels, and the typed plan of compose_compare_agg runs over
// the two spools.
class CompareAggCursor : public Cursor {
   public:
    CompareAggCursor(dstd::UniquePtr<Cursor> in, dstd::Vector<dstd::String> sch,
                     LazyFrame other, std::int64_t n_key, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          other_(std::move(other)),
          n_key_(n_key),
          budget_(budget) {}

    std::uint64_t resident_bytes() const override { return gov_.reserved(); }

    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        if (!built_) {
            auto building = build(max_rows);
            auto built = co_await std::move(building);
            if (!built) co_return built.error();
        }
        auto df = co_await gen_->next();
        if (!df) co_return MorselResult(dstd::nullopt);
        if (!*df) co_return df->error();
        out_names_ = (*df)->names;
        co_return MorselResult(morsel_of(std::move(**df)));
    }

    dstd::Optional<dstd::Vector<dstd::String>> out_names() const override {
        return out_names_;
    }

   private:
    dstd::Result<void> govern(spill::Spool& left, spill::Spool& right) {
        if (budget_ == 0 || budget_ == memory::NO_SPILL_BUDGET) return {};
        if (gov_.check(left.memory_bytes() + right.memory_bytes()) == 0)
            return {};
        auto left_spilled = left.spill();
        if (!left_spilled) return left_spilled.error();
        auto right_spilled = right.spill();
        if (!right_spilled) return right_spilled.error();
        gov_.spilled(left.memory_bytes() + right.memory_bytes());
        return {};
    }

    static std::uint64_t share_of(std::uint64_t budget) {
        return budget == 0 || budget == memory::NO_SPILL_BUDGET
                   ? memory::NO_SPILL_BUDGET
                   : budget / share::SPOOL;
    }

    // The type of a column is the first one a morsel gives it that is known:
    // a column that is null so far has the type Unknown.
    static void note_types(dstd::Vector<DataType>& types,
                           const dstd::Vector<Series>& cols) {
        if (types.empty()) types.resize(cols.size());
        for (std::size_t i = 0; i < cols.size() && i < types.size(); ++i)
            if (types[i].id == TypeId::Unknown) types[i] = cols[i].data_type();
    }

    static dstd::Result<dstd::Vector<Field>> fields_of(
        const dstd::Vector<dstd::String>& names,
        const dstd::Vector<DataType>& types) {
        if (!types.empty() && types.size() != names.size())
            return common::make_error<
                "compare_agg: a plan names {} columns and returns {}">(
                common::ErrorCode::Internal, names.size(), types.size());
        dstd::Vector<Field> out;
        for (std::size_t i = 0; i < names.size(); ++i)
            out.push_back(
                Field{names[i], types.empty() ? DataType{} : types[i], true});
        return out;
    }

    coro::CoroTask<dstd::Result<void>> build(std::int64_t max_rows) {
        auto lspool = dstd::make_shared<spill::Spool>(share_of(budget_));
        auto rspool = dstd::make_shared<spill::Spool>(share_of(budget_));
        dstd::Vector<dstd::String> lnames = sch_;
        dstd::Vector<DataType> ltypes;
        for (;;) {
            auto task = in_->next(max_rows);
            auto m = co_await std::move(task).or_fail();
            if (!m) break;
            note_types(ltypes, m->columns);
            auto added = lspool->add(std::move(m->columns), m->rows);
            if (!added) co_return added;
            auto governed = govern(*lspool, *rspool);
            if (!governed) co_return governed;
        }
        if (auto produced = in_->out_names(); produced && !produced->empty())
            lnames = std::move(*produced);
        in_.reset();

        const LazyFrame& other = other_;
        dstd::Vector<dstd::String> rnames;
        dstd::Vector<DataType> rtypes;
        auto gen = other.stream(max_rows);
        for (;;) {
            auto next_df = co_await gen.next();
            if (!next_df) break;
            if (!*next_df) co_return next_df->error();
            DataFrame* df = &**next_df;
            if (rtypes.empty()) rnames = df->names;
            note_types(rtypes, df->columns);
            dstd::Vector<Series> cols;
            for (const Series& c : df->columns)
                cols.push_back(c.encoding() == Encoding::Flat
                                   ? c.share()
                                   : c.materialize());
            auto added = rspool->add(std::move(cols), df->num_rows());
            if (!added) co_return added;
            auto governed = govern(*lspool, *rspool);
            if (!governed) co_return governed;
        }

        auto left_fields = fields_of(lnames, ltypes);
        if (!left_fields) co_return left_fields.error();
        auto right_fields = fields_of(rnames, rtypes);
        if (!right_fields) co_return right_fields.error();
        const dstd::Vector<Field> lf = std::move(*left_fields);
        const dstd::Vector<Field> rf = std::move(*right_fields);
        const LazyFrame left =
            LazyFrame::scan(dstd::make_shared<SpoolSource>(lspool, Schema{lf}));
        const LazyFrame right =
            LazyFrame::scan(dstd::make_shared<SpoolSource>(rspool, Schema{rf}));
        auto composed = compose_compare_agg(left, right, lf, rf,
                                            static_cast<std::size_t>(n_key_));
        if (!composed) co_return composed.error();
        if (budget_ != 0 && budget_ != memory::NO_SPILL_BUDGET)
            *composed = composed->memory_budget(budget_);
        plan_.emplace(std::move(*composed));
        gen_.emplace(plan_->stream(max_rows));
        built_ = true;
        co_return dstd::Result<void>{};
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<dstd::String> sch_;
    LazyFrame other_;
    std::int64_t n_key_;
    std::uint64_t budget_;
    Governor gov_{"compare_agg"};
    dstd::Optional<LazyFrame> plan_;
    dstd::Optional<coro::AsyncGenerator<dstd::Result<DataFrame>>> gen_;
    dstd::Optional<dstd::Vector<dstd::String>> out_names_;
    bool built_ = false;
};

}  // namespace

namespace lazy_internal {

CursorResult make_compare_agg(dstd::UniquePtr<Cursor> in,
                              dstd::Vector<dstd::String> sch, LazyFrame other,
                              std::int64_t n_key, std::uint64_t budget) {
    return dstd::make_unique<CompareAggCursor>(std::move(in), std::move(sch),
                                               std::move(other), n_key, budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
