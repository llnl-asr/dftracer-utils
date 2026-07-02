#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using dftracer::utils::utilities::indexer::ChunkStatistics;
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

        {
            auto writer = db1.begin_write();
            writer->init_schema();
            writer->commit();
        }
        {
            auto writer = db2.begin_write();
            writer->init_schema();
            writer->commit();
        }

        int id1;
        {
            auto writer = db1.begin_write();
            id1 = writer->get_or_create_file_info("a.pfw.gz", 0x1111);
            writer->commit();
        }
        int id2 = db2.get_file_info_id("a.pfw.gz");

        CHECK(id1 > 0);
        CHECK(id1 == id2);
    }

    TEST_CASE("rebuild clears per-file bloom and manifest data before reuse") {
        auto root = dft_utils_test::make_unique_test_path("idx_rebuild");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();

            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xAAAA);

            std::vector<unsigned char> blob = {0xDE, 0xAD, 0xBE, 0xEF};
            writer->insert_chunk_bloom_filter(file_id, 0, "name",
                                              std::span(blob), 4);
            writer->insert_file_bloom_filter(file_id, "name", std::span(blob),
                                             4);
            writer->insert_index_dimension(file_id, "name");
            writer->insert_hash_table_entry(0, "hashA", "resolvedA");
            writer->insert_event_range(file_id, 0, "POSIX", "read",
                                       std::vector<std::uint32_t>{1, 2, 3});
            writer->insert_metadata_lines(file_id, 0, "HH",
                                          std::vector<std::uint32_t>{0, 4});
            writer->commit();
        }

        CHECK(db.has_bloom_data(file_id));
        CHECK(db.has_manifest_data(file_id));
        CHECK(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK(db.resolve_hash(IndexDatabase::HashType::FILE, "hashA")
                  .has_value());

        int rebuilt_id;
        {
            auto writer = db.begin_write();
            rebuilt_id =
                writer->get_or_create_file_info("trace.pfw.gz", 0xBBBB);
            writer->commit();
        }
        CHECK(rebuilt_id == file_id);

        CHECK_FALSE(db.has_bloom_data(file_id));
        CHECK_FALSE(db.has_manifest_data(file_id));
        CHECK_FALSE(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK(db.query_chunk_bloom_filters(file_id, "name").empty());
        CHECK(db.query_event_ranges(file_id).empty());
        CHECK(db.query_metadata_lines(file_id).empty());
        CHECK(db.resolve_hash(IndexDatabase::HashType::FILE, "hashA")
                  .has_value());
    }

    TEST_CASE("writer context batches multiple files and all are readable") {
        auto root = dft_utils_test::make_unique_test_path("idx_writer_ctx");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();

        static constexpr int NUM_FILES = 100;
        static constexpr int BATCH_SIZE = 10;

        // Create file IDs first
        std::vector<int> file_ids;
        {
            auto writer = db.begin_write();
            for (int i = 0; i < NUM_FILES; ++i) {
                auto name = "file_" + std::to_string(i) + ".pfw.gz";
                int fid = writer->get_or_create_file_info(name, i + 1);
                file_ids.push_back(fid);
            }
            writer->commit();
        }
        CHECK(file_ids.size() == NUM_FILES);

        // Write scalar stats in batches
        for (int batch_start = 0; batch_start < NUM_FILES;
             batch_start += BATCH_SIZE) {
            auto writer = db.begin_write();
            int batch_end = std::min(batch_start + BATCH_SIZE, NUM_FILES);
            for (int i = batch_start; i < batch_end; ++i) {
                ChunkStatistics stats;
                stats.total_events = static_cast<std::uint64_t>(i + 1) * 100;
                writer->insert_file_scalar_stats(file_ids[i], stats, 1);
            }
            writer->commit();
        }

        // Verify ALL data is readable
        auto results = db.query_file_scalar_stats_batch(file_ids);
        CHECK(results.size() == NUM_FILES);

        std::uint64_t total_events = 0;
        for (int i = 0; i < NUM_FILES; ++i) {
            auto it = results.find(file_ids[i]);
            REQUIRE(it != results.end());
            CHECK(it->second.stats.total_events ==
                  static_cast<std::uint64_t>(i + 1) * 100);
            total_events += it->second.stats.total_events;
        }
        CHECK(total_events == 505000);  // sum of 100+200+...+10000
    }

    TEST_CASE("PID manifest - insert and query single file PIDs") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_single");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xAAAA);

            std::unordered_set<std::uint64_t> pids = {1234, 5678, 9012};
            writer->insert_file_pids(file_id, pids);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(1234) == 1);
        CHECK(result.count(5678) == 1);
        CHECK(result.count(9012) == 1);
    }

    TEST_CASE("PID manifest - query non-existent file returns empty set") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_empty");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();

        auto result = db.query_file_pids(999);
        CHECK(result.empty());
    }

    TEST_CASE("PID manifest - query all file PIDs") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_all");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id1, file_id2, file_id3;
        {
            auto writer = db.begin_write();
            writer->init_schema();

            file_id1 = writer->get_or_create_file_info("trace1.pfw.gz", 0xAAA1);
            file_id2 = writer->get_or_create_file_info("trace2.pfw.gz", 0xAAA2);
            file_id3 = writer->get_or_create_file_info("trace3.pfw.gz", 0xAAA3);

            writer->insert_file_pids(file_id1, {1000, 1001});
            writer->insert_file_pids(file_id2, {1000, 2000, 2001});
            writer->insert_file_pids(file_id3, {3000});
            writer->commit();
        }

        auto all_pids = db.query_all_file_pids();
        CHECK(all_pids.size() == 3);

        CHECK(all_pids[file_id1].size() == 2);
        CHECK(all_pids[file_id1].count(1000) == 1);
        CHECK(all_pids[file_id1].count(1001) == 1);

        CHECK(all_pids[file_id2].size() == 3);
        CHECK(all_pids[file_id2].count(1000) == 1);
        CHECK(all_pids[file_id2].count(2000) == 1);
        CHECK(all_pids[file_id2].count(2001) == 1);

        CHECK(all_pids[file_id3].size() == 1);
        CHECK(all_pids[file_id3].count(3000) == 1);
    }

    TEST_CASE("PID manifest - large PIDs") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_large");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xBBBB);

            // Use large PID values to test varint encoding
            std::unordered_set<std::uint64_t> pids = {
                0xFFFFFFFFULL,         // 32-bit max
                0x100000000ULL,        // Just over 32-bit
                0xFFFFFFFFFFFFFFFFULL  // 64-bit max
            };
            writer->insert_file_pids(file_id, pids);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(0xFFFFFFFFULL) == 1);
        CHECK(result.count(0x100000000ULL) == 1);
        CHECK(result.count(0xFFFFFFFFFFFFFFFFULL) == 1);
    }

    TEST_CASE("PID manifest - empty PID set not stored") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_empty_set");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xCCCC);

            std::unordered_set<std::uint64_t> empty_pids;
            writer->insert_file_pids(file_id, empty_pids);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.empty());
    }

    TEST_CASE("PID manifest - rebuild clears PIDs") {
        auto root = dft_utils_test::make_unique_test_path("idx_pid_rebuild");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xDDDD);
            writer->insert_file_pids(file_id, {1234, 5678});
            writer->commit();
        }

        CHECK(db.query_file_pids(file_id).size() == 2);

        // Rebuild with new checksum clears data
        {
            auto writer = db.begin_write();
            int rebuilt_id =
                writer->get_or_create_file_info("trace.pfw.gz", 0xEEEE);
            writer->commit();
            CHECK(rebuilt_id == file_id);
        }

        CHECK(db.query_file_pids(file_id).empty());
    }
}
