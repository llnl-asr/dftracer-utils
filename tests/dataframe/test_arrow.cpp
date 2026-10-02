#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <memory>

using dftracer::utils::dataframe::Buffer;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::OwnedArrow;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

constexpr const char* VIEW_LONG_A = "a string longer than twelve bytes";
constexpr const char* VIEW_LONG_B = "another quite long string value";

struct ViewFixture {
    std::uint8_t views[4 * 16] = {};
    std::uint8_t validity = 0b0111;
    std::string blob_a = VIEW_LONG_A;
    std::string blob_b = VIEW_LONG_B;
    const void* bufs[2] = {blob_a.data(), blob_b.data()};
    std::int64_t sizes[2] = {static_cast<std::int64_t>(blob_a.size()),
                             static_cast<std::int64_t>(blob_b.size())};
    int released = 0;

    static void release(void* ctx) {
        ++static_cast<ViewFixture*>(ctx)->released;
    }

    void put(int row, const std::string& s, std::int32_t index,
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
            OwnedArrow a = s.to_arrow();
            CHECK(std::string(a.schema()->format) == "vu");
            CHECK(a.array()->n_buffers == 5);
            CHECK(a.array()->buffers[1] == s.handle()->data->data());
            CHECK(a.array()->buffers[2] == f.blob_a.data());
            CHECK(a.array()->buffers[3] == f.blob_b.data());
            const auto* sizes =
                static_cast<const std::int64_t*>(a.array()->buffers[4]);
            CHECK(sizes[0] == f.sizes[0]);
            CHECK(sizes[1] == f.sizes[1]);

            Series back = Series::from_arrow(a.schema(), a.array());
            REQUIRE(back.valid());
            CHECK(back.handle()->encoding == Encoding::View);
            CHECK(back.handle()->data->data() == s.handle()->data->data());
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
        OwnedArrow a = s.to_arrow();
        CHECK(std::string(a.schema()->format) == "vz");
        Series back = Series::from_arrow(a.schema(), a.array());
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
            Series back = Series::from_arrow(&schema, &arr);
            REQUIRE(back.valid());
            CHECK(arr.release == nullptr);
            CHECK(back.handle()->data->data() == f.views + 16);
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
        col->data = Buffer::allocate(16);
        const std::int32_t codes[4] = {2, 1, 1, 0};
        std::memcpy(col->data->data(), codes, 16);
        col->set_child(std::shared_ptr<dftu_series>(values.release()));
        {
            Series dict{col};
            OwnedArrow a = dict.to_arrow();
            CHECK(std::string(a.schema()->dictionary->format) == "vu");
            CHECK(a.array()->dictionary->n_buffers == 5);
            Series back = Series::from_arrow(a.schema(), a.array());
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
        std::vector<std::int64_t> vals = {10, 20, 30, 40};
        Series s = Series::flat_i64(vals.data(),
                                    static_cast<std::int64_t>(vals.size()));

        OwnedArrow a = s.to_arrow();
        CHECK(static_cast<bool>(a));

        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 4);
        const std::int64_t* d = back.data<std::int64_t>();
        REQUIRE(d != nullptr);
        for (std::size_t i = 0; i < vals.size(); ++i) CHECK(d[i] == vals[i]);
    }

