#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <testing_utilities.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using dftracer::utils::utilities::indexer::IndexDatabase;

namespace {

bool table_exists(sqlite3* db, const char* name) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1,
        &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

int row_count(sqlite3* db, const char* table) {
    std::string sql = std::string("SELECT count(*) FROM ") + table + ";";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

}  // namespace

TEST_SUITE("IndexDatabase") {
    TEST_CASE("Create and open database") {
        auto path = dft_utils_test::make_unique_test_path("idx_create");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            CHECK(db.db() != nullptr);
        }

        CHECK(fs::exists(db_path));
        fs::remove(db_path);
    }

    TEST_CASE("init_base_schema creates tables") {
        auto path = dft_utils_test::make_unique_test_path("idx_base");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            CHECK_NOTHROW(db.init_base_schema());

            CHECK(table_exists(db.db(), "files"));
            CHECK(table_exists(db.db(), "checkpoints"));
            CHECK(table_exists(db.db(), "metadata"));
        }

        fs::remove(db_path);
    }

    TEST_CASE("init_bloom_schema creates tables") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            CHECK_NOTHROW(db.init_bloom_schema());

            CHECK(table_exists(db.db(), "chunk_bloom_filters"));
            CHECK(table_exists(db.db(), "file_bloom_filters"));
            CHECK(table_exists(db.db(), "chunk_statistics"));
            CHECK(table_exists(db.db(), "hash_resolutions"));
            CHECK(table_exists(db.db(), "index_dimensions"));
        }

        fs::remove(db_path);
    }

    TEST_CASE("init_manifest_schema creates tables") {
        auto path = dft_utils_test::make_unique_test_path("idx_manifest");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            CHECK_NOTHROW(db.init_manifest_schema());

            CHECK(table_exists(db.db(), "checkpoint_event_ranges"));
            CHECK(table_exists(db.db(), "checkpoint_metadata_lines"));
        }

        fs::remove(db_path);
    }

    TEST_CASE("Additive schema — bloom without base") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom_only");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            CHECK_NOTHROW(db.init_bloom_schema());
            CHECK(table_exists(db.db(), "chunk_bloom_filters"));
            CHECK(table_exists(db.db(), "file_bloom_filters"));
        }

        fs::remove(db_path);
    }

    TEST_CASE("get_or_create_file_info") {
        auto path = dft_utils_test::make_unique_test_path("idx_file_info");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();

            SUBCASE("Insert returns a positive id") {
                int id =
                    db.get_or_create_file_info("/trace/foo.pfw.gz", 0xDEAD);
                CHECK(id > 0);
            }

            SUBCASE("Same path and hash returns same id") {
                int id1 =
                    db.get_or_create_file_info("/trace/bar.pfw.gz", 0xBEEF);
                int id2 =
                    db.get_or_create_file_info("/trace/bar.pfw.gz", 0xBEEF);
                CHECK(id1 == id2);
            }

            SUBCASE("Hash mismatch re-inserts") {
                int id1 =
                    db.get_or_create_file_info("/trace/baz.pfw.gz", 0x1111);
                int id2 =
                    db.get_or_create_file_info("/trace/baz.pfw.gz", 0x2222);
                CHECK(id2 > 0);
                // id may or may not equal id1 — SQLite can reuse rowids
                (void)id1;
            }
        }

        fs::remove(db_path);
    }

    TEST_CASE("get_file_info_id returns -1 for unknown path") {
        auto path = dft_utils_test::make_unique_test_path("idx_unknown");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();

            CHECK(db.get_file_info_id("/nonexistent/path.pfw.gz") == -1);
        }

        fs::remove(db_path);
    }

    TEST_CASE("get_file_info_id returns correct id after insert") {
        auto path = dft_utils_test::make_unique_test_path("idx_lookup");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();

            int inserted =
                db.get_or_create_file_info("/trace/lookup.pfw.gz", 0xABCD);
            int looked_up = db.get_file_info_id("/trace/lookup.pfw.gz");
            CHECK(inserted == looked_up);
        }

        fs::remove(db_path);
    }

    TEST_CASE("has_bloom_data returns false when no data") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom_empty");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            CHECK_FALSE(db.has_bloom_data(1));
        }

        fs::remove(db_path);
    }

    TEST_CASE("has_bloom_data returns true after insert") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom_data");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int file_id =
                db.get_or_create_file_info("/trace/bloom.pfw.gz", 0x1234);

            const char* sql =
                "INSERT INTO chunk_bloom_filters"
                "(file_info_id, checkpoint_idx, dimension, bloom_data,"
                " num_entries)"
                " VALUES(?, 0, 'name', X'DEADBEEF', 1);";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, file_id);
            REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);

            CHECK(db.has_bloom_data(file_id));
            CHECK_FALSE(db.has_bloom_data(file_id + 999));
        }

        fs::remove(db_path);
    }

    TEST_CASE("has_manifest_data returns false when no data") {
        auto path = dft_utils_test::make_unique_test_path("idx_manifest_empty");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_manifest_schema();

            CHECK_FALSE(db.has_manifest_data(1));
        }

        fs::remove(db_path);
    }

    TEST_CASE("has_manifest_data returns true after insert") {
        auto path = dft_utils_test::make_unique_test_path("idx_manifest_data");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_manifest_schema();

            int file_id =
                db.get_or_create_file_info("/trace/manifest.pfw.gz", 0x5678);

            const char* sql =
                "INSERT INTO checkpoint_event_ranges"
                "(file_info_id, checkpoint_idx, cat, name,"
                " line_numbers, event_count)"
                " VALUES(?, 0, 'cat', 'ev', X'01', 1);";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, file_id);
            REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);

            CHECK(db.has_manifest_data(file_id));
            CHECK_FALSE(db.has_manifest_data(file_id + 999));
        }

        fs::remove(db_path);
    }

    TEST_CASE("Transaction commit") {
        auto path = dft_utils_test::make_unique_test_path("idx_txn");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();

            CHECK_NOTHROW(db.begin_transaction());
            db.get_or_create_file_info("/trace/txn_a.pfw.gz", 0xAAAA);
            db.get_or_create_file_info("/trace/txn_b.pfw.gz", 0xBBBB);
            CHECK_NOTHROW(db.commit_transaction());

            CHECK(row_count(db.db(), "files") == 2);
            CHECK(db.get_file_info_id("/trace/txn_a.pfw.gz") > 0);
            CHECK(db.get_file_info_id("/trace/txn_b.pfw.gz") > 0);
        }

        fs::remove(db_path);
    }

    TEST_CASE("Move semantics") {
        auto path = dft_utils_test::make_unique_test_path("idx_move");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase original(db_path);
            original.init_base_schema();
            int id =
                original.get_or_create_file_info("/trace/move.pfw.gz", 0x9999);

            IndexDatabase moved(std::move(original));

            REQUIRE(moved.db() != nullptr);
            CHECK(moved.get_file_info_id("/trace/move.pfw.gz") == id);
        }

        fs::remove(db_path);
    }
}

