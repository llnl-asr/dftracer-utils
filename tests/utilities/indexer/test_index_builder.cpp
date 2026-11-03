// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <index_test_helpers.h>
#include <simdjson.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>

using namespace dftracer::utils::utilities;

using namespace dftracer::utils;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::store;

using namespace dftu_utils_test;

namespace {

// The batch builder always (re)indexes what it is given; skip-if-indexed is a
// caller concern (resolve_and_build pre-filters), so `success` == indexed.
struct BuildOutcome {
    bool success = false;
    dstd::String index_path;
    std::size_t total_lines = 0;
};

BuildOutcome build_one(const dstd::String& gz, bool force = false) {
    BuildOutcome out;
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        auto cfg = dstd::make_shared<IndexBuildBatchConfig>();
        cfg->file_paths = {dstd::String(dstd::StringView(gz))};
        cfg->force_rebuild = force;
        auto r = co_await BatchBuilder::process(std::move(cfg));
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
dstd::Pair<dstd::String, dstd::Vector<std::uint64_t>> write_multi_member_gz(
    const dstd::Vector<dstd::String>& member_texts) {
    dstd::Vector<std::uint8_t> bytes;
    dstd::Vector<std::uint64_t> offsets;
    for (const auto& text : member_texts) {
        offsets.push_back(bytes.size());
        fileio::compress::GzipMemberCompressor comp;
        auto member = comp.compress_member(text.data(), text.size());
        REQUIRE(member.has_value());
        bytes.insert(bytes.end(), member->begin(), member->end());
    }
    // Put the fixture in its own directory: the index derives as
    // <dir>/.dftindex next to the trace, so a unique filename in the shared
    // temp root would still collapse every case onto one shared index and
    // corrupt it.
    auto dir = make_unique_test_path("members");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    auto path = dir / "members.pfw.gz";
    dstd::String ofs;
    ofs.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    (void)dstd::fs::write_file(dstd::PathView(path), ofs);
    return {dstd::String(path.str()), offsets};
}

dstd::String dftu_lines(int start_id, int count,
                        const dstd::String& name = "op") {
    dstd::String out;
    for (int i = 0; i < count; ++i) {
        out += "{\"id\":" + dstd::to_string(start_id + i) + ",\"name\":\"" +
               name +
               "\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,"
               "\"ts\":" +
               dstd::to_string(1000 + start_id + i) +
               ",\"dur\":10,\"ph\":\"X\"}\n";
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
        // Members deliberately end mid-line so lines straddle boundaries.
        const dstd::Vector<dstd::String> texts = {
            "line1\nlineA_par",      // ends mid-line
            "t1\nlineB\nlineC_par",  // starts + ends mid-line
            "t2\nline4\n",           // starts mid-line
        };
        auto [gz_file, offs] = write_multi_member_gz(texts);
        const std::uint64_t fsize = dstd::fs::file_size(gz_file).value();

        dstd::Vector<index::gzip::GzipMember> members(texts.size());
        for (std::size_t i = 0; i < texts.size(); ++i) {
            members[i].c_offset = offs[i];
            members[i].c_size =
                (i + 1 < offs.size() ? offs[i + 1] : fsize) - offs[i];
        }

        dftracer::utils::index::gzip::CheckpointIndexer::VisitorList
            no_visitors;

        std::uint64_t whole_lines = 0;
        run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
            auto a = co_await index::gzip::build_gzip_index_artifacts(
                gz_file, 1u << 20, no_visitors, &scope);
            REQUIRE(a.has_value());
            whole_lines = a->total_lines;
        });

        std::uint64_t sliced_lines = 0;
        for (std::size_t b = 0; b < members.size(); ++b) {
            index::gzip::GzipMemberSlice slice;
            slice.members = &members;
            slice.member_begin = b;
            slice.member_end = b + 1;
            run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
                auto a = co_await index::gzip::build_gzip_index_artifacts(
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
        dstd::String gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto result = build_one(gz_file);

        CHECK(result.success);
        CHECK(dstd::fs::exists(result.index_path).value());
    }

    TEST_CASE("Files under different index roots each build into their own") {
        dstd::Vector<dstd::String> files;
        for (const char* name : {"a", "b"}) {
            auto dir = make_unique_test_path(name);
            REQUIRE(dstd::fs::create_directories(dir).has_value());
            const dstd::String path = dstd::String((dir / "t.pfw.gz").str());
            const dstd::String text = dftu_lines(0, 20);
            dftracer::utils::utilities::fileio::compress::GzipMemberCompressor
                comp;
            auto member = comp.compress_member(text.data(), text.size());
            REQUIRE(member.has_value());
            (void)dstd::fs::write_file(
                dstd::PathView(path),
                dstd::StringView(reinterpret_cast<const char*>(member->data()),
                                 member->size()));
            files.push_back(path);
        }
        IndexBuildBatchResult r;
        run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            auto cfg = dstd::make_shared<IndexBuildBatchConfig>();
            for (const auto& file : files) cfg->file_paths.emplace_back(file);
            r = co_await BatchBuilder::process(std::move(cfg));
            co_return;
        });
        CHECK(r.indexed == 2);
        REQUIRE(r.results.size() == 2);
        for (std::size_t i = 0; i < files.size(); ++i) {
            CAPTURE(files[i]);
            const dstd::String idx = dstd::String(
                dftracer::utils::trace::internal::determine_index_path(files[i],
                                                                       ""));
            CHECK(dstd::StringView(r.results[i].index_path) ==
                  dstd::StringView(idx));
            const auto snapshot = index_snapshot(idx);
            CHECK(find_indexed_file(snapshot, files[i]).has_value());
        }
    }

