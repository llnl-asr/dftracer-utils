#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/trace/indexing/shard_manifest.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>

using dftracer::utils::DFTUtilsException;
using dftracer::utils::trace::IndexShardManifest;
using dftracer::utils::trace::parse_manifest;
using dftracer::utils::trace::read_shard_manifest;
using dftracer::utils::trace::resolve_shard_set_root;
using dftracer::utils::trace::SHARD_SET_DIRNAME;
using dftracer::utils::trace::to_json;
using dftracer::utils::trace::write_shard_manifest;

namespace {

IndexShardManifest sample() {
    IndexShardManifest m;
    m.schema_version = 8;
    m.shards.push_back({"shard-0000.dftindex", 0, 127, 128, 10482375});
    m.shards.push_back({"shard-0001.dftindex", 128, 200, 73, 512});
    return m;
}

using dftu_utils_test::ScopedTestDir;

}  // namespace

TEST_SUITE("shard_manifest") {
    TEST_CASE("round-trips through JSON") {
        IndexShardManifest in = sample();
        IndexShardManifest out = parse_manifest(to_json(in));

        CHECK(out.schema_version == in.schema_version);
        REQUIRE(out.shards.size() == in.shards.size());
        for (std::size_t i = 0; i < in.shards.size(); ++i) {
            CHECK(out.shards[i].path == in.shards[i].path);
            CHECK(out.shards[i].file_id_min == in.shards[i].file_id_min);
            CHECK(out.shards[i].file_id_max == in.shards[i].file_id_max);
            CHECK(out.shards[i].num_files == in.shards[i].num_files);
            CHECK(out.shards[i].num_events == in.shards[i].num_events);
        }
    }

    TEST_CASE("an empty manifest round-trips") {
        IndexShardManifest in;
        in.schema_version = 8;
        IndexShardManifest out = parse_manifest(to_json(in));
        CHECK(out.schema_version == 8);
        CHECK(out.shards.empty());
    }

    TEST_CASE("write then read from a directory") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        write_shard_manifest(dir.path().string(), sample());
        auto loaded = read_shard_manifest(dir.path().string());
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 2);
        CHECK(loaded->shards[1].file_id_min == 128);
        CHECK(loaded->shards[1].num_files == 73);
    }

    TEST_CASE("reading a directory with no manifest returns nullopt") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        CHECK_FALSE(read_shard_manifest(dir.path().string()).has_value());
    }

    // The publish is a rename over any prior manifest; a reader that opens the
    // file at any instant sees a complete manifest, never a partial one.
    TEST_CASE("write replaces a prior manifest atomically") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        write_shard_manifest(dir.path().string(), sample());

        IndexShardManifest second;
        second.schema_version = 8;
        second.shards.push_back({"only.dftindex", 0, 0, 1, 1});
        write_shard_manifest(dir.path().string(), second);

        auto loaded = read_shard_manifest(dir.path().string());
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 1);
        CHECK(loaded->shards[0].path == "only.dftindex");
    }

    TEST_CASE("a path with quotes and control chars survives escaping") {
        IndexShardManifest in;
        in.schema_version = 8;
        in.shards.push_back({"weird\"\\\n\tname.dftindex", 0, 0, 1, 1});
        IndexShardManifest out = parse_manifest(to_json(in));
        REQUIRE(out.shards.size() == 1);
        CHECK(out.shards[0].path == "weird\"\\\n\tname.dftindex");
    }

    TEST_CASE("resolve_shard_set_root finds a manifest directly or nested") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        CHECK(resolve_shard_set_root(dir.path().string()).empty());

        const std::string nested =
            (dir.path() / std::string(SHARD_SET_DIRNAME)).string();
        write_shard_manifest(nested, sample());
        CHECK(resolve_shard_set_root(dir.path().string()) == nested);

        // A manifest directly at the target takes precedence over the nested
        // one.
        write_shard_manifest(dir.path().string(), sample());
        CHECK(resolve_shard_set_root(dir.path().string()) ==
              dir.path().string());
    }

    TEST_CASE("malformed JSON throws") {
        CHECK_THROWS_AS(parse_manifest("{ not json"), DFTUtilsException);
    }

    TEST_CASE("a shard entry missing its path throws") {
        CHECK_THROWS_AS(
            parse_manifest(
                R"({"schema_version":8,"shards":[{"num_files":1}]})"),
            DFTUtilsException);
    }
}