TEST_SUITE("IndexDatabase - Bloom wrapper methods") {
    TEST_CASE("insert and query chunk bloom filter (span overload)") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/wrap.pfw.gz", 0x1234);

            std::vector<unsigned char> blob = {0xDE, 0xAD, 0xBE, 0xEF};
            db.insert_chunk_bloom_filter(fid, 0, "name", std::span(blob), 42);

            auto results = db.query_chunk_bloom_filters(fid, "name");
            REQUIRE(results.size() == 1);
            CHECK(results[0].checkpoint_idx == 0);
            CHECK(results[0].bloom_data == blob);
            CHECK(results[0].num_entries == 42);
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query chunk bloom filter (void* overload)") {
        auto path = dft_utils_test::make_unique_test_path("idx_bloom_wrap_raw");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/raw.pfw.gz", 0x5678);

            std::vector<unsigned char> blob = {0xCA, 0xFE};
            db.insert_chunk_bloom_filter(fid, 1, "fhash", blob.data(),
                                         static_cast<int>(blob.size()), 10);

            auto results = db.query_chunk_bloom_filters(fid, "fhash");
            REQUIRE(results.size() == 1);
            CHECK(results[0].checkpoint_idx == 1);
            CHECK(results[0].bloom_data == blob);
            CHECK(results[0].num_entries == 10);
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query file bloom filter") {
        auto path =
            dft_utils_test::make_unique_test_path("idx_file_bloom_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid =
                db.get_or_create_file_info("/trace/fbloom.pfw.gz", 0xABCD);

            std::vector<unsigned char> blob = {0x11, 0x22, 0x33};
            db.insert_file_bloom_filter(fid, "name", std::span(blob), 99);

            auto result = db.query_file_bloom_filter(fid, "name");
            REQUIRE(result.has_value());
            CHECK(result->bloom_data == blob);
            CHECK(result->num_entries == 99);
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query hash resolution") {
        auto path = dft_utils_test::make_unique_test_path("idx_hash_res_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/hres.pfw.gz", 0xFACE);

            db.insert_hash_resolution(fid, "fhash", "abc123", "/path/to/file");

            auto resolved = db.query_resolved_by_hash("fhash", "abc123");
            REQUIRE(resolved.has_value());
            CHECK(resolved.value() == "/path/to/file");

            auto not_found = db.query_resolved_by_hash("fhash", "nonexistent");
            CHECK_FALSE(not_found.has_value());
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query index dimensions") {
        auto path = dft_utils_test::make_unique_test_path("idx_dim_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/dim.pfw.gz", 0xBBBB);

            db.insert_index_dimension(fid, "name");
            db.insert_index_dimension(fid, "fhash");

            auto dims = db.query_index_dimensions(fid);
            CHECK(dims.size() == 2);

            CHECK(db.has_index_dimension(fid, "name"));
            CHECK(db.has_index_dimension(fid, "fhash"));
            CHECK_FALSE(db.has_index_dimension(fid, "nonexistent"));
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query chunk statistics") {
        auto path = dft_utils_test::make_unique_test_path("idx_stats_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/stats.pfw.gz", 0xCCCC);

            using ChunkStatistics = dftracer::utils::utilities::composites::
                dft::indexing::ChunkStatistics;
            ChunkStatistics stats;
            stats.total_events = 100;
            stats.min_timestamp_us = 1000;
            stats.max_timestamp_us = 5000;

            db.insert_chunk_statistics(fid, 0, stats);

            auto results = db.query_chunk_statistics(fid);
            REQUIRE(results.size() == 1);
            CHECK(results[0].checkpoint_idx == 0);
            CHECK(results[0].stats.total_events == 100);
        }

        fs::remove(db_path);
    }

    TEST_CASE("delete operations") {
        auto path = dft_utils_test::make_unique_test_path("idx_delete_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/del.pfw.gz", 0xDDDD);

            std::vector<unsigned char> blob = {0x01};
            db.insert_chunk_bloom_filter(fid, 0, "name", std::span(blob), 1);
            db.insert_file_bloom_filter(fid, "name", std::span(blob), 1);
            db.insert_hash_resolution(fid, "name", "h1", "v1");

            db.delete_chunk_bloom_filters(fid, "name");
            CHECK(db.query_chunk_bloom_filters(fid, "name").empty());

            db.delete_file_bloom_filter(fid, "name");
            CHECK_FALSE(db.query_file_bloom_filter(fid, "name").has_value());

            db.delete_hash_resolutions(fid);
            CHECK_FALSE(db.query_resolved_by_hash("name", "h1").has_value());
        }

        fs::remove(db_path);
    }

    TEST_CASE("string_view accepts std::string and const char*") {
        auto path = dft_utils_test::make_unique_test_path("idx_sv_compat");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_bloom_schema();

            int fid = db.get_or_create_file_info("/trace/sv.pfw.gz", 0xEEEE);

            // const char*
            std::vector<unsigned char> blob = {0x01};
            db.insert_chunk_bloom_filter(fid, 0, "name", std::span(blob), 1);

            // std::string
            std::string dim = "fhash";
            db.insert_chunk_bloom_filter(fid, 1, dim, std::span(blob), 2);

            // std::string_view
            std::string_view sv_dim = "hhash";
            db.insert_chunk_bloom_filter(fid, 2, sv_dim, std::span(blob), 3);

            CHECK(db.query_chunk_bloom_filters(fid, "name").size() == 1);
            CHECK(db.query_chunk_bloom_filters(fid, dim).size() == 1);
            CHECK(db.query_chunk_bloom_filters(fid, sv_dim).size() == 1);
        }

        fs::remove(db_path);
    }
}