    TEST_CASE("Build with bloom") {
        TestEnvironment env(valgrind_scale(1000, 10));
        dstd::String gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto result = build_one(gz_file);

        REQUIRE(result.success);
        REQUIRE(dstd::fs::exists(result.index_path).value());

        const auto snapshot = index_snapshot(result.index_path);
        const auto file = require_indexed_file(snapshot, gz_file);
        CHECK(pruning_tier_current(snapshot, file));
    }

    TEST_CASE("Member table covers every gzip member") {
        const dstd::Vector<dstd::String> texts = {
            dftu_lines(0, 40), dftu_lines(40, 25), dftu_lines(65, 35)};
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);
        const std::uint64_t file_size = dstd::fs::file_size(gz_file).value();

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        const auto snapshot = index_snapshot(result.index_path);
        const auto file = require_indexed_file(snapshot, gz_file);

        auto members = index_take(index::members(snapshot, file));
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

    TEST_CASE("Member scan finds every member across scan windows") {
        // Hex text compresses to about half, so the members span several
        // scan windows and many headers sit near a window edge.
        dstd::Vector<dstd::String> texts;
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        while (texts.size() < 400) {
            dstd::String t;
            const std::size_t lines = 600 + texts.size() % 97;
            for (std::size_t l = 0; l < lines; ++l) {
                for (int k = 0; k < 6; ++k) {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                    static constexpr char HEX[] = "0123456789abcdef";
                    for (int b = 0; b < 16; ++b)
                        t += HEX[(state >> (b * 4)) & 0xF];
                }
                t += '\n';
            }
            texts.push_back(std::move(t));
        }
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);
        const std::uint64_t file_size = dstd::fs::file_size(gz_file).value();
        REQUIRE(file_size >
                2 * dftracer::utils::index::gzip::MEMBER_SCAN_WINDOW);

