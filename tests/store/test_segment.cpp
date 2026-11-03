// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/segment.h>
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>

using namespace dftracer::utils;

namespace {

using namespace dftracer::utils::store;

constexpr std::uint64_t RANDOM_SEED = 0x5EC7105EED1234C0ULL;
constexpr std::size_t PAIR_COUNT = 10000;

int compare_strings(const dstd::String& a, const dstd::String& b) {
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    if (common != 0) {
        const int cmp = std::memcmp(a.data(), b.data(), common);
        if (cmp != 0) return cmp;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

struct BytewiseLess {
    bool operator()(const dstd::String& a, const dstd::String& b) const {
        return compare_strings(a, b) < 0;
    }
};

using SortedMap = dstd::Map<dstd::String, dstd::String, BytewiseLess>;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

dstd::String as_string(dstd::ByteView bytes) {
    if (bytes.empty()) return {};
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

bool same_bytes(dstd::ByteView a, dstd::ByteView b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

std::uint32_t read_u32_le_at(dstd::ByteView bytes, std::size_t offset) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

dstd::String random_bytes(std::mt19937_64& rng, std::size_t max_length) {
    std::uniform_int_distribution<std::size_t> length_dist(0, max_length);
    std::uniform_int_distribution<unsigned> byte_dist(0, 255);
    dstd::String out(length_dist(rng), '\0');
    for (char& c : out) c = static_cast<char>(byte_dist(rng));
    return out;
}

dstd::String random_key(std::mt19937_64& rng) { return random_bytes(rng, 32); }

dstd::String random_value(std::mt19937_64& rng) {
    return random_bytes(rng, 512);
}

void append_fold(dstd::ByteView accumulated, dstd::ByteView value,
                 dstd::Vector<std::uint8_t>& out) {
    out.clear();
    if (!accumulated.empty()) {
        const auto* begin =
            reinterpret_cast<const std::uint8_t*>(accumulated.data());
        out.insert(out.end(), begin, begin + accumulated.size());
    }
    if (!value.empty()) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
        out.insert(out.end(), begin, begin + value.size());
    }
}

struct PlacerContext {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t crc = 0;
};

engine::BigRef make_big_ref(void* context, dstd::ByteView value) {
    const auto* ctx = static_cast<const PlacerContext*>(context);
    return {ctx->offset + static_cast<std::uint64_t>(value.size()), ctx->length,
            ctx->crc + static_cast<std::uint32_t>(value.size() * 7U)};
}

engine::BigRef fixed_placer(void* context, dstd::ByteView) {
    const auto* ctx = static_cast<const PlacerContext*>(context);
    return {ctx->offset, ctx->length, ctx->crc};
}

}  // namespace

TEST_SUITE("index store segment") {
    TEST_CASE("random entries round-trip by key and by range") {
        std::mt19937_64 rng(RANDOM_SEED);
        engine::SegmentBuilder builder;
        SortedMap expected;
        for (std::size_t i = 0; i < PAIR_COUNT; ++i) {
            const dstd::String key = random_key(rng);
            const dstd::String value = random_value(rng);
            builder.add(view_of(key), view_of(value));
            expected[key] = value;
        }

        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        CHECK(sealed->size() % engine::SEGMENT_ALIGN == 0);
        CHECK(sealed->size() > engine::GRANULE_BYTES * 10);
        CHECK(builder.entry_count() == expected.size());
        CHECK(as_string(builder.first_key()) == expected.begin()->first);
        CHECK(as_string(builder.last_key()) == expected.rbegin()->first);

        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        const engine::SegmentView& view = *opened;
        CHECK(view.entry_count() == expected.size());

        for (const auto& [key, value] : expected) {
            const auto entry = view.find(view_of(key));
            REQUIRE(entry.has_value());
            CHECK_FALSE(entry->is_big());
            CHECK(same_bytes(entry->value(), view_of(value)));
        }

        for (std::size_t i = 0; i < 1000; ++i) {
            dstd::String key = random_key(rng);
            while (expected.find(key) != expected.end()) {
                key = random_key(rng);
            }
            CHECK_FALSE(view.find(view_of(key)).has_value());
        }

        for (std::size_t i = 0; i < 1000; ++i) {
            const dstd::String start = random_key(rng);
            auto cursor = view.lower_bound(view_of(start));
            auto expected_it = expected.lower_bound(start);
            while (!cursor.at_end()) {
                REQUIRE(expected_it != expected.end());
                const auto entry = cursor.entry();
                CHECK(as_string(entry.key()) == expected_it->first);
                CHECK_FALSE(entry.is_big());
                CHECK(same_bytes(entry.value(), view_of(expected_it->second)));
                ++expected_it;
                cursor.next();
            }
            CHECK(expected_it == expected.end());
        }
    }

    TEST_CASE("equal keys keep the last added value") {
        engine::SegmentBuilder builder;
        builder.add(view_of(dstd::String("key")),
                    view_of(dstd::String("first")));
        builder.add(view_of(dstd::String("key")),
                    view_of(dstd::String("second")));
        builder.add(view_of(dstd::String("key")),
                    view_of(dstd::String("third")));

        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        CHECK(opened->entry_count() == 1);

        const auto entry = opened->find(view_of(dstd::String("key")));
        REQUIRE(entry.has_value());
        CHECK(as_string(entry->value()) == "third");
    }

    TEST_CASE("a fold combines equal keys in add order") {
        engine::SegmentBuilder builder;
        builder.add(view_of(dstd::String("key")), view_of(dstd::String("A")));
        builder.add(view_of(dstd::String("key")), view_of(dstd::String("B")));
        builder.add(view_of(dstd::String("key")), view_of(dstd::String("C")));
        builder.add(view_of(dstd::String("key")), view_of(dstd::String("D")));

        const auto sealed = builder.seal(append_fold, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());

        const auto entry = opened->find(view_of(dstd::String("key")));
        REQUIRE(entry.has_value());
        CHECK(as_string(entry->value()) == "ABCD");
    }

    TEST_CASE("values above the threshold come back as big refs") {
        engine::SegmentBuilder builder;
        const dstd::String inline_value(engine::BIG_VALUE_BYTES, 'i');
        const dstd::String big_value(engine::BIG_VALUE_BYTES + 1, 'b');
        const dstd::String other_big_value(engine::BIG_VALUE_BYTES + 123, 'c');
        builder.add(view_of(dstd::String("inline")), view_of(inline_value));
        builder.add(view_of(dstd::String("big")), view_of(big_value));
        builder.add(view_of(dstd::String("other")), view_of(other_big_value));

        const engine::PlacerFn placer = make_big_ref;
        PlacerContext context{0, 0x1020304050607080ULL, 0};
        const auto sealed = builder.seal(nullptr, placer, &context);
        REQUIRE(sealed.has_value());
        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());

        const auto inline_entry = opened->find(view_of(dstd::String("inline")));
        REQUIRE(inline_entry.has_value());
        CHECK_FALSE(inline_entry->is_big());
        CHECK(inline_entry->value().size() == engine::BIG_VALUE_BYTES);

        const auto big_entry = opened->find(view_of(dstd::String("big")));
        REQUIRE(big_entry.has_value());
        CHECK(big_entry->is_big());
        CHECK(big_entry->big_ref().offset ==
              make_big_ref(&context, view_of(big_value)).offset);
        CHECK(big_entry->big_ref().length ==
              make_big_ref(&context, view_of(big_value)).length);
        CHECK(big_entry->big_ref().crc ==
              make_big_ref(&context, view_of(big_value)).crc);

        const auto other_entry = opened->find(view_of(dstd::String("other")));
        REQUIRE(other_entry.has_value());
        CHECK(other_entry->is_big());
        CHECK(other_entry->big_ref().offset ==
              make_big_ref(&context, view_of(other_big_value)).offset);
        CHECK(other_entry->big_ref().length ==
              make_big_ref(&context, view_of(other_big_value)).length);
        CHECK(other_entry->big_ref().crc ==
              make_big_ref(&context, view_of(other_big_value)).crc);
    }

    TEST_CASE("a tiny segment matches its exact bytes") {
        engine::SegmentBuilder builder;
        builder.add(view_of(dstd::String("a")), view_of(dstd::String("1")));
        builder.add(view_of(dstd::String("b")),
                    view_of(dstd::String(engine::BIG_VALUE_BYTES + 1, 'b')));
        builder.add(view_of(dstd::String("c")), view_of(dstd::String("xy")));

        PlacerContext context{0x0102030405060708ULL, 0x1112131415161718ULL,
                              0x21222324U};
        const auto sealed = builder.seal(nullptr, fixed_placer, &context);
        REQUIRE(sealed.has_value());

        const std::array<std::uint8_t, 64> expected{
            'S',  'E',  'C',  'T',  0x40, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
            0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x00,
            0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 'a',  0x01, 0x02, 'a',
            '1',  0x01, 0x29, 'b',  0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
            0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x24, 0x23,
            0x22, 0x21, 0x01, 0x04, 'c',  'x',  'y',  0x00, 0x00};
        REQUIRE(sealed->size() == expected.size());
        CHECK(same_bytes(view_of(*sealed),
                         dstd::ByteView{expected.data(), expected.size()}));
    }

    TEST_CASE("two entries over the granule byte limit make two granules") {
        engine::SegmentBuilder builder;
        const dstd::String first_value(engine::GRANULE_BYTES - 5U, 'v');
        builder.add(view_of(dstd::String("a")), view_of(first_value));
        builder.add(view_of(dstd::String("b")), view_of(dstd::String("w")));

        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        REQUIRE(sealed->size() > 20U);
        CHECK(read_u32_le_at(view_of(*sealed), 12U) == 2U);

        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        CHECK(opened->entry_count() == 2);
    }

    TEST_CASE("lower_bound and find are deterministic around granules") {
        engine::SegmentBuilder builder;
        const dstd::String large_value(engine::GRANULE_BYTES - 5U, 'v');
        builder.add(view_of(dstd::String("b")), view_of(large_value));
        builder.add(view_of(dstd::String("f")), view_of(large_value));
        builder.add(view_of(dstd::String("h")), view_of(large_value));

        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        const engine::SegmentView& view = *opened;

        const auto check_lower = [&](const dstd::String& key,
                                     const char* expected) {
            auto cursor = view.lower_bound(view_of(key));
            REQUIRE_FALSE(cursor.at_end());
            CHECK(as_string(cursor.entry().key()) == dstd::String(expected));
        };

        check_lower("a", "b");
        check_lower("b", "b");
        check_lower("e", "f");
        check_lower("c", "f");
        CHECK(view.lower_bound(view_of(dstd::String("z"))).at_end());

        CHECK_FALSE(view.find(view_of(dstd::String("a"))).has_value());
        const auto first = view.find(view_of(dstd::String("b")));
        REQUIRE(first.has_value());
        CHECK(as_string(first->key()) == "b");
        CHECK_FALSE(view.find(view_of(dstd::String("c"))).has_value());
        const auto second = view.find(view_of(dstd::String("f")));
        REQUIRE(second.has_value());
        CHECK(as_string(second->key()) == "f");
        CHECK_FALSE(view.find(view_of(dstd::String("z"))).has_value());
    }

    TEST_CASE("an empty builder seals an empty segment") {
        engine::SegmentBuilder builder;
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        CHECK(sealed->size() % engine::SEGMENT_ALIGN == 0);

        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        CHECK(opened->entry_count() == 0);
        CHECK_FALSE(opened->find(view_of(dstd::String("absent"))).has_value());
        CHECK(opened->lower_bound(view_of(dstd::String("absent"))).at_end());
        CHECK(builder.entry_count() == 0);
        CHECK(builder.first_key().size() == 0);
        CHECK(builder.last_key().size() == 0);
    }

    TEST_CASE("targeted field corruption returns Parse") {
        const auto make_base = [] {
            engine::SegmentBuilder builder;
            builder.add(view_of(dstd::String("a")), view_of(dstd::String("1")));
            builder.add(view_of(dstd::String("b")), view_of(dstd::String("2")));
            const auto sealed = builder.seal(nullptr, nullptr, nullptr);
            REQUIRE(sealed.has_value());
            return dstd::Vector<std::uint8_t>(sealed->begin(), sealed->end());
        };
        const dstd::Vector<std::uint8_t> base = make_base();
        REQUIRE(base.size() == 40U);

        const auto check_parse = [&](std::size_t offset, std::uint8_t value) {
            dstd::Vector<std::uint8_t> mutated = base;
            REQUIRE(offset < mutated.size());
            mutated[offset] = value;
            const auto opened = engine::SegmentView::open(view_of(mutated));
            REQUIRE_FALSE(opened.has_value());
            CHECK(opened.error().is(dstd::Condition::Parse));
        };

        check_parse(8U, 1U);
        check_parse(12U, 0U);
        check_parse(20U, 31U);
        check_parse(24U, 0U);
        check_parse(30U, 0x80U);
        check_parse(36U, 'a');
        check_parse(38U, 1U);
    }

    TEST_CASE("a big value with a null placer is an error") {
        engine::SegmentBuilder builder;
        builder.add(view_of(dstd::String("key")),
                    view_of(dstd::String(engine::BIG_VALUE_BYTES + 1, 'b')));

        const auto failed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().is(dstd::Condition::InvalidArgument));
        CHECK(builder.entry_count() == 0);
        CHECK(builder.first_key().empty());
        CHECK(builder.last_key().empty());

        PlacerContext context{0, 0x1020304050607080ULL, 0};
        const auto sealed = builder.seal(nullptr, make_big_ref, &context);
        REQUIRE(sealed.has_value());
        CHECK(builder.entry_count() == 1);
        CHECK(as_string(builder.first_key()) == "key");
        CHECK(as_string(builder.last_key()) == "key");
    }

    TEST_CASE("the spans of a sealed segment cut its pages at its granules") {
        engine::SegmentBuilder builder;
        const dstd::String value(engine::GRANULE_BYTES / 2 - 32, 'v');
        for (std::size_t i = 0; i < 12; ++i) {
            const dstd::String key = "key-" + dstd::to_string(i);
            builder.add(view_of(key), view_of(value));
        }
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        REQUIRE(sealed->size() > 4 * engine::PAGE_BYTES);
        const auto opened = engine::SegmentView::open_index(view_of(*sealed));
        REQUIRE(opened.has_value());
        const engine::SegmentView& view = *opened;
        REQUIRE(view.granule_count() > 4);

        dstd::Vector<engine::SegmentSpan> spans;
        REQUIRE(
            store::engine::append_segment_spans(view_of(*sealed), 100, spans));
        REQUIRE(spans.size() == view.granule_count() + 1);
        CHECK(spans.front().offset == 100);
        CHECK(spans.front().length == view.granule(0).offset);
        for (std::size_t i = 0; i < view.granule_count(); ++i) {
            const engine::SegmentView::GranuleSpan granule = view.granule(i);
            CHECK(spans[i + 1].offset == 100 + granule.offset);
            if (i + 1 < view.granule_count()) {
                CHECK(spans[i + 1].length == granule.length);
            }
        }
        CHECK(spans[1].offset == spans[0].offset + spans[0].length);
        for (std::size_t i = 2; i < spans.size(); ++i) {
            CHECK(spans[i].offset == spans[i - 1].offset + spans[i - 1].length);
        }
        CHECK(spans.back().offset + spans.back().length ==
              100 + sealed->size());
    }

    TEST_CASE("a segment of at most one page is one span") {
        engine::SegmentBuilder builder;
        builder.add(view_of(dstd::String("a")), view_of(dstd::String("1")));
        builder.add(view_of(dstd::String("b")), view_of(dstd::String("2")));
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        REQUIRE(sealed->size() <= engine::PAGE_BYTES);

        dstd::Vector<engine::SegmentSpan> spans;
        REQUIRE(
            store::engine::append_segment_spans(view_of(*sealed), 4096, spans));
        REQUIRE(spans.size() == 1);
        CHECK(spans[0].offset == 4096);
        CHECK(spans[0].length == sealed->size());

        dstd::Vector<engine::SegmentSpan> garbage_spans;
        const dstd::String garbage(4096, 'x');
        CHECK_FALSE(store::engine::append_segment_spans(view_of(garbage), 0,
                                                        garbage_spans));
        CHECK(garbage_spans.empty());
    }

    TEST_CASE("the head and the granule walk agree with find") {
        engine::SegmentBuilder builder;
        dstd::Vector<dstd::String> keys;
        const dstd::String value(1000, 'v');
        for (std::size_t i = 0; i < 300; ++i) {
            const dstd::String digits = dstd::to_string(i);
            const dstd::String key =
                "key-" + dstd::String(4 - digits.size(), '0') + digits;
            keys.push_back(key);
            builder.add(view_of(key), view_of(value));
        }
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const auto opened = engine::SegmentView::open(view_of(*sealed));
        REQUIRE(opened.has_value());
        const engine::SegmentView& view = *opened;
        REQUIRE(view.granule_count() > 4);
        const std::uint32_t length = static_cast<std::uint32_t>(sealed->size());
        const std::uint32_t head_bytes =
            engine::SegmentView::head_length(view_of(*sealed));
        REQUIRE(head_bytes > 0);
        REQUIRE(head_bytes < sealed->size());
        const dstd::ByteView head{sealed->data(), head_bytes};

        const auto check = [&](const dstd::String& key) {
            const auto expected = view.find(view_of(key));
            const auto granule =
                engine::SegmentView::find_granule(head, length, view_of(key));
            REQUIRE(granule.has_value());
            if (!granule->has_value()) {
                CHECK_FALSE(expected.has_value());
                return;
            }
            const engine::SegmentView::GranuleSpan span = **granule;
            REQUIRE(span.offset >= head_bytes);
            REQUIRE(span.offset + span.length <= length);
            const auto actual = engine::SegmentView::find_in_granule(
                dstd::ByteView{sealed->data() + span.offset, span.length},
                view_of(key));
            REQUIRE(actual.has_value());
            CHECK(actual->has_value() == expected.has_value());
            if (expected.has_value() && actual->has_value()) {
                CHECK(same_bytes((*actual)->key(), expected->key()));
                CHECK(same_bytes((*actual)->value(), expected->value()));
            }
        };

        for (const dstd::String& key : keys) check(key);
        check(keys.front() + "x");
        for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
            check(keys[i] + "x");
        }
        check("aaa");
        check("zzz");

        CHECK_FALSE(engine::SegmentView::find_granule(
                        head, length, view_of(dstd::String("aaa")))
                        ->has_value());
        CHECK_FALSE(engine::SegmentView::find_in_granule(
                        dstd::ByteView{sealed->data() + head_bytes,
                                       length - head_bytes},
                        view_of(dstd::String("zzz")))
                        ->has_value());
    }

