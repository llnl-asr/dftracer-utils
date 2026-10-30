#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/extent_codec.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
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

dstd::Vector<std::uint8_t> text_bytes(std::size_t length) {
    static constexpr char LINE[] =
        "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":7,\"dur\":12}\n";
    dstd::Vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>(LINE[i % (sizeof(LINE) - 1)]);
    }
    return bytes;
}

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

std::size_t entry_at(std::size_t i) {
    return store::EXTENT_HEADER_BYTES + i * store::EXTENT_TABLE_ENTRY_BYTES;
}

/// Writes the header crc of a stored extent again after a change to its
/// table, so the check under test is the one that refuses it.
void reseal(dstd::Vector<std::uint8_t>& stored) {
    const std::uint32_t count = read_u32_le(stored.data());
    std::uint32_t crc = store::crc32(0, stored.data(), 8);
    crc =
        store::crc32(crc, stored.data() + 12, store::EXTENT_HEADER_BYTES - 12);
    write_u32_le(stored.data() + 8,
                 store::crc32(crc, stored.data() + entry_at(0),
                              count * store::EXTENT_TABLE_ENTRY_BYTES));
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

    TEST_CASE("a span marked alone takes a page no other span shares") {
        const std::uint32_t head = 200;
        const std::uint32_t granule = PAGE - 64;
        const std::uint32_t at = 100;
        const auto starts = starts_of({{0, at},
                                       {at, head, true},
                                       {at + head, granule},
                                       {at + head + granule, granule}},
                                      at + head + 2 * granule);
        REQUIRE(starts.size() == 4);
        CHECK(starts[0] == 0);
        CHECK(starts[1] == at);
        CHECK(starts[2] == at + head);
        CHECK(starts[3] == at + head + granule);
    }

    TEST_CASE("a span marked alone at the end of the extent ends no page") {
        const auto starts = starts_of({{0, 10}, {10, 20, true}}, 30);
        REQUIRE(starts.size() == 2);
        CHECK(starts[0] == 0);
        CHECK(starts[1] == 10);
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

    TEST_CASE("the none codec stores every page raw") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(2 * PAGE);
        const std::uint32_t starts[] = {0, PAGE};
        auto stored = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), ALIGN,
            store::PageCodec::None);
        REQUIRE(stored.has_value());
        auto pages = store::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() == 2);
        for (std::size_t i = 0; i < pages->size(); ++i) {
            CHECK(pages->entry(i).codec == store::PageCodec::None);
            CHECK(pages->entry(i).stored_length == PAGE);
        }
        dstd::Vector<std::uint8_t> decoded = *stored;
        REQUIRE(store::decode_extent(decoded).has_value());
        CHECK(decoded == raw);

        CHECK_FALSE(store::encode_extent(
                        view_of(raw), dstd::Span<const std::uint32_t>(starts),
                        ALIGN, static_cast<store::PageCodec>(7))
                        .has_value());
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
            reseal(bad);
            CHECK(refuses(bad));
        }
        SUBCASE("a reserved byte of the header set") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[13] = 1;
            CHECK(refuses(bad));
        }
        SUBCASE("a reserved byte of an entry set") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[entry_at(0) + 22] = 1;
            reseal(bad);
            CHECK(refuses(bad));
        }
        SUBCASE("a table entry out of bounds") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(0) + 8,
                         static_cast<std::uint32_t>(bad.size()));
            reseal(bad);
            CHECK(refuses(bad));
        }
        SUBCASE("raw ranges that are not contiguous") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(1), PAGE + 1);
            reseal(bad);
            CHECK(refuses(bad));
        }
        SUBCASE("a page crc that does not match") {
            dstd::Vector<std::uint8_t> bad = stored;
            write_u32_le(bad.data() + entry_at(0) + 16,
                         read_u32_le(bad.data() + entry_at(0) + 16) ^ 1U);
            reseal(bad);
            CHECK(refuses(bad));
        }
        SUBCASE("a table that does not match its header crc") {
            dstd::Vector<std::uint8_t> bad = stored;
            bad[entry_at(1) + 2] ^= 0x01U;
            CHECK(refuses(bad));
            CHECK_FALSE(store::ExtentPages::open_table(view_of(bad), bad.size())
                            .has_value());
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
            reseal(bad);
            CHECK(refuses(bad));
        }
    }

    TEST_CASE("a page table opens alone and its pages decode one at a time") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(3 * PAGE - 100);
        const std::uint32_t starts[] = {0, PAGE, 2 * PAGE};
        auto encoded = store::encode_extent(
            view_of(raw), dstd::Span<const std::uint32_t>(starts), ALIGN);
        REQUIRE(encoded.has_value());
        const dstd::Vector<std::uint8_t>& stored = *encoded;

        const std::size_t head = store::ExtentPages::table_length(
            ByteView{stored.data(), store::EXTENT_HEADER_BYTES});
        CHECK(head == entry_at(3));
        CHECK(store::ExtentPages::table_length(ByteView{
                  stored.data(), store::EXTENT_HEADER_BYTES - 1}) == 0);

        auto table = store::ExtentPages::open_table(
            ByteView{stored.data(), head}, stored.size());
        REQUIRE(table.has_value());
        REQUIRE(table->size() == 3);
        CHECK(table->raw_length() == raw.size());
        dstd::Vector<std::uint8_t> out;
        CHECK_FALSE(table->decode(0, out).has_value());

        for (std::size_t i = 0; i < table->size(); ++i) {
            const store::PageEntry& entry = table->entry(i);
            dstd::Vector<std::uint8_t> page(entry.raw_length);
            REQUIRE(store::ExtentPages::decode_page(
                        entry,
                        ByteView{stored.data() + entry.stored_offset,
                                 entry.stored_length},
                        page.data())
                        .has_value());
            CHECK(std::equal(page.begin(), page.end(),
                             raw.begin() + entry.raw_offset));
            CHECK_FALSE(store::ExtentPages::decode_page(
                            entry,
                            ByteView{stored.data() + entry.stored_offset,
                                     entry.stored_length - 1U},
                            page.data())
                            .has_value());
        }

        const store::PageEntry& last = table->entry(2);
        CHECK_FALSE(store::ExtentPages::open_table(
                        ByteView{stored.data(), head},
                        last.stored_offset + last.stored_length - 1U)
                        .has_value());
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

    TEST_CASE("metadata round-trips stored raw or as zstd") {
        dstd::Vector<std::uint8_t> repeated;
        for (int i = 0; i < 4096; ++i) {
            for (const char c : std::string_view("/p/lustre/trace/file.pfw.gz"))
                repeated.push_back(static_cast<std::uint8_t>(c));
        }
        const auto compressed =
            store::encode_meta(view_of(repeated), store::META_ZSTD_LEVEL);
        CHECK(compressed[0] ==
              static_cast<std::uint8_t>(store::PageCodec::Zstd));
        CHECK(compressed.size() < repeated.size() / 8);
        auto back = store::decode_meta(view_of(compressed));
        REQUIRE(back.has_value());
        CHECK(*back == repeated);

        std::mt19937_64 random(7);
        dstd::Vector<std::uint8_t> noise(4096);
        for (std::uint8_t& byte : noise)
            byte = static_cast<std::uint8_t>(random());
        const auto raw =
            store::encode_meta(view_of(noise), store::META_ZSTD_LEVEL);
        CHECK(raw[0] == static_cast<std::uint8_t>(store::PageCodec::None));
        CHECK(raw.size() == noise.size() + 1);
        auto raw_back = store::decode_meta(view_of(raw));
        REQUIRE(raw_back.has_value());
        CHECK(*raw_back == noise);

        const auto empty = store::encode_meta(
            view_of(dstd::Vector<std::uint8_t>{}), store::META_ZSTD_LEVEL);
        auto empty_back = store::decode_meta(view_of(empty));
        REQUIRE(empty_back.has_value());
        CHECK(empty_back->empty());
    }

    TEST_CASE("metadata with a bad codec, length or frame does not decode") {
        dstd::Vector<std::uint8_t> repeated(65536, std::uint8_t{'a'});
        const auto stored =
            store::encode_meta(view_of(repeated), store::META_ZSTD_LEVEL);
        REQUIRE(stored[0] == static_cast<std::uint8_t>(store::PageCodec::Zstd));

        auto unknown = stored;
        unknown[0] = 9;
        CHECK_FALSE(store::decode_meta(view_of(unknown)).has_value());

        auto longer = stored;
        write_u32_le(longer.data() + 1,
                     static_cast<std::uint32_t>(repeated.size() + 1));
        CHECK_FALSE(store::decode_meta(view_of(longer)).has_value());

        auto huge = stored;
        write_u32_le(huge.data() + 1, store::EXTENT_MAX_RAW_BYTES + 1U);
        CHECK_FALSE(store::decode_meta(view_of(huge)).has_value());

        auto cut = stored;
        cut.resize(cut.size() - 1);
        CHECK_FALSE(store::decode_meta(view_of(cut)).has_value());

        CHECK_FALSE(store::decode_meta(ByteView{}).has_value());
    }

    TEST_CASE("an extent compressed with a dictionary names it and needs it") {
        dstd::Vector<std::uint8_t> samples;
        dstd::Vector<std::size_t> sizes;
        std::mt19937_64 rng(11);
        const auto line = [&rng]() {
            return "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":" +
                   std::to_string(rng() % 4096) + ",\"file\":\"/p/lustre/run/" +
                   std::to_string(rng() % 97) +
                   "/data.h5\",\"size\":" + std::to_string(rng() % 65536) +
                   "}\n";
        };
        for (int sample = 0; sample < 256; ++sample) {
            const std::size_t before = samples.size();
            while (samples.size() - before < 4096) {
                for (const char c : line())
                    samples.push_back(static_cast<std::uint8_t>(c));
            }
            sizes.push_back(samples.size() - before);
        }
        auto trained = store::train_page_dict(
            view_of(samples),
            dstd::Span<const std::size_t>(sizes.data(), sizes.size()));
        REQUIRE(trained.has_value());
        CHECK(trained->size() <= store::PAGE_DICT_BYTES);

        store::DictRef ref;
        ref.offset = 8192;
        ref.length = static_cast<std::uint32_t>(trained->size());
        ref.crc = store::crc32(0, trained->data(), trained->size());
        auto encoder = store::PageDict::for_encode(view_of(*trained), ref);
        REQUIRE(encoder.has_value());
        auto decoder = store::PageDict::for_decode(view_of(*trained), ref);
        REQUIRE(decoder.has_value());

        dstd::Vector<std::uint8_t> raw;
        while (raw.size() < 16 * 1024) {
            for (const char c : line())
                raw.push_back(static_cast<std::uint8_t>(c));
        }
        raw.resize(16 * 1024);
        const std::uint32_t length = static_cast<std::uint32_t>(raw.size());
        dstd::Vector<std::uint32_t> starts;
        for (std::uint32_t at = 0; at < length; at += 1024)
            starts.push_back(at);
        auto plain = store::encode_extent(view_of(raw), starts, ALIGN);
        auto stored =
            store::encode_extent(view_of(raw), starts, ALIGN,
                                 store::PageCodec::Zstd, encoder->get());
        REQUIRE(plain.has_value());
        REQUIRE(stored.has_value());

        auto pages = store::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        CHECK(pages->dict().offset == ref.offset);
        CHECK(pages->dict().length == ref.length);
        CHECK(pages->dict().crc == ref.crc);
        std::size_t page_bytes = 0;
        for (std::size_t i = 0; i < pages->size(); ++i) {
            CHECK(pages->entry(i).codec == store::PageCodec::ZstdDict);
            page_bytes += pages->entry(i).stored_length;
        }
        auto plain_pages = store::ExtentPages::open(view_of(*plain));
        REQUIRE(plain_pages.has_value());
        CHECK(plain_pages->dict().empty());
        std::size_t plain_bytes = 0;
        for (std::size_t i = 0; i < plain_pages->size(); ++i) {
            plain_bytes += plain_pages->entry(i).stored_length;
        }
        CHECK(page_bytes < plain_bytes);

        auto with_dict = *stored;
        REQUIRE(store::decode_extent(with_dict, decoder->get()).has_value());
        CHECK(with_dict == raw);

        auto without_dict = *stored;
        CHECK_FALSE(store::decode_extent(without_dict).has_value());

        store::DictRef other = ref;
        other.offset += 4096;
        auto elsewhere = store::PageDict::for_decode(view_of(*trained), other);
        REQUIRE(elsewhere.has_value());
        auto wrong_dict = *stored;
        CHECK_FALSE(
            store::decode_extent(wrong_dict, elsewhere->get()).has_value());

        auto resealed = *stored;
        reseal(resealed);
        CHECK(store::ExtentPages::open(view_of(resealed)).has_value());
    }

    TEST_CASE("a dictionary whose bytes or header fields disagree is refused") {
        dstd::Vector<std::uint8_t> samples = text_bytes(256 * 4096);
        dstd::Vector<std::size_t> sizes(256, 4096);
        auto trained = store::train_page_dict(
            view_of(samples),
            dstd::Span<const std::size_t>(sizes.data(), sizes.size()));
        REQUIRE(trained.has_value());
        store::DictRef ref;
        ref.offset = 4096;
        ref.length = static_cast<std::uint32_t>(trained->size());
        ref.crc = store::crc32(0, trained->data(), trained->size());

        store::DictRef bad_crc = ref;
        bad_crc.crc ^= 1U;
        CHECK_FALSE(store::PageDict::for_decode(view_of(*trained), bad_crc)
                        .has_value());
        store::DictRef bad_length = ref;
        bad_length.length -= 1U;
        CHECK_FALSE(store::PageDict::for_decode(view_of(*trained), bad_length)
                        .has_value());
        CHECK_FALSE(
            store::PageDict::for_decode(view_of(*trained), store::DictRef{})
                .has_value());

        const dstd::Vector<std::uint8_t> raw = text_bytes(PAGE);
        auto stored = store::encode_extent(view_of(raw),
                                           starts_of({{0, PAGE}}, PAGE), ALIGN);
        REQUIRE(stored.has_value());
        auto stray = *stored;
        write_u32_le(stray.data() + 24, 7U);
        reseal(stray);
        CHECK_FALSE(store::ExtentPages::open(view_of(stray)).has_value());

        auto orphan = *stored;
        orphan[entry_at(0) + 20] =
            static_cast<std::uint8_t>(store::PageCodec::ZstdDict);
        reseal(orphan);
        CHECK_FALSE(store::ExtentPages::open(view_of(orphan)).has_value());
    }
}
