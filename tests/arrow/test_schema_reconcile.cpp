// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/schema_reconcile.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>
#include <task_result.h>

#include <nanoarrow/nanoarrow.hpp>

using namespace dftracer::utils;
using namespace dftracer::utils::arrow;
using namespace dftracer::utils::dataframe;

namespace {

/// A one-row, one-column frame named "x": the batch a Python consumer would
/// hand the reconciler.
arrow::Batch one_row(dstd::StringView type, std::int64_t i64, double f64) {
    Series column =
        type == "int64" ? Series::flat_i64(&i64, 1) : Series::flat_f64(&f64, 1);
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

namespace {

/// A struct frame with one list column "l" over `child`.
arrow::Batch list_batch(Series child) {
    const dstd::Vector<std::int32_t> off{0, 2};
    DataFrame frame;
    frame.names.push_back(dstd::SharedString{"l"});
    frame.columns.push_back(Series::list(off, std::move(child)));
    return dftu_utils_test::ok(arrow::to_arrow(frame));
}

Series two_strings() {
    dstd::Vector<dstd::String> values;
    values.push_back(dstd::String("p"));
    values.push_back(dstd::String("q"));
    return Series::strings(values);
}

/// Merges `locked` and reconciles `other` under it; true when the column "l"
/// of the result is `other`'s, moved in.
bool moves_under(arrow::Batch locked, arrow::Batch other) {
    SchemaReconciler reconciler;
    reconciler.merge(locked.schema());
    REQUIRE(reconciler.finalize() == 0);
    nanoarrow::UniqueSchema out_schema;
    REQUIRE(reconciler.copy_schema(out_schema.get()) == 0);
    ArrowArray out{};
    REQUIRE(reconciler.reconcile(other.schema(),
                                 const_cast<ArrowArray*>(other.array()),
                                 &out) == 0);
    ArrowArrayView view;
    ArrowError error{};
    REQUIRE(ArrowArrayViewInitFromSchema(&view, out_schema.get(), &error) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayViewSetArray(&view, &out, &error) == NANOARROW_OK);
    CHECK(ArrowArrayViewValidate(&view, NANOARROW_VALIDATION_LEVEL_FULL,
                                 &error) == NANOARROW_OK);
    const bool moved = !ArrowArrayViewIsNull(view.children[0], 0);
    ArrowArrayViewReset(&view);
    out.release(&out);
    return moved;
}

}  // namespace

TEST_CASE("a list column whose child layout differs goes to _extra") {
    auto dictionary = [] { return two_strings().dictionary_encode(); };
    CHECK_FALSE(
        moves_under(list_batch(dictionary()), list_batch(two_strings())));
    CHECK_FALSE(
        moves_under(list_batch(two_strings()), list_batch(dictionary())));
    CHECK(moves_under(list_batch(dictionary()), list_batch(dictionary())));
    CHECK(moves_under(list_batch(two_strings()), list_batch(two_strings())));
}

namespace {

/// A dictionary<int32, utf8> schema named `name`.
void dictionary_child(ArrowSchema* s, dstd::CStringView name) {
    // A fixed size list already made its child; free it before the new one.
    if (s->release != nullptr) s->release(s);
    REQUIRE(ArrowSchemaInitFromType(s, NANOARROW_TYPE_INT32) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(s, name.c_str()) == NANOARROW_OK);
    REQUIRE(ArrowSchemaAllocateDictionary(s) == NANOARROW_OK);
    REQUIRE(ArrowSchemaInitFromType(s->dictionary, NANOARROW_TYPE_STRING) ==
            NANOARROW_OK);
}

}  // namespace

TEST_CASE("a missing struct or fixed size list of dictionaries fills valid") {
    // Locked: x int64, s struct<a: dict>, f fixed_size_list[2]<dict>.
    nanoarrow::UniqueSchema locked;
    ArrowSchemaInit(locked.get());
    REQUIRE(ArrowSchemaSetTypeStruct(locked.get(), 3) == NANOARROW_OK);
    REQUIRE(ArrowSchemaInitFromType(locked->children[0],
                                    NANOARROW_TYPE_INT64) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(locked->children[0], "x") == NANOARROW_OK);
    ArrowSchema* s = locked->children[1];
    ArrowSchemaInit(s);
    REQUIRE(ArrowSchemaSetTypeStruct(s, 1) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(s, "s") == NANOARROW_OK);
    dictionary_child(s->children[0], "a");
    ArrowSchema* f = locked->children[2];
    ArrowSchemaInit(f);
    REQUIRE(ArrowSchemaSetTypeFixedSize(f, NANOARROW_TYPE_FIXED_SIZE_LIST, 2) ==
            NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(f, "f") == NANOARROW_OK);
    dictionary_child(f->children[0], "item");

    SchemaReconciler reconciler;
    reconciler.merge(locked.get());
    REQUIRE(reconciler.finalize() == 0);
    nanoarrow::UniqueSchema out_schema;
    REQUIRE(reconciler.copy_schema(out_schema.get()) == 0);

    arrow::Batch other = one_row("int64", 7, 0.0);
    ArrowArray out{};
    REQUIRE(reconciler.reconcile(other.schema(),
                                 const_cast<ArrowArray*>(other.array()),
                                 &out) == 0);
    ArrowArrayView view;
    ArrowError error{};
    REQUIRE(ArrowArrayViewInitFromSchema(&view, out_schema.get(), &error) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayViewSetArray(&view, &out, &error) == NANOARROW_OK);
    CHECK(ArrowArrayViewValidate(&view, NANOARROW_VALIDATION_LEVEL_FULL,
                                 &error) == NANOARROW_OK);
    CHECK(ArrowArrayViewGetIntUnsafe(view.children[0], 0) == 7);
    CHECK(ArrowArrayViewIsNull(view.children[1], 0));
    CHECK(ArrowArrayViewIsNull(view.children[2], 0));
    CHECK(dstd::StringView(out_schema->children[1]->name) == "s");
    CHECK(dstd::StringView(out_schema->children[1]->children[0]->name) == "a");
    CHECK(dstd::StringView(out_schema->children[2]->name) == "f");
    // Each code under a null parent is in range of its dictionary.
    const ArrowArray* a = out.children[1]->children[0];
    const ArrowArray* item = out.children[2]->children[0];
    CHECK(a->length == 1);
    CHECK(a->dictionary->length >= 1);
    CHECK(item->length == 2);
    CHECK(item->dictionary->length >= 1);
    ArrowArrayViewReset(&view);
    out.release(&out);
}
