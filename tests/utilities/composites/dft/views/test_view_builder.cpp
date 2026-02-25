#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <doctest/doctest.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::views;
using namespace dftracer::utils::utilities::composites::dft::indexing;

// Helper: create a .bidx with 4 checkpoints
// Checkpoint layout:
//   0: name={read,write}, cat={POSIX}
//   1: name={open,close}, cat={POSIX}
//   2: name={train}, cat={compute}
//   3: name={forward}, cat={compute,ai_framework}
static void populate_test_bidx(const std::string& bidx_path,
                               const std::string& file_path) {
    BloomIndexDatabase bidx(bidx_path);
    bidx.init_schema();

    int fid = bidx.get_or_create_file_info(file_path, 40000);

    bidx.begin_transaction();

    struct ChunkDims {
        std::vector<std::string> names;
        std::vector<std::string> cats;
    };

    ChunkDims chunks[] = {
        {{"read", "write"}, {"POSIX"}},
        {{"open", "close"}, {"POSIX"}},
        {{"train"}, {"compute"}},
        {{"forward"}, {"compute", "ai_framework"}},
    };

    // File-level blooms (union of all chunks)
    BloomFilter file_name_bloom(100, 0.01);
    BloomFilter file_cat_bloom(100, 0.01);

    for (int ckpt = 0; ckpt < 4; ++ckpt) {
        BloomFilter name_bloom(100, 0.01);
        for (const auto& n : chunks[ckpt].names) {
            name_bloom.add(n);
            file_name_bloom.add(n);
        }
        auto name_blob = name_bloom.serialize();
        queries::insert_chunk_bloom_filter(
            bidx.db(), fid, static_cast<std::uint64_t>(ckpt), "name",
            name_blob.data(), static_cast<int>(name_blob.size()),
            name_bloom.num_entries());

        BloomFilter cat_bloom(100, 0.01);
        for (const auto& c : chunks[ckpt].cats) {
            cat_bloom.add(c);
            file_cat_bloom.add(c);
        }
        auto cat_blob = cat_bloom.serialize();
        queries::insert_chunk_bloom_filter(
            bidx.db(), fid, static_cast<std::uint64_t>(ckpt), "cat",
            cat_blob.data(), static_cast<int>(cat_blob.size()),
            cat_bloom.num_entries());
    }

    // File-level bloom filters
    auto name_blob = file_name_bloom.serialize();
    queries::insert_file_bloom_filter(bidx.db(), fid, "name", name_blob.data(),
                                      static_cast<int>(name_blob.size()),
                                      file_name_bloom.num_entries());

    auto cat_blob = file_cat_bloom.serialize();
    queries::insert_file_bloom_filter(bidx.db(), fid, "cat", cat_blob.data(),
                                      static_cast<int>(cat_blob.size()),
                                      file_cat_bloom.num_entries());

    queries::insert_index_dimension(bidx.db(), fid, "name");
    queries::insert_index_dimension(bidx.db(), fid, "cat");

    bidx.commit_transaction();
}

