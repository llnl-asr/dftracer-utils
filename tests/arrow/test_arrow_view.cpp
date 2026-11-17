// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/arrow/ipc.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

/// A VIEW column over two blobs: row 0 is inline, rows 1 and 2 out of line.
/// The IPC writer must write the values, not the 16-byte views.
struct ViewFixture {
    dstd::String blob_a = "a value longer than twelve";
    dstd::String blob_b = "zz another long value here";
    std::uint8_t views[3 * 16] = {};
    std::uint8_t validity = 0b0000'0111;
    const void* bufs[2] = {blob_a.data(), blob_b.data()};
    std::int64_t sizes[2] = {static_cast<std::int64_t>(blob_a.size()),
                             static_cast<std::int64_t>(blob_b.size())};

    static void put(std::uint8_t* v, dstd::StringView s, std::int32_t index,
                    std::int32_t offset) {
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

    ViewFixture() {
        put(views, "short", 0, 0);
        put(views + 16, blob_a, 0, 0);
        put(views + 32, blob_b, 1, 0);
    }

    Series make() const {
        return Series{dftu_series_new_string_view(DFTU_TYPE_STRING, views, 3,
                                                  &validity, bufs, sizes, 2,
                                                  nullptr, nullptr)};
    }
};

}  // namespace

TEST_CASE("IPC writes a view column as values") {
    ViewFixture f;
    DataFrame frame;
    frame.names = dstd::make_shared_strings({"s"});
    frame.columns.push_back(f.make());
    REQUIRE(frame.columns[0].valid());

    arrow::WriterOptions options;
    options.compression = arrow::Compression::None;
    arrow::Writer writer = arrow::Writer::to_buffer(options);
    REQUIRE(writer.write(frame));
    REQUIRE(writer.finish());
    const dstd::Vector<std::uint8_t>& bytes = writer.take_bytes();
    const dstd::String text(bytes.begin(), bytes.end());
    CHECK(text.find("a value longer than twelve") != dstd::String::npos);
    CHECK(text.find("zz another long value here") != dstd::String::npos);
}
