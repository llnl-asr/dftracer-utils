// SPDX-License-Identifier: MIT
// LazyFrame::sink and the plan it runs on: one Source that writes every frame
// the plan below it streams to a registered sink, then yields one row of the
// totals. The duql executor wraps a pipeline the same way, so a sink has one
// implementation whichever surface asks for it.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/internal/sink_plan.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

// The registered sink names, for the error a missing one gives.
dstd::String sink_choices() {
    const dstd::Vector<dstd::String> names = registered_sink_names();
    if (names.empty()) return dstd::String("none are registered");
    dstd::String out;
    dstd::fmt_append_join<", ">(out, names);
    return out;
}

// One bound output. The writer is released here when the run stops before
// finalize - an error, or a cancelled task - so the plugin sees abort() once
// and never alongside finalize().
struct Output {
    dstd::UniquePtr<SinkWriter> writer;

    Output() = default;
    Output(Output&&) = default;
    Output& operator=(Output&&) = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    ~Output() {
        if (writer) writer->abort();
    }
};

// Writes what the plan below streams, then the one summary row. The whole run
// happens inside the first next(), because a sink has no rows to hand out
// until the output is finished. The totals are this cursor's own: a plan is
// re-runnable, and a second run of it writes the frames again, so it must not
// add to what the first run counted.
class SinkRows final : public Cursor {
   public:
    SinkRows(LazyFrame inner, std::int64_t batch_size, Output output,
             dstd::SharedPtr<SinkSummary> counts)
        : inner_(std::move(inner)),
          batch_size_(batch_size),
          output_(std::move(output)),
          counts_(std::move(counts)) {}

    coro::CoroTask<MorselResult> next(std::int64_t max_rows) override {
        if (!summary_) {
            auto draining = drain();
            auto drained = co_await std::move(draining);
            if (!drained) co_return drained.error();
        }
        auto next_task = summary_->next(max_rows);
        co_return co_await std::move(next_task);
    }

   private:
    coro::CoroTask<dstd::Result<void>> drain() {
        auto stream = inner_.stream(batch_size_);
        for (;;) {
            auto pulling = stream.next();
            auto frame = co_await std::move(pulling);
            if (!frame) break;
            if (!*frame) co_return (*frame).error();
            dstd::Result<void> wrote = output_.writer->write(**frame);
            if (!wrote) co_return wrote.error();
            ++totals_.batches;
            totals_.rows += (*frame)->num_rows();
        }
        dstd::Result<void> done = output_.writer->finalize();
        // Released either way: after finalize the handle is gone, so abort()
        // must not run on it.
        output_.writer.reset();
        if (!done) co_return done.error();
        if (counts_) *counts_ = totals_;

        DataFrame summary;
        summary.names.emplace_back("rows");
        summary.names.emplace_back("batches");
        summary.columns.push_back(Series::flat_i64(&totals_.rows, 1));
        summary.columns.push_back(Series::flat_i64(&totals_.batches, 1));
        auto scanned = InMemorySource(std::move(summary)).scan({});
        if (!scanned) co_return scanned.error();
        summary_ = std::move(scanned->cursor);
        co_return {};
    }

    LazyFrame inner_;
    std::int64_t batch_size_;
    Output output_;
    dstd::SharedPtr<SinkSummary> counts_;
    SinkSummary totals_{};
    dstd::UniquePtr<Cursor> summary_;
};

// The plan a sink runs over: one row of `rows` and `batches`, bound to the
// sink when it is scanned and finalized after the last frame.
class SinkSource final : public Source {
   public:
    SinkSource(LazyFrame inner, dstd::SharedPtr<SinkFactory> sink,
               dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options,
               std::int64_t batch_size, dstd::SharedPtr<SinkSummary> counts)
        : inner_(std::move(inner)),
          sink_(std::move(sink)),
          options_(std::move(options)),
          batch_size_(batch_size),
          counts_(std::move(counts)) {}

    Schema schema() const override {
        Schema s;
        s.fields.push_back(
            Field{dstd::String("rows"), scalar(TypeId::Int64), false});
        s.fields.push_back(
            Field{dstd::String("batches"), scalar(TypeId::Int64), false});
        return s;
    }

    dstd::Result<ScanResult> scan(const ScanRequest& req) const override {
        dstd::Vector<dstd::String> names;
        dstd::Vector<TypeId> types;
        const Schema out = inner_.output_schema();
        names.reserve(out.fields.size());
        types.reserve(out.fields.size());
        for (const Field& field : out.fields) {
            names.push_back(field.name);
            types.push_back(field.type.id);
        }
        auto bound = sink_->bind(names, types, options_);
        if (!bound) return bound.error();
        Output output;
        output.writer = std::move(*bound);
        ScanResult r;
        r.filters.assign(req.filters.size(), Pushed::No);
        r.cursor = dstd::make_unique<SinkRows>(inner_, batch_size_,
                                               std::move(output), counts_);
        return r;
    }

   private:
    LazyFrame inner_;
    dstd::SharedPtr<SinkFactory> sink_;
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options_;
    std::int64_t batch_size_;
    dstd::SharedPtr<SinkSummary> counts_;
};

}  // namespace

dstd::Result<LazyFrame> sink_plan(
    const LazyFrame& plan, dstd::StringView name,
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options,
    std::int64_t batch_size, dstd::SharedPtr<SinkSummary> counts) {
    dstd::SharedPtr<SinkFactory> sink = find_sink(name);
    if (!sink)
        return common::make_error<"no sink named '{}'; registered sinks: {}">(
            common::ErrorCode::NotFound, name, sink_choices());
    if (!counts) counts = dstd::make_shared<SinkSummary>();
    return LazyFrame::scan(
        dstd::make_shared<SinkSource>(plan, std::move(sink), std::move(options),
                                      batch_size, std::move(counts)));
}

coro::CoroTask<dstd::Result<SinkSummary>> LazyFrame::sink(
    dstd::StringView name,
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options,
    std::int64_t batch_size) const {
    if (error_) co_return *error_;
    auto counts = dstd::make_shared<SinkSummary>();
    auto wrapped =
        sink_plan(*this, name, std::move(options), batch_size, counts);
    if (!wrapped) co_return wrapped.error();
    LazyFrame plan = std::move(*wrapped);
    auto collecting = plan.collect();
    auto collected = co_await std::move(collecting);
    if (!collected) co_return collected.error();
    co_return *counts;
}

}  // namespace dftracer::utils::dataframe
