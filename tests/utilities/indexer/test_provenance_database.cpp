#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/sqlite/statement.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>
#include <doctest/doctest.h>
#include <sqlite3.h>
#include <testing_utilities.h>

#include <string>

using namespace dftracer::utils::utilities::indexer;
using dftracer::utils::sqlite::SqliteStmt;

static bool table_exists(sqlite3* db, const std::string& table_name) {
    SqliteStmt stmt(
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;");
    stmt.bind_text(1, table_name);
    return sqlite3_step(stmt) == SQLITE_ROW;
}

TEST_SUITE("ProvenanceDatabase") {
    TEST_CASE("Create and open database") {
        auto path =
            dft_utils_test::make_unique_test_path("provdb_create").string() +
            ".pidx";
        CHECK_NOTHROW(ProvenanceDatabase db(path));
        CHECK(fs::exists(path));
        fs::remove(path);
    }

    TEST_CASE("init_schema creates tables") {
        auto path =
            dft_utils_test::make_unique_test_path("provdb_schema").string() +
            ".pidx";
        ProvenanceDatabase db(path);
        CHECK_NOTHROW(db.init_schema());

        sqlite3* raw = db.db().get();
        CHECK(table_exists(raw, "file_info"));
        CHECK(table_exists(raw, "provenance_info"));
        CHECK(table_exists(raw, "provenance_sources"));
        CHECK(table_exists(raw, "provenance_group"));
        CHECK(table_exists(raw, "provenance_segments"));

        fs::remove(path);
    }

    TEST_CASE("get_or_create_file_info") {
        auto path =
            dft_utils_test::make_unique_test_path("provdb_file_info").string() +
            ".pidx";
        ProvenanceDatabase db(path);
        db.init_schema();

        SUBCASE("insert returns valid id") {
            int id = db.get_or_create_file_info("/data/trace.pfw.gz", 0xDEAD);
            CHECK(id >= 1);
        }

        SUBCASE("same path and hash returns same id") {
            int id1 = db.get_or_create_file_info("/data/trace.pfw.gz", 0xBEEF);
            int id2 = db.get_or_create_file_info("/data/trace.pfw.gz", 0xBEEF);
            CHECK(id1 == id2);
        }

        SUBCASE("same path different hash replaces row") {
            int id1 = db.get_or_create_file_info("/data/other.pfw.gz", 0xAAAA);
            int id2 = db.get_or_create_file_info("/data/other.pfw.gz", 0xBBBB);
            CHECK(id2 >= 1);
            (void)id1;
        }

        SUBCASE("distinct paths get distinct ids") {
            int id1 = db.get_or_create_file_info("/data/a.pfw.gz", 0x1111);
            int id2 = db.get_or_create_file_info("/data/b.pfw.gz", 0x2222);
            CHECK(id1 != id2);
        }

        fs::remove(path);
    }

    TEST_CASE("get_file_info_id returns -1 for unknown") {
        auto path =
            dft_utils_test::make_unique_test_path("provdb_unknown").string() +
            ".pidx";
        ProvenanceDatabase db(path);
        db.init_schema();

        CHECK(db.get_file_info_id("/nonexistent/path.pfw.gz") == -1);

        fs::remove(path);
    }

    TEST_CASE("Transaction commit") {
        auto path =
            dft_utils_test::make_unique_test_path("provdb_txn").string() +
            ".pidx";
        ProvenanceDatabase db(path);
        db.init_schema();

        CHECK_NOTHROW(db.begin_transaction());
        int id = db.get_or_create_file_info("/data/txn.pfw.gz", 0xCAFE);
        CHECK_NOTHROW(db.commit_transaction());

        CHECK(id >= 1);
        CHECK(db.get_file_info_id("/data/txn.pfw.gz") == id);

        fs::remove(path);
    }

    TEST_CASE("determine_provenance_index_path - empty index_dir") {
        SUBCASE("plain path gets .pidx suffix") {
            auto result = determine_provenance_index_path("/data/trace.pfw.gz");
            CHECK(result == "/data/trace.pfw.gz.pidx");
        }

        SUBCASE("path without extension gets .pidx suffix") {
            auto result = determine_provenance_index_path("/data/trace");
            CHECK(result == "/data/trace.pidx");
        }
    }

    TEST_CASE("determine_provenance_index_path - with index_dir") {
        SUBCASE("places filename.pidx under index_dir") {
            auto result =
                determine_provenance_index_path("/data/trace.pfw.gz", "/idx");
            CHECK(result == "/idx/trace.pfw.gz.pidx");
        }

        SUBCASE("nested source path uses only filename") {
            auto result = determine_provenance_index_path(
                "/deep/nested/dir/run.pfw.gz", "/scratch/indices");
            CHECK(result == "/scratch/indices/run.pfw.gz.pidx");
        }
    }
}