        dstd::Vector<dftracer::utils::index::gzip::GzipMember> found;
        bool ok = false;
        const int fd = ::open(gz_file.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            ok = co_await dftracer::utils::index::gzip::
                enumerate_gzip_member_candidates(fd, file_size, found);
        });
        ::close(fd);
        REQUIRE(ok);
        REQUIRE(found.size() == c_offsets.size());
        for (std::size_t i = 0; i < found.size(); ++i) {
            CAPTURE(i);
            CHECK(found[i].c_offset == c_offsets[i]);
            CHECK(found[i].isize == texts[i].size());
            const std::uint64_t next =
                i + 1 < found.size() ? c_offsets[i + 1] : file_size;
            CHECK(found[i].c_size == next - c_offsets[i]);
        }
    }

    TEST_CASE("Pruner chunks are gzip members") {
        const dstd::Vector<dstd::String> texts = {
            dftu_lines(0, 40), dftu_lines(40, 25, "rare"), dftu_lines(65, 35)};
        auto [gz_file, c_offsets] = write_multi_member_gz(texts);

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        const auto snapshot = index_snapshot(result.index_path);
        const auto file = require_indexed_file(snapshot, gz_file);

        auto members = index_take(index::members(snapshot, file));
        auto spans = index_take(index::spans(snapshot, file));
        REQUIRE(spans.size() == members.size());
        for (std::size_t i = 0; i < spans.size(); ++i) {
            CHECK(spans[i].uc_offset == members[i].uc_offset);
            CHECK(spans[i].uc_size == members[i].uc_size);
            CHECK(spans[i].first_line_num == members[i].first_line_num);
            CHECK(spans[i].last_line_num == members[i].last_line_num);
        }

        // Events are attributed to the member that holds them, so a name
        // confined to one member yields exactly that chunk.
        auto rare_chunks = index_take(index::posting_granules(
            snapshot, file, "name",
            dftracer::utils::index::extensions::kinds::value_hash("rare")));
        REQUIRE(rare_chunks.size() == 1);
        CHECK(rare_chunks[0] == 1);

        auto common_chunks = index_take(index::posting_granules(
            snapshot, file, "name",
            dftracer::utils::index::extensions::kinds::value_hash("op")));
        std::sort(common_chunks.begin(), common_chunks.end());
        REQUIRE(common_chunks.size() == 2);
        CHECK(common_chunks[0] == 0);
        CHECK(common_chunks[1] == 2);
    }

    TEST_CASE("Member table records a single-member file") {
        auto [gz_file, c_offsets] = write_multi_member_gz({dftu_lines(0, 30)});

        auto result = build_one(gz_file);
        REQUIRE(result.success);

        const auto snapshot = index_snapshot(result.index_path);
        const auto file = require_indexed_file(snapshot, gz_file);

        auto members = index_take(index::members(snapshot, file));
        REQUIRE(members.size() == 1);
        CHECK(members[0].c_offset == 0);
        CHECK(members[0].uc_offset == 0);
        CHECK(members[0].first_line_num == 1);
        CHECK(members[0].last_line_num == 30);
    }

    // No builder-level skip: a repeat build re-indexes and must stay valid.
    TEST_CASE("Repeat build re-indexes and stays valid") {
        TestEnvironment env(valgrind_scale(1000, 10));
        dstd::String gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto first = build_one(gz_file);
        REQUIRE(first.success);

        auto second = build_one(gz_file);
        CHECK(second.success);

        const auto snapshot = index_snapshot(second.index_path);
        const auto file = require_indexed_file(snapshot, gz_file);
        CHECK(pruning_tier_current(snapshot, file));
    }

    TEST_CASE("Force rebuild") {
        TestEnvironment env(valgrind_scale(1000, 10));
        dstd::String gz_file = env.create_dft_test_gzip_file(
            static_cast<int>(valgrind_scale(1000, 10)));

        auto first = build_one(gz_file);
        REQUIRE(first.success);

        auto second = build_one(gz_file, /*force=*/true);
        CHECK(second.success);
    }

    TEST_CASE("Result has correct line count") {
        const std::size_t n_idx_lines = valgrind_scale(1000, 10);
        TestEnvironment env(n_idx_lines);
        dstd::String gz_file =
            env.create_dft_test_gzip_file(static_cast<int>(n_idx_lines));

        auto result = build_one(gz_file);

        REQUIRE(result.success);
        CHECK(result.total_lines > 0);
        CHECK(result.total_lines >= n_idx_lines);
    }
}
