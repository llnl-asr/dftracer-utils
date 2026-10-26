#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/extent_codec.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;

using dftracer::utils::ByteView;
using dftracer::utils::read_u32_le;
using dftracer::utils::write_u32_le;

constexpr std::size_t ALIGN = 4096;
constexpr std::uint32_t PAGE = static_cast<std::uint32_t>(store::PAGE_BYTES);

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

/// Repeated event-like text, which zstd shrinks well.
dstd::Vector<std::uint8_t> text_bytes(std::size_t length) {
    static constexpr char LINE[] =
        "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":7,\"dur\":12}\n";
    dstd::Vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>(LINE[i % (sizeof(LINE) - 1)]);
    }
    return bytes;
}

/// Uniform random bytes, which zstd cannot shrink.
dstd::Vector<std::uint8_t> random_bytes(std::size_t length) {
    std::mt19937_64 rng(42);
    dstd::Vector<std::uint8_t> bytes(length);
    for (std::uint8_t& byte : bytes) byte = static_cast<std::uint8_t>(rng());
    return bytes;
}

dstd::Vector<std::uint32_t> starts_of(std::vector<store::SegmentSpan> spans,
                                      std::uint32_t raw_length) {
    return store::page_starts(dstd::Span<const store::SegmentSpan>(spans),
                              raw_length);
}

/// The offset of the page table entry of page `i` in a stored extent.
std::size_t entry_at(std::size_t i) {
    return store::EXTENT_HEADER_BYTES + i * store::EXTENT_TABLE_ENTRY_BYTES;
}

}  // namespace

