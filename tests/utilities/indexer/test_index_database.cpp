#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

namespace fs = std::filesystem;
using dftracer::utils::utilities::indexer::IndexDatabase;

TEST_SUITE("IndexDatabase") {
    TEST_CASE("normalizes legacy .idx-style input to root-local .dftindex") {
        auto root = dft_utils_test::make_unique_test_path("idx_root");
        fs::create_directories(root);
        auto legacy_like = (root / "trace.pfw.gz.idx").string();

        IndexDatabase db(legacy_like);
        CHECK(fs::exists(root / ".dftindex"));
    }

    TEST_CASE("file registry is shared within one .dftindex root") {
        auto root = dft_utils_test::make_unique_test_path("idx_shared");
        fs::create_directories(root);

        IndexDatabase db1((root / ".dftindex").string());
        IndexDatabase db2((root / "other-name.idx").string());

        db1.init_base_schema();
        db2.init_base_schema();

        int id1 = db1.get_or_create_file_info("a.pfw.gz", 0x1111);
        int id2 = db2.get_file_info_id("a.pfw.gz");

        CHECK(id1 > 0);
        CHECK(id1 == id2);
    }

    TEST_CASE("rebuild clears per-file bloom and manifest data before reuse") {
        auto root = dft_utils_test::make_unique_test_path("idx_rebuild");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_base_schema();
        db.init_bloom_schema();
        db.init_manifest_schema();

        const int file_id = db.get_or_create_file_info("trace.pfw.gz", 0xAAAA);

        std::vector<unsigned char> blob = {0xDE, 0xAD, 0xBE, 0xEF};
        db.insert_chunk_bloom_filter(file_id, 0, "name", std::span(blob), 4);
        db.insert_file_bloom_filter(file_id, "name", std::span(blob), 4);
        db.insert_index_dimension(file_id, "name");
        db.insert_hash_resolution(file_id, "fhash", "hashA", "resolvedA");
        db.insert_event_range(file_id, 0, "POSIX", "read",
                              std::vector<std::uint32_t>{1, 2, 3});
        db.insert_metadata_lines(file_id, 0, "HH",
                                 std::vector<std::uint32_t>{0, 4});

        CHECK(db.has_bloom_data(file_id));
        CHECK(db.has_manifest_data(file_id));
        CHECK(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK(db.query_resolved_by_hash("fhash", "hashA").has_value());

        const int rebuilt_id =
            db.get_or_create_file_info("trace.pfw.gz", 0xBBBB);
        CHECK(rebuilt_id == file_id);

        CHECK_FALSE(db.has_bloom_data(file_id));
        CHECK_FALSE(db.has_manifest_data(file_id));
        CHECK_FALSE(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK(db.query_chunk_bloom_filters(file_id, "name").empty());
        CHECK(db.query_event_ranges(file_id).empty());
        CHECK(db.query_metadata_lines(file_id).empty());
        CHECK_FALSE(db.query_resolved_by_hash("fhash", "hashA").has_value());
    }

    TEST_CASE("rollback discards transactional writes") {
        auto root = dft_utils_test::make_unique_test_path("idx_rollback");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_base_schema();
        db.init_bloom_schema();

        const int file_id = db.get_or_create_file_info("trace.pfw.gz", 0xAAAA);
        std::vector<unsigned char> blob = {0xAB, 0xCD};

        db.begin_transaction();
        db.insert_file_bloom_filter(file_id, "name", std::span(blob), 2);
        db.insert_hash_resolution(file_id, "fhash", "hashA", "resolvedA");
        db.rollback_transaction();

        CHECK_FALSE(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK_FALSE(db.query_resolved_by_hash("fhash", "hashA").has_value());
    }
}
