#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/visitors/bloom_core.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
using namespace dftracer::utils::trace::visitors;
using namespace dftu_utils_test;

namespace {

// The batch builder always (re)indexes what it is given; skip-if-indexed is a
// caller concern (resolve_and_build pre-filters), so `success` == indexed.
struct BuildOutcome {
    bool success = false;
    std::string index_path;
    std::size_t total_lines = 0;
};

BuildOutcome build_one(const std::string& gz, bool force = false,
                       std::size_t sub_chunk_events = 0) {
    BuildOutcome out;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto cfg = std::make_shared<IndexBuildBatchConfig>();
        cfg->file_paths = {gz};
        cfg->force_rebuild = force;
        if (sub_chunk_events > 0)
            cfg->bloom_config.sub_chunk_events = sub_chunk_events;
        auto r =
            co_await IndexBatchBuilderUtility::process(&scope, std::move(cfg));
        out.success = r.indexed >= 1 && r.failed == 0;
        if (!r.results.empty()) {
            out.index_path = r.results[0].index_path;
            out.total_lines = r.results[0].total_lines;
        }
        co_return;
    });
    return out;
}

// Build a gzip file out of `member_texts`, one gzip member each, and return
// its path plus the compressed offset of every member.
std::pair<std::string, std::vector<std::uint64_t>> write_multi_member_gz(
    const std::vector<std::string>& member_texts) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> offsets;
    for (const auto& text : member_texts) {
        offsets.push_back(bytes.size());
        GzipMemberCompressor comp;
        auto member = comp.compress_member(text.data(), text.size());
        REQUIRE(member.has_value());
        bytes.insert(bytes.end(), member->begin(), member->end());
    }
    // Put the fixture in its own directory: the index derives as
    // <dir>/.dftindex next to the trace, so a unique filename in the shared
    // temp root would still collapse every case onto one shared RocksDB and
    // corrupt it.
    auto dir = make_unique_test_path("members");
    fs::create_directories(dir);
    auto path = dir / "members.pfw.gz";
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ofs.close();
    return {path.string(), offsets};
}

std::string dftu_lines(int start_id, int count,
                       const std::string& name = "op") {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out +=
            "{\"id\":" + std::to_string(start_id + i) + ",\"name\":\"" + name +
            "\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,"
            "\"ts\":" +
            std::to_string(1000 + start_id + i) + ",\"dur\":10,\"ph\":\"X\"}\n";
    }
    return out;
}

}  // namespace