TEST_SUITE("store extent codec") {
    TEST_CASE("small segments share one page") {
        const auto starts = starts_of({{0, 100}, {100, 200}, {300, 50}}, 350);
        REQUIRE(starts.size() == 1);
        CHECK(starts[0] == 0);
    }

    TEST_CASE("a segment of exactly one page keeps one page") {
        const auto single = starts_of({{0, PAGE}}, PAGE);
        REQUIRE(single.size() == 1);
        CHECK(single[0] == 0);

        const auto pair = starts_of({{0, PAGE}, {PAGE, PAGE}}, 2 * PAGE);
        REQUIRE(pair.size() == 2);
        CHECK(pair[0] == 0);
        CHECK(pair[1] == PAGE);
    }

    TEST_CASE("a segment over a page starts its own page and is cut inside") {
        const std::uint32_t length = PAGE + 1000;
        const auto starts = starts_of({{0, 100}, {100, length}}, 100 + length);
        REQUIRE(starts.size() == 3);
        CHECK(starts[0] == 0);
        CHECK(starts[1] == 100);
        CHECK(starts[2] == 100 + PAGE);
    }

    TEST_CASE("a gap moves the page to the segment after it") {
        const auto starts = starts_of({{0, 10}, {PAGE, 10}}, PAGE + 10);
        REQUIRE(starts.size() == 2);
        CHECK(starts[0] == 0);
        CHECK(starts[1] == PAGE);
    }

    TEST_CASE("bytes after the last segment are cut every page") {
        const auto starts = starts_of({{0, 10}}, 3 * PAGE);
        REQUIRE(starts.size() == 3);
        CHECK(starts[0] == 0);
        CHECK(starts[1] == PAGE);
        CHECK(starts[2] == 2 * PAGE);
    }

    TEST_CASE("a compressible page is zstd and an incompressible one is raw") {
        dstd::Vector<std::uint8_t> raw = text_bytes(PAGE);
        const dstd::Vector<std::uint8_t> tail = random_bytes(5000);
        raw.insert(raw.end(), tail.begin(), tail.end());
        const std::uint32_t starts[] = {0, PAGE};
        auto stored = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), ALIGN);
        REQUIRE(stored.has_value());
        CHECK(stored->size() % ALIGN == 0);
        CHECK(read_u32_le(stored->data()) == 2);
        CHECK(read_u32_le(stored->data() + 4) == raw.size());

        auto pages = store::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() == 2);
        CHECK(pages->raw_length() == raw.size());
        CHECK(pages->entry(0).codec == store::PageCodec::Zstd);
        CHECK(pages->entry(1).codec == store::PageCodec::None);
        CHECK(pages->entry(0).raw_offset == 0);
        CHECK(pages->entry(0).raw_length == PAGE);
        CHECK(pages->entry(1).raw_offset == PAGE);
        CHECK(pages->entry(1).raw_length == 5000);
        CHECK(pages->entry(1).stored_length == 5000);
        CHECK(pages->entry(0).stored_offset == entry_at(2));
        CHECK(pages->entry(1).stored_offset ==
              pages->entry(0).stored_offset + pages->entry(0).stored_length);

        for (std::size_t i = 0; i < pages->size(); ++i) {
            dstd::Vector<std::uint8_t> page;
            REQUIRE(pages->decode(i, page).has_value());
            const store::PageEntry& entry = pages->entry(i);
            REQUIRE(page.size() == entry.raw_length);
            CHECK(std::equal(page.begin(), page.end(),
                             raw.begin() + entry.raw_offset));
        }

        auto whole = *stored;
        REQUIRE(store::decode_extent(whole).has_value());
        CHECK(whole == raw);
    }

    TEST_CASE("covering names the pages a segment reads") {
        dstd::Vector<std::uint8_t> raw = text_bytes(3 * PAGE);
        const std::uint32_t starts[] = {0, PAGE, 2 * PAGE};
        auto stored = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), ALIGN);
        REQUIRE(stored.has_value());
        auto pages = store::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() == 3);

        auto inside = pages->covering(0, 10);
        CHECK(inside.first == 0);
        CHECK(inside.second == 1);
        auto across = pages->covering(50, PAGE);
        CHECK(across.first == 0);
        CHECK(across.second == 2);
        auto at = pages->covering(2 * PAGE, 1);
        CHECK(at.first == 2);
        CHECK(at.second == 3);
        auto empty = pages->covering(2 * PAGE, 0);
        CHECK(empty.first == empty.second);
    }

    TEST_CASE("an extent that does not decode is a Parse error") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(2 * PAGE);
        const std::uint32_t starts[] = {0, PAGE};
        auto encoded = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), ALIGN);
        REQUIRE(encoded.has_value());
        const dstd::Vector<std::uint8_t>& stored = *encoded;
        REQUIRE(store::ExtentPages::open(view_of(stored)).has_value());

        const auto refuses = [](const dstd::Vector<std::uint8_t>& bad) {
            auto pages = store::ExtentPages::open(view_of(bad));
            if (pages.has_value()) {
                dstd::Vector<std::uint8_t> decoded = bad;
                auto whole = store::decode_extent(decoded);
                return !whole.has_value() &&
                       whole.error().is(dstd::Condition::Parse);
            }
            return pages.error().is(dstd::Condition::Parse);
        };

        SUBCASE("unknown codec") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[entry_at(1) + 20] = 7;
            CHECK(refuses(bad));
        }
        SUBCASE("a reserved byte of the header set") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[9] = 1;
            CHECK(refuses(bad));
        }
        SUBCASE("a reserved byte of an entry set") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[entry_at(0) + 22] = 1;
            CHECK(refuses(bad));
        }
        SUBCASE("a table entry out of bounds") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(0) + 8,
                         static_cast<std::uint32_t>(bad.size()));
            CHECK(refuses(bad));
        }
        SUBCASE("raw ranges that are not contiguous") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(1), PAGE + 1);
            CHECK(refuses(bad));
        }
        SUBCASE("a page crc that does not match") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(0) + 16,
                         read_u32_le(bad.data() + entry_at(0) + 16) ^ 1U);
            CHECK(refuses(bad));
        }
        SUBCASE("a damaged payload") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[entry_at(2)] ^= 0xA5U;
            CHECK(refuses(bad));
        }
        SUBCASE("fewer bytes than a header") {
            dstd::Vector<std::uint8_t> bad(store::EXTENT_HEADER_BYTES - 1, 0);
            CHECK(refuses(bad));
        }
        SUBCASE("a page longer than a page") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(0) + 4, PAGE + 1);
            CHECK(refuses(bad));
        }
    }

    TEST_CASE("encode refuses starts that do not cut the extent into pages") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(60000);
        const std::vector<std::uint32_t> empty;
        CHECK_FALSE(store::encode_extent(view_of(raw),
                                         dstd::Span<const std::uint32_t>(empty),
                                         ALIGN)
                        .has_value());

        const std::uint32_t shifted[] = {1};
        CHECK_FALSE(
            store::encode_extent(
                view_of(raw), dstd::Span<const std::uint32_t>(shifted), ALIGN)
                .has_value());

        const std::uint32_t unordered[] = {PAGE, 0};
        CHECK_FALSE(
            store::encode_extent(
                view_of(raw), dstd::Span<const std::uint32_t>(unordered), ALIGN)
                .has_value());

        const std::uint32_t too_big[] = {0};
        CHECK_FALSE(
            store::encode_extent(
                view_of(raw), dstd::Span<const std::uint32_t>(too_big), ALIGN)
                .has_value());
    }

    TEST_CASE("an empty extent is refused") {
        const dstd::Vector<std::uint8_t> raw;
        const std::uint32_t starts[] = {0};
        CHECK_FALSE(
            store::encode_extent(view_of(raw),
                                 dstd::Span<const std::uint32_t>(starts), ALIGN)
                .has_value());
        CHECK_FALSE(
            store::encode_extent(view_of(raw),
                                 dstd::Span<const std::uint32_t>(starts), 0)
                .has_value());
    }

    TEST_CASE("one page of one page keeps no padding without an alignment") {
        const dstd::Vector<std::uint8_t> raw = random_bytes(100);
        const std::uint32_t starts[] = {0};
        auto stored = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), 1);
        REQUIRE(stored.has_value());
        CHECK(stored->size() == store::EXTENT_HEADER_BYTES +
                                    store::EXTENT_TABLE_ENTRY_BYTES +
                                    raw.size());
        auto decoded = *stored;
        REQUIRE(store::decode_extent(decoded).has_value());
        CHECK(decoded == raw);
    }
}
