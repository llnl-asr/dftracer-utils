// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/plan/chunk_pruner.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cmath>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

extensions::ChunkDimensionStats dim(dstd::String name,
                                    dstd::Vector<dstd::String> values) {
    extensions::ChunkDimensionStats ds;
    ds.dimension = std::move(name);
    for (const auto& v : values) ds.observe(v);
    return ds;
}

// A file of three chunks (two lines each): chunk 0 has cat {a}, chunk 1 has
// cat {b} and chunk 2's cat counts went over the cap (no counts record);
// dur spans [10, 20], [30, 40], [50, 60].
dstd::String build_index(dstd::CStringView dir, dstd::CStringView trace) {
    const auto path = dstd::String(dir) + "/.dftindex";
    auto opened = dftu_utils_test::open_test_index(path);
    auto w = dftu_utils_test::register_test_file(opened, trace, 1);
    const dstd::Vector<dstd::Vector<dstd::String>> cats = {{"a", "a"},
                                                           {"b", "b"}};
    for (std::uint64_t c = 0; c < 3; ++c) {
        trace::index::put_gzip_member(
            w, {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
        auto dur = dim("dur", {});
        dur.zone.bounds().add(std::uint64_t{10 + c * 20});
        dur.zone.bounds().add(std::uint64_t{20 + c * 20});
        dftu_utils_test::put_dimension_stats(w, c, dur, 2);
        if (c < 2)
            dftu_utils_test::put_dimension_stats(w, c, dim("cat", cats[c]), 2);
    }
    dftu_utils_test::mark_built(w);
    dftu_utils_test::commit_test_build(w);
    return path;
}

dstd::Vector<std::uint64_t> prune(dstd::CStringView index,
                                  dstd::CStringView trace,
                                  dstd::StringView text) {
    auto q = duql::Query::from_string(text);
    REQUIRE(q.has_value());
    trace::index::plan::ChunkPruner pruner;
    auto out = dftu_utils_test::ok(
        pruner({dstd::String(index), dstd::String(trace), std::move(*q)})
            .get());
    REQUIRE(out.success);
    if (out.candidate_checkpoints.empty() && out.file_may_match)
        return {0, 1, 2};
    return out.candidate_checkpoints;
}

}  // namespace

TEST_SUITE("index kinds") {
    TEST_CASE("payloads round-trip and reject garbage") {
        extensions::kinds::Zone zone;
        zone.add(std::uint64_t{3});
        zone.add(std::uint64_t{9});
        zone.add_nulls(2);
        zone.observed = 5;
        zone.present = 4;
        auto z = extensions::kinds::decode_zone(
            extensions::kinds::encode_zone(zone));
        REQUIRE(z.has_value());
        CHECK(z->type() == extensions::BoundType::UInt64);
        CHECK(z->min_u64() == 3);
        CHECK(z->max_u64() == 9);
        CHECK(z->null_count() == 2);
        CHECK(z->nan_count() == 0);
        CHECK(z->exact());
        CHECK(z->observed == 5);
        CHECK(z->present == 4);
        CHECK_FALSE(z->histogram.has_value());
        CHECK_FALSE(extensions::kinds::decode_zone("\x01").has_value());
        // A payload of another version is not read.
        auto other = extensions::kinds::encode_zone(zone);
        other[0] =
            static_cast<char>(extensions::kinds::ZONE_PAYLOAD_VERSION + 1);
        CHECK_FALSE(extensions::kinds::decode_zone(other).has_value());

        extensions::kinds::CountsGranule over{7, dstd::nullopt};
        auto c = extensions::kinds::decode_counts(
            extensions::kinds::encode_counts(over));
        REQUIRE(c.has_value());
        CHECK(c->observed == 7);
        CHECK_FALSE(c->compressed.has_value());

        trace::index::extensions::ScalableBloomFilter bf(16, 0.01);
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
                trace::index::put_gzip_member(
                    w, {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
                auto te = dim("te", {});
                te.zone.bounds().add(
                    static_cast<std::uint64_t>(spans[c].first));
                te.zone.bounds().add(
                    static_cast<std::uint64_t>(spans[c].second));
                dftu_utils_test::put_dimension_stats(w, c, te, 2);
            }
            dftu_utils_test::mark_built(w);
            dftu_utils_test::commit_test_build(w);
        }
        auto result = trace::index::plan::prune_file(
                          {path, trace, nullptr, nullptr,
                           dstd::Pair<double, double>{50.0, 0.0}, 2})
                          .get();
        REQUIRE(result.has_value());
        CHECK(result->candidates == dstd::Vector<std::uint64_t>{1});
    }

    TEST_CASE("a zone whose bounds are out of order prunes nothing") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_bad_zone");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/bad_zone.pfw.gz";
        const dstd::String path((dir / ".dftindex").str());
        {
            auto idx = dftu_utils_test::open_test_index(path);
            auto w = dftu_utils_test::register_test_file(idx, trace, 1);
            trace::index::put_gzip_member(w, {0, 0, 100, 0, 200, 1, 2});
            extensions::kinds::Zone zone;
            zone.assign(extensions::BoundType::Int64, 2, 0, 0, true, true);
            zone.set_min(std::int64_t{20});
            zone.set_max(std::int64_t{5});
            zone.observed = 2;
            zone.present = 2;
            const auto payload = extensions::kinds::encode_zone(zone);
            trace::index::put_path_granule(
                w, trace::index::store::IndexExtension::Zonemap, "dur", 0,
                payload);
            trace::index::put_path(
                w, trace::index::store::IndexExtension::Zonemap, "dur");
            dftu_utils_test::mark_built(w);
            dftu_utils_test::commit_test_build(w);
        }
        auto q = duql::Query::from_string("dur > 100");
        REQUIRE(q.has_value());
        const auto pruned = trace::index::plan::prune_file(
                                {path, trace, &*q, nullptr, dstd::nullopt})
                                .get();
        REQUIRE(pruned.has_value());
        CHECK(pruned->candidates == dstd::Vector<std::uint64_t>{0});
    }

    TEST_CASE("an unreadable zone prunes nothing") {
        // A zone of another payload version and a zone with a NaN bound are no
        // evidence: the chunk is read.
        for (int bad = 0; bad < 2; ++bad) {
            CAPTURE(bad);
            auto dir =
                dftu_utils_test::make_unique_test_path("kinds_bad_zone2");
            REQUIRE(dstd::fs::create_directories(dir).has_value());
            const dstd::String trace = "/fake/bad_zone2.pfw.gz";
            const dstd::String path((dir / ".dftindex").str());
            {
                auto idx = dftu_utils_test::open_test_index(path);
                auto w = dftu_utils_test::register_test_file(idx, trace, 1);
                trace::index::put_gzip_member(w, {0, 0, 100, 0, 200, 1, 2});
                extensions::kinds::Zone zone;
                if (bad == 0) {
                    zone.add(std::int64_t{1});
                    zone.add(std::int64_t{5});
                } else {
                    zone.assign(extensions::BoundType::Double, 2, 0, 0, true,
                                true);
                    zone.set_min(std::nan(""));
                    zone.set_max(5.0);
                }
                zone.observed = 2;
                zone.present = 2;
                auto payload = extensions::kinds::encode_zone(zone);
                if (bad == 0)
                    payload[0] = static_cast<char>(
                        extensions::kinds::ZONE_PAYLOAD_VERSION + 1);
                trace::index::put_path_granule(
                    w, trace::index::store::IndexExtension::Zonemap, "dur", 0,
                    payload);
                trace::index::put_path(
                    w, trace::index::store::IndexExtension::Zonemap, "dur");
                dftu_utils_test::mark_built(w);
                dftu_utils_test::commit_test_build(w);
            }
            for (const char* text : {"dur > 100", "dur < 0"}) {
                auto q = duql::Query::from_string(text);
                REQUIRE(q.has_value());
                const auto pruned =
                    trace::index::plan::prune_file(
                        {path, trace, &*q, nullptr, dstd::nullopt})
                        .get();
                REQUIRE(pruned.has_value());
                CHECK(pruned->candidates == dstd::Vector<std::uint64_t>{0});
            }
        }
    }

    TEST_CASE("a ts bound that is not an integer is an error") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_bad_ts");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/bad_ts.pfw.gz";
        const auto index = build_index(dstd::String(dir.str()), trace);

        auto bad = duql::Query::from_string(R"(ts > "abc")");
        REQUIRE(bad.has_value());
        const auto refused = trace::index::plan::prune_file(
                                 {index, trace, &*bad, nullptr, dstd::nullopt})
                                 .get();
        REQUIRE_FALSE(refused.has_value());
        CHECK(common::errorcode_of(refused.error()) ==
              common::ErrorCode::InvalidArgument);
        CHECK(dstd::String(refused.error().message()).find("abc") !=
              dstd::String::npos);

        auto fraction = duql::Query::from_string("ts > 1.5");
        REQUIRE(fraction.has_value());
        CHECK(trace::index::plan::prune_file(
                  {index, trace, &*fraction, nullptr, dstd::nullopt})
                  .get());
    }
}
