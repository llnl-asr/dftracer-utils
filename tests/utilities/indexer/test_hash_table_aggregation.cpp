#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils;
using dftracer::utils::trace::aggregators::AggregationConfig;
using dftracer::utils::trace::indexing::resolve_and_build_index;
using dftracer::utils::trace::indexing::ResolveAndBuildInput;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftu_utils_test::run_coro;

namespace {

void write_single_member(const std::string& path, const std::string& content) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

// A trace with one FH metadata entry (hash "f00d" -> a file path) plus data
// events referencing it, so the file hash table has something to resolve.
std::string make_trace_with_fh(int n) {
    std::string s = "[\n";
    s +=
        R"({"id":1,"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"h1","name":"node0","value":"h1"}})";
    s += "\n";
    s +=
        R"({"id":2,"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"hhash":"h1","name":"/scratch/data/x.bin","value":"f00d"}})";
    s += "\n";
    for (int i = 0; i < n; ++i) {
        s +=
            R"({"id":)" + std::to_string(i + 3) +
            R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
            std::to_string(1000 + i) +
            R"(,"dur":100,"ph":"X","args":{"hhash":"h1","fhash":"f00d","ret":4096}})";
        s += "\n";
    }
    return s;
}

}  // namespace

TEST_SUITE("hash_table_aggregation") {
    // The aggregation-only build (no bloom) that dfanalyzer uses
    // must still populate the file hash table, or fhash -> file_name never
    // resolves.
    TEST_CASE("aggregation build populates the file hash table") {
        auto dir = dftu_utils_test::make_unique_test_path("hashagg");
        fs::create_directories(dir);
        const std::string gz = (dir / "trace.pfw.gz").string();
        const std::string index_dir = (dir / "idx").string();
        write_single_member(gz, make_trace_with_fh(50));

        ResolveAndBuildInput input;
        input.directory = dir.string();
        input.index_dir = index_dir;
        input.require_checkpoints = true;
        input.require_bloom = false;
        input.build_bloom = false;
        input.require_aggregation = true;
        input.aggregation_config = AggregationConfig{};

        std::string index_path;
        run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
            auto res = co_await resolve_and_build_index(&scope, input);
            index_path = res.index_path;
            co_return;
        });
        REQUIRE_FALSE(index_path.empty());

        IndexDatabase db(
            index_path,
            ::dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
        auto file_hashes = db.query_hash_table(IndexDatabase::HashType::FILE);
        CHECK_FALSE(file_hashes.empty());
        auto it = file_hashes.find("f00d");
        REQUIRE(it != file_hashes.end());
        CHECK(it->second == "/scratch/data/x.bin");
    }
}