    TEST_CASE("a window of the granules of a range holds every key of it") {
        engine::SegmentBuilder builder;
        dstd::Vector<dstd::String> keys;
        const dstd::String value(1000, 'v');
        for (std::size_t i = 0; i < 300; ++i) {
            const dstd::String digits = dstd::to_string(i);
            const dstd::String key =
                "key-" + dstd::String(4 - digits.size(), '0') + digits;
            keys.push_back(key);
            builder.add(view_of(key), view_of(value));
        }
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const std::uint32_t length = static_cast<std::uint32_t>(sealed->size());
        const std::uint32_t head_bytes =
            engine::SegmentView::head_length(view_of(*sealed));
        REQUIRE(head_bytes > 0);
        const dstd::ByteView head{sealed->data(), head_bytes};

        // The first key of each granule, so bounds also fall on granule edges.
        dstd::Vector<dstd::String> bounds = {"aaa", "zzz", keys.back() + "x"};
        std::uint32_t last_offset = 0;
        for (const dstd::String& key : keys) {
            const auto granule =
                engine::SegmentView::find_granule(head, length, view_of(key));
            REQUIRE(granule.has_value());
            REQUIRE(granule->has_value());
            if ((**granule).offset != last_offset) bounds.push_back(key);
            last_offset = (**granule).offset;
        }
        REQUIRE(bounds.size() > 7);
        for (std::size_t i = 0; i < keys.size(); i += 37) {
            bounds.push_back(keys[i] + "x");
        }

        const auto range_of = [&](const dstd::String& begin,
                                  const dstd::String& end) {
            dstd::Vector<dstd::String> out;
            for (const dstd::String& key : keys) {
                if (key >= begin && (end.empty() || key < end)) {
                    out.push_back(key);
                }
            }
            return out;
        };
        const auto windowed = [&](const dstd::String& begin,
                                  const dstd::String& end) {
            auto opened = engine::SegmentView::open_head(head, length);
            REQUIRE(opened.has_value());
            const engine::SegmentView::GranuleSpan span =
                opened->range_span(view_of(begin), view_of(end));
            REQUIRE(span.offset >= head_bytes);
            REQUIRE(span.offset + span.length <= length);
            auto view = std::move(*opened).with_window(
                dstd::ByteView{sealed->data() + span.offset, span.length},
                span.offset);
            REQUIRE(view.has_value());
            dstd::Vector<dstd::String> out;
            for (auto cursor = view->lower_bound(view_of(begin));
                 !cursor.at_end(); cursor.next()) {
                const dstd::ByteView key = cursor.entry().key();
                dstd::String text(reinterpret_cast<const char*>(key.data()),
                                  key.size());
                if (!end.empty() && text >= end) break;
                CHECK(same_bytes(cursor.entry().value(), view_of(value)));
                out.push_back(std::move(text));
            }
            return out;
        };

        for (const dstd::String& begin : bounds) {
            CHECK(windowed(begin, dstd::String()) == range_of(begin, ""));
            for (const dstd::String& end : bounds) {
                if (end <= begin) continue;
                CHECK(windowed(begin, end) == range_of(begin, end));
            }
        }
    }

