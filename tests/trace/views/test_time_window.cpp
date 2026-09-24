#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

struct Event {
    std::uint64_t ts;
    std::uint64_t dur;
};

// Events over gzip members of `member_bytes`, plus one thread_name record, and
// an index for them.
std::string write_trace(TestEnvironment& env, const std::string& name,
                        const std::vector<Event>& events,
                        std::size_t member_bytes) {
    std::ostringstream out;
    out << "[\n";
    out << R"({"name":"thread_name","ph":"M","pid":1,"tid":1,)"
           R"("args":{"name":"main"}})"
        << "\n";
    int id = 0;
    for (const auto& e : events)
        out << R"({"id":)" << id++ << R"(,"name":"read","cat":"POSIX",)"
            << R"("pid":1,"tid":1,"ph":"X","ts":)" << e.ts << R"(,"dur":)"
            << e.dur << R"(,"args":{"bytes":1}})" << "\n";
    out << "]\n";
    const auto plain = env.get_dir() + "/" + name + ".pfw";
    {
        std::ofstream(plain) << out.str();
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               member_bytes));
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

std::int64_t rows_in(const std::string& gz, double b, double e) {
    return view_of(gz).time_range(b, e).collect().get().num_rows();
}

double agg_in(const std::string& gz, double b, double e, AggOp op,
              const std::string& field) {
    auto df = view_of(gz)
                  .time_range(b, e)
                  .group_by({GroupKey::name()})
                  .agg({{op, field, "v"}})
                  .collect()
                  .get();
    return df.num_rows() == 0 ? 0.0 : bnum(df, 0, "v");
}

// count, sum(dur), busy, utilization and active per time_bucket ("" with no
// bucket), of every "read" event.
struct Occ {
    double count = 0;
    double sum = 0;
    double busy = 0;
    double util = 0;
    double active = 0;
};

std::map<std::string, Occ> occupancy(const View& v) {
    auto df = v.group_by({GroupKey::name()})
                  .agg({{AggOp::Count, "", "n"},
                        {AggOp::Sum, "dur", "s"},
                        {AggOp::Busy, "", "b"},
                        {AggOp::Utilization, "", "u"},
                        {AggOp::Active, "", "a"}})
                  .collect()
                  .get();
    std::map<std::string, Occ> out;
    for (std::int64_t r = 0; r < df.num_rows(); ++r) {
        const std::string key =
            bhas(df, "time_bucket") ? bstr(df, r, "time_bucket") : "";
        out[key] = {bnum(df, r, "n"), bnum(df, r, "s"), bnum(df, r, "b"),
                    bnum(df, r, "u"), bnum(df, r, "a")};
    }
    return out;
}

Occ window_occ(const std::string& gz, double b, double e) {
    auto m = occupancy(view_of(gz).time_range(b, e));
    return m.empty() ? Occ{} : m.begin()->second;
}

// The length of the union of `events` clamped to [b, e).
double union_in(const std::vector<Event>& events, double b, double e) {
    std::vector<std::pair<double, double>> iv;
    for (const auto& ev : events) {
        const double s = std::max(static_cast<double>(ev.ts), b);
        const double t = std::min(static_cast<double>(ev.ts + ev.dur), e);
        if (t > s) iv.push_back({s, t});
    }
    std::sort(iv.begin(), iv.end());
    double busy = 0, end = -1;
    for (const auto& [s, t] : iv) {
        if (s > end) {
            busy += t - s;
            end = t;
        } else if (t > end) {
            busy += t - end;
            end = t;
        }
    }
    return busy;
}

}  // namespace