TEST_SUITE("ViewBuilderUtility") {
    TEST_CASE("ViewBuilder - IO view filters to POSIX chunks") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_view_builder_io")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        ViewBuilderInput input;
        input.with_view(ViewDefinition::io_view())
            .with_file_path(file_path)
            .with_bidx_path(bidx_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.file_may_match);
        CHECK(output.total_checkpoints == 4);
        // IO view matches POSIX (checkpoints 0, 1) and names like read/write
        // (0, 1)
        CHECK(output.candidates.size() >= 2);
        CHECK(output.skipped_checkpoints >= 2);

        // Verify byte ranges are computed
        for (const auto& c : output.candidates) {
            CHECK(c.end_byte > c.start_byte);
        }

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewBuilder - Compute view filters to compute chunks") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_view_builder_compute")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        ViewBuilderInput input;
        input.with_view(ViewDefinition::compute_view())
            .with_file_path(file_path)
            .with_bidx_path(bidx_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.file_may_match);
        // Compute view: cat={compute, comm, device, ai_framework, ai_root}
        // Matches checkpoints 2 and 3
        CHECK(output.candidates.size() >= 2);
        CHECK(output.skipped_checkpoints >= 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewBuilder - File-level skip for absent values") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_view_builder_skip")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        ViewDefinition view;
        view.with_name("nonexistent");
        ViewPredicate pred;
        pred.with_bloom_dim("name", {"nonexistent_op"});
        view.with_predicate(std::move(pred));

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_bidx_path(bidx_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK_FALSE(output.file_may_match);
        CHECK(output.candidates.empty());
        CHECK(output.skipped_checkpoints == 4);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewBuilder - No bloom predicates returns all chunks") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_view_builder_nobl")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        // View with only time_range, no bloom dims
        ViewDefinition view;
        view.with_name("time_only");
        ViewPredicate pred;
        pred.with_time_range(0.0, 100000.0);
        view.with_predicate(std::move(pred));

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_bidx_path(bidx_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.file_may_match);
        CHECK(output.candidates.size() == 4);
        CHECK(output.skipped_checkpoints == 0);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewBuilder - No bidx path returns all chunks") {
        ViewDefinition view;
        view.with_name("no_bidx");
        ViewPredicate pred;
        pred.with_bloom_dim("name", {"read"});
        view.with_predicate(std::move(pred));

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path("/fake/file.pfw.gz")
            .with_bidx_path("")  // No bloom index
            .with_uncompressed_size(30000)
            .with_num_checkpoints(3);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.file_may_match);
        CHECK(output.candidates.size() == 3);
        CHECK(output.skipped_checkpoints == 0);
    }

    TEST_CASE("ViewBuilder - Byte range computation") {
        // No bidx so all chunks are returned -- verify byte ranges
        ViewDefinition view;
        view.with_name("byte_range_test");

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path("/fake/file.pfw.gz")
            .with_bidx_path("")
            .with_uncompressed_size(12000)
            .with_num_checkpoints(3);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        REQUIRE(output.candidates.size() == 3);

        // 12000 / 3 = 4000 bytes per checkpoint
        CHECK(output.candidates[0].checkpoint_idx == 0);
        CHECK(output.candidates[0].start_byte == 0);
        CHECK(output.candidates[0].end_byte == 4000);

        CHECK(output.candidates[1].checkpoint_idx == 1);
        CHECK(output.candidates[1].start_byte == 4000);
        CHECK(output.candidates[1].end_byte == 8000);

        // Last checkpoint covers remainder
        CHECK(output.candidates[2].checkpoint_idx == 2);
        CHECK(output.candidates[2].start_byte == 8000);
        CHECK(output.candidates[2].end_byte == 12000);
    }

    TEST_CASE("ViewBuilder - Zero checkpoints defaults to 1") {
        ViewDefinition view;
        view.with_name("zero_ckpt");

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path("/fake/file.pfw.gz")
            .with_bidx_path("")
            .with_uncompressed_size(10000)
            .with_num_checkpoints(0);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.total_checkpoints == 1);
        REQUIRE(output.candidates.size() == 1);
        CHECK(output.candidates[0].start_byte == 0);
        CHECK(output.candidates[0].end_byte == 10000);
    }

    TEST_CASE("ViewBuilder - Dimension alias resolution") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_view_builder_alias")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";

        // Create bidx with fhash dimension
        BloomIndexDatabase bidx(bidx_path);
        bidx.init_schema();
        int fid = bidx.get_or_create_file_info(file_path, 10000);
        bidx.begin_transaction();

        BloomFilter fhash_bloom(100, 0.01);
        fhash_bloom.add("hash123");
        auto blob = fhash_bloom.serialize();

        queries::insert_file_bloom_filter(bidx.db(), fid, "fhash", blob.data(),
                                          static_cast<int>(blob.size()),
                                          fhash_bloom.num_entries());
        queries::insert_chunk_bloom_filter(
            bidx.db(), fid, 0, "fhash", blob.data(),
            static_cast<int>(blob.size()), fhash_bloom.num_entries());
        queries::insert_index_dimension(bidx.db(), fid, "fhash");
        queries::insert_hash_resolution(bidx.db(), fid, "fhash", "hash123",
                                        "/data/file.h5");
        bidx.commit_transaction();

        // Use "file" alias which should resolve to "fhash"
        ViewDefinition view;
        view.with_name("alias_test");
        ViewPredicate pred;
        pred.with_bloom_dim("file", {"/data/file.h5"});
        view.with_predicate(std::move(pred));

        ViewBuilderInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_bidx_path(bidx_path)
            .with_uncompressed_size(10000)
            .with_num_checkpoints(1);

        ViewBuilderUtility builder;
        auto output = builder.process(input).get();

        CHECK(output.success);
        CHECK(output.file_may_match);
        CHECK(output.candidates.size() == 1);

        fs::remove_all(test_dir);
    }
}