TEST_SUITE("IndexBuilder") {
    // Slices (member_begin > 0) split a file across ranks. strip drops a
    // slice's leading partial line and extend pulls the line straddling its
    // end from the next member, so every straddling line is counted by
    // exactly one slice. If either is wrong, summed slice line counts drift
    // from the whole-file count.
    TEST_CASE("Sliced builds conserve lines across member boundaries") {
        namespace gz = dftracer::utils::utilities::indexer::internal::gzip;
        using dftracer::utils::utilities::indexer::internal::GzipMember;

        // Members deliberately end mid-line so lines straddle boundaries.
        const std::vector<std::string> texts = {
            "line1\nlineA_par",      // ends mid-line
            "t1\nlineB\nlineC_par",  // starts + ends mid-line
            "t2\nline4\n",           // starts mid-line
        };
        auto [gz_file, offs] = write_multi_member_gz(texts);
        const std::uint64_t fsize = fs::file_size(gz_file);

        std::vector<GzipMember> members(texts.size());
        for (std::size_t i = 0; i < texts.size(); ++i) {
            members[i].c_offset = offs[i];
            members[i].c_size =
                (i + 1 < offs.size() ? offs[i + 1] : fsize) - offs[i];
        }

        dftracer::utils::utilities::indexer::internal::Indexer::VisitorList
            no_visitors;

        std::uint64_t whole_lines = 0;
        run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
            auto a = co_await gz::build_gzip_index_artifacts(
                gz_file, 1u << 20, no_visitors, &scope);
            REQUIRE(a.has_value());
            whole_lines = a->total_lines;
        });

        std::uint64_t sliced_lines = 0;
        for (std::size_t b = 0; b < members.size(); ++b) {
            gz::GzipMemberSlice slice;
            slice.members = &members;
            slice.member_begin = b;
            slice.member_end = b + 1;
            run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
                auto a = co_await gz::build_gzip_index_artifacts(
                    gz_file, 1u << 20, no_visitors, &scope, &slice);
                REQUIRE(a.has_value());
                sliced_lines += a->total_lines;
            });
        }

        CHECK(whole_lines == 5);
        CHECK(sliced_lines == whole_lines);
    }

    TEST_CASE("Build checkpoint-only index") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto result = build_one(gz_file);

        CHECK(result.success);
        CHECK(fs::exists(result.index_path));
    }

    TEST_CASE("Build with bloom") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto result = build_one(gz_file);

        REQUIRE(result.success);
        REQUIRE(fs::exists(result.index_path));

        IndexDatabase db(result.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
    }

    TEST_CASE("Member table covers every gzip member") {
        const std::vector<std::string> texts = {
            dftu_lines(0, 40), dftu_lines(40, 25), dftu_lines(65, 35)};
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);
        const std::uint64_t file_size = fs::file_size(gz_file);

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        IndexDatabase db(result.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);

        auto members = db.query_gzip_members(fid);
        REQUIRE(members.size() == texts.size());

        std::uint64_t expected_uc = 0;
        std::uint64_t expected_line = 1;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto& m = members[i];
            CHECK(m.member_idx == i);
            CHECK(m.c_offset == c_offsets[i]);
            CHECK(m.uc_offset == expected_uc);
            CHECK(m.uc_size == texts[i].size());
            CHECK(m.first_line_num == expected_line);
            expected_uc += m.uc_size;
            expected_line = m.last_line_num + 1;
        }
        CHECK(members.back().c_offset + members.back().c_size == file_size);
        CHECK(members.back().last_line_num == 100);
    }

    TEST_CASE("Sub-chunk zone-maps cover every event within a member") {
        const std::vector<std::string> texts = {
            dftu_lines(0, 40), dftu_lines(40, 25), dftu_lines(65, 35)};
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);

        const std::size_t sub = 10;
        auto result = build_one(gz_file, /*force=*/false, sub);
        REQUIRE(result.success);

        IndexDatabase db(result.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);
        auto rows = db.query_chunk_statistics(fid);
        REQUIRE(rows.size() == texts.size());
        for (const auto& row : rows) {
            const auto& s = row.stats;
            const auto& buckets = s.sub_zonemaps;
            REQUIRE(!buckets.empty());

            std::uint64_t summed = 0;
            for (const auto& z : buckets) {
                summed += z.event_count;
                CHECK(z.event_count > 0);
                CHECK(z.event_count <= sub);
                // Bucket zone-map lies within the member's range.
                CHECK(z.min_timestamp_us >= s.min_timestamp_us);
                CHECK(z.max_timestamp_us <= s.max_timestamp_us);
                CHECK(z.min_duration_us == 10);
                CHECK(z.max_duration_us == 10);
            }
            CHECK(summed == s.total_events);
            CHECK(buckets.size() == (s.total_events + sub - 1) / sub);
        }
    }

    TEST_CASE("Pruner chunks are gzip members") {
        const std::vector<std::string> texts = {
            dftu_lines(0, 40), dftu_lines(40, 25, "rare"), dftu_lines(65, 35)};
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        IndexDatabase db(result.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);

        auto members = db.query_gzip_members(fid);
        auto spans = db.query_chunk_spans(fid);
        REQUIRE(spans.size() == members.size());
        for (std::size_t i = 0; i < spans.size(); ++i) {
            CHECK(spans[i].uc_offset == members[i].uc_offset);
            CHECK(spans[i].uc_size == members[i].uc_size);
            CHECK(spans[i].first_line_num == members[i].first_line_num);
            CHECK(spans[i].last_line_num == members[i].last_line_num);
        }

        // Events are attributed to the member that holds them, so a name
        // confined to one member yields exactly that chunk.
        auto rare_chunks = db.query_name_chunk_postings("rare", fid);
        REQUIRE(rare_chunks.size() == 1);
        CHECK(rare_chunks[0] == 1);

        auto common_chunks = db.query_name_chunk_postings("op", fid);
        std::sort(common_chunks.begin(), common_chunks.end());
        REQUIRE(common_chunks.size() == 2);
        CHECK(common_chunks[0] == 0);
        CHECK(common_chunks[1] == 2);
    }

    TEST_CASE("Member table records a single-member file") {
        auto [gz_file, c_offsets] = write_multi_member_gz({dftu_lines(0, 30)});

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        IndexDatabase db(result.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);

        auto members = db.query_gzip_members(fid);
        REQUIRE(members.size() == 1);
        CHECK(members[0].c_offset == 0);
        CHECK(members[0].uc_offset == 0);
        CHECK(members[0].first_line_num == 1);
        CHECK(members[0].last_line_num == 30);
    }

    // No builder-level skip: a repeat build re-indexes and must stay valid.
    TEST_CASE("Repeat build re-indexes and stays valid") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto first = build_one(gz_file);
        REQUIRE(first.success);

        auto second = build_one(gz_file);
        CHECK(second.success);

        IndexDatabase db(second.index_path);
        int fid = db.get_file_info_id(internal::get_logical_path(gz_file));
        REQUIRE(fid >= 0);
        CHECK(db.has_bloom_data(fid));
    }

    TEST_CASE("Force rebuild") {
        TestEnvironment env(valgrind_scale(1000, 10));
        std::string gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto first = build_one(gz_file);
        REQUIRE(first.success);

        auto second = build_one(gz_file, /*force=*/true);
        CHECK(second.success);
    }

    TEST_CASE("Result has correct line count") {
        const std::size_t n_idx_lines = valgrind_scale(1000, 10);
        TestEnvironment env(n_idx_lines);
        std::string gz_file =
            env.create_dft_test_gzip_file(static_cast<int>(n_idx_lines));

        auto result = build_one(gz_file);

        REQUIRE(result.success);
        CHECK(result.total_lines > 0);
        CHECK(result.total_lines >= n_idx_lines);
    }
}
