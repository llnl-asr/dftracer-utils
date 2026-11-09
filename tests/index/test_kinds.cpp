// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/prune.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

extensions::ChunkDimensionStats dim(dstd::String name,
                                    dstd::Vector<dstd::String> values) {
    extensions::ChunkDimensionStats ds;
    ds.dimension = std::move(name);
    ds.value_type = "string";
    for (const auto& v : values) ds.observe(v);
    return ds;
}

// A file of three chunks (two lines each): chunk 0 has cat {a}, chunk 1 has
// cat {b} and chunk 2's cat counts went over the cap (no counts record);
// dur spans [10, 20], [30, 40], [50, 60].
dstd::String build_index(const dstd::String& dir, const dstd::String& trace) {
    const auto path = dir + "/.dftindex";
    auto opened = dftu_utils_test::open_test_index(path);
    auto w = dftu_utils_test::register_test_file(opened, trace, 1);
    const dstd::Vector<dstd::Vector<dstd::String>> cats = {{"a", "a"},
                                                           {"b", "b"}};
    for (std::uint64_t c = 0; c < 3; ++c) {
        index::put_gzip_member(
            w, {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
        auto dur = dim("dur", {});
        dur.value_type = "uint";
        dur.min_value = dstd::to_string(10 + c * 20);
        dur.max_value = dstd::to_string(20 + c * 20);
        dftu_utils_test::put_dimension_stats(w, c, dur, 2);
        if (c < 2)
            dftu_utils_test::put_dimension_stats(w, c, dim("cat", cats[c]), 2);
    }
    dftu_utils_test::mark_built(w);
    dftu_utils_test::commit_test_build(w);
    return path;
}

dstd::Vector<std::uint64_t> prune(const dstd::String& index,
                                  const dstd::String& trace,
                                  const dstd::String& text) {
    auto q = duql::Query::from_string(text);
    REQUIRE(q.has_value());
    index::plan::ChunkPruner pruner;
    auto out = dftu_utils_test::ok(pruner({index, trace, std::move(*q)}).get());
    REQUIRE(out.success);
    if (out.candidate_checkpoints.empty() && out.file_may_match)
        return {0, 1, 2};
    return out.candidate_checkpoints;
}

}  // namespace

TEST_SUITE("index kinds") {
    TEST_CASE("payloads round-trip and reject garbage") {
        extensions::kinds::Zone zone{"uint", "3", "9", 5, 4, dstd::nullopt};
        auto z = extensions::kinds::decode_zone(
            extensions::kinds::encode_zone(zone));
        REQUIRE(z.has_value());
        CHECK(z->value_type == "uint");
        CHECK(z->min == "3");
        CHECK(z->max == "9");
        CHECK(z->observed == 5);
        CHECK(z->present == 4);
        CHECK_FALSE(z->histogram.has_value());
        CHECK_FALSE(extensions::kinds::decode_zone("\x01").has_value());

        extensions::kinds::CountsGranule over{7, dstd::nullopt};
        auto c = extensions::kinds::decode_counts(
            extensions::kinds::encode_counts(over));
        REQUIRE(c.has_value());
        CHECK(c->observed == 7);
        CHECK_FALSE(c->compressed.has_value());

        index::extensions::ScalableBloomFilter bf(16, 0.01);
        bf.add("x");
        auto blob = bf.serialize();
        auto b = extensions::kinds::decode_bloom(
            extensions::kinds::encode_bloom(blob, 1));
        REQUIRE(b.has_value());
        CHECK(b->possibly_contains("x"));
        CHECK_FALSE(extensions::kinds::decode_bloom("short").has_value());
    }

    TEST_CASE("each kind answers only the leaves it can") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_conditions");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/kinds.pfw.gz";
        const auto index = build_index(dstd::String(dir.str()), trace);

        // counts: chunk 2 went over the cap, so it is kept.
        CHECK(prune(index, trace, R"(cat == "a")") ==
              dstd::Vector<std::uint64_t>{0, 2});
        CHECK(prune(index, trace, R"(cat in ["b"])") ==
              dstd::Vector<std::uint64_t>{1, 2});
        // zonemap: ranges.
        CHECK(prune(index, trace, "dur > 35") ==
              dstd::Vector<std::uint64_t>{1, 2});
        // NOT: counts prove chunk 0 is all "a"; chunk 2 has no proof.
        CHECK(prune(index, trace, R"(not (cat == "a"))") ==
              dstd::Vector<std::uint64_t>{1, 2});
        // NOT over a range: only chunk 0 lies wholly under 25.
        CHECK(prune(index, trace, "not (dur < 25)") ==
              dstd::Vector<std::uint64_t>{1, 2});
        // A path no kind indexed keeps every chunk.
        CHECK(prune(index, trace, R"(args.other == "z")") ==
              dstd::Vector<std::uint64_t>{0, 1, 2});
    }

    TEST_CASE("a time range keeps a chunk whose events end inside it") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_time");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/time.pfw.gz";
        const dstd::String path((dir / ".dftindex").str());
        {
            auto idx = dftu_utils_test::open_test_index(path);
            auto w = dftu_utils_test::register_test_file(idx, trace, 1);
            // Both chunks start their events before 50; only chunk 1's last
            // event runs past it.
            const dstd::Pair<int, int> spans[] = {{10, 20}, {10, 100}};
            for (std::uint64_t c = 0; c < 2; ++c) {
                index::put_gzip_member(
                    w, {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
                auto te = dim("te", {});
                te.value_type = "uint";
                te.min_value = dstd::to_string(spans[c].first);
                te.max_value = dstd::to_string(spans[c].second);
                dftu_utils_test::put_dimension_stats(w, c, te, 2);
            }
            dftu_utils_test::mark_built(w);
            dftu_utils_test::commit_test_build(w);
        }
        auto result =
            index::plan::prune_file({path, trace, nullptr, nullptr,
                                     dstd::Pair<double, double>{50.0, 0.0}, 2})
                .get();
        REQUIRE(result.has_value());
        CHECK(result->candidates == dstd::Vector<std::uint64_t>{1});
    }

    TEST_CASE("a corrupt zone bound is an error, not a skipped prune") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_bad_zone");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/bad_zone.pfw.gz";
        const dstd::String path((dir / ".dftindex").str());
        {
            auto idx = dftu_utils_test::open_test_index(path);
            auto w = dftu_utils_test::register_test_file(idx, trace, 1);
            index::put_gzip_member(w, {0, 0, 100, 0, 200, 1, 2});
            auto dur = dim("dur", {});
            dur.value_type = "uint";
            dur.min_value = "not-a-number";
            dur.max_value = "20";
            dftu_utils_test::put_dimension_stats(w, 0, dur, 2);
            dftu_utils_test::mark_built(w);
            dftu_utils_test::commit_test_build(w);
        }
        auto q = duql::Query::from_string("dur > 5");
        REQUIRE(q.has_value());
        CHECK_THROWS_WITH_AS((void)index::plan::prune_file(
                                 {path, trace, &*q, nullptr, dstd::nullopt})
                                 .get(),
                             doctest::Contains("corrupt zone map"),
                             std::exception);
    }

    TEST_CASE("a ts bound that is not an integer is an error") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_bad_ts");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/bad_ts.pfw.gz";
        const auto index = build_index(dstd::String(dir.str()), trace);

        auto bad = duql::Query::from_string(R"(ts > "abc")");
        REQUIRE(bad.has_value());
        CHECK_THROWS_WITH_AS((void)index::plan::prune_file(
                                 {index, trace, &*bad, nullptr, dstd::nullopt})
                                 .get(),
                             doctest::Contains("abc"), std::exception);

        auto fraction = duql::Query::from_string("ts > 1.5");
        REQUIRE(fraction.has_value());
        CHECK(index::plan::prune_file(
                  {index, trace, &*fraction, nullptr, dstd::nullopt})
                  .get());
    }
}
