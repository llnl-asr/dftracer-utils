// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

namespace {

index::store::IndexShardManifest sample() {
    index::store::IndexShardManifest m;
    m.format_version = 8;
    m.shards.push_back({"shard-0000.dftindex", 128, 10482375});
    m.shards.push_back({"shard-0001.dftindex", 73, 512});
    return m;
}

}  // namespace

TEST_SUITE("shard_manifest") {
    TEST_CASE("round-trips through JSON") {
        index::store::IndexShardManifest in = sample();
        const dstd::String json = index::store::to_json(in);
        CHECK(json.find("file_id") == dstd::String::npos);
        index::store::IndexShardManifest out =
            index::store::parse_manifest(json);

        CHECK(out.format_version == in.format_version);
        REQUIRE(out.shards.size() == in.shards.size());
        for (std::size_t i = 0; i < in.shards.size(); ++i) {
            CHECK(out.shards[i].path == in.shards[i].path);
            CHECK(out.shards[i].num_files == in.shards[i].num_files);
            CHECK(out.shards[i].num_events == in.shards[i].num_events);
        }
    }

    TEST_CASE("an empty manifest round-trips") {
        index::store::IndexShardManifest in;
        in.format_version = 8;
        index::store::IndexShardManifest out =
            index::store::parse_manifest(index::store::to_json(in));
        CHECK(out.format_version == 8);
        CHECK(out.shards.empty());
    }

    TEST_CASE("write then read from a directory") {
        dftu_utils_test::ScopedTestDir dir("dftu_shard_manifest_test");
        index::store::write_shard_manifest(dstd::String(dir.path().str()),
                                           sample());
        auto loaded =
            index::store::read_shard_manifest(dstd::String(dir.path().str()));
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 2);
        CHECK(loaded->shards[1].num_files == 73);
        CHECK(loaded->shards[1].num_events == 512);
    }

    TEST_CASE("reading a directory with no manifest returns nullopt") {
        dftu_utils_test::ScopedTestDir dir("dftu_shard_manifest_test");
        CHECK_FALSE(
            index::store::read_shard_manifest(dstd::String(dir.path().str()))
                .has_value());
    }

    // The publish is a rename over any prior manifest; a reader that opens the
    // file at any instant sees a complete manifest, never a partial one.
    TEST_CASE("write replaces a prior manifest atomically") {
        dftu_utils_test::ScopedTestDir dir("dftu_shard_manifest_test");
        index::store::write_shard_manifest(dstd::String(dir.path().str()),
                                           sample());

        index::store::IndexShardManifest second;
        second.format_version = 8;
        second.shards.push_back({"only.dftindex", 1, 1});
        index::store::write_shard_manifest(dstd::String(dir.path().str()),
                                           second);

        auto loaded =
            index::store::read_shard_manifest(dstd::String(dir.path().str()));
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->shards.size() == 1);
        CHECK(loaded->shards[0].path == "only.dftindex");
    }

    TEST_CASE("a path with quotes and control chars survives escaping") {
        index::store::IndexShardManifest in;
        in.format_version = 8;
        in.shards.push_back({"weird\"\\\n\tname.dftindex", 1, 1});
        index::store::IndexShardManifest out =
            index::store::parse_manifest(index::store::to_json(in));
        REQUIRE(out.shards.size() == 1);
        CHECK(out.shards[0].path == "weird\"\\\n\tname.dftindex");
    }

    TEST_CASE("resolve_shard_set_root names the root holding the manifest") {
        dftu_utils_test::ScopedTestDir dir("dftu_shard_manifest_test");
        CHECK(
            index::store::resolve_shard_set_root(dstd::String(dir.path().str()))
                .empty());

        const dstd::String root = dstd::String(
            (dir.path() / dstd::String(dftracer::utils::index::INDEX_ROOT_NAME))
                .str());
        index::store::write_shard_manifest(root, sample());
        CHECK(index::store::resolve_shard_set_root(
                  dstd::String(dir.path().str())) == root);
        CHECK(index::store::resolve_shard_set_root(root) == root);
        CHECK(dstd::fs::exists(index::shard_manifest_path(root)).value());
        CHECK_FALSE(
            dstd::fs::exists(dir.path() / dstd::String(".dftindex-shards"))
                .value());
    }

    TEST_CASE("a manifest with no format version has none, not 0") {
        auto m = index::store::parse_manifest(R"({"shards":[]})");
        CHECK_FALSE(m.format_version.has_value());
        auto written =
            index::store::parse_manifest(index::store::to_json(sample()));
        REQUIRE(written.format_version.has_value());
        CHECK(*written.format_version == 8);
    }

    TEST_CASE("malformed JSON throws") {
        CHECK_THROWS_AS(index::store::parse_manifest("{ not json"),
                        dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("a shard entry missing its path throws") {
        CHECK_THROWS_AS(
            index::store::parse_manifest(
                R"({"format_version":8,"shards":[{"num_files":1}]})"),
            dftracer::utils::common::DFTUtilsException);
    }
}
