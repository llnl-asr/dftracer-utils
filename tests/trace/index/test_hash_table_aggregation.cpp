// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/resolve_and_build.h>
#include <dftracer/utils/trace/index/plan/view_resolver.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

namespace {

void write_single_member(dstd::CStringView path, dstd::StringView content) {
    fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    dstd::String ofs;
    ofs.append(reinterpret_cast<const char*>(member->data()), member->size());
    dstd::fs::write_file(dstd::PathView(path), ofs).discard();
}

// A trace with one FH metadata entry (hash "f00d" -> a file path) plus data
// events referencing it, so the file dictionary has something to resolve.
dstd::String make_trace_with_fh(int n) {
    dstd::String s = "[\n";
    s +=
        R"({"id":1,"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":4,"args":{"hhash":"h1","name":"node0","value":"h1"}})";
    s += "\n";
    s +=
        R"({"id":2,"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":4,"args":{"hhash":"h1","name":"/scratch/data/x.bin","value":"f00d"}})";
    s += "\n";
    for (int i = 0; i < n; ++i) {
        s +=
            R"({"id":)" + dstd::to_string(i + 3) +
            R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
            dstd::to_string(1000 + i) +
            R"(,"dur":100,"ph":1,"args":{"hhash":"h1","fhash":"f00d","ret":4096}})";
        s += "\n";
    }
    return s;
}

}  // namespace

TEST_SUITE("hash_table_aggregation") {
    // A build that keeps only the aggregation tier (no bloom) must still
    // populate the file dictionary, or fhash -> file_name never resolves.
    TEST_CASE("aggregation build populates the file hash table") {
        auto dir = dftu_utils_test::make_unique_test_path("hashagg");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String gz = dstd::String((dir / "trace.pfw.gz").str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_single_member(gz, make_trace_with_fh(50));

        trace::index::build::ResolveAndBuildInput input;
        input.directory = dir.str();
        input.index_dir = index_dir;
        input.require_checkpoints = true;
        input.require_bloom = false;
        input.build_bloom = false;

        dstd::String index_path;
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            auto res =
                co_await trace::index::build::resolve_and_build_index(input);
            if (res) index_path = res->index_path;
            co_return;
        });
        REQUIRE_FALSE(index_path.empty());

        using ::dftracer::utils::trace::index::plan::GroupResolver;
        const dftracer::utils::trace::index::plan::RowSetFile file{gz,
                                                                   index_path};
        const GroupResolver::Column column{"path", "files", "fhash", "path"};
        const GroupResolver files(
            dstd::Span<const dftracer::utils::trace::index::plan::RowSetFile>(
                &file, 1),
            dstd::Span<const GroupResolver::Column>(&column, 1));
        CHECK(files.value("path", "f00d") == "/scratch/data/x.bin");
    }
}
