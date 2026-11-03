// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <atomic>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

TEST_SUITE("View") {
    TEST_CASE(
        "View - builder ops return independent views (lazy, no shared state)") {
        const auto& s = shared_trace();
        View base = View::from_file(dstd::String(s.gz), dstd::String(s.idx));
        View posix = base.duql(R"(cat == "POSIX")");
        View stdio = base.duql(R"(cat == "STDIO")");

        // Deriving posix/stdio must not mutate base or each other.
        StringSink s_base, s_posix, s_stdio;
        base.sink_json(s_base).get();
        posix.sink_json(s_posix).get();
        stdio.sink_json(s_stdio).get();

        CHECK(s_base.lines().size() == 50);
        CHECK(s_posix.lines().size() == 30);
        CHECK(s_stdio.lines().size() == 20);
    }

    TEST_CASE("View - query filters to matching events") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .duql(R"(cat == "POSIX")")
                         .sink_json(sink)
                         .get();

        auto lines = sink.lines();
        CHECK(lines.size() == 30);
        CHECK(count_containing(lines, "POSIX") == 30);
        CHECK(count_containing(lines, "STDIO") == 0);
        CHECK(stats.events_matched == 30);
        CHECK(stats.events_scanned >= 30);
    }

    TEST_CASE("View - chained filters AND together") {
        const auto& s = shared_trace();
        StringSink sink;
        View::from_file(dstd::String(s.gz), dstd::String(s.idx))
            .duql(R"(cat == "POSIX")")
            .duql(R"(name == "read")")
            .sink_json(sink)
            .get();

        CHECK(sink.lines().size() == 30);  // all POSIX are "read"
    }

    TEST_CASE("View - no filter streams all events") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .sink_json(sink)
                         .get();

        CHECK(sink.lines().size() == 50);
        CHECK(stats.events_matched == 50);
    }

    TEST_CASE("View - head caps exported events") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .head(10)
                         .sink_json(sink)
                         .get();

        CHECK(sink.lines().size() == 10);
        CHECK(stats.truncated);
    }

    TEST_CASE("View - head/slice paginate collect rows") {
        const auto& s = shared_trace();
        auto full = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                        .group_by({GroupKey::name()})
                        .collect()
                        .get();
        REQUIRE(full.num_rows() == 2);  // read (POSIX), fwrite (STDIO)

        auto one = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                       .group_by({GroupKey::name()})
                       .head(1)
                       .collect()
                       .get();
        CHECK(one.num_rows() == 1);

        auto rest = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                        .group_by({GroupKey::name()})
                        .slice(1, std::numeric_limits<std::int64_t>::max())
                        .collect()
                        .get();
        CHECK(rest.num_rows() == 1);
    }

    TEST_CASE("View - a completed scan covers every chunk it read") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .sink_json(sink)
                         .get();

        CHECK(stats.chunks_scanned > 0);
        CHECK(stats.chunks_covered == stats.chunks_scanned);
    }

    // Coverage is what the scan read, not what it returned: a head stops the
    // fan-out but does not un-read a chunk the scan had already drained.
    TEST_CASE("View - a head does not retract coverage of a drained chunk") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .head(10)
                         .sink_json(sink)
                         .get();

        CHECK(stats.truncated);
        CHECK(stats.chunks_covered == stats.chunks_scanned);
    }

    // The invariant an artifact built during a scan depends on: a chunk cut
    // short must never be claimed, or a later query prunes against data that
    // was never seen and silently loses matches.
    TEST_CASE("View - a chunk abandoned mid-scan is never claimed") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // A chunk is handed over one 4 MB read at a time, so it takes a chunk
        // bigger than that for a cancel to land inside one rather than
        // between two. 60k events is about 5 MB uncompressed.
        dstd::String gz = create_mixed_trace(env, 60000, 0);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        SUBCASE("cancelled before the first batch") {
            StringSink sink;
            auto stats = View::from_file(dstd::String(gz), dstd::String(idx))
                             .cancel_when([] { return true; })
                             .sink_json(sink)
                             .get();
            CHECK(stats.chunks_covered == 0);
        }

        SUBCASE("cancelled part-way through a chunk") {
            // Flips once the first batch has been handed over, so the scan is
            // inside a chunk when it sees the cancel.
            struct CancelAfterFirstBatch : ExportSink {
                std::atomic<std::uint64_t> events{0};
                void write(dstd::StringView data) override {
                    if (data == "\n") events.fetch_add(1);
                }
            } sink;

            auto stats =
                View::from_file(dstd::String(gz), dstd::String(idx))
                    .cancel_when([&] { return sink.events.load() > 0; })
                    .sink_json(sink)
                    .get();

            REQUIRE(stats.chunks_scanned == 1);
            // Part of the chunk, but not all of it, and so none of it.
            CHECK(sink.events.load() > 0);
            CHECK(sink.events.load() < 60000);
            CHECK(stats.chunks_covered == 0);
        }
    }

    // Regression: a multi-member gzip must not drop the event straddling each
    // member boundary. Small members force many boundaries; every event must be
    // read exactly once (no drop, no duplicate).
    TEST_CASE("View - multi-member scan reads every event") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const int n = 60;  // ~10 members at 512 bytes
        dstd::String gz = create_multimember_trace(env, n, 512);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        dftracer::utils::index::Indexer::open(
            {dstd::String(dstd::StringView(gz))})
            .build();

        StringSink sink;
        auto stats = View::from_file(dstd::String(gz), dstd::String(idx))
                         .sink_json(sink)
                         .get();

        CHECK(stats.events_matched == static_cast<std::uint64_t>(n));
        CHECK(sink.lines().size() == static_cast<std::size_t>(n));
    }

    // sink_trace writes the events back out through the parallel writer as a
    // multi-member gzip trace; every event must survive the round-trip.
    TEST_CASE("View - sink_trace round-trips every event") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const int n = 60;  // multi-member source; small members below
        dstd::String gz = create_multimember_trace(env, n, 512);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dstd::String out = env.get_dir() + "/merged.pfw.gz";
        TraceWriteOptions opts;
        opts.output_path = out;
        opts.member_size = 4096;  // small members -> many boundaries
        opts.num_workers = 4;
        opts.compress = true;
        auto stats = View::from_file(dstd::String(gz), dstd::String(idx))
                         .sink_trace(opts)
                         .get();
        CHECK(stats.events_matched == static_cast<std::uint64_t>(n));

        dstd::String oidx = dstd::String(determine_index_path(out, ""));
        StringSink sink;
        auto rstats = View::from_file(dstd::String(out), dstd::String(oidx))
                          .sink_json(sink)
                          .get();
        CHECK(rstats.events_matched == static_cast<std::uint64_t>(n));
        CHECK(sink.lines().size() == static_cast<std::size_t>(n));
    }

    // A write that cannot open its output fails; the scan feeding it through
    // a bounded channel must not wait forever on the stopped writer.
    TEST_CASE("View - an indexed sink_trace to an unwritable path fails") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_multimember_trace(env, 300, 512);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        TraceWriteOptions opts;
        opts.output_path = env.get_dir() + "/no_such_dir/out.pfw.gz";
        opts.member_size = 4096;
        opts.num_workers = 4;
        opts.build_index = true;
        CHECK_THROWS_AS(View::from_file(dstd::String(gz), dstd::String(idx))
                            .sink_trace(opts)
                            .get(),
                        dftracer::utils::common::DFTUtilsException);
    }

    // The index built inline during sink_trace (build_index) must be
    // byte-for-byte equivalent, for reads and pruned queries, to one built
    // lazily by the standard indexer over the identical output bytes.
    TEST_CASE("View - fused inline index matches a lazily-built index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Mixed cats so a cat filter can prune whole members.
        dstd::String gz = create_mixed_trace(env, 20, 20);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // A: write the output with the index fused into the write.
        dstd::String dirA = env.get_dir() + "/a";
        REQUIRE(dstd::fs::create_directories(dirA).has_value());
        const dstd::String outAgz = dirA + "/out.pfw.gz";
        TraceWriteOptions opts;
        opts.output_path = outAgz;
        opts.member_size = 512;  // small members -> several to key per-member
        opts.num_workers = 4;
        opts.compress = true;
        opts.build_index = true;
        auto st = View::from_file(dstd::String(gz), dstd::String(idx))
                      .sink_trace(opts)
                      .get();
        REQUIRE(dstd::fs::exists(outAgz).value());
        const dstd::String idxA =
            dstd::String(determine_index_path(outAgz, ""));
        // The write built the index, not a separate pass.
        REQUIRE(dstd::fs::exists(idxA).value());

        // B: identical output bytes in a fresh dir; View auto-indexes it lazily
        // via the standard indexer (the ground truth).
        dstd::String dirB = env.get_dir() + "/b";
        REQUIRE(dstd::fs::create_directories(dirB).has_value());
        const dstd::String outBgz = dirB + "/out.pfw.gz";
        REQUIRE(dstd::fs::copy_file(outAgz, outBgz,
                                    dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        const dstd::String idxB =
            dstd::String(determine_index_path(outBgz, ""));

        auto run = [](const dstd::String& f, const dstd::String& ix,
                      const char* q) {
            StringSink s;
            View v = View::from_file(dstd::String(f), dstd::String(ix));
            if (q != nullptr) v = v.duql(q);
            v.sink_json(s).get();
            auto ls = s.lines();
            std::sort(ls.begin(), ls.end());
            return ls;
        };

        // Full scan and pruned queries: fused (A) must equal lazy (B).
        CHECK(run(outAgz, idxA, nullptr) == run(outBgz, idxB, nullptr));
        CHECK(run(outAgz, idxA, R"(cat == "POSIX")") ==
              run(outBgz, idxB, R"(cat == "POSIX")"));
        CHECK(run(outAgz, idxA, R"(cat == "STDIO")") ==
              run(outBgz, idxB, R"(cat == "STDIO")"));
        // A query matching nothing: the bloom must prune without dropping a
        // real match (both empty).
        CHECK(run(outAgz, idxA, R"(name == "does_not_exist")").empty());
        CHECK(run(outBgz, idxB, R"(name == "does_not_exist")").empty());
        // Fused full scan returns every written event.
        CHECK(run(outAgz, idxA, nullptr).size() == st.events_matched);
    }

    // Parts are folded and compressed on parallel workers, so the member
    // table, the bloom pruning and the columns must still equal a fresh build
    // of the same bytes.
    TEST_CASE("View - fused multi-part index matches a lazily-built index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_mixed_trace(env, 400, 400);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        dstd::String dirA = env.get_dir() + "/a";
        dstd::String dirB = env.get_dir() + "/b";
        REQUIRE(dstd::fs::create_directories(dirA).has_value());
        REQUIRE(dstd::fs::create_directories(dirB).has_value());
        TraceWriteOptions opts;
        opts.output_path = dirA + "/out.pfw.gz";
        opts.member_size = 512;
        opts.part_size = 8192;
        opts.num_workers = 4;
        opts.build_index = true;
        View::from_file(dstd::String(gz), dstd::String(idx))
            .sink_trace(opts)
            .get();

        dstd::Vector<dstd::String> partsA, partsB;
        for (int i = 0; dstd::fs::exists(utilities::fileio::gzip_part_path(
                                             opts.output_path, i, true))
                            .value();
             ++i) {
            partsA.emplace_back(
                utilities::fileio::gzip_part_path(opts.output_path, i, true));
            partsB.push_back(
                dirB + "/" +
                dstd::String(dstd::PathView(partsA.back()).filename()));
            REQUIRE(dstd::fs::copy_file(partsA.back(), partsB.back(),
                                        dstd::fs::CopyMode::FailIfExists)
                        .has_value());
        }
        REQUIRE(partsA.size() > 2);

        const dstd::String idxA =
            dstd::String(determine_index_path(partsA[0], ""));
        const dstd::String idxB =
            dstd::String(determine_index_path(partsB[0], ""));
        std::uint64_t scannedA = 0, scannedB = 0, matchedA = 0, matchedB = 0;
        dstd::Vector<dstd::String> colsA, colsB;
        for (std::size_t i = 0; i < partsA.size(); ++i) {
            auto q = [&](const dstd::String& f, const dstd::String& ix,
                         std::uint64_t& sc, std::uint64_t& mt,
                         dstd::Vector<dstd::String>& cols) {
                StringSink sink;
                auto stt = View::from_file(dstd::String(f), dstd::String(ix))
                               .duql(R"(cat == "STDIO")")
                               .sink_json(sink)
                               .get();
                sc += stt.events_scanned;
                mt += stt.events_matched;
                for (auto& c :
                     View::from_file(dstd::String(f), dstd::String(ix))
                         .columns())
                    cols.emplace_back(c);
            };
            q(partsA[i], idxA, scannedA, matchedA, colsA);
            q(partsB[i], idxB, scannedB, matchedB, colsB);
        }
        CHECK(matchedA == 400);
        CHECK(matchedA == matchedB);
        CHECK(scannedA == scannedB);
        CHECK(colsA == colsB);

        for (std::size_t i = 0; i < partsA.size(); ++i) {
            auto ma = dftu_utils_test::indexed_members(idxA, partsA[i]);
            auto mb = dftu_utils_test::indexed_members(idxB, partsB[i]);
            REQUIRE(!ma.empty());
            REQUIRE(ma.size() == mb.size());
            for (std::size_t k = 0; k < ma.size(); ++k) {
                CHECK(ma[k].member_idx == mb[k].member_idx);
                CHECK(ma[k].c_offset == mb[k].c_offset);
                CHECK(ma[k].c_size == mb[k].c_size);
                CHECK(ma[k].uc_offset == mb[k].uc_offset);
                CHECK(ma[k].uc_size == mb[k].uc_size);
                CHECK(ma[k].first_line_num == mb[k].first_line_num);
                CHECK(ma[k].last_line_num == mb[k].last_line_num);
            }
        }
    }

    TEST_CASE("View - group_by + agg (count/sum/min/max/mean)") {
        const auto& s = shared_trace();  // POSIX dur 10..39
        auto table = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Count, "", "n"},
                               {AggOp::Sum, "dur", "sum_dur"},
                               {AggOp::Min, "dur", "min_dur"},
                               {AggOp::Max, "dur", "max_dur"},
                               {AggOp::Mean, "dur", "mean_dur"}})
                         .collect()
                         .get();

        CHECK(bhas(table, "cat"));
        REQUIRE(table.num_rows() == 2);  // POSIX, STDIO

        std::int64_t posix = -1;
        for (std::int64_t i = 0; i < table.num_rows(); ++i)
            if (bstr(table, i, "cat") == "posix") posix = i;
        REQUIRE(posix >= 0);
        // 30 POSIX events, dur = 10,11,...,39
        CHECK(bnum(table, posix, "n") == doctest::Approx(30));        // count
        CHECK(bnum(table, posix, "sum_dur") ==
              doctest::Approx(30 * 24.5));                            // sum
        CHECK(bnum(table, posix, "min_dur") == doctest::Approx(10));  // min
        CHECK(bnum(table, posix, "max_dur") == doctest::Approx(39));  // max
        CHECK(bnum(table, posix, "mean_dur") == doctest::Approx(24.5));  // mean
    }

    TEST_CASE("View - collect with no group_by returns the matching events") {
        const auto& s = shared_trace();  // 30 POSIX + 20 STDIO = 50 events
        auto table = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .collect()
                         .get();
        REQUIRE(table.num_rows() == 50);
        // Every event row carries the top-level columns.
        for (const char* c : {"name", "cat", "pid", "tid", "ts", "dur", "ph"})
            CHECK(bhas(table, c));
        // Not an aggregate: there is no synthesized count column.
        CHECK_FALSE(bhas(table, "count"));

        // A filter narrows the event rows; select projects columns.
        auto posix = View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                         .duql(R"(cat == "POSIX")")
                         .select({"name", "dur"})
                         .collect()
                         .get();
        CHECK(posix.num_rows() == 30);
        REQUIRE(posix.num_columns() == 2);
        CHECK(bhas(posix, "name"));
        CHECK(bhas(posix, "dur"));
    }

    TEST_CASE(
        "View - a flattened arg named like a top-level field stays a "
        "distinct args.<key> column") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Each event's top-level "name" is "read"/"write"; its arg "name" is a
        // different string, so the row frame must carry both "name" (top-level)
        // and "args.name" (the arg) without a schema collision.
        dstd::String pfw = env.get_dir() + "/collide.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,)"
                   R"("ts":1000,"dur":10,"args":{"name":"inner_read"}})"
                << "\n";
            ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,)"
                   R"("ts":1100,"dur":10,"args":{"name":"inner_write"}})"
                << "\n";
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // Empty select: every column, including both "name" and "args.name".
        dataframe::DataFrame all =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .collect()
                .get();
        REQUIRE(bhas(all, "name"));
        REQUIRE(bhas(all, "args.name"));
        dstd::Map<dstd::String, dstd::String> top_by_arg;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            top_by_arg[bstr(all, i, "name")] = bstr(all, i, "args.name");
        CHECK(top_by_arg["read"] == "inner_read");
        CHECK(top_by_arg["write"] == "inner_write");

        // A bare "name" select still resolves to the top-level field; the arg
        // is only reachable via its "args." prefix.
        dataframe::DataFrame bare_name =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .select({"name"})
                .collect()
                .get();
        REQUIRE(bare_name.num_columns() == 1);
        CHECK(bhas(bare_name, "name"));
        CHECK_FALSE(bhas(bare_name, "args.name"));

        dataframe::DataFrame arg_name =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .select({"args.name"})
                .collect()
                .get();
        REQUIRE(arg_name.num_columns() == 1);
        CHECK(bhas(arg_name, "args.name"));
        CHECK_FALSE(bhas(arg_name, "name"));
    }

    TEST_CASE(
        "View - nested args are columns and short names resolve to args") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String pfw = env.get_dir() + "/nested.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"X","name":"hot","cat":"APP","pid":1,"tid":1,)"
                   R"("ts":1000,"dur":42,"args":{"dur":{"p99":7000},)"
                   R"("counters":{"X":{"p50":3}},"label":{"v":"A"},)"
                   R"("cqe.raw_ns":5}})"
                << "\n";
            ofs << R"({"ph":"X","name":"cold","cat":"APP","pid":1,"tid":1,)"
                   R"("ts":1010,"dur":20,"args":{"dur":{"p99":500},)"
                   R"("counters":{"X":{"p50":3}},"label":{"v":"B"}}})"
                << "\n";
            ofs << R"({"ph":"X","name":"flat","cat":"APP","pid":1,"tid":1,)"
                   R"("ts":2000,"dur":7,"args":{"run":1}})"
                << "\n";
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));
        {
            StringSink index_build;
            View::from_file(dstd::String(gz), dstd::String(idx))
                .sink_json(index_build)
                .get();
        }
        View v = View::from_file(dstd::String(gz), dstd::String(idx));

        dataframe::DataFrame all = v.collect().get();
        REQUIRE(all.num_rows() == 3);
        dstd::Vector<dstd::String> collected = all.names;
        dstd::Vector<dstd::String> declared = v.lazy().schema();
        std::sort(collected.begin(), collected.end());
        std::sort(declared.begin(), declared.end());
        auto joined = [](const dstd::Vector<dstd::String>& xs) {
            dstd::String out;
            for (const auto& x : xs) out += x + ",";
            return out;
        };
        CAPTURE(joined(collected));
        CAPTURE(joined(declared));
        CHECK(collected == declared);
        for (const char* c : {"args.dur.p99", "args.counters.X.p50",
                              "args.label.v", "args.cqe.raw_ns", "args.run"})
            CHECK(std::binary_search(collected.begin(), collected.end(),
                                     dstd::String(c)));
        dstd::Vector<dstd::String> index_cols;
        for (const auto& name : v.columns()) index_cols.emplace_back(name);
        for (const dstd::String& c : collected) {
            if (c.rfind("args.", 0) != 0) continue;
            CAPTURE(c);
            CHECK(std::find(index_cols.begin(), index_cols.end(),
                            dstd::String(c.substr(5))) != index_cols.end());
        }

        dataframe::DataFrame sel =
            v.select({"name", "dur", "dur.p99"}).collect().get();
        REQUIRE(sel.names ==
                dstd::Vector<dstd::String>{"name", "dur", "args.dur.p99"});
        dstd::Map<dstd::String, dstd::Pair<double, double>> by_name;
        for (std::int64_t i = 0; i < sel.num_rows(); ++i)
            if (bstr(sel, i, "name") != "flat")
                by_name[bstr(sel, i, "name")] = {bnum(sel, i, "dur"),
                                                 bnum(sel, i, "args.dur.p99")};
        CHECK(by_name["hot"] == dstd::Pair<double, double>{42, 7000});
        CHECK(by_name["cold"] == dstd::Pair<double, double>{20, 500});

        dataframe::DataFrame grouped =
            v.group_by({GroupKey::field("counters.X.p50")})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get();
        REQUIRE(grouped.num_rows() == 2);
        dstd::Map<dstd::String, double> counts;
        for (std::int64_t i = 0; i < grouped.num_rows(); ++i)
            counts[dstd::String(dataframe::cell_to_string(
                grouped.columns[0], i))] = bnum(grouped, i, "n");
        CHECK(counts["3"] == 2);

        dataframe::DataFrame tree = test_view_common::run(
            v.duql(R"(name != "flat")")
                .call_tree({"pid", "tid"}, "ts", "dur", "label.v")
                .collect());
        dstd::Vector<dstd::String> labels;
        for (std::int64_t i = 0; i < tree.num_rows(); ++i)
            labels.push_back(bstr(tree, i, "name"));
        std::sort(labels.begin(), labels.end());
        CHECK(labels == dstd::Vector<dstd::String>{"A", "B"});
    }

    TEST_CASE(
        "View - agg_numeric_args aggregates every numeric arg as a mean") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_trace_with_counters(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // ph="C" cpu counters: (user_pct,idle_pct) = (40,60) and (80,20).
        auto table = View::from_file(dstd::String(gz), dstd::String(idx))
                         .phase(Phase::Counters)
                         .group_by({GroupKey::name()})
                         .agg_numeric_args()
                         .collect()
                         .get();

        // No fixed agg -> "count", then the sorted union of numeric args.
        CHECK(bhas(table, "name"));
        CHECK(bhas(table, "count"));
        CHECK(bhas(table, "idle_pct"));
        CHECK(bhas(table, "user_pct"));

        REQUIRE(table.num_rows() == 1);
        CHECK(bstr(table, 0, "name") == "cpu");
        CHECK(bnum(table, 0, "count") == doctest::Approx(2));  // count
        CHECK(bnum(table, 0, "idle_pct") ==
              doctest::Approx(40));                            // mean (60,20)
        CHECK(bnum(table, 0, "user_pct") ==
              doctest::Approx(60));                            // mean (40,80)
    }

    TEST_CASE(
        "View - agg_numeric_args applies a set of reductions per numeric arg") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_trace_with_counters(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // ph="C" cpu counters: user_pct=(40,80), idle_pct=(60,20).
        auto table =
            View::from_file(dstd::String(gz), dstd::String(idx))
                .phase(Phase::Counters)
                .group_by({GroupKey::name()})
                .agg_numeric_args({AggSpec(AggOp::Sum), AggSpec(AggOp::Max),
                                   AggSpec(AggOp::Mean)})
                .collect()
                .get();

        REQUIRE(table.num_rows() == 1);
        // Each arg emits one <op>_<arg> column; the legacy bare name is gone.
        CHECK_FALSE(bhas(table, "user_pct"));
        CHECK(bnum(table, 0, "sum_user_pct") == doctest::Approx(120));  // 40+80
        CHECK(bnum(table, 0, "max_user_pct") == doctest::Approx(80));
        CHECK(bnum(table, 0, "mean_user_pct") == doctest::Approx(60));
        CHECK(bnum(table, 0, "sum_idle_pct") == doctest::Approx(80));   // 60+20
        CHECK(bnum(table, 0, "max_idle_pct") == doctest::Approx(60));
        CHECK(bnum(table, 0, "mean_idle_pct") == doctest::Approx(40));
    }

    TEST_CASE(
        "View - select a counter arg then group_by/agg over it (expr path)") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // ph="X" events with no "cycles", plus ph="C" counters whose "cycles"
        // arg is a string, so the selected column is String-typed. The Python
        // two-step .group_by(k).agg("count:cycles"/"sum:cycles") lowers to
        // group_agg_expr, which used to feed an invalid (non-numeric) evaluated
        // column into the aggregator and segfault.
        dstd::String pfw = env.get_dir() + "/cyc.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < 4; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << (1000 + i * 100) << R"(,"dur":10,"args":{}})" << "\n";
            const char* vals[] = {"lots", "few", "some"};
            for (int i = 0; i < 6; ++i) {
                const char* nm = (i % 2 == 0) ? "cpu" : "gpu";
                ofs << R"({"ph":"C","name":")" << nm
                    << R"(","cat":"sys","pid":0,"tid":0,"ts":)"
                    << (2000 + i * 10) << R"(,"args":{"cycles":")"
                    << vals[i % 3] << R"("}})"
                    << "\n";
            }
        }
        dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        (void)dstd::fs::remove(pfw);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        // Non-deterministic corruption: repeat so an uninitialized/garbage read
        // has several chances to surface.
        for (int rep = 0; rep < 8; ++rep) {
            dataframe::DataFrame rows =
                View::from_file(dstd::String(gz), dstd::String(idx))
                    .select({"name", "cycles"})
                    .collect()
                    .get();
            REQUIRE(rows.num_columns() == 2);
            REQUIRE(bhas(rows, "args.cycles"));
            REQUIRE(rows.num_rows() == 10);
            REQUIRE(rows.columns[static_cast<std::size_t>(
                                     bcol(rows, "args.cycles"))]
                        .type() == dataframe::TypeId::String);

            // Tally (name, cycles) pairs; a null cycles reads as empty.
            dstd::Map<dstd::String, int> name_counts, cyc_counts;
            int nnull = 0;
            for (std::int64_t i = 0; i < rows.num_rows(); ++i) {
                name_counts[bstr(rows, i, "name")]++;
                const dstd::String c = bstr(rows, i, "args.cycles");
                if (c.empty())
                    nnull++;
                else
                    cyc_counts[c]++;
            }
            CHECK(name_counts["read"] == 4);
            CHECK(name_counts["cpu"] == 3);
            CHECK(name_counts["gpu"] == 3);
            CHECK(nnull == 4);  // the 4 ph="X" rows lack "cycles"
            CHECK(cyc_counts["lots"] == 2);
            CHECK(cyc_counts["few"] == 2);
            CHECK(cyc_counts["some"] == 2);

            // The expr-aggregate path the Python GroupBy uses: count carries a
            // value expr (ignored), count_valid counts the present strings,
            // and a numeric sum over the String value is refused rather than
            // reported as a number.
            dstd::Vector<const dataframe::Series*> inputs;
            for (const auto& col : rows.columns) inputs.push_back(&col);
            const auto cyc = dataframe::expr_col(
                static_cast<std::int32_t>(bcol(rows, "args.cycles")));
            const auto key = dataframe::expr_col(
                static_cast<std::int32_t>(bcol(rows, "name")));
            dstd::Vector<dataframe::AggExprSpec> specs = {
                dataframe::agg_count("count_cycles")};
            specs[0].value = cyc;
            dataframe::AggExprSpec present;
            present.op = dataframe::AggOp::CountValid;
            present.value = cyc;
            present.out = "present_cycles";
            specs.push_back(present);
            dataframe::DataFrame g =
                dataframe::group_agg_expr(key, specs, inputs, "name");

            REQUIRE(bhas(g, "count_cycles"));
            REQUIRE(bhas(g, "present_cycles"));
            REQUIRE(g.num_rows() == 3);  // read, cpu, gpu
            for (std::int64_t i = 0; i < g.num_rows(); ++i) {
                const dstd::String nm = bstr(g, i, "name");
                CHECK(bnum(g, i, "count_cycles") ==
                      doctest::Approx(nm == "read" ? 4 : 3));
                CHECK(bnum(g, i, "present_cycles") ==
                      doctest::Approx(nm == "read" ? 0 : 3));
            }
            CHECK_THROWS_AS(dataframe::group_agg_expr(
                                key, {dataframe::agg_sum(cyc, "sum_cycles")},
                                inputs, "name"),
                            std::invalid_argument);
        }
    }

    TEST_CASE("View - sink_counters emits a ph=C event per group") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String gz = create_trace_with_counters(env);
        dstd::String idx = dstd::String(determine_index_path(gz, ""));

        StringSink sink;
        View::from_file(dstd::String(gz), dstd::String(idx))
            .phase(Phase::Counters)
            .group_by({GroupKey::name()})
            .agg_numeric_args()
            .sink_counters(sink)
            .get();

        auto lines = sink.lines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"cpu")") != dstd::String::npos);
        CHECK(lines[0].find(R"("ph":2)") != dstd::String::npos);
        CHECK(lines[0].find(R"("user_pct":60)") != dstd::String::npos);
        CHECK(lines[0].find(R"("idle_pct":40)") != dstd::String::npos);
    }

    TEST_CASE("View - from_directory scans a tree of traces") {
        TestEnvironment env{200};
        dstd::String dir = env.get_dir() + "/tree";
        REQUIRE(dstd::fs::create_directories(dir + "/sub").has_value());

        // Distinct cat + pid per file so the grouped counts unambiguously
        // report which files (and how many events) the scan actually read.
        auto write_trace = [](const dstd::String& gz, const dstd::String& cat,
                              int pid, int n) {
            dstd::String pfw = gz.substr(0, gz.size() - 3);  // drop ".gz"
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < n; ++i)
                ofs << R"({"ph":"X","name":"read","cat":")" << cat
                    << R"(","pid":)" << pid << R"(,"tid":1,"ts":)" << (1000 + i)
                    << R"(,"dur":5,"args":{}})" << "\n";
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw, gz);
            (void)dstd::fs::remove(pfw);
        };
        write_trace(dir + "/a.pfw.gz", "AAA", 1, 10);
        write_trace(dir + "/b.pfw.gz", "BBB", 2, 20);
        write_trace(dir + "/sub/c.pfw.gz", "CCC", 3, 5);  // nested: recursive

        View v = View::from_directory(dstd::String(dir)).get();
        auto table = v.phase(Phase::Events)
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Count, "", "count"}})
                         .collect()
                         .get();

        dstd::Map<dstd::String, double> counts;
        for (std::int64_t i = 0; i < table.num_rows(); ++i)
            counts[bstr(table, i, "cat")] = bnum(table, i, "count");
        CHECK(counts["aaa"] == doctest::Approx(10.0));  // cat is lowercased
        CHECK(counts["bbb"] == doctest::Approx(20.0));
        CHECK(counts["ccc"] == doctest::Approx(5.0));
    }
}