TEST_SUITE("IndexDatabase - Manifest wrapper methods") {
    TEST_CASE("insert and query event ranges") {
        auto path =
            dft_utils_test::make_unique_test_path("idx_event_range_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_manifest_schema();

            int fid = db.get_or_create_file_info("/trace/ev.pfw.gz", 0x1111);

            std::vector<std::uint32_t> lines = {10, 20, 30};

            // vector overload
            db.insert_event_range(fid, 0, "cat1", "event1", lines);

            // span overload
            std::vector<std::uint32_t> lines2 = {40, 50};
            db.insert_event_range(fid, 1, "cat2", "event2", std::span(lines2));

            auto results = db.query_event_ranges(fid);
            CHECK(results.size() == 2);

            auto ckpt0 = db.query_event_ranges_for_checkpoint(fid, 0);
            REQUIRE(ckpt0.size() == 1);
            CHECK(ckpt0[0].cat == "cat1");
            CHECK(ckpt0[0].name == "event1");
        }

        fs::remove(db_path);
    }

    TEST_CASE("insert and query metadata lines") {
        auto path =
            dft_utils_test::make_unique_test_path("idx_meta_lines_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_manifest_schema();

            int fid = db.get_or_create_file_info("/trace/meta.pfw.gz", 0x2222);

            std::vector<std::uint32_t> lines = {5, 15, 25};
            db.insert_metadata_lines(fid, 0, "traceEvents", lines);

            auto results = db.query_metadata_lines(fid);
            REQUIRE(results.size() == 1);
            CHECK(results[0].meta_type == "traceEvents");

            auto ckpt0 = db.query_metadata_lines_for_checkpoint(fid, 0);
            CHECK(ckpt0.size() == 1);
        }

        fs::remove(db_path);
    }

    TEST_CASE("delete event ranges and metadata lines") {
        auto path =
            dft_utils_test::make_unique_test_path("idx_manifest_del_wrap");
        auto db_path = path.string() + ".idx";

        {
            IndexDatabase db(db_path);
            db.init_base_schema();
            db.init_manifest_schema();

            int fid = db.get_or_create_file_info("/trace/mdel.pfw.gz", 0x3333);

            std::vector<std::uint32_t> lines = {1, 2, 3};
            db.insert_event_range(fid, 0, "cat", "name", lines);
            db.insert_metadata_lines(fid, 0, "meta", lines);

            db.delete_event_ranges(fid);
            CHECK(db.query_event_ranges(fid).empty());

            db.delete_metadata_lines(fid);
            CHECK(db.query_metadata_lines(fid).empty());
        }

        fs::remove(db_path);
    }
}
