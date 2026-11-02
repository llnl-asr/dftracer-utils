// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <span>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::index::extensions;
using namespace dftracer::utils::index::plan;
using dftracer::utils::duql::Query;

using dftracer::utils::index::gzip::GzipMemberRecord;

// `members` replaces the base member of the same index, or adds a new one.
// `name_file_bloom` adds the file-level "name" bloom that the tier-0 file skip
// probes. The base fixture writes none, which is itself a case worth keeping:
// a file without blooms must stay unpruned.
static void populate_test_idx(const std::string& index_path,
                              const std::string& file_path,
                              const std::vector<GzipMemberRecord>& members = {},
                              bool name_file_bloom = false) {
    auto index = dftu_utils_test::open_test_index(index_path);
    auto writer = dftu_utils_test::register_test_file(
        index, dftu_utils_test::index_key(file_path), 12345);

    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("POSIX");
        cat_ds.observe("POSIX");
        dftu_utils_test::put_dimension_stats(writer, 0, cat_ds, 2);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("read");
        name_ds.observe("read");
        dftu_utils_test::put_dimension_stats(writer, 0, name_ds, 2);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("100");
        dur_ds.observe("200");
        dftu_utils_test::put_dimension_stats(writer, 0, dur_ds, 2);

        for (const char* path : {"cat", "name", "dur"})
            dftracer::utils::index::put_path(
                writer, dftu_utils_test::IndexExtension::Bloom, path);
    }

    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("STDIO");
        cat_ds.observe("STDIO");
        dftu_utils_test::put_dimension_stats(writer, 1, cat_ds, 2);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("write");
        name_ds.observe("write");
        dftu_utils_test::put_dimension_stats(writer, 1, name_ds, 2);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("500");
        dur_ds.observe("600");
        dftu_utils_test::put_dimension_stats(writer, 1, dur_ds, 2);
    }

    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("POSIX");
        cat_ds.observe("MPI");
        dftu_utils_test::put_dimension_stats(writer, 2, cat_ds, 2);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("read");
        name_ds.observe("send");
        dftu_utils_test::put_dimension_stats(writer, 2, name_ds, 2);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("50");
        dur_ds.observe("1000");
        dftu_utils_test::put_dimension_stats(writer, 2, dur_ds, 2);
    }
    // Two data events per chunk, one line each, so the statistics describe
    // every record and all-match proofs (NOT) are allowed.
    std::vector<GzipMemberRecord> all;
    for (std::uint64_t ckpt = 0; ckpt < 3; ++ckpt) {
        dftracer::utils::index::schemas::dft::ChunkStatistics stats;
        stats.total_events = 2;
        dftracer::utils::index::put_chunk_statistics(writer, ckpt, stats);
        all.push_back(GzipMemberRecord{
            .member_idx = ckpt,
            .c_offset = ckpt * 100,
            .c_size = 100,
            .uc_offset = ckpt * 200,
            .uc_size = 200,
            .first_line_num = ckpt * 2 + 1,
            .last_line_num = ckpt * 2 + 2,
        });
    }
    for (const auto& member : members) {
        if (member.member_idx < all.size())
            all[member.member_idx] = member;
        else
            all.push_back(member);
    }
    for (const auto& member : all)
        dftracer::utils::index::put_gzip_member(writer, member);

    if (name_file_bloom) {
        ScalableBloomFilter bloom(1024, 0.01);
        bloom.add("read");
        bloom.add("write");
        bloom.add("send");
        auto blob = bloom.serialize();
        dftu_utils_test::put_file_bloom(
            writer, "name",
            std::span<const unsigned char>(blob.data(), blob.size()),
            bloom.num_entries());
    }

    dftu_utils_test::mark_built(writer);
    dftu_utils_test::commit_test_build(writer);
}

static ChunkPrunerOutput run_pruner(const std::string& index_path,
                                    const std::string& file_path,
                                    const char* query_str) {
    auto q = Query::from_string(query_str);
    REQUIRE(q.has_value());

    ChunkPrunerInput input{index_path, file_path, std::move(*q), nullptr};

    ChunkPruner pruner;
    return pruner(input).get();
}

