#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace lazy_internal {

// compare_agg as a plan: the metrics of each side are prefixed `l_` / `r_`,
// the sides are outer joined on the key columns and sorted by them, and each
// metric both sides carry and that is numeric gets `delta_` and `pct_` columns.
LazyFrame compose_compare_agg(const LazyFrame& base, const LazyFrame& variant,
                              const std::vector<Field>& lhs,
                              const std::vector<Field>& rhs, std::size_t nk) {
    if (nk > lhs.size() || nk > rhs.size())
        throw std::invalid_argument(
            "compare_agg: n_key exceeds a frame's column count");
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < nk; ++i) {
        if (lhs[i].name != rhs[i].name)
            throw std::invalid_argument(
                "compare_agg: key column " + std::to_string(i) + " is '" +
                lhs[i].name + "' vs '" + rhs[i].name + "'");
        keys.push_back(lhs[i].name);
    }
    std::vector<std::string> from_l, to_l, from_r, to_r;
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        from_l.push_back(lhs[i].name);
        to_l.push_back("l_" + lhs[i].name);
    }
    for (std::size_t i = nk; i < rhs.size(); ++i) {
        from_r.push_back(rhs[i].name);
        to_r.push_back("r_" + rhs[i].name);
    }
    LazyFrame out = base.rename_columns(from_l, to_l)
                        .join(variant.rename_columns(from_r, to_r), keys, keys,
                              JoinHow::Outer, "_right")
                        .sort_by_multi(keys, false);
    const Schema joined = out.output_schema();
    auto index_of = [&](const std::string& name) {
        for (std::size_t i = 0; i < joined.fields.size(); ++i)
            if (joined.fields[i].name == name)
                return static_cast<std::int32_t>(i);
        return std::int32_t{-1};
    };
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        const std::string& m = lhs[i].name;
        const std::int32_t li = index_of("l_" + m), ri = index_of("r_" + m);
        if (ri < 0 || !is_numeric_dispatchable(joined.fields[li].type.id) ||
            !is_numeric_dispatchable(joined.fields[ri].type.id))
            continue;
        const Expr delta = expr_col(ri) - expr_col(li);
        out = out.with_column("delta_" + m, delta)
                  .with_column("pct_" + m, (delta * lit(1.0)) /
                                               (expr_col(li) * lit(1.0)) *
                                               lit(100.0));
    }
    return out;
}

// A plan source over a spool: the rows a cursor produced, with the schema
// read off its first morsel, replayable and spilled past the budget.

}  // namespace lazy_internal

namespace {

class SpoolSource : public Source {
   public:
    SpoolSource(std::shared_ptr<spill::Spool> spool, Schema schema)
        : spool_(std::move(spool)), schema_(std::move(schema)) {}

    Schema schema() const override { return schema_; }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        std::unique_ptr<Cursor> reader = spool_->reader();
        if (req.projection.empty()) {
            r.cursor = std::move(reader);
        } else {
            std::vector<std::size_t> pick;
            for (const std::string& name : req.projection) {
                const auto it = std::find_if(
                    schema_.fields.begin(), schema_.fields.end(),
                    [&](const Field& f) { return f.name == name; });
                if (it == schema_.fields.end())
                    throw std::out_of_range("scan: no column named " + name);
                pick.push_back(
                    static_cast<std::size_t>(it - schema_.fields.begin()));
            }
            r.cursor = std::make_unique<ProjectCursor>(std::move(reader),
                                                       std::move(pick));
        }
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::shared_ptr<spill::Spool> spool_;
    Schema schema_;
};

// compare_agg over two plans whose columns are only known once they run: both
// sides are spooled (and spill past the budget), their names and types are read
// off the spooled morsels, and the typed plan of compose_compare_agg runs over
// the two spools.
class CompareAggCursor : public Cursor {
   public:
    CompareAggCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                     LazyFrame other, std::int64_t n_key, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          other_(std::move(other)),
          n_key_(n_key),
          budget_(budget) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        auto df = co_await gen_->next();
        if (!df) co_return std::nullopt;
        out_names_ = df->names;
        co_return morsel_of(std::move(*df));
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    static std::uint64_t share_of(std::uint64_t budget) {
        return budget == 0 || budget == NO_SPILL_BUDGET ? NO_SPILL_BUDGET
                                                        : budget / share::SPOOL;
    }

    // The type of a column is the first one a morsel gives it that is known:
    // a column that is null so far has the type Unknown.
    static void note_types(std::vector<DataType>& types,
                           const std::vector<Series>& cols) {
        if (types.empty()) types.resize(cols.size());
        for (std::size_t i = 0; i < cols.size() && i < types.size(); ++i)
            if (types[i].id == TypeId::Unknown) types[i] = cols[i].data_type();
    }

    static std::vector<Field> fields_of(const std::vector<std::string>& names,
                                        const std::vector<DataType>& types) {
        if (!types.empty() && types.size() != names.size())
            throw std::runtime_error(
                "compare_agg: a plan names " + std::to_string(names.size()) +
                " columns and returns " + std::to_string(types.size()));
        std::vector<Field> out;
        for (std::size_t i = 0; i < names.size(); ++i)
            out.push_back(
                Field{names[i], types.empty() ? DataType{} : types[i], true});
        return out;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        auto lspool = std::make_shared<spill::Spool>(share_of(budget_));
        auto rspool = std::make_shared<spill::Spool>(share_of(budget_));
        std::vector<std::string> lnames = sch_;
        std::vector<DataType> ltypes;
        while (auto m = co_await in_->next(max_rows)) {
            note_types(ltypes, m->columns);
            lspool->add(std::move(m->columns), m->rows);
        }
        if (auto produced = in_->out_names(); produced && !produced->empty())
            lnames = std::move(*produced);
        in_.reset();

        const LazyFrame& other = other_;
        std::vector<std::string> rnames;
        std::vector<DataType> rtypes;
        auto gen = other.stream(max_rows);
        while (auto df = co_await gen.next()) {
            if (rtypes.empty()) rnames = df->names;
            note_types(rtypes, df->columns);
            std::vector<Series> cols;
            for (const Series& c : df->columns)
                cols.push_back(c.encoding() == Encoding::Flat
                                   ? c.share()
                                   : c.materialize());
            rspool->add(std::move(cols), df->num_rows());
        }

        const std::vector<Field> lf = fields_of(lnames, ltypes);
        const std::vector<Field> rf = fields_of(rnames, rtypes);
        const LazyFrame left =
            LazyFrame::scan(std::make_shared<SpoolSource>(lspool, Schema{lf}));
        const LazyFrame right =
            LazyFrame::scan(std::make_shared<SpoolSource>(rspool, Schema{rf}));
        LazyFrame composed = compose_compare_agg(
            left, right, lf, rf, static_cast<std::size_t>(n_key_));
        if (budget_ != 0 && budget_ != NO_SPILL_BUDGET)
            composed = composed.memory_budget(budget_);
        plan_.emplace(std::move(composed));
        gen_.emplace(plan_->stream(max_rows));
        built_ = true;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    LazyFrame other_;
    std::int64_t n_key_;
    std::uint64_t budget_;
    std::optional<LazyFrame> plan_;
    std::optional<coro::AsyncGenerator<DataFrame>> gen_;
    std::optional<std::vector<std::string>> out_names_;
    bool built_ = false;
};

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_compare_agg(std::unique_ptr<Cursor> in,
                                         std::vector<std::string> sch,
                                         LazyFrame other, std::int64_t n_key,
                                         std::uint64_t budget) {
    return std::make_unique<CompareAggCursor>(std::move(in), std::move(sch),
                                              std::move(other), n_key, budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