    TEST_CASE("a damaged header or index leaves the head unusable") {
        engine::SegmentBuilder builder;
        const dstd::String value(engine::GRANULE_BYTES / 2 - 32, 'v');
        for (std::size_t i = 0; i < 6; ++i) {
            const dstd::String key = "key-" + dstd::to_string(i);
            builder.add(view_of(key), view_of(value));
        }
        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        const std::uint32_t length = static_cast<std::uint32_t>(sealed->size());
        const std::uint32_t head_bytes =
            engine::SegmentView::head_length(view_of(*sealed));
        REQUIRE(head_bytes > 0);
        const dstd::String key = "key-3";

        const auto refuses = [&](const dstd::Vector<std::uint8_t>& bytes,
                                 std::uint32_t head) {
            const dstd::ByteView head_view{bytes.data(), head};
            const auto granule = engine::SegmentView::find_granule(
                head_view, length, view_of(key));
            REQUIRE_FALSE(granule.has_value());
            CHECK(granule.error().is(dstd::Condition::Parse));
        };

        SUBCASE("a magic that is not a segment magic") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            bad[0] = 'X';
            CHECK(engine::SegmentView::head_length(
                      dstd::ByteView{bad.data(), head_bytes}) == 0);
            refuses(bad, head_bytes);
        }
        SUBCASE("a reserved byte of the header set") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            bad[17] = 1;
            CHECK(engine::SegmentView::head_length(
                      dstd::ByteView{bad.data(), head_bytes}) == 0);
            refuses(bad, head_bytes);
        }
        SUBCASE("a header of another segment length") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            const auto other = static_cast<std::uint32_t>(bad.size() * 2);
            bad[4] = static_cast<std::uint8_t>(other & 0xFFU);
            bad[5] = static_cast<std::uint8_t>((other >> 8U) & 0xFFU);
            bad[6] = static_cast<std::uint8_t>((other >> 16U) & 0xFFU);
            bad[7] = static_cast<std::uint8_t>((other >> 24U) & 0xFFU);
            refuses(bad, head_bytes);
        }
        SUBCASE("an index entry that runs past the head") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            bad.resize(head_bytes - 1);
            CHECK(engine::SegmentView::head_length(
                      dstd::ByteView{bad.data(), bad.size()}) == 0);
            refuses(bad, static_cast<std::uint32_t>(bad.size()));
        }
        SUBCASE("an index offset that does not follow the one before it") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            const std::size_t second =
                20U + 8U + 1U + dstd::String("key-0").size();
            bad[second] = 0;
            bad[second + 1U] = 0;
            bad[second + 2U] = 0;
            bad[second + 3U] = 0;
            refuses(bad, head_bytes);
        }
        SUBCASE("a granule length of zero") {
            dstd::Vector<std::uint8_t> bad(sealed->begin(), sealed->end());
            bad[24] = 0;
            bad[25] = 0;
            bad[26] = 0;
            bad[27] = 0;
            refuses(bad, head_bytes);
        }
    }

    TEST_CASE("corrupt segments return Parse or stay in bounds") {
        engine::SegmentBuilder builder;
        dstd::Vector<dstd::String> keys;
        for (std::size_t i = 0; i < 200; ++i) {
            const dstd::String key = "key-" + dstd::to_string(i);
            const dstd::String value(64, static_cast<char>('a' + (i % 26)));
            keys.push_back(key);
            builder.add(view_of(key), view_of(value));
        }

        const auto sealed = builder.seal(nullptr, nullptr, nullptr);
        REQUIRE(sealed.has_value());
        dstd::Vector<std::uint8_t> original;
        original.insert(original.end(), sealed->begin(), sealed->end());

        std::mt19937_64 rng(RANDOM_SEED + 1);
        for (std::size_t run = 0; run < 2000; ++run) {
            dstd::Vector<std::uint8_t> mutated = original;
            if ((rng() & 1U) == 0) {
                mutated.resize(rng() % original.size());
            } else {
                const unsigned flips = 1U + static_cast<unsigned>(rng() % 4U);
                for (unsigned i = 0; i < flips; ++i) {
                    const std::size_t at = rng() % mutated.size();
                    mutated[at] ^=
                        static_cast<std::uint8_t>(1U << (rng() % 8U));
                }
            }

            const auto opened = engine::SegmentView::open(view_of(mutated));
            if (!opened.has_value()) {
                CHECK(opened.error().is(dstd::Condition::Parse));
                continue;
            }

            for (const dstd::String& key : keys) {
                (void)opened->find(view_of(key));
            }
            auto cursor = opened->lower_bound(dstd::ByteView{});
            std::size_t steps = 0;
            while (!cursor.at_end() && steps < 200) {
                (void)cursor.entry();
                cursor.next();
                ++steps;
            }
        }
    }
}
