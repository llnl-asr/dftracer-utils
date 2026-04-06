#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/provenance_database.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

namespace fs = std::filesystem;
using namespace dftracer::utils::utilities::indexer;

TEST_SUITE("ProvenanceDatabase") {
    TEST_CASE("uses the same root-local .dftindex path") {
        auto root = dft_utils_test::make_unique_test_path("prov_root");
        fs::create_directories(root);

        auto resolved =
            determine_provenance_index_path((root / "trace.pfw.gz").string());
        CHECK(resolved == (root / ".dftindex").string());

        ProvenanceDatabase db(resolved);
        CHECK(fs::exists(root / ".dftindex"));
    }

    TEST_CASE("stores and queries provenance records in shared DB") {
        auto root = dft_utils_test::make_unique_test_path("prov_records");
        fs::create_directories(root);

        ProvenanceDatabase db((root / ".dftindex").string());
        db.init_schema();

        int file_id =
            db.get_or_create_file_info((root / "out.pfw.gz").string(), 0xCAFE);
        CHECK(file_id > 0);
        CHECK(db.get_file_info_id((root / "out.pfw.gz").string()) == file_id);

        db.insert_info(file_id, "tool", "dftracer_organize");
        db.insert_group(file_id, "group0", "cat == POSIX");
        db.insert_source(file_id, 7, "/src/a.pfw.gz", 12, "hash7");
        db.insert_segment(file_id, 7, 3, 100, 140, 9);

        auto sources = db.query_sources(file_id);
        REQUIRE(sources.size() == 1);
        CHECK(sources[0].source_idx == 7);
        CHECK(sources[0].path == "/src/a.pfw.gz");
        CHECK(sources[0].num_checkpoints == 12);
        CHECK(sources[0].event_hash == "hash7");

        auto segments = db.query_segments(file_id, 7);
        REQUIRE(segments.size() == 1);
        CHECK(segments[0].source_checkpoint == 3);
        CHECK(segments[0].output_line_start == 100);
        CHECK(segments[0].output_line_end == 140);
        CHECK(segments[0].event_count == 9);

        CHECK(db.query_info(file_id, "tool") == "dftracer_organize");
        CHECK(db.query_group_name(file_id) == "group0");
        CHECK(db.query_group_predicate(file_id) == "cat == POSIX");
    }

    TEST_CASE("keeps provenance for multiple outputs in one shared root") {
        auto root = dft_utils_test::make_unique_test_path("prov_multi");
        fs::create_directories(root);

        ProvenanceDatabase db((root / ".dftindex").string());
        db.init_schema();

        const auto out_a = (root / "io.pfw.gz").string();
        const auto out_b = (root / "compute.pfw.gz").string();

        const int file_a = db.get_or_create_file_info(out_a, 0xA001);
        const int file_b = db.get_or_create_file_info(out_b, 0xB002);
        CHECK(file_a > 0);
        CHECK(file_b > 0);
        CHECK(file_a != file_b);

        db.begin_transaction();
        db.insert_group(file_a, "io", R"(cat == "POSIX")");
        db.insert_source(file_a, 0, "/src/trace0.pfw.gz", 3, "ha");
        db.insert_segment(file_a, 0, 1, 0, 5, 3);

        db.insert_group(file_b, "compute", R"(cat == "APP")");
        db.insert_source(file_b, 1, "/src/trace1.pfw.gz", 2, "hb");
        db.insert_segment(file_b, 1, 0, 0, 3, 1);
        db.commit_transaction();

        CHECK(db.get_file_info_id(out_a) == file_a);
        CHECK(db.get_file_info_id(out_b) == file_b);

        CHECK(db.query_group_name(file_a) == "io");
        CHECK(db.query_group_name(file_b) == "compute");

        auto segments_a = db.query_all_segments(file_a);
        auto segments_b = db.query_all_segments(file_b);
        REQUIRE(segments_a.size() == 1);
        REQUIRE(segments_b.size() == 1);
        CHECK(segments_a[0].event_count == 3);
        CHECK(segments_b[0].event_count == 1);
    }

    TEST_CASE("rebuild-style writes overwrite provenance for the same output") {
        auto root = dft_utils_test::make_unique_test_path("prov_rebuild");
        fs::create_directories(root);

        ProvenanceDatabase db((root / ".dftindex").string());
        db.init_schema();

        const auto out = (root / "group.pfw.gz").string();

        const int original_id = db.get_or_create_file_info(out, 0x1111);
        db.begin_transaction();
        db.insert_info(original_id, "tool", "dftracer_organize");
        db.insert_group(original_id, "io", R"(cat == "POSIX")");
        db.insert_source(original_id, 0, "/src/trace0.pfw.gz", 4, "old");
        db.insert_segment(original_id, 0, 0, 0, 4, 2);
        db.commit_transaction();

        const int rebuilt_id = db.get_or_create_file_info(out, 0x2222);
        CHECK(rebuilt_id == original_id);

        db.begin_transaction();
        db.insert_info(rebuilt_id, "tool", "dftracer_organize_v2");
        db.insert_group(rebuilt_id, "io", R"(cat == "MPI")");
        db.insert_source(rebuilt_id, 0, "/src/trace0.pfw.gz", 8, "new");
        db.insert_segment(rebuilt_id, 0, 0, 10, 18, 5);
        db.commit_transaction();

        CHECK(db.query_info(rebuilt_id, "tool") == "dftracer_organize_v2");
        CHECK(db.query_group_predicate(rebuilt_id) == R"(cat == "MPI")");

        auto sources = db.query_sources(rebuilt_id);
        REQUIRE(sources.size() == 1);
        CHECK(sources[0].num_checkpoints == 8);
        CHECK(sources[0].event_hash == "new");

        auto segments = db.query_segments(rebuilt_id, 0);
        REQUIRE(segments.size() == 1);
        CHECK(segments[0].output_line_start == 10);
        CHECK(segments[0].output_line_end == 18);
        CHECK(segments[0].event_count == 5);
    }

    TEST_CASE("rollback discards provenance writes") {
        auto root = dft_utils_test::make_unique_test_path("prov_rollback");
        fs::create_directories(root);

        ProvenanceDatabase db((root / ".dftindex").string());
        db.init_schema();

        const int file_id =
            db.get_or_create_file_info((root / "out.pfw.gz").string(), 0xCAFE);

        db.begin_transaction();
        db.insert_info(file_id, "tool", "dftracer_organize");
        db.insert_group(file_id, "group0", "cat == POSIX");
        db.insert_source(file_id, 7, "/src/a.pfw.gz", 12, "hash7");
        db.insert_segment(file_id, 7, 3, 100, 140, 9);
        db.rollback_transaction();

        CHECK(db.query_info(file_id, "tool").empty());
        CHECK(db.query_group_name(file_id).empty());
        CHECK(db.query_group_predicate(file_id).empty());
        CHECK(db.query_sources(file_id).empty());
        CHECK(db.query_segments(file_id, 7).empty());
    }
}
