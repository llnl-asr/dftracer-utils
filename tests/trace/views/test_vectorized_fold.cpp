#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/vectorized_fold.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <vector>

using dftracer::utils::StringIntern;
using dftracer::utils::trace::RecordPhase;
namespace df = dftracer::utils::dataframe;
namespace detail = dftracer::utils::trace::views::detail;
using detail::FoldBatch;
using detail::FoldEvent;
using detail::ScanUnit;
using detail::VectorizedFold;

namespace {

FoldEvent ev(StringIntern& in, std::uint64_t pid, std::uint64_t ts,
             std::uint64_t dur) {
    FoldEvent e;
    e.cat_id = in.get_or_insert("POSIX");
    e.name_id = in.get_or_insert("read");
    e.pid = pid;
    e.tid = 1;
    e.ts = ts;
    e.dur = dur;
    e.phase = RecordPhase::COMPLETE;
    e.has_dur = true;
    return e;
}

// Sum a numeric column with a real SIMD reduce over the materialized batch.
std::int64_t col_sum(const df::DataFrame& d, const char* name) {
    for (std::size_t i = 0; i < d.names.size(); ++i)
        if (d.names[i] == name) {
            dftu_scalar s =
                dftu_series_reduce(d.columns[i].handle(), DFTU_REDUCE_SUM);
            return s.kind == DFTU_SCALAR_TAG_U64
                       ? static_cast<std::int64_t>(s.value.u)
                       : s.value.i;
        }
    return -1;
}

struct Acc {
    std::int64_t rows = 0;
    std::int64_t dur_sum = 0;
};

VectorizedFold<Acc> make_fold(StringIntern& in) {
    return VectorizedFold<Acc>(
        in, [] { return Acc{}; },
        [](Acc& a, const df::DataFrame& d) {
            a.rows += d.num_rows();
            a.dur_sum += col_sum(d, "dur");
        },
        [](Acc& into, const Acc& other) {
            into.rows += other.rows;
            into.dur_sum += other.dur_sum;
        });
}

}  // namespace

TEST_SUITE("VectorizedFold") {
    TEST_CASE("a batch is folded as columns, not per event") {
        StringIntern in;
        auto fold = make_fold(in);
        std::vector<FoldEvent> evs = {ev(in, 1, 100, 5), ev(in, 1, 110, 5),
                                      ev(in, 2, 120, 5)};
        ScanUnit unit{};
        fold.step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        CHECK(fold.result().rows == 3);
        CHECK(fold.result().dur_sum == 15);  // SIMD reduce of the dur column
    }

    TEST_CASE("per-worker slices fold independently and merge") {
        StringIntern in;
        auto proto = make_fold(in);
        auto a = proto.slice();  // fresh state, like a worker slot
        auto b = proto.slice();
        std::vector<FoldEvent> ea = {ev(in, 1, 1, 10), ev(in, 1, 2, 10)};
        std::vector<FoldEvent> eb = {ev(in, 2, 3, 7)};
        ScanUnit unit{};
        a->step(FoldBatch{std::span<const FoldEvent>(ea), unit, {}});
        b->step(FoldBatch{std::span<const FoldEvent>(eb), unit, {}});
        a->merge(*b);

        const auto* av = static_cast<VectorizedFold<Acc>*>(a.get());
        CHECK(av->result().rows == 3);
        CHECK(av->result().dur_sum == 27);
    }

    TEST_CASE("a metadata-only batch materializes nothing") {
        StringIntern in;
        auto fold = make_fold(in);
        FoldEvent m = ev(in, 1, 1, 0);
        m.phase = RecordPhase::METADATA;
        std::vector<FoldEvent> evs = {m};
        ScanUnit unit{};
        fold.step(FoldBatch{std::span<const FoldEvent>(evs), unit, {}});
        CHECK(fold.result().rows == 0);
        CHECK(fold.result().dur_sum == 0);
    }
}
