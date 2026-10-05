#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using dftracer::utils::index::gzip::GzipBuildArtifacts;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftracer::utils::index::gzip::GzipRecordKind;
using dftracer::utils::index::store::IndexDatabase;

namespace {

GzipMemberRecord record(std::uint64_t idx, std::uint64_t c_offset,
                        std::uint64_t uc_offset, GzipRecordKind kind,
                        std::uint8_t bits) {
    GzipMemberRecord r{};
    r.member_idx = idx;
    r.c_offset = c_offset;
    r.c_size = 100;
    r.uc_offset = uc_offset;
    r.uc_size = 1000;
    r.first_line_num = idx * 10 + 1;
    r.last_line_num = idx * 10 + 10;
    r.kind = kind;
    r.bits = bits;
    return r;
}

}  // namespace

TEST_SUITE("member pieces") {
    TEST_CASE("member, head and restart records round trip with a window") {
        auto root = dftu_utils_test::make_unique_test_path("member_pieces");
        fs::create_directories(root);
        IndexDatabase db((root / ".dftindex").string());

        GzipBuildArtifacts arts;
        arts.checkpoint_size = 1000;
        arts.total_lines = 40;
        arts.total_uc_size = 4000;
        arts.members = {record(0, 0, 0, GzipRecordKind::MEMBER, 0),
                        record(1, 100, 1000, GzipRecordKind::HEAD, 0),
                        record(2, 200, 2000, GzipRecordKind::RESTART, 5),
                        record(3, 300, 3000, GzipRecordKind::RESTART, 0)};
        std::string window(32768, '\0');
        for (std::size_t i = 0; i < window.size(); ++i)
            window[i] = static_cast<char>(i * 31);
        arts.restart_windows = {{2, window}, {3, std::string(32768, 'x')}};

        int fid = -1;
        {
            auto w = db.begin_write();
            fid = w->file_id_for("trace.pfw.gz");
            dftracer::utils::index::gzip::persist_gzip_index_artifacts(*w, fid,
                                                                       arts);
            w->commit();
        }

        const auto got = db.query_gzip_members(fid);
        REQUIRE(got.size() == 4);
        for (std::size_t i = 0; i < got.size(); ++i) {
            CAPTURE(i);
            CHECK(got[i].member_idx == arts.members[i].member_idx);
            CHECK(got[i].c_offset == arts.members[i].c_offset);
            CHECK(got[i].uc_offset == arts.members[i].uc_offset);
            CHECK(got[i].last_line_num == arts.members[i].last_line_num);
            CHECK(got[i].kind == arts.members[i].kind);
            CHECK(got[i].bits == arts.members[i].bits);
        }
        CHECK(db.query_restart_window(fid, 2) == window);
        CHECK(db.query_restart_window(fid, 3) == std::string(32768, 'x'));
        CHECK_FALSE(db.query_restart_window(fid, 1).has_value());

        const auto spans = db.query_chunk_spans(fid);
        REQUIRE(spans.size() == 4);
        CHECK(spans[2].uc_offset == 2000);
    }

    TEST_CASE("a rebuild drops the windows of the old pieces") {
        auto root = dftu_utils_test::make_unique_test_path("member_pieces_re");
        fs::create_directories(root);
        IndexDatabase db((root / ".dftindex").string());

        GzipBuildArtifacts split;
        split.members = {record(0, 0, 0, GzipRecordKind::HEAD, 0),
                         record(1, 100, 1000, GzipRecordKind::RESTART, 3)};
        split.restart_windows = {{1, std::string(32768, 'w')}};
        GzipBuildArtifacts whole;
        whole.members = {record(0, 0, 0, GzipRecordKind::MEMBER, 0)};

        int fid = -1;
        for (const auto* arts : {&split, &whole}) {
            auto w = db.begin_write();
            fid = w->file_id_for("trace.pfw.gz");
            dftracer::utils::index::gzip::persist_gzip_index_artifacts(*w, fid,
                                                                       *arts);
            w->commit();
        }
        const auto got = db.query_gzip_members(fid);
        REQUIRE(got.size() == 1);
        CHECK(got[0].kind == GzipRecordKind::MEMBER);
        CHECK_FALSE(db.query_restart_window(fid, 1).has_value());
    }
}
