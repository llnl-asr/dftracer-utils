// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

#include <dftracer/utils/python/schema_reconcile.cpp>

using namespace dftracer::utils;
using utilities::arrow::ColumnType;
using utilities::arrow::RecordBatchBuilder;

namespace {

auto int_batch() {
    RecordBatchBuilder b;
    const std::size_t x = b.add_or_get_column("x", ColumnType::Int64);
    b.append_int64(x, 7);
    b.end_row();
    return dftu_utils_test::ok(b.finish());
}

auto double_batch() {
    RecordBatchBuilder b;
    const std::size_t x = b.add_or_get_column("x", ColumnType::Double);
    b.append_double(x, 1.5);
    b.end_row();
    return dftu_utils_test::ok(b.finish());
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
    auto first = int_batch();
    auto second = double_batch();

    python::SchemaReconciler reconciler;
    reconciler.merge(first.schema());
    reconciler.merge(second.schema());
    REQUIRE(reconciler.finalize() == 0);

    ArrowArray same{};
    REQUIRE(reconciler.reconcile(first.schema(), first.array(), &same) == 0);
    {
        View x(NANOARROW_TYPE_INT64, same.children[0]);
        CHECK(ArrowArrayViewGetIntUnsafe(&x.view, 0) == 7);
        View extra(NANOARROW_TYPE_STRING, same.children[1]);
        CHECK(ArrowArrayViewIsNull(&extra.view, 0));
    }
    same.release(&same);

    ArrowArray other{};
    REQUIRE(reconciler.reconcile(second.schema(), second.array(), &other) == 0);
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