    TEST_CASE("String Series round-trips through the public Arrow bridge") {
        Series s = Series::strings({"alpha", "beta", "gamma"});

        OwnedArrow a = s.to_arrow();
        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 3);
        CHECK(back.string_at(0) == "alpha");
        CHECK(back.string_at(1) == "beta");
        CHECK(back.string_at(2) == "gamma");
    }

    TEST_CASE("DataFrame round-trips schema, columns, and values") {
        std::vector<std::int64_t> ints = {1, 2, 3};
        std::vector<double> reals = {1.5, 2.5, 3.5};

        DataFrame df;
        df.names = {"id", "value"};
        df.columns.push_back(Series::flat_i64(
            ints.data(), static_cast<std::int64_t>(ints.size())));
        df.columns.push_back(Series::flat_f64(
            reals.data(), static_cast<std::int64_t>(reals.size())));

        OwnedArrow a = df.to_arrow();
        CHECK(static_cast<bool>(a));

        DataFrame back = DataFrame::from_arrow(a.schema(), a.array());
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

        OwnedArrow a = dict.to_arrow();
        CHECK(static_cast<bool>(a));

        Series back = Series::from_arrow(a.schema(), a.array());
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

        OwnedArrow a = dict.to_arrow();
        CHECK(static_cast<bool>(a));

        // Slice off the first 2 rows via the Arrow offset, keeping the same
        // index/dictionary buffers: import must honor the index-buffer offset.
        a.array()->offset = 2;
        a.array()->length = 4;

        Series back = Series::from_arrow(a.schema(), a.array());
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
        std::vector<std::int64_t> a = {10, 20, 30};
        std::vector<double> b = {1.5, 2.5, 3.5};
        std::vector<Series> fields;
        fields.push_back(Series::flat_i64(a.data(), 3));
        fields.push_back(Series::flat_f64(b.data(), 3));
        Series st = Series::structs({"a", "b"}, std::move(fields));

        DataFrame df;
        df.names = {"s"};
        df.columns.push_back(std::move(st));

        OwnedArrow arw = df.to_arrow();
        DataFrame back = DataFrame::from_arrow(arw.schema(), arw.array());
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
        std::vector<std::string_view> v = {"a", "", "c", "d"};
        const std::uint8_t valid = 0b1101;
        Series s = Series::strings(v, &valid);
        OwnedArrow a = s.to_arrow();
        a.array()->offset = 1;
        a.array()->length = 2;
        a.array()->null_count = 1;
        Series back = Series::from_arrow(a.schema(), a.array());
        REQUIRE(back.valid());
        REQUIRE(back.length() == 2);
        CHECK(back.is_null(0));
        CHECK_FALSE(back.is_null(1));
        CHECK(back.string_at(1) == "c");
    }

    TEST_CASE("Importing a sliced bool array keeps the slice's bits") {
        std::vector<std::uint8_t> bits(3, 0);
        for (int i = 0; i < 20; ++i)
            if (i % 4 == 0)
                bits[i / 8] |= static_cast<std::uint8_t>(1 << (i % 8));
        Series s = Series::flat(TypeId::Bool, bits.data(), 20);
        OwnedArrow a = s.to_arrow();
        a.array()->offset = 3;
        a.array()->length = 10;
        Series back = Series::from_arrow(a.schema(), a.array());
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
        OwnedArrow src = base.filter(mask).to_arrow();
        REQUIRE(src.array()->dictionary != nullptr);
        auto* sel = static_cast<std::int64_t*>(
            const_cast<void*>(src.array()->buffers[1]));
        sel[1] = -1;
        Series filled = Series::from_arrow(src.schema(), src.array());
        REQUIRE(filled.valid());

        OwnedArrow a = filled.to_arrow();
        REQUIRE(a.array()->dictionary != nullptr);
        const auto* idx =
            static_cast<const std::int64_t*>(a.array()->buffers[1]);
        const std::int64_t dict_len = a.array()->dictionary->length;
        for (int i = 0; i < 2; ++i) {
            CHECK(idx[i] >= 0);
            CHECK(idx[i] < dict_len);
        }
        CHECK(a.array()->null_count == 1);
        Series back = Series::from_arrow(a.schema(), a.array());
        CHECK(back.string_at(0) == "x");
        CHECK(back.is_null(1));
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    TEST_CASE("to_ipc writes a dictionary column") {
        DataFrame df;
        df.names = {"s"};
        df.columns.push_back(
            Series::strings({"a", "b", "a", "a"}).dictionary_encode());
        std::vector<std::uint8_t> bytes;
        CHECK_NOTHROW(bytes = df.to_ipc());
        CHECK_FALSE(bytes.empty());
    }
#endif
}
#else
TEST_SUITE("dataframe_arrow_public") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
