#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <limits>
#include <map>

#include "test_view_common.h"

TEST_SUITE("View") {
    TEST_CASE(
        "View - builder ops return independent views (lazy, no shared state)") {
        const auto& s = shared_trace();
        View base = View::from_file(s.gz, s.idx).metadata(false);
        View posix = base.query(R"(cat == "POSIX")");
        View stdio = base.query(R"(cat == "STDIO")");

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
        auto stats = View::from_file(s.gz, s.idx)
                         .metadata(false)
                         .query(R"(cat == "POSIX")")
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
        View::from_file(s.gz, s.idx)
            .metadata(false)
            .query(R"(cat == "POSIX")")
            .query(R"(name == "read")")
            .sink_json(sink)
            .get();

        CHECK(sink.lines().size() == 30);  // all POSIX are "read"
    }

    TEST_CASE("View - no filter streams all events") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats =
            View::from_file(s.gz, s.idx).metadata(false).sink_json(sink).get();

        CHECK(sink.lines().size() == 50);
        CHECK(stats.events_matched == 50);
    }

    TEST_CASE("View - head caps exported events") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(s.gz, s.idx)
                         .metadata(false)
                         .head(10)
                         .sink_json(sink)
                         .get();

        CHECK(sink.lines().size() == 10);
        CHECK(stats.truncated);
    }

    TEST_CASE("View - head/slice paginate collect rows") {
        const auto& s = shared_trace();
        auto full = View::from_file(s.gz, s.idx)
                        .group_by({GroupKey::name()})
                        .collect()
                        .get();
        REQUIRE(full.num_rows() == 2);  // read (POSIX), fwrite (STDIO)

        auto one = View::from_file(s.gz, s.idx)
                       .group_by({GroupKey::name()})
                       .head(1)
                       .collect()
                       .get();
        CHECK(one.num_rows() == 1);

        auto rest = View::from_file(s.gz, s.idx)
                        .group_by({GroupKey::name()})
                        .slice(1, std::numeric_limits<std::int64_t>::max())
                        .collect()
                        .get();
        CHECK(rest.num_rows() == 1);
    }

    TEST_CASE("View - a completed scan covers every chunk it read") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats =
            View::from_file(s.gz, s.idx).metadata(false).sink_json(sink).get();

        CHECK(stats.chunks_scanned > 0);
        CHECK(stats.chunks_covered == stats.chunks_scanned);
    }

    // Coverage is what the scan read, not what it returned: a head stops the
    // fan-out but does not un-read a chunk the scan had already drained.
    TEST_CASE("View - a head does not retract coverage of a drained chunk") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(s.gz, s.idx)
                         .metadata(false)
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
        std::string gz = create_mixed_trace(env, 60000, 0);
        std::string idx = determine_index_path(gz, "");

        SUBCASE("cancelled before the first batch") {
            StringSink sink;
            auto stats = View::from_file(gz, idx)
                             .metadata(false)
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
                void write(std::string_view data) override {
                    if (data == "\n") events.fetch_add(1);
                }
            } sink;

            auto stats =
                View::from_file(gz, idx)
                    .metadata(false)
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
        std::string gz = create_multimember_trace(env, n, 512);
        std::string idx = determine_index_path(gz, "");

        StringSink sink;
        auto stats =
            View::from_file(gz, idx).metadata(false).sink_json(sink).get();

        CHECK(stats.events_matched == static_cast<std::uint64_t>(n));
        CHECK(sink.lines().size() == static_cast<std::size_t>(n));
    }

    // sink_trace writes the events back out through the parallel writer as a
    // multi-member gzip trace; every event must survive the round-trip.
    TEST_CASE("View - sink_trace round-trips every event") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        const int n = 60;  // multi-member source; small members below
        std::string gz = create_multimember_trace(env, n, 512);
        std::string idx = determine_index_path(gz, "");

        std::string out = env.get_dir() + "/merged.pfw.gz";
        TraceWriteOptions opts;
        opts.output_path = out;
        opts.member_size = 4096;  // small members -> many boundaries
        opts.num_workers = 4;
        opts.compress = true;
        auto stats =
            View::from_file(gz, idx).metadata(false).sink_trace(opts).get();
        CHECK(stats.events_matched == static_cast<std::uint64_t>(n));

        std::string oidx = determine_index_path(out, "");
        StringSink sink;
        auto rstats =
            View::from_file(out, oidx).metadata(false).sink_json(sink).get();
        CHECK(rstats.events_matched == static_cast<std::uint64_t>(n));
        CHECK(sink.lines().size() == static_cast<std::size_t>(n));
    }

    // The index built inline during sink_trace (build_index) must be
    // byte-for-byte equivalent, for reads and pruned queries, to one built
    // lazily by the standard indexer over the identical output bytes.
    TEST_CASE("View - fused inline index matches a lazily-built index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Mixed cats so a cat filter can prune whole members.
        std::string gz = create_mixed_trace(env, 20, 20);
        std::string idx = determine_index_path(gz, "");

        // A: write the output with the index fused into the write.
        std::string dirA = env.get_dir() + "/a";
        fs::create_directories(dirA);
        const std::string outAgz = dirA + "/out.pfw.gz";
        TraceWriteOptions opts;
        opts.output_path = outAgz;
        opts.member_size = 512;  // small members -> several to key per-member
        opts.num_workers = 4;
        opts.compress = true;
        opts.build_index = true;
        auto st =
            View::from_file(gz, idx).metadata(false).sink_trace(opts).get();
        REQUIRE(fs::exists(outAgz));
        const std::string idxA = determine_index_path(outAgz, "");
        // The write built the index, not a separate pass.
        REQUIRE(fs::exists(idxA));

        // B: identical output bytes in a fresh dir; View auto-indexes it lazily
        // via the standard indexer (the ground truth).
        std::string dirB = env.get_dir() + "/b";
        fs::create_directories(dirB);
        const std::string outBgz = dirB + "/out.pfw.gz";
        fs::copy_file(outAgz, outBgz);
        const std::string idxB = determine_index_path(outBgz, "");

        auto run = [](const std::string& f, const std::string& ix,
                      const char* q) {
            StringSink s;
            View v = View::from_file(f, ix).metadata(false);
            if (q != nullptr) v = v.query(q);
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

    TEST_CASE("View - group_by + agg (count/sum/min/max/mean)") {
        const auto& s = shared_trace();  // POSIX dur 10..39
        auto table = View::from_file(s.gz, s.idx)
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
        auto table =
            View::from_file(s.gz, s.idx).metadata(false).collect().get();
        REQUIRE(table.num_rows() == 50);
        // Every event row carries the top-level columns.
        for (const char* c : {"name", "cat", "pid", "tid", "ts", "dur", "ph"})
            CHECK(bhas(table, c));
        // Not an aggregate: there is no synthesized count column.
        CHECK_FALSE(bhas(table, "count"));

        // A filter narrows the event rows; select projects columns.
        auto posix = View::from_file(s.gz, s.idx)
                         .metadata(false)
                         .query(R"(cat == "POSIX")")
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
        std::string pfw = env.get_dir() + "/collide.pfw";
        {
            std::ofstream ofs(pfw);
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,)"
                   R"("ts":1000,"dur":10,"args":{"name":"inner_read"}})"
                << "\n";
            ofs << R"({"ph":"X","name":"write","cat":"POSIX","pid":1,"tid":1,)"
                   R"("ts":1100,"dur":10,"args":{"name":"inner_write"}})"
                << "\n";
        }
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");

        // Empty select: every column, including both "name" and "args.name".
        dataframe::DataFrame all =
            View::from_file(gz, idx).metadata(false).collect().get();
        REQUIRE(bhas(all, "name"));
        REQUIRE(bhas(all, "args.name"));
        std::map<std::string, std::string> top_by_arg;
        for (std::int64_t i = 0; i < all.num_rows(); ++i)
            top_by_arg[bstr(all, i, "name")] = bstr(all, i, "args.name");
        CHECK(top_by_arg["read"] == "inner_read");
        CHECK(top_by_arg["write"] == "inner_write");

        // A bare "name" select still resolves to the top-level field; the arg
        // is only reachable via its "args." prefix.
        dataframe::DataFrame bare_name = View::from_file(gz, idx)
                                             .metadata(false)
                                             .select({"name"})
                                             .collect()
                                             .get();
        REQUIRE(bare_name.num_columns() == 1);
        CHECK(bhas(bare_name, "name"));
        CHECK_FALSE(bhas(bare_name, "args.name"));

        dataframe::DataFrame arg_name = View::from_file(gz, idx)
                                            .metadata(false)
                                            .select({"args.name"})
                                            .collect()
                                            .get();
        REQUIRE(arg_name.num_columns() == 1);
        CHECK(bhas(arg_name, "args.name"));
        CHECK_FALSE(bhas(arg_name, "name"));
    }

    TEST_CASE(
        "View - agg_numeric_args aggregates every numeric arg as a mean") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_trace_with_counters(env);
        std::string idx = determine_index_path(gz, "");

        // ph="C" cpu counters: (user_pct,idle_pct) = (40,60) and (80,20).
        auto table = View::from_file(gz, idx)
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
        std::string gz = create_trace_with_counters(env);
        std::string idx = determine_index_path(gz, "");

        // ph="C" cpu counters: user_pct=(40,80), idle_pct=(60,20).
        auto table =
            View::from_file(gz, idx)
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
        std::string pfw = env.get_dir() + "/cyc.pfw";
        {
            std::ofstream ofs(pfw);
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
        std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        std::string idx = determine_index_path(gz, "");

        // Non-deterministic corruption: repeat so an uninitialized/garbage read
        // has several chances to surface.
        for (int rep = 0; rep < 8; ++rep) {
            dataframe::DataFrame rows = View::from_file(gz, idx)
                                            .metadata(false)
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
            std::map<std::string, int> name_counts, cyc_counts;
            int nnull = 0;
            for (std::int64_t i = 0; i < rows.num_rows(); ++i) {
                name_counts[bstr(rows, i, "name")]++;
                const std::string c = bstr(rows, i, "args.cycles");
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
            std::vector<const dataframe::Series*> inputs;
            for (const auto& col : rows.columns) inputs.push_back(&col);
            const auto cyc = dataframe::expr_col(
                static_cast<std::int32_t>(bcol(rows, "args.cycles")));
            const auto key = dataframe::expr_col(
                static_cast<std::int32_t>(bcol(rows, "name")));
            std::vector<dataframe::AggExprSpec> specs = {
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
                const std::string nm = bstr(g, i, "name");
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
        std::string gz = create_trace_with_counters(env);
        std::string idx = determine_index_path(gz, "");

        StringSink sink;
        View::from_file(gz, idx)
            .phase(Phase::Counters)
            .group_by({GroupKey::name()})
            .agg_numeric_args()
            .sink_counters(sink)
            .get();

        auto lines = sink.lines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("name":"cpu")") != std::string::npos);
        CHECK(lines[0].find(R"("ph":2)") != std::string::npos);
        CHECK(lines[0].find(R"("user_pct":60)") != std::string::npos);
        CHECK(lines[0].find(R"("idle_pct":40)") != std::string::npos);
    }

    TEST_CASE("View - from_directory scans a tree of traces") {
        TestEnvironment env{200};
        std::string dir = env.get_dir() + "/tree";
        fs::create_directories(dir + "/sub");

        // Distinct cat + pid per file so the grouped counts unambiguously
        // report which files (and how many events) the scan actually read.
        auto write_trace = [](const std::string& gz, const std::string& cat,
                              int pid, int n) {
            std::string pfw = gz.substr(0, gz.size() - 3);  // drop ".gz"
            std::ofstream ofs(pfw);
            for (int i = 0; i < n; ++i)
                ofs << R"({"ph":"X","name":"read","cat":")" << cat
                    << R"(","pid":)" << pid << R"(,"tid":1,"ts":)" << (1000 + i)
                    << R"(,"dur":5,"args":{}})" << "\n";
            ofs.close();
            dftu_utils_test::compress_file_to_gzip(pfw, gz);
            fs::remove(pfw);
        };
        write_trace(dir + "/a.pfw.gz", "AAA", 1, 10);
        write_trace(dir + "/b.pfw.gz", "BBB", 2, 20);
        write_trace(dir + "/sub/c.pfw.gz", "CCC", 3, 5);  // nested: recursive

        View v = View::from_directory(dir).get();
        auto table = v.phase(Phase::Events)
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Count, "", "count"}})
                         .collect()
                         .get();

        std::map<std::string, double> counts;
        for (std::int64_t i = 0; i < table.num_rows(); ++i)
            counts[bstr(table, i, "cat")] = bnum(table, i, "count");
        CHECK(counts["aaa"] == doctest::Approx(10.0));  // cat is lowercased
        CHECK(counts["bbb"] == doctest::Approx(20.0));
        CHECK(counts["ccc"] == doctest::Approx(5.0));
    }
}
