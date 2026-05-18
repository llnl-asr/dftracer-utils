#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_query_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

// Helper to set up a .idx database with test data
static void populate_test_idx(const std::string& index_path,
                              const std::string& file_path) {
    IndexDatabase idx_db(index_path);
    auto writer = idx_db.begin_write();
    writer->init_schema();

    int fid =
        writer->get_or_create_file_info(get_logical_path(file_path), 12345);

    for (int ckpt = 0; ckpt < 3; ++ckpt) {
        BloomFilter name_bloom(100, 0.01);
        if (ckpt == 0) {
            name_bloom.add("read");
            name_bloom.add("write");
        } else if (ckpt == 1) {
            name_bloom.add("open");
            name_bloom.add("close");
        } else {
            name_bloom.add("read");
            name_bloom.add("stat");
        }

        auto blob = name_bloom.serialize();
        writer->insert_chunk_bloom_filter(
            fid, static_cast<std::uint64_t>(ckpt), "name", blob.data(),
            static_cast<int>(blob.size()), name_bloom.num_entries());

        BloomFilter cat_bloom(100, 0.01);
        if (ckpt == 0 || ckpt == 2) {
            cat_bloom.add("POSIX");
        } else {
            cat_bloom.add("storage");
        }

        auto cat_blob = cat_bloom.serialize();
        writer->insert_chunk_bloom_filter(
            fid, static_cast<std::uint64_t>(ckpt), "cat", cat_blob.data(),
            static_cast<int>(cat_blob.size()), cat_bloom.num_entries());
    }

    BloomFilter file_name_bloom(100, 0.01);
    file_name_bloom.add("read");
    file_name_bloom.add("write");
    file_name_bloom.add("open");
    file_name_bloom.add("close");
    file_name_bloom.add("stat");
    auto name_blob = file_name_bloom.serialize();
    writer->insert_file_bloom_filter(fid, "name", name_blob.data(),
                                     static_cast<int>(name_blob.size()),
                                     file_name_bloom.num_entries());

    BloomFilter file_cat_bloom(100, 0.01);
    file_cat_bloom.add("POSIX");
    file_cat_bloom.add("storage");
    auto cat_blob = file_cat_bloom.serialize();
    writer->insert_file_bloom_filter(fid, "cat", cat_blob.data(),
                                     static_cast<int>(cat_blob.size()),
                                     file_cat_bloom.num_entries());

    BloomFilter fhash_bloom(100, 0.01);
    fhash_bloom.add("abc123");
    auto fhash_blob = fhash_bloom.serialize();
    writer->insert_file_bloom_filter(fid, "fhash", fhash_blob.data(),
                                     static_cast<int>(fhash_blob.size()),
                                     fhash_bloom.num_entries());

    for (int ckpt = 0; ckpt < 3; ++ckpt) {
        auto blob = fhash_bloom.serialize();
        writer->insert_chunk_bloom_filter(
            fid, static_cast<std::uint64_t>(ckpt), "fhash", blob.data(),
            static_cast<int>(blob.size()), fhash_bloom.num_entries());
    }

    writer->insert_hash_table_entry(0, "abc123", "./data/file.h5");

    writer->insert_index_dimension(fid, "name");
    writer->insert_index_dimension(fid, "cat");
    writer->insert_index_dimension(fid, "fhash");
    writer->commit();
}

TEST_SUITE("BloomQueryUtility") {
    TEST_CASE("BloomQuery - File-level skip for absent value") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query").string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        BloomQueryInput input;
        input.with_index_path(index_path)
            .with_file_path(file_path)
            .with_predicate("name", {"nonexistent_operation"});

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        CHECK(output.file_may_match == false);
        CHECK(output.candidate_checkpoints.empty());

        fs::remove_all(test_dir);
    }

    TEST_CASE("BloomQuery - Chunk-level filtering") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query_chunk")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        BloomQueryInput input;
        input.with_index_path(index_path)
            .with_file_path(file_path)
            .with_predicate("name", {"read"});

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        CHECK(output.file_may_match == true);
        // "read" is in checkpoint 0 and 2
        CHECK(output.candidate_checkpoints.size() >= 2);
        CHECK(output.total_checkpoints == 3);

        fs::remove_all(test_dir);
    }

    TEST_CASE("BloomQuery - Multi-dimension AND") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query_and")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        BloomQueryInput input;
        input.with_index_path(index_path)
            .with_file_path(file_path)
            .with_predicate("name", {"open"})
            .with_predicate("cat", {"storage"});

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        // "open" is in checkpoint 1, "storage" is in checkpoint 1
        // AND: checkpoint 1
        CHECK(output.file_may_match == true);
        CHECK(output.candidate_checkpoints.size() >= 1);

        fs::remove_all(test_dir);
    }

    TEST_CASE("BloomQuery - Empty predicates returns all") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query_empty")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        BloomQueryInput input;
        input.with_index_path(index_path).with_file_path(file_path);

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        CHECK(output.file_may_match == true);

        fs::remove_all(test_dir);
    }

    TEST_CASE("BloomQuery - Hash resolution (query by resolved value)") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query_hash")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // Query by resolved value (not hash)
        BloomQueryInput input;
        input.with_index_path(index_path)
            .with_file_path(file_path)
            .with_predicate("fhash", {"./data/file.h5"});

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        CHECK(output.file_may_match == true);
        // All 3 checkpoints have the fhash
        CHECK(output.candidate_checkpoints.size() == 3);

        fs::remove_all(test_dir);
    }

    TEST_CASE("BloomQuery - OR within dimension") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_bloom_query_or")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        BloomQueryInput input;
        input.with_index_path(index_path)
            .with_file_path(file_path)
            .with_predicate("name", {"read", "open"});

        BloomQueryUtility query;
        auto output = query.process(input).get();

        CHECK(output.success == true);
        CHECK(output.file_may_match == true);
        // read is in 0,2; open is in 1 => union is 0,1,2
        CHECK(output.candidate_checkpoints.size() == 3);

        fs::remove_all(test_dir);
    }
}
