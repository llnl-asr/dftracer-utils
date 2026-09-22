#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using dftracer::utils::utilities::indexer::ChunkStatistics;
using dftracer::utils::utilities::indexer::ColumnType;
using dftracer::utils::utilities::indexer::IndexDatabase;

TEST_SUITE("IndexDatabase") {
    TEST_CASE("normalizes legacy .idx-style input to root-local .dftindex") {
        auto root = dftu_utils_test::make_unique_test_path("idx_root");
        fs::create_directories(root);
        auto legacy_like = (root / "trace.pfw.gz.idx").string();

        IndexDatabase db(legacy_like);
        CHECK(fs::exists(root / ".dftindex"));
    }

    TEST_CASE("file registry is shared within one .dftindex root") {
        auto root = dftu_utils_test::make_unique_test_path("idx_shared");
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

        const std::string p = (root / "a.pfw.gz").string();
        int id1;
        {
            auto writer = db1.begin_write();
            id1 = writer->get_or_create_file_info(p, 0x1111);
            writer->commit();
        }
        int id2 = db2.get_file_info_id(p);

        CHECK(id1 > 0);
        CHECK(id1 == id2);
    }

    TEST_CASE("rebuild clears per-file bloom data before reuse") {
        auto root = dftu_utils_test::make_unique_test_path("idx_rebuild");
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
            writer->commit();
        }

        CHECK(db.has_bloom_data(file_id));
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
        CHECK_FALSE(db.query_file_bloom_filter(file_id, "name").has_value());
        CHECK(db.query_chunk_bloom_filters(file_id, "name").empty());
        CHECK(db.resolve_hash(IndexDatabase::HashType::FILE, "hashA")
                  .has_value());
    }

    TEST_CASE("writer context batches multiple files and all are readable") {
        auto root = dftu_utils_test::make_unique_test_path("idx_writer_ctx");
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

    // query_file_pids projects the distinct PIDs from the file's pid:tid count
    // map (FILE_PID_TID_COUNTS, written by the bloom/stats pass); same pid
    // across tids collapses to one.
    TEST_CASE("PID query - distinct PIDs from pid:tid counts") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_single");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xAAAA);

            dftracer::utils::StringViewMap<std::uint64_t> counts;
            counts.emplace("1234:1", 3);
            counts.emplace("1234:2", 1);  // same pid, different tid
            counts.emplace("5678:1", 2);
            counts.emplace("9012:7", 5);
            writer->insert_file_pid_tid_counts(file_id, counts);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(1234) == 1);
        CHECK(result.count(5678) == 1);
        CHECK(result.count(9012) == 1);
    }

    TEST_CASE("PID query - non-existent file returns empty set") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_empty");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();

        auto result = db.query_file_pids(999);
        CHECK(result.empty());
    }

    TEST_CASE("PID query - all file PIDs") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_all");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id1, file_id2, file_id3;
        {
            auto writer = db.begin_write();
            writer->init_schema();

            file_id1 = writer->get_or_create_file_info("trace1.pfw.gz", 0xAAA1);
            file_id2 = writer->get_or_create_file_info("trace2.pfw.gz", 0xAAA2);
            file_id3 = writer->get_or_create_file_info("trace3.pfw.gz", 0xAAA3);

            auto pid_tid = [](std::initializer_list<const char*> keys) {
                dftracer::utils::StringViewMap<std::uint64_t> m;
                for (const char* k : keys) m.emplace(k, 1);
                return m;
            };
            writer->insert_file_pid_tid_counts(file_id1,
                                               pid_tid({"1000:1", "1001:1"}));
            writer->insert_file_pid_tid_counts(
                file_id2, pid_tid({"1000:1", "2000:1", "2001:1"}));
            writer->insert_file_pid_tid_counts(file_id3, pid_tid({"3000:1"}));
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

    TEST_CASE("PID query - large PIDs") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_large");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = writer->get_or_create_file_info("trace.pfw.gz", 0xBBBB);

            dftracer::utils::StringViewMap<std::uint64_t> counts;
            counts.emplace("4294967295:1", 1);            // 32-bit max
            counts.emplace("4294967296:1", 1);            // just over 32-bit
            counts.emplace("18446744073709551615:1", 1);  // 64-bit max
            writer->insert_file_pid_tid_counts(file_id, counts);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(0xFFFFFFFFULL) == 1);
        CHECK(result.count(0x100000000ULL) == 1);
        CHECK(result.count(0xFFFFFFFFFFFFFFFFULL) == 1);
    }

    TEST_CASE("column store round-trips and unions across files") {
        auto root = dftu_utils_test::make_unique_test_path("idx_columns");
        fs::create_directories(root);
        IndexDatabase db((root / ".dftindex").string());

        // Empty before anything is written (old-index compatibility path).
        CHECK(db.query_all_columns().empty());

        int f1, f2;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            f1 = writer->get_or_create_file_info("a.pfw.gz", 0x1111);
            f2 = writer->get_or_create_file_info("b.pfw.gz", 0x2222);
            writer->insert_index_dimension(f1, "name");  // "d|" space
            writer->insert_column(f1, "cat", ColumnType::String);
            writer->insert_column(f1, "mhost", ColumnType::Int64);
            // duplicate across files with a wider type: folds to Float64.
            writer->insert_column(f2, "mhost", ColumnType::Float64);
            writer->insert_column(f2, "fhash", ColumnType::String);
            writer->commit();
        }

        auto cols = db.query_all_columns();  // sorted, de-duplicated union
        CHECK(cols == std::vector<std::string>{"cat", "fhash", "mhost"});

        auto typed = db.query_all_column_types();  // name+type, folded
        REQUIRE(typed.size() == 3);
        CHECK(typed[0] ==
              std::pair<std::string, ColumnType>{"cat", ColumnType::String});
        CHECK(typed[1] ==
              std::pair<std::string, ColumnType>{"fhash", ColumnType::String});
        CHECK(typed[2] ==
              std::pair<std::string, ColumnType>{"mhost", ColumnType::Float64});

        // Columns live under "c|" and must not leak into the dimension scan.
        auto dims = db.query_index_dimensions(f1);
        CHECK(std::find(dims.begin(), dims.end(), "name") != dims.end());
        CHECK(std::find(dims.begin(), dims.end(), "cat") == dims.end());
    }
}

