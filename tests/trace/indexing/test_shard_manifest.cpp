#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>

using dftracer::utils::index::shard_manifest_path;
using dftracer::utils::index::store::IndexShardManifest;
using dftracer::utils::index::store::parse_manifest;
using dftracer::utils::index::store::read_shard_manifest;
using dftracer::utils::index::store::resolve_shard_set_root;
using dftracer::utils::index::store::to_json;
using dftracer::utils::index::store::write_shard_manifest;

namespace {

IndexShardManifest sample() {
    IndexShardManifest m;
    m.format_version = 8;
    m.shards.push_back({"shard-0000.dftindex", 128, 10482375});
    m.shards.push_back({"shard-0001.dftindex", 73, 512});
    return m;
}

using dftu_utils_test::ScopedTestDir;

}  // namespace

TEST_SUITE("shard_manifest") {
    TEST_CASE("round-trips through JSON") {
        IndexShardManifest in = sample();
        const std::string json = to_json(in);
        CHECK(json.find("file_id") == std::string::npos);
        IndexShardManifest out = parse_manifest(json);

        CHECK(out.format_version == in.format_version);
        REQUIRE(out.shards.size() == in.shards.size());
        for (std::size_t i = 0; i < in.shards.size(); ++i) {
            CHECK(out.shards[i].path == in.shards[i].path);
            CHECK(out.shards[i].num_files == in.shards[i].num_files);
            CHECK(out.shards[i].num_events == in.shards[i].num_events);
        }
    }

    TEST_CASE("an empty manifest round-trips") {
        IndexShardManifest in;
        in.format_version = 8;
        IndexShardManifest out = parse_manifest(to_json(in));
        CHECK(out.format_version == 8);
        CHECK(out.shards.empty());
    }

    TEST_CASE("write then read from a directory") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        write_shard_manifest(dir.path().string(), sample());
        auto loaded = read_shard_manifest(dir.path().string());
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 2);
        CHECK(loaded->shards[1].num_files == 73);
        CHECK(loaded->shards[1].num_events == 512);
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
        second.format_version = 8;
        second.shards.push_back({"only.dftindex", 1, 1});
        write_shard_manifest(dir.path().string(), second);

        auto loaded = read_shard_manifest(dir.path().string());
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 1);
        CHECK(loaded->shards[0].path == "only.dftindex");
    }

    TEST_CASE("a path with quotes and control chars survives escaping") {
        IndexShardManifest in;
        in.format_version = 8;
        in.shards.push_back({"weird\"\\\n\tname.dftindex", 1, 1});
        IndexShardManifest out = parse_manifest(to_json(in));
        REQUIRE(out.shards.size() == 1);
        CHECK(out.shards[0].path == "weird\"\\\n\tname.dftindex");
    }

    TEST_CASE("resolve_shard_set_root names the root holding the manifest") {
        ScopedTestDir dir("dftu_shard_manifest_test");
        CHECK(resolve_shard_set_root(dir.path().string()).empty());

        const std::string root =
            (dir.path() / std::string(dftracer::utils::index::INDEX_ROOT_NAME))
                .string();
        write_shard_manifest(root, sample());
        CHECK(resolve_shard_set_root(dir.path().string()) == root);
        CHECK(resolve_shard_set_root(root) == root);
        CHECK(dftracer::utils::common::fs::exists(shard_manifest_path(root)));
        CHECK_FALSE(dftracer::utils::common::fs::exists(
            dir.path() / std::string(".dftindex-shards")));
    }

    TEST_CASE("a manifest with no format version has none, not 0") {
        auto m = parse_manifest(R"({"shards":[]})");
        CHECK_FALSE(m.format_version.has_value());
        auto written = parse_manifest(to_json(sample()));
        REQUIRE(written.format_version.has_value());
        CHECK(*written.format_version == 8);
    }

    TEST_CASE("malformed JSON throws") {
        CHECK_THROWS_AS(parse_manifest("{ not json"),
                        dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("a shard entry missing its path throws") {
        CHECK_THROWS_AS(
            parse_manifest(
                R"({"format_version":8,"shards":[{"num_files":1}]})"),
            dftracer::utils::common::DFTUtilsException);
    }
}
