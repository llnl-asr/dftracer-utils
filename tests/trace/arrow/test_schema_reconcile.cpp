// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/schema_reconcile.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

using namespace dftracer::utils;
using namespace dftracer::utils::arrow;
using namespace dftracer::utils::dataframe;

namespace {

/// A one-row, one-column frame named "x": the batch a Python consumer would
/// hand the reconciler.
arrow::Batch one_row(const char* type, std::int64_t i64, double f64) {
    Series column = dstd::StringView(type) == "int64"
                        ? Series::flat_i64(&i64, 1)
                        : Series::flat_f64(&f64, 1);
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"x"});
    frame.columns.push_back(std::move(column));
    return dftu_utils_test::ok(arrow::to_arrow(frame));
}

struct View {
    ArrowArrayView view;
    View(ArrowType type, const ArrowArray* array) {
        ArrowArrayViewInitFromType(&view, type);
        REQUIRE(ArrowArrayViewSetArray(&view, array, nullptr) == NANOARROW_OK);
    }
    ~View() { ArrowArrayViewReset(&view); }
};

}  // namespace

TEST_CASE("a column whose type differs from the locked one goes to _extra") {
    auto first = one_row("int64", 7, 0.0);
    auto second = one_row("float64", 0, 1.5);

    SchemaReconciler reconciler;
    reconciler.merge(first.schema());
    reconciler.merge(second.schema());
    REQUIRE(reconciler.finalize() == 0);

    ArrowArray same{};
    REQUIRE(reconciler.reconcile(first.schema(),
                                 const_cast<ArrowArray*>(first.array()),
                                 &same) == 0);
    {
        View x(NANOARROW_TYPE_INT64, same.children[0]);
        CHECK(ArrowArrayViewGetIntUnsafe(&x.view, 0) == 7);
        View extra(NANOARROW_TYPE_STRING, same.children[1]);
        CHECK(ArrowArrayViewIsNull(&extra.view, 0));
    }
    same.release(&same);

    ArrowArray other{};
    REQUIRE(reconciler.reconcile(second.schema(),
                                 const_cast<ArrowArray*>(second.array()),
                                 &other) == 0);
    {
        View x(NANOARROW_TYPE_INT64, other.children[0]);
        CHECK(ArrowArrayViewIsNull(&x.view, 0));
        View extra(NANOARROW_TYPE_STRING, other.children[1]);
        REQUIRE_FALSE(ArrowArrayViewIsNull(&extra.view, 0));
        const ArrowStringView text =
            ArrowArrayViewGetStringUnsafe(&extra.view, 0);
        CHECK(dstd::StringView(text.data,
                               static_cast<std::size_t>(text.size_bytes)) ==
              "{\"x\":1.5}");
    }
    other.release(&other);
}