TEST_SUITE("ChunkPruner") {
    TEST_CASE("Pruner - equality match via dictionary") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_eq").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        auto out = run_pruner(index_path, file_path, R"(cat == "POSIX")");
        CHECK(out.success);
        CHECK(out.total_checkpoints == 3);
        // Chunks 0 and 2 have POSIX, chunk 1 has only STDIO
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 0);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - equality no match") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_eq_none")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        auto out = run_pruner(index_path, file_path, R"(cat == "HDF5")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.empty());
        CHECK_FALSE(out.file_may_match);
    }

    TEST_CASE("Pruner - in operator") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_in").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        auto out =
            run_pruner(index_path, file_path, R"(cat in ["POSIX", "STDIO"])");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 3);
    }

    TEST_CASE("Pruner - not in operator") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_notin")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // Chunk 0: only POSIX → excluded by not in ["POSIX"]
        // Chunk 1: only STDIO → kept
        // Chunk 2: POSIX + MPI → MPI not in list → kept
        auto out = run_pruner(index_path, file_path, R"(cat not in ["POSIX"])");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - AND intersection") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_and").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // cat == "POSIX" → chunks 0, 2
        // name == "read" → chunks 0, 2
        // AND → chunks 0, 2
        auto out = run_pruner(index_path, file_path,
                              R"(cat == "POSIX" and name == "read")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
    }

    TEST_CASE("Pruner - AND narrows results") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_and2").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // cat == "POSIX" → chunks 0, 2
        // name == "send" → chunk 2 only
        // AND → chunk 2
        auto out = run_pruner(index_path, file_path,
                              R"(cat == "POSIX" and name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 1);
        CHECK(out.candidate_checkpoints[0] == 2);
    }

    TEST_CASE("Pruner - OR union") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_or").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // cat == "STDIO" → chunk 1
        // name == "send" → chunk 2
        // OR → chunks 1, 2
        auto out = run_pruner(index_path, file_path,
                              R"(cat == "STDIO" or name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - NOT via dictionary") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_not").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // cat == "STDIO" → chunk 1
        // NOT → chunks 0, 2
        auto out = run_pruner(index_path, file_path, R"(not cat == "STDIO")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 0);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - NOT keeps a chunk with records the stats missed") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_not_meta")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        // Chunk 1 holds a third line (a metadata record, say) that has no
        // cat, so `not cat == "STDIO"` may match it.
        populate_test_idx(index_path, file_path,
                          {GzipMemberRecord{
                              .member_idx = 1,
                              .c_offset = 100,
                              .c_size = 100,
                              .uc_offset = 200,
                              .uc_size = 300,
                              .first_line_num = 3,
                              .last_line_num = 5,
                          }});
        auto out = run_pruner(index_path, file_path, R"(not cat == "STDIO")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints == std::vector<std::uint64_t>{0, 1, 2});
    }

    TEST_CASE("Pruner - a member without statistics stays a candidate") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_bare_member")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        // Member 3 holds only metadata records, so the index has no
        // statistics or bloom rows for it.
        populate_test_idx(index_path, file_path,
                          {GzipMemberRecord{
                              .member_idx = 3,
                              .c_offset = 300,
                              .c_size = 100,
                              .uc_offset = 600,
                              .uc_size = 200,
                              .first_line_num = 7,
                              .last_line_num = 8,
                          }});
        auto neg = run_pruner(index_path, file_path, R"(not cat == "STDIO")");
        CHECK(neg.total_checkpoints == 4);
        CHECK(neg.candidate_checkpoints == std::vector<std::uint64_t>{0, 2, 3});
        auto eq = run_pruner(index_path, file_path, R"(cat == "POSIX")");
        CHECK(eq.candidate_checkpoints == std::vector<std::uint64_t>{0, 2, 3});
    }

    TEST_CASE("Pruner - NOT keeps a chunk holding both sides") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_not_mixed")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // Chunk 2 holds cat POSIX AND MPI, so its MPI events match the
        // negation. The pruner used to complement the operand's MAY-match set
        // ({0, 2}) and answer {1}, dropping chunk 2 and every MPI event in it.
        auto out = run_pruner(index_path, file_path, R"(not cat == "POSIX")");
        CHECK(out.success);
        REQUIRE(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - NOT over a range keeps straddling chunks") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_not_range")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // dur ranges: chunk 0 [100,200], chunk 1 [500,600], chunk 2 [50,1000].
        // Only chunk 1 has EVERY event over 300, so only it can be dropped;
        // chunk 2 straddles and must be scanned.
        auto out = run_pruner(index_path, file_path, "not dur > 300");
        CHECK(out.success);
        REQUIRE(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 0);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - range via min/max") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_range")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        // dur > "500": chunk 0 max=200 (skip), chunk 1 max=600 (keep),
        // chunk 2 max=1000 (keep)
        auto out = run_pruner(index_path, file_path, R"(dur > "500")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - case-insensitive keywords") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_case").string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        auto out = run_pruner(index_path, file_path,
                              R"(cat == "POSIX" AND name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 1);
    }

    TEST_CASE("Pruner - file bloom skips the file before any chunk work") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_pruner_file_bloom")
                .string();
        common::fs::create_directories(test_dir);
        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path, {}, true);

        SUBCASE("absent value prunes the whole file") {
            auto out = run_pruner(index_path, file_path, R"(name == "absent")");
            CHECK(out.success);
            CHECK_FALSE(out.file_may_match);
            CHECK(out.candidate_checkpoints.empty());
        }

        SUBCASE("present value still reaches chunk pruning") {
            auto out = run_pruner(index_path, file_path, R"(name == "read")");
            CHECK(out.success);
            CHECK(out.file_may_match);
            CHECK(out.candidate_checkpoints.size() == 2);
        }

        SUBCASE("OR keeps the file when one side may match") {
            auto out = run_pruner(index_path, file_path,
                                  R"(name == "absent" OR name == "read")");
            CHECK(out.success);
            CHECK(out.file_may_match);
        }

        SUBCASE("AND prunes when either side is absent") {
            auto out = run_pruner(index_path, file_path,
                                  R"(name == "read" AND name == "absent")");
            CHECK(out.success);
            CHECK_FALSE(out.file_may_match);
        }

        SUBCASE("non-equality predicates stay conservative") {
            auto out = run_pruner(index_path, file_path, R"(dur > 100)");
            CHECK(out.success);
            CHECK(out.file_may_match);
        }
    }
}