TEST_SUITE("TimeWindow") {
    TEST_CASE("a window keeps the events that start in it") {
        TestEnvironment env(10);
        std::vector<Event> events;
        for (int i = 0; i < 10; ++i)
            events.push_back({1000 + static_cast<std::uint64_t>(i) * 100, 5});
        const auto gz = write_trace(env, "one", events, 1 << 20);

        auto df = view_of(gz).time_range(1200, 1500).collect().get();
        REQUIRE(df.num_rows() == 3);
        std::vector<double> ts;
        for (std::int64_t r = 0; r < df.num_rows(); ++r)
            ts.push_back(bnum(df, r, "ts"));
        std::sort(ts.begin(), ts.end());
        CHECK(ts == std::vector<double>{1200, 1300, 1400});

        CHECK(agg_in(gz, 1200, 1500, AggOp::Count, "") == 3);
        CHECK(view_of(gz)
                  .time_range(1200, 1500)
                  .query(R"(name == "read")")
                  .collect()
                  .get()
                  .num_rows() == 3);
        // The end is excluded; an event at the start is kept.
        CHECK(rows_in(gz, 1200, 1300) == 1);
    }

    TEST_CASE("metadata records are not windowed") {
        TestEnvironment env(10);
        const auto gz =
            write_trace(env, "meta", {{1000, 5}, {5000, 5}}, 1 << 20);
        StringSink sink;
        view_of(gz)
            .time_range(4000, 6000)
            .emit_all_metadata(true)
            .sink_json(sink)
            .get();
        const auto& lines = sink.lines();
        CHECK(count_containing(lines, "thread_name") == 1);
        CHECK(count_containing(lines, R"("ts":5000)") == 1);
        CHECK(count_containing(lines, R"("ts":1000)") == 0);
    }

    TEST_CASE("windows over many chunks select exactly and tile") {
        TestEnvironment env(10);
        // Long events: many start in one chunk and end several chunks later,
        // so start pruning and end pruning differ.
        std::vector<Event> events;
        for (int i = 0; i < 4000; ++i)
            events.push_back(
                {1000000 + static_cast<std::uint64_t>(i) * 50,
                 static_cast<std::uint64_t>(i % 7 == 0 ? 20000 : 30)});
        const auto gz = write_trace(env, "many", events, 4 * 1024);

        auto expect = [&](double b, double e) {
            std::int64_t n = 0;
            for (const auto& ev : events)
                n += (b <= static_cast<double>(ev.ts) &&
                      static_cast<double>(ev.ts) < e)
                         ? 1
                         : 0;
            return n;
        };
        for (auto [b, e] :
             std::vector<std::pair<double, double>>{{1010000, 1060000},
                                                    {1000000, 1000050},
                                                    {1199950, 1300000},
                                                    {0, 1000000},
                                                    {1100025, 1100075}}) {
            CAPTURE(b);
            CAPTURE(e);
            CHECK(rows_in(gz, b, e) == expect(b, e));
        }

        const double a = 1000000, m = 1070000, c = 1150000;
        CHECK(agg_in(gz, a, m, AggOp::Count, "") +
                  agg_in(gz, m, c, AggOp::Count, "") ==
              agg_in(gz, a, c, AggOp::Count, ""));
        CHECK(agg_in(gz, a, m, AggOp::Sum, "dur") +
                  agg_in(gz, m, c, AggOp::Sum, "dur") ==
              agg_in(gz, a, c, AggOp::Sum, "dur"));
    }

    TEST_CASE("chunks with events at ts 0 are skipped outside the window") {
        TestEnvironment env(10);
        // Every chunk holds clockless events at ts 0 next to real ones, so its
        // start bounds always reach 0.
        std::vector<Event> events;
        for (int i = 0; i < 4000; ++i)
            events.push_back(
                {i % 5 == 0 ? 0
                            : 1000000 + static_cast<std::uint64_t>(i) * 5000,
                 30});
        const auto gz = write_trace(env, "zero", events, 4 * 1024);

        auto expect = [&](double b, double e) {
            std::int64_t n = 0;
            for (const auto& ev : events)
                n += (b <= static_cast<double>(ev.ts) &&
                      static_cast<double>(ev.ts) < e)
                         ? 1
                         : 0;
            return n;
        };
        for (auto [b, e] : std::vector<std::pair<double, double>>{
                 {11000000, 12000000}, {0, 1}, {0, 2000000}, {19000000, 0}}) {
            CAPTURE(b);
            CAPTURE(e);
            CHECK(rows_in(gz, b, e) == expect(b, e));
        }
        const auto stats =
            run(view_of(gz)
                    .metadata(false)
                    .time_range(11000000, 12000000)
                    .map_batches<int>(
                        [](int&, const std::vector<std::string_view>&) {},
                        [](int&& a, int&&) { return a; }, 1))
                .stats;
        CHECK(stats.chunks_scanned < 10);
    }

    TEST_CASE("occupancy is clipped to the window") {
        TestEnvironment env(10);
        // Crosses the left edge, inside, crosses the right edge, and a
        // zero-duration event inside.
        const auto gz = write_trace(
            env, "edges", {{900, 400}, {1500, 100}, {1900, 600}, {1700, 0}},
            1 << 20);
        const Occ o = window_occ(gz, 1000, 2000);
        CHECK(o.count == 3);
        CHECK(o.sum == 700);
        CHECK(o.busy == 500);
        CHECK(o.util == doctest::Approx(0.5));
        CHECK(o.active == 1);

        const auto whole =
            write_trace(env, "whole", {{0, 5000}, {3000, 10}}, 1 << 20);
        const Occ w = window_occ(whole, 1000, 2000);
        CHECK(w.count == 0);
        CHECK(w.busy == 1000);
        CHECK(w.util == doctest::Approx(1.0));
        CHECK(w.active == 1);
    }

    TEST_CASE("occupancy of adjacent windows tiles") {
        TestEnvironment env(10);
        std::vector<Event> events;
        for (int i = 0; i < 4000; ++i)
            events.push_back(
                {1000000 + static_cast<std::uint64_t>(i) * 50,
                 static_cast<std::uint64_t>(i % 7 == 0 ? 20000 : 30)});
        const auto gz = write_trace(env, "tile", events, 4 * 1024);

        const double a = 1000000, m = 1070000, c = 1150000;
        const Occ l = window_occ(gz, a, m);
        const Occ r = window_occ(gz, m, c);
        const Occ all = window_occ(gz, a, c);
        CHECK(l.busy == union_in(events, a, m));
        CHECK(r.busy == union_in(events, m, c));
        CHECK(l.busy + r.busy == all.busy);
        CHECK(l.busy <= m - a);
        CHECK(l.count + r.count == all.count);
        CHECK(l.sum + r.sum == all.sum);
    }

    TEST_CASE("a long event is split across buckets") {
        TestEnvironment env(10);
        const auto gz = write_trace(env, "long", {{10000, 3500}}, 1 << 20);
        const auto m = occupancy(view_of(gz).time_bucket(1000));
        REQUIRE(m.size() == 4);
        for (const char* k : {"10000", "11000", "12000"}) {
            CAPTURE(k);
            CHECK(m.at(k).busy == 1000);
            CHECK(m.at(k).util == doctest::Approx(1.0));
            CHECK(m.at(k).active == 1);
        }
        CHECK(m.at("13000").busy == 500);
        CHECK(m.at("13000").util == doctest::Approx(0.5));
        CHECK(m.at("10000").count == 1);
        CHECK(m.at("10000").sum == 3500);
        for (const char* k : {"11000", "12000", "13000"}) {
            CAPTURE(k);
            CHECK(m.at(k).count == 0);
        }

        const auto w =
            occupancy(view_of(gz).time_range(11500, 12500).time_bucket(1000));
        REQUIRE(w.size() == 2);
        CHECK(w.at("11000").busy == 500);
        CHECK(w.at("11000").util == doctest::Approx(1.0));
        CHECK(w.at("12000").busy == 500);
        CHECK(w.at("11000").count + w.at("12000").count == 0);
    }

    TEST_CASE("buckets of many events tile the window") {
        TestEnvironment env(10);
        std::vector<Event> events;
        for (int i = 0; i < 4000; ++i)
            events.push_back(
                {1000000 + static_cast<std::uint64_t>(i) * 50,
                 static_cast<std::uint64_t>(i % 7 == 0 ? 20000 : 30)});
        const auto gz = write_trace(env, "buckets", events, 4 * 1024);
        const double a = 1010000, c = 1150000;
        const auto m =
            occupancy(view_of(gz).time_range(a, c).time_bucket(10000));
        Occ total;
        for (const auto& [k, o] : m) {
            const double b = std::stod(k);
            CAPTURE(k);
            CHECK(o.busy ==
                  union_in(events, std::max(a, b), std::min(c, b + 10000)));
            total.busy += o.busy;
            total.count += o.count;
            total.sum += o.sum;
        }
        const Occ all = window_occ(gz, a, c);
        CHECK(total.busy == all.busy);
        CHECK(total.count == all.count);
        CHECK(total.sum == all.sum);
    }

    TEST_CASE("occupancy without a window or bucket is unchanged") {
        TestEnvironment env(10);
        const std::vector<Event> events{{1000, 100}, {1050, 100}, {2000, 50}};
        const auto gz = write_trace(env, "plain", events, 1 << 20);
        const auto m = occupancy(view_of(gz));
        REQUIRE(m.size() == 1);
        const Occ& o = m.at("");
        CHECK(o.count == 3);
        CHECK(o.busy == 200);
        CHECK(o.util == doctest::Approx(200.0 / 1050.0));
        CHECK(o.active == 2);
    }

    TEST_CASE("a session windows occupancy and rows alike") {
        TestEnvironment env(10);
        const auto gz = write_trace(
            env, "session", {{900, 400}, {1500, 100}, {1900, 600}}, 1 << 20);
        View t = view_of(gz).time_range(1000, 2000);
        TraceSession s = t.session();
        Deferred<dataframe::DataFrame> occ = s.collect(
            t.group_by({GroupKey::name()})
                .agg({{AggOp::Busy, "", "b"}, {AggOp::Count, "", "n"}}));
        Deferred<dataframe::DataFrame> rows = s.collect(t.lazy());
        run(s.execute());
        CHECK(bnum(occ.get(), 0, "b") == 500);
        CHECK(bnum(occ.get(), 0, "n") == 2);
        CHECK(rows.get().num_rows() == 2);
    }

    TEST_CASE("numeric args see only the events that start in the window") {
        TestEnvironment env(10);
        const auto gz =
            write_trace(env, "args", {{900, 400}, {1500, 100}}, 1 << 20);
        auto df = view_of(gz)
                      .time_range(1000, 2000)
                      .group_by({GroupKey::name()})
                      .agg({{AggOp::Busy, "", "b"}})
                      .agg_numeric_args({AggSpec(AggOp::Count)})
                      .collect()
                      .get();
        REQUIRE(df.num_rows() == 1);
        CHECK(bnum(df, 0, "b") == 400);
        int seen = 0;
        for (std::size_t c = 0; c < df.names.size(); ++c)
            if (df.names[c].find("bytes") != std::string::npos) {
                CAPTURE(df.names[c]);
                CHECK(bnum(df, 0, df.names[c]) == 1);
                ++seen;
            }
        CHECK(seen == 1);
    }
}
