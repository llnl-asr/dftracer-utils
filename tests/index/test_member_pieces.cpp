// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using dftracer::utils::index::gzip::GzipBuildArtifacts;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftracer::utils::index::gzip::GzipRecordKind;
namespace idx = dftracer::utils::index;

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

// Writes `arts` as the members of "trace.pfw.gz" in one build.
void persist(const std::string& index_root, const GzipBuildArtifacts& arts) {
    auto index = dftracer::utils::index::Index::open(
        dftracer::utils::index::store::internal::normalize_index_root(
            index_root));
    REQUIRE(index);
    auto build = index->build("trace.pfw.gz", {});
    REQUIRE(build);
    dftracer::utils::index::gzip::persist_gzip_index_artifacts(*build, arts);
    REQUIRE(build->commit().get());
}

}  // namespace

TEST_SUITE("member pieces") {
    TEST_CASE("member, head and restart records round trip with a window") {
        auto root = dftu_utils_test::make_unique_test_path("member_pieces");
        REQUIRE(
            dftracer::utils::dstd::fs::create_directories(root).has_value());

        GzipBuildArtifacts arts;
        arts.checkpoint_size = 1000;
        arts.total_lines = 40;
        arts.total_uc_size = 4000;
        arts.members = {record(0, 0, 0, GzipRecordKind::Member, 0),
                        record(1, 100, 1000, GzipRecordKind::Head, 0),
                        record(2, 200, 2000, GzipRecordKind::Restart, 5),
                        record(3, 300, 3000, GzipRecordKind::Restart, 0)};
        std::string window(32768, '\0');
        for (std::size_t i = 0; i < window.size(); ++i)
            window[i] = static_cast<char>(i * 31);
        arts.restart_windows = {{2, window}, {3, std::string(32768, 'x')}};

        persist(std::string((root / ".dftindex").str()), arts);
        const auto snapshot = dftu_utils_test::index_snapshot(
            std::string((root / ".dftindex").str()));
        const auto file = snapshot.file("trace.pfw.gz");
        REQUIRE(file.has_value());
        const auto restart = [&](std::uint64_t member) {
            return dftu_utils_test::index_take(
                idx::restart_window(snapshot, *file, member));
        };

        const auto got =
            dftu_utils_test::index_take(idx::members(snapshot, *file));
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
        REQUIRE(restart(2).has_value());
        CHECK(std::string_view(restart(2)->data(), restart(2)->size()) ==
              window);
        REQUIRE(restart(3).has_value());
        CHECK(std::string_view(restart(3)->data(), restart(3)->size()) ==
              std::string(32768, 'x'));
        CHECK_FALSE(restart(1).has_value());

        const auto spans =
            dftu_utils_test::index_take(idx::spans(snapshot, *file));
        REQUIRE(spans.size() == 4);
        CHECK(spans[2].uc_offset == 2000);
    }

    TEST_CASE("a rebuild drops the windows of the old pieces") {
        auto root = dftu_utils_test::make_unique_test_path("member_pieces_re");
        REQUIRE(
            dftracer::utils::dstd::fs::create_directories(root).has_value());

        GzipBuildArtifacts split;
        split.members = {record(0, 0, 0, GzipRecordKind::Head, 0),
                         record(1, 100, 1000, GzipRecordKind::Restart, 3)};
        split.restart_windows = {{1, std::string(32768, 'w')}};
        GzipBuildArtifacts whole;
        whole.members = {record(0, 0, 0, GzipRecordKind::Member, 0)};

        for (const auto* arts : {&split, &whole})
            persist(std::string((root / ".dftindex").str()), *arts);
        const auto snapshot = dftu_utils_test::index_snapshot(
            std::string((root / ".dftindex").str()));
        const auto file = snapshot.file("trace.pfw.gz");
        REQUIRE(file.has_value());
        const auto got =
            dftu_utils_test::index_take(idx::members(snapshot, *file));
        REQUIRE(got.size() == 1);
        CHECK(got[0].kind == GzipRecordKind::Member);
        CHECK_FALSE(
            dftu_utils_test::index_take(idx::restart_window(snapshot, *file, 1))
                .has_value());
    }
}
