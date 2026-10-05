// The size and alignment budgets of the structs that carry type- or
// function-specific parameters (openspec memory-layout). A change that grows
// one past its budget raises the budget and says why in its design.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <string>

namespace df = dftracer::utils::dataframe;

namespace {

template <class T, std::size_t SIZE, std::size_t ALIGN>
constexpr bool within() {
    static_assert(sizeof(T) <= SIZE, "struct grew past its size budget");
    static_assert(alignof(T) <= ALIGN, "struct grew past its alignment");
    return true;
}

constexpr std::size_t SERIES_BUDGET = 120;
constexpr std::size_t DATA_TYPE_BUDGET = 40;
// std::string is 24 bytes in libc++ and 32 in libstdc++, so a budget counts
// its strings by sizeof and stays tight on both.
constexpr std::size_t STR = sizeof(std::string);
constexpr std::size_t FIELD_BUDGET = 48 + 1 * STR;
constexpr std::size_t MORSEL_BUDGET = 56;
constexpr std::size_t AGG_SPEC_BUDGET = 24 + 1 * STR;
constexpr std::size_t WINDOW_COLUMN_BUDGET = 48 + 3 * STR;
constexpr std::size_t FOLD_EVENT_BUDGET = 152;
constexpr std::size_t LOOKUP_BUDGET = 112 + 2 * STR;

static_assert(within<dftu_series, SERIES_BUDGET, 8>());
static_assert(within<df::DataType, DATA_TYPE_BUDGET, 8>());
static_assert(within<df::Field, FIELD_BUDGET, 8>());
static_assert(within<df::Morsel, MORSEL_BUDGET, 8>());
static_assert(within<df::AggSpec, AGG_SPEC_BUDGET, 8>());
static_assert(within<df::WindowColumn, WINDOW_COLUMN_BUDGET, 8>());
static_assert(within<dftracer::utils::trace::views::detail::FoldEvent,
                     FOLD_EVENT_BUDGET, 8>());
static_assert(within<dftracer::utils::duql::TLookup, LOOKUP_BUDGET, 8>());

}  // namespace

TEST_CASE("the layout budgets hold") {
    CHECK(sizeof(dftu_series) <= SERIES_BUDGET);
    CHECK(sizeof(df::DataType) <= DATA_TYPE_BUDGET);
    CHECK(sizeof(df::Field) <= FIELD_BUDGET);
    CHECK(sizeof(df::Morsel) <= MORSEL_BUDGET);
    CHECK(sizeof(df::AggSpec) <= AGG_SPEC_BUDGET);
    CHECK(sizeof(df::WindowColumn) <= WINDOW_COLUMN_BUDGET);
    CHECK(sizeof(dftracer::utils::trace::views::detail::FoldEvent) <=
          FOLD_EVENT_BUDGET);
    CHECK(sizeof(dftracer::utils::duql::TLookup) <= LOOKUP_BUDGET);
}