namespace {

using namespace dftracer::utils::trace;

constexpr int DICT_ROWS = 1000;

// Row i: cat c<i%3>, name n<i%2>, args.path p<i%900>, and args.metric
// m<i%3> on even i only.
View dict_trace(TestEnvironment& env) {
    const dstd::String pfw = env.get_dir() + "/dict.pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (int i = 0; i < DICT_ROWS; ++i) {
            o << R"({"ph":"X","name":"n)" << i % 2 << R"(","cat":"c)" << i % 3
              << R"(","pid":1,"tid":1,"ts":)" << 1000 + i * 10
              << R"(,"dur":5,"args":{"path":"p)" << i % 900 << '"';
            if (i % 2 == 0) o << R"(,"metric":"m)" << i % 3 << '"';
            o << "}}\n";
        }
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
    dstd::fs::remove(pfw).has_value();
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))})
        .build();
    return View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
}

dstd::String cell(const dataframe::Series& s, std::int64_t r) {
    return s.is_null(r) ? "<null>" : dstd::String(s.string_at(r));
}

}  // namespace

TEST_SUITE("View scan string columns") {
    TEST_CASE(
        "a scan batch is a dictionary or a view column over intern chunks") {
        dstd::Vector<std::uint32_t> cat_ids, path_ids;
        dataframe::Series cat, path;
        {
            dstd::StringIntern intern;
            for (int i = 0; i < 1000; ++i) {
                cat_ids.push_back(
                    intern.get_or_insert("category-" + dstd::to_string(i % 3)));
                path_ids.push_back(intern.get_or_insert(
                    "/data/dir/file-" + dstd::to_string(i % 900)));
            }
            cat_ids[7] = dstd::StringIntern::NO_ID;
            cat = views::detail::interned_column(cat_ids, intern);
            path = views::detail::interned_column(path_ids, intern);
        }

        CHECK(cat.encoding() == dataframe::Encoding::Dictionary);
        CHECK(cat.handle()->child()->length == 3);
        CHECK(path.encoding() == dataframe::Encoding::View);
        REQUIRE(cat.length() == 1000);
        REQUIRE(path.length() == 1000);
        CHECK(cat.null_count() == 1);
        for (std::int64_t r = 0; r < 1000; ++r) {
            CHECK(cell(cat, r) ==
                  (r == 7 ? "<null>" : "category-" + dstd::to_string(r % 3)));
            CHECK(cell(path, r) ==
                  "/data/dir/file-" + dstd::to_string(r % 900));
        }
    }

    TEST_CASE("repeated, unique and null-bearing string columns") {
        TestEnvironment env(10);
        const dataframe::DataFrame f =
            test_view_common::run(dict_trace(env).collect());
        REQUIRE(f.num_rows() == DICT_ROWS);

        const dataframe::Series cat = f.column("cat");

        const dataframe::Series path = f.column("args.path");

        const dataframe::Series metric = f.column("args.metric");

        const dataframe::Series ts = f.column("ts");
        for (std::int64_t r = 0; r < f.num_rows(); ++r) {
            const auto i = (ts.data<std::uint64_t>()[r] - 1000) / 10;
            CAPTURE(i);
            CHECK(cell(cat, r) == "c" + dstd::to_string(i % 3));
            CHECK(cell(path, r) == "p" + dstd::to_string(i % 900));
            CHECK(cell(metric, r) ==
                  (i % 2 == 0 ? "m" + dstd::to_string(i % 3) : "<null>"));
        }
    }

    TEST_CASE("filter, group and sort read string values") {
        TestEnvironment env(10);
        const View v = dict_trace(env);
        auto q = [&](const char* text) {
            return test_view_common::run(v.duql(text).collect());
        };

        CHECK(q(R"(where cat == "c1")").num_rows() == 333);
        CHECK(q(R"(where starts_with(cat, "c"))").num_rows() == DICT_ROWS);
        CHECK(q(R"(where args.metric == "m0")").num_rows() == 167);

        const dataframe::DataFrame g =
            q("group cat { n = count() } | sort cat");
        REQUIRE(g.num_rows() == 3);
        const dataframe::Series gc = g.column("cat");
        CHECK(cell(gc, 0) == "c0");
        CHECK(cell(gc, 1) == "c1");
        CHECK(cell(gc, 2) == "c2");

        const dataframe::DataFrame s = q("sort -cat | take 1 | select cat");
        REQUIRE(s.num_rows() == 1);
        CHECK(cell(s.column("cat"), 0) == "c2");
    }
}
