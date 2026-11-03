// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_TYPED_COLLECT_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_TYPED_COLLECT_FOLD_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views::detail {

/// The raw-scan fallback for collect_typed: split events by phase into the
/// three TypedResult families (regular=COMPLETE, aggregated=AGGREGATED,
/// counters= COUNTER; metadata dropped) as per-event rows
/// (ts/dur/pid/tid/name/cat/ph), so a query the tier cannot key on still
/// returns rows. `intern` must be the table fuse extracts events with, and must
/// outlive the build_* calls.
class TypedCollectFold : public Fold {
   public:
    explicit TypedCollectFold(const dftracer::utils::dstd::StringIntern& intern)
        : intern_(&intern) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return false; }
    bool wants_metadata() const override { return false; }

    dstd::UniquePtr<Fold> slice() const override {
        return dstd::make_unique<TypedCollectFold>(*intern_);
    }

    void step(const FoldBatch& batch) override {
        for (const auto& ev : batch.events) {
            switch (ev.phase) {
                case RecordPhase::Complete:
                    regular_.push(ev);
                    break;
                case RecordPhase::Aggregated:
                    aggregated_.push(ev);
                    break;
                case RecordPhase::Counter:
                    counters_.push(ev);
                    break;
                case RecordPhase::Unknown:
                case RecordPhase::Metadata:
                    break;
            }
        }
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    void merge(Fold& other) override {
        auto& o = static_cast<TypedCollectFold&>(other);
        regular_.append(std::move(o.regular_));
        aggregated_.append(std::move(o.aggregated_));
        counters_.append(std::move(o.counters_));
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    // Drain each family once: to_frame moves the numeric columns into the
    // result with no copy.
    dftracer::utils::dataframe::DataFrame build_regular() {
        return std::move(regular_).to_frame(*intern_);
    }
    dftracer::utils::dataframe::DataFrame build_aggregated() {
        return std::move(aggregated_).to_frame(*intern_);
    }
    dftracer::utils::dataframe::DataFrame build_counters() {
        return std::move(counters_).to_frame(*intern_);
    }

   private:
    // String ids are held raw and resolved once in to_frame, after the merge,
    // so step() touches no shared intern.
    struct Cols {
        dstd::Vector<std::uint64_t> ts, dur, pid, tid;
        dstd::Vector<std::uint32_t> name_id, cat_id;
        dstd::Vector<std::int64_t> ph;
        dstd::Vector<std::uint8_t> has_dur;

        void push(const FoldEvent& ev) {
            ts.push_back(ev.ts);
            dur.push_back(ev.dur);
            has_dur.push_back(ev.has_dur ? 1 : 0);
            pid.push_back(ev.pid);
            tid.push_back(ev.tid);
            name_id.push_back(ev.name_id);
            cat_id.push_back(ev.cat_id);
            ph.push_back(static_cast<std::int64_t>(ev.phase));
        }

        void append(Cols&& o) {
            auto ext = [](auto& dst, auto& src) {
                dst.insert(dst.end(), src.begin(), src.end());
            };
            ext(ts, o.ts);
            ext(dur, o.dur);
            ext(has_dur, o.has_dur);
            ext(pid, o.pid);
            ext(tid, o.tid);
            ext(name_id, o.name_id);
            ext(cat_id, o.cat_id);
            ext(ph, o.ph);
        }

        dftracer::utils::dataframe::DataFrame to_frame(
            const dftracer::utils::dstd::StringIntern& intern) && {
            namespace df = dftracer::utils::dataframe;
            // from_borrowed moves the vector in and borrows its buffer; a
            // vector move keeps that buffer, so data() taken first stays valid.
            // An empty column takes the copy path instead: from_borrowed does
            // not release its owner for a zero-byte buffer, so borrowing would
            // leak.
            auto column = [](auto&& v, df::TypeId type) {
                if (v.empty()) return df::Series::flat(type, nullptr, 0);
                const void* p = v.data();
                return df::Series::from_borrowed(type, p, v.size(),
                                                 std::move(v));
            };
            // A record without a numeric dur has no duration (null row), as
            // a row-frame dur column reports it.
            auto dur_cell_validity = [](const dstd::Vector<std::uint8_t>& has) {
                dstd::Vector<std::uint8_t> v((has.size() + 7) / 8, 0);
                bool any_missing = false;
                for (std::size_t r = 0; r < has.size(); ++r) {
                    if (has[r] != 0)
                        v[r >> 3] |= static_cast<std::uint8_t>(1u << (r & 7));
                    else
                        any_missing = true;
                }
                if (!any_missing) v.clear();
                return v;
            };
            df::DataFrame out;
            out.names.emplace_back("name");
            out.columns.push_back(interned_column(name_id, intern));
            out.names.emplace_back("cat");
            out.columns.push_back(interned_column(cat_id, intern));
            out.names.emplace_back("pid");
            out.columns.push_back(column(std::move(pid), df::TypeId::Uint64));
            out.names.emplace_back("tid");
            out.columns.push_back(column(std::move(tid), df::TypeId::Uint64));
            out.names.emplace_back("ts");
            out.columns.push_back(column(std::move(ts), df::TypeId::Uint64));
            out.names.emplace_back("dur");
            dstd::Vector<std::uint8_t> bits = dur_cell_validity(has_dur);
            if (dur.empty()) {
                out.columns.push_back(
                    df::Series::flat(df::TypeId::Uint64, nullptr, 0));
            } else {
                const void* p = dur.data();
                out.columns.push_back(df::Series::from_borrowed(
                    df::TypeId::Uint64, p, dur.size(), std::move(dur),
                    bits.empty() ? nullptr : bits.data()));
            }
            out.names.emplace_back("ph");
            out.columns.push_back(column(std::move(ph), df::TypeId::Int64));
            return out;
        }
    };

    const dftracer::utils::dstd::StringIntern* intern_;
    Cols regular_, aggregated_, counters_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_TYPED_COLLECT_FOLD_H
