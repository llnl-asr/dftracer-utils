// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <arrow/arrow_helpers.h>
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr const char* VIEW_LONG_A = "a string longer than twelve bytes";
constexpr const char* VIEW_LONG_B = "another quite long string value";

struct ViewFixture {
    std::uint8_t views[4 * 16] = {};
    std::uint8_t validity = 0b0111;
    dstd::String blob_a = VIEW_LONG_A;
    dstd::String blob_b = VIEW_LONG_B;
    const void* bufs[2] = {blob_a.data(), blob_b.data()};
    std::int64_t sizes[2] = {static_cast<std::int64_t>(blob_a.size()),
                             static_cast<std::int64_t>(blob_b.size())};
    int released = 0;

    static void release(void* ctx) {
        ++static_cast<ViewFixture*>(ctx)->released;
    }

    void put(int row, const dstd::String& s, std::int32_t index,
             std::int32_t offset) {
        std::uint8_t* v = views + row * 16;
        const auto size = static_cast<std::int32_t>(s.size());
        std::memset(v, 0, 16);
        std::memcpy(v, &size, 4);
        if (size <= 12) {
            std::memcpy(v + 4, s.data(), s.size());
        } else {
            std::memcpy(v + 4, s.data(), 4);
            std::memcpy(v + 8, &index, 4);
            std::memcpy(v + 12, &offset, 4);
        }
    }

    // rows: "short", VIEW_LONG_A, VIEW_LONG_B, null
    ViewFixture() {
        put(0, "short", 0, 0);
        put(1, blob_a, 0, 0);
        put(2, blob_b, 1, 0);
    }

    Series make(TypeId type = TypeId::String) {
        return Series{dftu_series_new_string_view(static_cast<dftu_dtype>(type),
                                                  views, 4, &validity, bufs,
                                                  sizes, 2, release, this)};
    }
};

}  // namespace