namespace {

std::string write_file(const fs::path& path, std::string_view contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    return path.string();
}

}  // namespace

TEST_SUITE("IndexDatabase staleness") {
    TEST_CASE("get_file_stat round-trips stored mtime and size") {
        auto root = dftu_utils_test::make_unique_test_path("stale_stat");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "hello world");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        auto stat = db.get_file_stat(a);
        REQUIRE(stat.has_value());
        CHECK(stat->size == fs::file_size(a));
        CHECK(stat->mtime != 0);
    }

    TEST_CASE("fresh index reports nothing stale") {
        auto root = dftu_utils_test::make_unique_test_path("stale_fresh");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbbbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a, b});

        auto result = db.find_stale_files({a, b});
        CHECK_FALSE(result.stale());
        CHECK(result.changed.empty());
        CHECK(result.added.empty());
        CHECK(result.removed.empty());
    }

    TEST_CASE("size change is detected as changed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_size");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "original");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        write_file(root / "a.pfw", "original plus more bytes");

        auto result = db.find_stale_files({a});
        CHECK(result.stale());
        REQUIRE(result.changed.size() == 1);
        CHECK(result.changed[0] == a);
    }

    TEST_CASE("mtime change with same size is detected as changed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_mtime");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "same-size-content");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        auto bumped = fs::last_write_time(a) + std::chrono::hours(48);
        fs::last_write_time(a, bumped);

        auto result = db.find_stale_files({a});
        REQUIRE(result.changed.size() == 1);
        CHECK(result.changed[0] == a);
    }

    TEST_CASE("newly added file is reported as added") {
        auto root = dftu_utils_test::make_unique_test_path("stale_added");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        auto b = write_file(root / "b.pfw", "bbb");
        auto result = db.find_stale_files({a, b});
        CHECK(result.changed.empty());
        REQUIRE(result.added.size() == 1);
        CHECK(result.added[0] == b);
    }

    TEST_CASE("file removed from disk is reported as removed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_removed");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a, b});

        auto result = db.find_stale_files({a});
        REQUIRE(result.removed.size() == 1);
        CHECK(result.removed[0] == b);
    }

    TEST_CASE("outdated schema forces a full rebuild") {
        auto root = dftu_utils_test::make_unique_test_path("stale_schema");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        db.db()->put("_schema_version",
                     dftracer::utils::rocksdb::KeyCodec::encode_be32(1));

        auto result = db.find_stale_files({a});
        CHECK(result.schema_outdated);
        CHECK(result.stale());
    }

    TEST_CASE("size or mtime change makes a file stale") {
        auto root = dftu_utils_test::make_unique_test_path("fresh_stat");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "original");
        auto b = write_file(root / "b.pfw", "same-size-content");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a, b});

        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Fresh);

        write_file(root / "a.pfw", "original plus more");  // size change
        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Stale);

        auto bumped = fs::last_write_time(b) + std::chrono::hours(48);
        fs::last_write_time(b, bumped);  // mtime change, same size
        CHECK(db.check_freshness(b) == IndexDatabase::Freshness::Stale);
    }

    TEST_CASE("unregistered and schema-stale files are not fresh") {
        auto root = dftu_utils_test::make_unique_test_path("fresh_negatives");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        // Registered file with no changes is fresh; unregistered is stale.
        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Fresh);
        CHECK(db.check_freshness(b) == IndexDatabase::Freshness::Stale);

        db.db()->put("_schema_version",
                     dftracer::utils::rocksdb::KeyCodec::encode_be32(1));
        CHECK(db.check_freshness(a) ==
              IndexDatabase::Freshness::SchemaOutdated);
    }

    TEST_CASE("a pre-v15 (v14) index is detected stale, not misread") {
        auto root = dftu_utils_test::make_unique_test_path("stale_v14");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        db.register_files({a});

        db.db()->put("_schema_version",
                     dftracer::utils::rocksdb::KeyCodec::encode_be32(14));

        CHECK(db.schema_outdated());
        CHECK(db.check_freshness(a) ==
              IndexDatabase::Freshness::SchemaOutdated);
        auto result = db.find_stale_files({a});
        CHECK(result.schema_outdated);
        CHECK(result.stale());
    }
}