TEST_SUITE("dataframe_arrow_public") {
    TEST_CASE("View column exports as vu with shared buffers and round-trips") {
        ViewFixture f;
        {
            Series s = f.make();
            REQUIRE(s.valid());
            arrow::Batch a = test_types::exported(s);
            CHECK(dstd::String(a.schema()->format) == "vu");
            CHECK(test_types::mutable_array(a)->n_buffers == 5);
            CHECK(test_types::mutable_array(a)->buffers[1] ==
                  s.handle()->data.data());
            CHECK(test_types::mutable_array(a)->buffers[2] == f.blob_a.data());
            CHECK(test_types::mutable_array(a)->buffers[3] == f.blob_b.data());
            const auto* sizes = static_cast<const std::int64_t*>(
                test_types::mutable_array(a)->buffers[4]);
            CHECK(sizes[0] == f.sizes[0]);
            CHECK(sizes[1] == f.sizes[1]);

            Series back = test_types::import_series(a.schema(), a.array());
            REQUIRE(back.valid());
            CHECK(back.handle()->encoding == Encoding::View);
            CHECK(back.handle()->data.data() == s.handle()->data.data());
            REQUIRE(back.length() == 4);
            CHECK(back.string_at(0) == "short");
            CHECK(back.string_at(1) == VIEW_LONG_A);
            CHECK(back.string_at(2) == VIEW_LONG_B);
            CHECK(back.is_null(3));
        }
        CHECK(f.released == 1);
    }

    TEST_CASE("Binary view column exports as vz") {
        ViewFixture f;
        Series s = f.make(TypeId::Binary);
        REQUIRE(s.valid());
        arrow::Batch a = test_types::exported(s);
        CHECK(dstd::String(a.schema()->format) == "vz");
        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        CHECK(back.type() == TypeId::Binary);
        CHECK(back.string_at(2) == VIEW_LONG_B);
    }

    TEST_CASE("Importing a sliced view array shifts the views buffer") {
        ViewFixture f;
        int arrow_released = 0;
        ArrowSchema schema;
        REQUIRE(ArrowSchemaInitFromType(&schema, NANOARROW_TYPE_STRING_VIEW) ==
                NANOARROW_OK);
        struct Hand {
            const void* buffers[5];
            int* released;
        } hand{
            {&f.validity, f.views, f.blob_a.data(), f.blob_b.data(), f.sizes},
            &arrow_released};
        ArrowArray arr;
        std::memset(&arr, 0, sizeof(arr));
        arr.length = 3;
        arr.offset = 1;
        arr.null_count = -1;
        arr.n_buffers = 5;
        arr.buffers = hand.buffers;
        arr.private_data = &hand;
        arr.release = [](ArrowArray* a) {
            ++*static_cast<Hand*>(a->private_data)->released;
            a->release = nullptr;
        };
        {
            Series back = test_types::import_series(&schema, &arr);
            REQUIRE(back.valid());
            CHECK(arr.release == nullptr);
            CHECK(back.handle()->data.data() == f.views + 16);
            REQUIRE(back.length() == 3);
            CHECK(back.string_at(0) == VIEW_LONG_A);
            CHECK(back.string_at(1) == VIEW_LONG_B);
            CHECK(back.is_null(2));
            CHECK(arrow_released == 0);
        }
        CHECK(arrow_released == 1);
        schema.release(&schema);
    }

    TEST_CASE("Dictionary over a view child exports values as vu") {
        ViewFixture f;
        Series values = f.make();
        REQUIRE(values.valid());
        auto* col = new dftu_series();
        col->type = TypeId::String;
        col->encoding = Encoding::Dictionary;
        col->length = 4;
        col->data = new_buffer(16);
        const std::int32_t codes[4] = {2, 1, 1, 0};
        std::memcpy(writable(col->data), codes, 16);
        col->set_child(adopt_series(values.release()));
        {
            Series dict{col};
            arrow::Batch a = test_types::exported(dict);
            CHECK(dstd::String(a.schema()->dictionary->format) == "vu");
            CHECK(test_types::mutable_array(a)->dictionary->n_buffers == 5);
            Series back = test_types::import_series(a.schema(), a.array());
            REQUIRE(back.valid());
            Series flat = back.materialize();
            REQUIRE(flat.length() == 4);
            CHECK(flat.string_at(0) == VIEW_LONG_B);
            CHECK(flat.string_at(1) == VIEW_LONG_A);
            CHECK(flat.string_at(2) == VIEW_LONG_A);
            CHECK(flat.string_at(3) == "short");
        }
        CHECK(f.released == 1);
    }

    TEST_CASE("Series round-trips through the public Arrow bridge") {
        dstd::Vector<std::int64_t> vals = {10, 20, 30, 40};
        Series s = Series::flat_i64(vals.data(),
                                    static_cast<std::int64_t>(vals.size()));

        arrow::Batch a = test_types::exported(s);
        CHECK(a.is_valid());

        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 4);
        const std::int64_t* d = back.data<std::int64_t>();
        REQUIRE(d != nullptr);
        for (std::size_t i = 0; i < vals.size(); ++i) CHECK(d[i] == vals[i]);
    }

    TEST_CASE("String Series round-trips through the public Arrow bridge") {
        Series s = Series::strings({"alpha", "beta", "gamma"});

        arrow::Batch a = test_types::exported(s);
        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "alpha");
        CHECK(back.string_at(1) == "beta");
        CHECK(back.string_at(2) == "gamma");
    }

    TEST_CASE("DataFrame round-trips schema, columns, and values") {
        dstd::Vector<std::int64_t> ints = {1, 2, 3};
        dstd::Vector<double> reals = {1.5, 2.5, 3.5};

        DataFrame df;
        df.names = dstd::make_shared_strings({"id", "value"});
        df.columns.push_back(Series::flat_i64(
            ints.data(), static_cast<std::int64_t>(ints.size())));
        df.columns.push_back(Series::flat_f64(
            reals.data(), static_cast<std::int64_t>(reals.size())));

        arrow::Batch a = test_types::exported(df);
        CHECK(a.is_valid());

        DataFrame back = test_types::import_frame(a.schema(), a.array());
        REQUIRE(back.num_columns() == 2);
        REQUIRE(back.num_rows() == 3);
        REQUIRE(back.names.size() == 2);
        CHECK(back.names[0] == "id");
        CHECK(back.names[1] == "value");

        Series id = back.column("id");
        Series value = back.column("value");
        REQUIRE(id.valid());
        REQUIRE(value.valid());
        const std::int64_t* idp = id.data<std::int64_t>();
        const double* vp = value.data<double>();
        REQUIRE(idp != nullptr);
        REQUIRE(vp != nullptr);
        for (std::size_t i = 0; i < ints.size(); ++i) {
            CHECK(idp[i] == ints[i]);
            CHECK(vp[i] == doctest::Approx(reals[i]));
        }
    }

    TEST_CASE("Dictionary column round-trips through the Arrow bridge") {
        Series s = Series::strings({"a", "b", "a", "c", "b", "a"});
        Series dict = s.dictionary_encode();
        REQUIRE(dict.valid());
        REQUIRE(dict.encoding() == Encoding::Dictionary);

        arrow::Batch a = test_types::exported(dict);
        CHECK(a.is_valid());

        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 6);
        CHECK(back.encoding() == Encoding::Dictionary);

        Series flat = back.materialize();
        REQUIRE(flat.valid());
        REQUIRE(flat.length() == 6);
        const char* expected[] = {"a", "b", "a", "c", "b", "a"};
        for (std::int64_t i = 0; i < 6; ++i)
            CHECK(flat.string_at(i) == expected[i]);
    }

    TEST_CASE("Sliced dictionary array round-trips through the Arrow bridge") {
        Series s = Series::strings({"a", "b", "a", "c", "b", "a"});
        Series dict = s.dictionary_encode();
        REQUIRE(dict.valid());

        arrow::Batch a = test_types::exported(dict);
        CHECK(a.is_valid());

        // Slice off the first 2 rows via the Arrow offset, keeping the same
        // index/dictionary buffers: import must honor the index-buffer offset.
        test_types::mutable_array(a)->offset = 2;
        test_types::mutable_array(a)->length = 4;

        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 4);

        Series flat = back.materialize();
        REQUIRE(flat.valid());
        REQUIRE(flat.length() == 4);
        const char* expected[] = {"a", "c", "b", "a"};
        for (std::int64_t i = 0; i < 4; ++i)
            CHECK(flat.string_at(i) == expected[i]);
    }

    TEST_CASE("Struct column round-trips through the Arrow bridge") {
        dstd::Vector<std::int64_t> a = {10, 20, 30};
        dstd::Vector<double> b = {1.5, 2.5, 3.5};
        dstd::Vector<Series> fields;
        fields.push_back(Series::flat_i64(a.data(), 3));
        fields.push_back(Series::flat_f64(b.data(), 3));
        Series st = Series::structs({"a", "b"}, std::move(fields));

        DataFrame df;
        df.names = dstd::make_shared_strings({"s"});
        df.columns.push_back(std::move(st));

        arrow::Batch arw = test_types::exported(df);
        DataFrame back = test_types::import_frame(arw.schema(), arw.array());
        REQUIRE(back.num_columns() == 1);
        REQUIRE(back.num_rows() == 3);
        Series s = back.column("s");
        REQUIRE(s.valid());
        REQUIRE(s.num_children() == 2);
        const std::int64_t* ap = s.child(0).data<std::int64_t>();
        const double* bp = s.child(1).data<double>();
        REQUIRE(ap != nullptr);
        REQUIRE(bp != nullptr);
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(ap[i] == a[i]);
            CHECK(bp[i] == doctest::Approx(b[i]));
        }
    }

    TEST_CASE("Importing a sliced string array keeps the slice's rows") {
        dstd::Vector<dstd::StringView> v = {"a", "", "c", "d"};
        const std::uint8_t valid = 0b1101;
        Series s = Series::strings(v, &valid);
        arrow::Batch a = test_types::exported(s);
        test_types::mutable_array(a)->offset = 1;
        test_types::mutable_array(a)->length = 2;
        test_types::mutable_array(a)->null_count = 1;
        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 2);
        CHECK(back.is_null(0));
        CHECK_FALSE(back.is_null(1));
        CHECK(back.string_at(1) == "c");
    }

    TEST_CASE("Importing a sliced bool array keeps the slice's bits") {
        dstd::Vector<std::uint8_t> bits(3, 0);
        for (int i = 0; i < 20; ++i)
            if (i % 4 == 0)
                bits[i / 8] |= static_cast<std::uint8_t>(1 << (i % 8));
        Series s = Series::flat(TypeId::Bool, bits.data(), 20);
        arrow::Batch a = test_types::exported(s);
        test_types::mutable_array(a)->offset = 3;
        test_types::mutable_array(a)->length = 10;
        Series back = test_types::import_series(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 10);
        const std::uint8_t* d = back.data<std::uint8_t>();
        for (int i = 0; i < 10; ++i)
            CHECK(((d[i / 8] >> (i % 8)) & 1) == ((i + 3) % 4 == 0 ? 1 : 0));
    }

    TEST_CASE("Exporting outer-fill selection rows gives in-range indices") {
        Series base = Series::strings({"x", "y", "z"});
        std::uint8_t mask_bits = 0b101;
        Series mask = Series::flat(TypeId::Bool, &mask_bits, 3);
        arrow::Batch src = test_types::exported(base.filter(mask));
        REQUIRE(src.array()->dictionary != nullptr);
        auto* sel = static_cast<std::int64_t*>(
            const_cast<void*>(src.array()->buffers[1]));
        sel[1] = -1;
        Series filled = test_types::import_series(src.schema(), src.array());
        REQUIRE(filled.valid());

        arrow::Batch a = test_types::exported(filled);
        REQUIRE(test_types::mutable_array(a)->dictionary != nullptr);
        const auto* idx = static_cast<const std::int64_t*>(
            test_types::mutable_array(a)->buffers[1]);
        const std::int64_t dict_size =
            test_types::mutable_array(a)->dictionary->length;
        for (int i = 0; i < 2; ++i) {
            CHECK(idx[i] >= 0);
            CHECK(idx[i] < dict_size);
        }
        CHECK(test_types::mutable_array(a)->null_count == 1);
        Series back = test_types::import_series(a.schema(), a.array());
        CHECK(back.string_at(0) == "x");
        CHECK(back.is_null(1));
    }

    TEST_CASE("the IPC writer takes a dictionary column") {
        DataFrame df;
        df.names = dstd::make_shared_strings({"s"});
        df.columns.push_back(
            Series::strings({"a", "b", "a", "a"}).dictionary_encode());
        auto bytes = test_types::export_ipc(df);
        CHECK_FALSE(bytes.empty());
    }

    TEST_CASE("an IPC stream round-trips a frame") {
        const std::uint64_t ts[3] = {500, 501, 502};
        DataFrame df;
        df.names = dstd::make_shared_strings({"cat", "ts"});
        df.columns.push_back(Series::strings({"STDIO", "STDIO", "STDIO"}));
        df.columns.push_back(Series::flat(TypeId::Uint64, ts, 3));
        auto bytes = test_types::export_ipc(df);
        DataFrame back = test_types::import_ipc(bytes);
        REQUIRE(back.columns.size() == 2);
        CHECK(back.columns[1].type() == TypeId::Uint64);
        CHECK(back.columns[1].length() == 3);
        CHECK(back.columns[1].data<std::uint64_t>()[2] == 502);
    }
}
