// SPDX-License-Identifier: MIT
// The index answers an unfiltered query the way a full scan of the traces
// does. Every supported shape is first answered by the scan with the partials
// bypassed, then the traces are deleted and the same shape is asked again.
// Every cell that differs is reported.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace df = dftracer::utils::dataframe;
namespace ix = dftracer::utils::index;
namespace scan = dftracer::utils::trace::views::detail::scan;

namespace {

constexpr int FILES = 3;
constexpr int READS = 8;
constexpr int WRITES = 6;
constexpr int INSTANTS = 4;
constexpr int CPU_COUNTERS = 5;
constexpr int PREAGG_COUNTERS = 3;
constexpr int AGGREGATED = 2;
constexpr int SAMPLES = 3;
constexpr int SECONDS = 3;
constexpr std::size_t MEMBER_BYTES = 1024;
constexpr std::size_t MAX_REPORTED_CELLS = 8;
constexpr std::uint64_t BASE_TS = 1'000'000'000;
constexpr std::uint64_t MICROS_PER_S = 1'000'000;
constexpr std::uint64_t WINDOW_US = 1'000'000;
constexpr double NUMERIC_TOLERANCE = 1e-9;
constexpr double QUANTILE_TOLERANCE = 0.03;
const char* const DATA_HASH = "2222222222222202";
// Files 0 and 1 ran on one host and file 2 on another. Each file is one
// process.
const char* const HOSTS[FILES] = {"0a0a0a0a0a0a0a01", "0a0a0a0a0a0a0a01",
                                  "0b0b0b0b0b0b0b02"};
const char* const HOST_NAMES[FILES] = {"node-a", "node-a", "node-b"};
const char* const OWN_HASHES[FILES] = {"1111111111111101", "1111111111111102",
                                       "1111111111111103"};

std::uint64_t read_dur(int file, int j) { return 10 + (j * 7 + file * 3) % 40; }
std::uint64_t write_dur(int j) { return 20 + j % 9; }

dstd::String with_dur(std::uint64_t dur) {
    return dstd::String(R"(,"dur":)") + dstd::to_string(dur);
}

void write_trace(dftu_utils_test::FileOut& o, int i) {
    const dstd::String pid = dstd::to_string(100 + i);
    const dstd::String host = dstd::String(R"("hhash":")") + HOSTS[i] + R"(")";
    const dstd::String own =
        dstd::String(R"("fhash":")") + OWN_HASHES[i] + R"(")";
    const dstd::String data = dstd::String(R"("fhash":")") + DATA_HASH + R"(")";
    auto rec = [&](const char* ph, const dstd::String& name,
                   const dstd::String& cat, const dstd::String& tid,
                   std::uint64_t ts, const dstd::String& dur,
                   const dstd::String& args) {
        o << R"({"ph":")" << ph << R"(","name":")" << name << R"(","cat":")"
          << cat << R"(","pid":)" << pid << R"(,"tid":)" << tid << R"(,"ts":)"
          << ts << dur << R"(,"args":{)" << args << "}}\n";
    };

    rec("M", "FH", "dftracer", "0", BASE_TS, dstd::String(),
        dstd::String(R"("value":")") + OWN_HASHES[i] + R"(","name":"/data/t)" +
            dstd::to_string(i) + R"(.pfw")");
    rec("M", "HH", "dftracer", "0", BASE_TS, dstd::String(),
        dstd::String(R"("value":")") + HOSTS[i] + R"(","name":")" +
            HOST_NAMES[i] + R"(")");
    rec("M", "SH", "dftracer", "0", BASE_TS, dstd::String(),
        R"("value":"ab12cd34ef56ab78","name":"read")");
    rec("M", "PR", "dftracer", "0", BASE_TS, dstd::String(),
        dstd::String(R"("name":"rank","value":)") + dstd::to_string(i));
    rec("M", "thread_name", "dftracer", "0", BASE_TS, dstd::String(),
        R"("name":"1","value":"main")");

    for (int s = 0; s < SECONDS; ++s) {
        const std::uint64_t base =
            BASE_TS + static_cast<std::uint64_t>(s) * MICROS_PER_S;
        for (int j = 0; j < READS; ++j) {
            const dstd::String dur =
                j % 4 == 3 ? dstd::String() : with_dur(read_dur(i, j));
            rec("X", "read", "POSIX", "1",
                base + static_cast<std::uint64_t>(j) * 100, dur,
                host + "," + own + R"(,"ret":)" +
                    dstd::to_string(512 * (1 + j % 4)) + R"(,"offset":)" +
                    dstd::to_string(j * 512));
        }
        if (s != 0) continue;
        for (int j = 0; j < WRITES; ++j)
            rec("X", "fwrite", "STDIO", "2",
                base + 40000 + static_cast<std::uint64_t>(j) * 100,
                with_dur(write_dur(j)),
                host + "," + data + R"(,"ret":)" +
                    dstd::to_string(128 * (1 + j % 3)));
        for (int e = 0; e < INSTANTS; ++e)
            rec("i", "task_begin", "app", "1",
                base + 5000 + static_cast<std::uint64_t>(e) * 700,
                dstd::String(), host);
        for (int e = 0; e < CPU_COUNTERS; ++e)
            rec("C", "cpu", "sys", "0",
                base + 20000 + static_cast<std::uint64_t>(e) * 100,
                dstd::String(),
                host + R"(,"user_pct":)" + dstd::to_string(30 + e * 5) +
                    R"(,"idle_pct":)" + dstd::to_string(70 - e * 5));
        // A counter of counts and sums, as a folded run writes it.
        for (int e = 0; e < PREAGG_COUNTERS; ++e)
            rec("C", "pwrite", "STDIO", "2",
                base + 60000 + static_cast<std::uint64_t>(e) * 100,
                dstd::String(),
                host + "," + data +
                    R"(,"count":4,"dur_sum":40,"dur_min":5,"dur_max":20)" +
                    R"(,"ret_sum":4096,"offset_sum":8192)");
        // A folded event: the parser reads ph "A" as an aggregated record.
        for (int e = 0; e < AGGREGATED; ++e)
            rec("A", "read", "POSIX", "1",
                base + 70000 + static_cast<std::uint64_t>(e) * 100,
                dstd::String(),
                host + "," + own +
                    R"(,"count":3,"dur_sum":30,"dur_min":8,"dur_max":12)");
        // A sampling profiler's counter: a profile record, not a system one.
        for (int e = 0; e < SAMPLES; ++e)
            rec("C", "sample", "app", "0",
                base + 80000 + static_cast<std::uint64_t>(e) * 100,
                dstd::String(), host + R"(,"dur":7,"count":1)");
    }
}

struct Fixture {
    TestEnvironment env{200};
    dstd::Vector<dstd::String> gz;
    dstd::String index_path;

    Fixture() {
        REQUIRE(env.is_valid());
        for (int i = 0; i < FILES; ++i) {
            const dstd::String dir = env.get_dir() + "/f" + dstd::to_string(i);
            REQUIRE(dstd::fs::create_directories(dir).has_value());
            const dstd::String plain = dir + "/t.pfw";
            {
                dftu_utils_test::FileOut o(plain);
                write_trace(o, i);
            }
            const dstd::String out = plain + ".gz";
            REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
                plain, out, MEMBER_BYTES));
            (void)dstd::fs::remove(plain);
            gz.push_back(out);
        }
        ix::IndexerOptions o;
        o.index_dir = env.get_dir() + "/idx";
        o.checkpoint_size = MEMBER_BYTES;
        ix::Indexer::open(dftu_utils_test::dstd_paths(gz), o).build();
        index_path =
            dstd::String(determine_index_path(gz.front(), o.index_dir));
    }

    dstd::Vector<ViewFile> files() const {
        dstd::Vector<ViewFile> out;
        for (const auto& g : gz)
            out.push_back({dstd::String(g), dstd::String(index_path)});
        return out;
    }

    View view() const { return View::from_files(files()); }

    void delete_traces() const {
        for (const auto& g : gz) dstd::fs::remove(g).has_value();
    }
};

df::DataFrame scan_only(const scan::ScanPlan& p) {
    namespace detail = dftracer::utils::trace::views::detail;
    static Runtime rt;
    df::DataFrame out;
    rt.block_on(
        coro::named("scan-only",
                    coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                        auto st = co_await detail::build_engine_agg_state(*p);
                        out = detail::finalize_engine_result(*st, *p);
                    })));
    return detail::apply_agg_post_ops(std::move(out), *p);
}

struct Shape {
    dstd::String label;
    dstd::Vector<GroupKey> keys;
    dstd::Vector<dstd::String> columns;
    std::uint64_t window_us = 0;
};

dstd::Vector<Shape> shapes() {
    using K = GroupKey;
    return {
        {"no group", {}, {}},
        {"cat", {K::cat()}, {"cat"}},
        {"name", {K::name()}, {"name"}},
        {"cat+name", {K::cat(), K::name()}, {"cat", "name"}},
        {"pid", {K::pid()}, {"pid"}},
        {"tid", {K::tid()}, {"tid"}},
        {"hhash", {K::hhash()}, {"hhash"}},
        {"fhash", {K::fhash()}, {"fhash"}},
        {"pid+tid", {K::pid(), K::tid()}, {"pid", "tid"}},
        {"cat+pid+tid", {K::cat(), K::pid(), K::tid()}, {"cat", "pid", "tid"}},
        {"name+1s", {K::name()}, {"time_bucket", "name"}, WINDOW_US}};
}

struct AggCase {
    dstd::String label;
    AggSpec spec;
};

dstd::Vector<AggCase> agg_cases() {
    return {{"count", {AggOp::Count, "", "n"}},
            {"sum_dur", {AggOp::Sum, "dur", "sum_dur"}},
            {"min_dur", {AggOp::Min, "dur", "min_dur"}},
            {"max_dur", {AggOp::Max, "dur", "max_dur"}},
            {"mean_dur", {AggOp::Mean, "dur", "mean_dur"}},
            {"var_dur", {AggOp::Var, "dur", "var_dur"}},
            {"sum_size", {AggOp::Sum, "size", "sum_size"}},
            {"min_ts", {AggOp::Min, "ts", "min_ts"}},
            {"max_ts", {AggOp::Max, "ts", "max_ts"}},
            {"p50_dur", {AggOp::Pct, "dur", "p50_dur", "", 0.5}},
            {"p90_dur", {AggOp::Pct, "dur", "p90_dur", "", 0.9}},
            {"distinct_pid", {AggOp::SetUnion, "pid", "pids"}},
            {"distinct_tid", {AggOp::SetUnion, "tid", "tids"}}};
}

dstd::String format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return buf;
}

const df::Series* column_of(const df::DataFrame& f, const dstd::String& name) {
    const std::int64_t c = bcol(f, name);
    return c < 0 ? nullptr : &f.columns[static_cast<std::size_t>(c)];
}

dstd::String printable(dstd::String s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ',';
    return s;
}

dstd::String cell_text(const df::Series& column, std::int64_t row) {
    if (column.is_null(row)) return "null";
    const df::Series c = column.materialize();
    using T = df::TypeId;
    if (c.type() == T::String || c.type() == T::LargeString)
        return printable(dstd::String(c.string_at(row)));
    if (c.type() == T::Int64)
        return format_double(static_cast<double>(c.data<std::int64_t>()[row]));
    if (c.type() == T::Uint64)
        return format_double(static_cast<double>(c.data<std::uint64_t>()[row]));
    if (c.type() == T::Int32)
        return format_double(static_cast<double>(c.data<std::int32_t>()[row]));
    if (c.type() == T::Uint32)
        return format_double(static_cast<double>(c.data<std::uint32_t>()[row]));
    if (c.type() == T::Bool)
        return c.data<std::uint8_t>()[row] == 0 ? "false" : "true";
    return format_double(c.data<double>()[row]);
}

using Cells = dstd::Map<dstd::String, dstd::String>;

dstd::String key_of(const df::DataFrame& f, std::int64_t row,
                    const dstd::Vector<dstd::String>& keys) {
    dstd::String k;
    for (const auto& name : keys) {
        const df::Series* c = column_of(f, name);
        k += c == nullptr ? dstd::String("missing") : cell_text(*c, row);
        k += '|';
    }
    return k;
}

Cells cells_of(const df::DataFrame& f, const dstd::Vector<dstd::String>& keys,
               const dstd::String& column) {
    Cells out;
    const df::Series* c = column_of(f, column);
    if (c == nullptr) return out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out[key_of(f, r, keys)] = cell_text(*c, r);
    return out;
}

bool parse_number(const dstd::String& s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool same_cell(const dstd::String& index_text, const dstd::String& scan_text,
               double tolerance) {
    if (index_text == "null" || scan_text == "null")
        return index_text == scan_text;
    double a = 0.0;
    double b = 0.0;
    if (!parse_number(index_text, a) || !parse_number(scan_text, b))
        return index_text == scan_text;
    if (!std::isfinite(a) || !std::isfinite(b)) return index_text == scan_text;
    if (a == b) return true;
    const double scale = std::max(std::abs(a), std::abs(b));
    return std::abs(a - b) <= tolerance * scale;
}

struct Line {
    dstd::String shape;
    dstd::String agg;
    dstd::String status;
    dstd::String note;
    dstd::Vector<dstd::String> cells;
};

// The cells the View declines and must keep declining: a SetUnion key on every
// shape, and every aggregate of the name + 1 s window shape, whose time bucket
// an untyped plan cannot answer from the partials.
bool expected_declined(const dstd::String& shape, const dstd::String& agg) {
    return agg == "distinct_pid" || agg == "distinct_tid" || shape == "name+1s";
}

dstd::String first_line(dstd::String s) {
    if (const auto nl = s.find('\n'); nl != dstd::String::npos) s.resize(nl);
    if (s.size() > 96) s.resize(96);
    return s;
}

dstd::String join(const dstd::Vector<dstd::String>& parts) {
    dstd::String out;
    for (const auto& p : parts) {
        if (!out.empty()) out += "; ";
        out += p;
    }
    return out;
}

Cells view_cells(const dstd::Vector<ViewFile>& files,
                 const dstd::Vector<GroupKey>& keys,
                 const dstd::Vector<dstd::String>& columns,
                 const AggSpec& spec) {
    View v = View::from_files(files);
    if (!keys.empty()) v = v.group_by(keys);
    return cells_of(v.agg({spec}).collect().get(), columns,
                    dstd::String(spec.out_name));
}

void check_same_cells(const Cells& want, const Cells& got) {
    REQUIRE(!want.empty());
    if (want.size() != got.size())
        for (const auto& [k, v] : want)
            std::printf("  want %s = %s\n", k.c_str(), v.c_str());
    if (want.size() != got.size())
        for (const auto& [k, v] : got)
            std::printf("  got  %s = %s\n", k.c_str(), v.c_str());
    CHECK(got.size() == want.size());
    for (const auto& [key, value] : want) {
        const auto it = got.find(key);
        REQUIRE_MESSAGE(it != got.end(), key);
        CHECK_MESSAGE(same_cell(it->second, value, NUMERIC_TOLERANCE), key);
    }
}

}  // namespace

TEST_SUITE("ViewIndexParity") {
    TEST_CASE("the index answers every unfiltered shape like a scan") {
        Fixture fx;
        const dstd::Vector<Shape> shape_list = shapes();
        const dstd::Vector<AggCase> cases = agg_cases();

        dstd::Map<dstd::String, dstd::Map<dstd::String, Cells>> expected;
        dstd::Map<dstd::String, dstd::String> scan_error;
        for (const auto& sh : shape_list) {
            for (const auto& ac : cases) {
                auto p = scan::from_files(fx.files());
                if (!sh.keys.empty()) p = scan::group_by(p, sh.keys);
                if (sh.window_us != 0) p = scan::time_bucket(p, sh.window_us);
                p = scan::agg(p, {ac.spec});
                try {
                    expected[sh.label][ac.label] =
                        cells_of(scan_only(p), sh.columns,
                                 dstd::String(ac.spec.out_name));
                } catch (const std::exception& e) {
                    scan_error[sh.label + "/" + ac.label] = e.what();
                }
            }
        }

        fx.delete_traces();

        dstd::Vector<Line> lines;
        std::size_t equal = 0;
        std::size_t differs = 0;
        std::size_t declined = 0;
        std::size_t no_scan = 0;
        std::size_t listed = 0;
        for (const auto& sh : shape_list)
            for (const auto& ac : cases)
                if (expected_declined(sh.label, ac.label)) ++listed;
        for (const auto& sh : shape_list) {
            for (const auto& ac : cases) {
                INFO(sh.label, " / ", ac.label);
                Line line{sh.label, ac.label, "", "", {}};
                const bool listed_decline =
                    expected_declined(sh.label, ac.label);
                const auto known = scan_error.find(sh.label + "/" + ac.label);
                if (known != scan_error.end()) {
                    line.status = "no-scan";
                    line.note = known->second;
                    ++no_scan;
                    CHECK_MESSAGE(false,
                                  "the scan of this cell failed: ", line.note);
                    lines.push_back(std::move(line));
                    continue;
                }
                const Cells& want = expected[sh.label][ac.label];
                Cells got;
                try {
                    View v = fx.view();
                    if (!sh.keys.empty()) v = v.group_by(sh.keys);
                    if (sh.window_us != 0) v = v.time_bucket(sh.window_us);
                    got = cells_of(v.agg({ac.spec}).collect().get(), sh.columns,
                                   dstd::String(ac.spec.out_name));
                } catch (const std::exception& e) {
                    line.status = "declined";
                    line.note = first_line(e.what());
                    ++declined;
                    CHECK_MESSAGE(
                        listed_decline,
                        "a cell declined that is not on the list: ", line.note);
                    lines.push_back(std::move(line));
                    continue;
                }
                CHECK_MESSAGE(!listed_decline,
                              "a cell on the declined list answered");

                const double tolerance = (ac.spec.op == AggOp::Pct)
                                             ? QUANTILE_TOLERANCE
                                             : NUMERIC_TOLERANCE;
                dstd::Vector<dstd::String> diffs;
                for (const auto& [key, value] : want) {
                    const auto it = got.find(key);
                    if (it == got.end()) {
                        diffs.push_back(key + ": index=absent scan=" + value);
                    } else if (!same_cell(it->second, value, tolerance)) {
                        diffs.push_back(key + ": index=" + it->second +
                                        " scan=" + value);
                    }
                }
                for (const auto& [key, value] : got)
                    if (want.find(key) == want.end())
                        diffs.push_back(key + ": index=" + value +
                                        " scan=absent");
                if (diffs.empty()) {
                    line.status = "equal";
                    ++equal;
                } else {
                    line.status = "differs";
                    line.note = dstd::to_string(diffs.size()) + " of " +
                                dstd::to_string(want.size()) + " groups";
                    for (std::size_t i = 0;
                         i < diffs.size() && i < MAX_REPORTED_CELLS; ++i)
                        line.cells.push_back(diffs[i]);
                    if (diffs.size() > MAX_REPORTED_CELLS)
                        line.cells.push_back(
                            "+" +
                            dstd::to_string(diffs.size() - MAX_REPORTED_CELLS) +
                            " more");
                    ++differs;
                }
                CHECK_MESSAGE(diffs.empty(), join(diffs));
                lines.push_back(std::move(line));
            }
        }

        std::printf(
            "\n=== index versus scan, unfiltered traces: %zu shapes x %zu "
            "aggregates\n\n",
            shape_list.size(), cases.size());
        dstd::String shape;
        for (const auto& l : lines) {
            if (l.shape != shape) {
                std::printf("%s\n", l.shape.c_str());
                shape = l.shape;
            }
            std::printf("  %-12s %-9s %s\n", l.agg.c_str(), l.status.c_str(),
                        l.note.c_str());
            for (const auto& c : l.cells) std::printf("      %s\n", c.c_str());
        }
        std::printf("\nequal %zu, differs %zu, declined %zu, no-scan %zu\n",
                    equal, differs, declined, no_scan);
        CHECK(equal == 110);
        CHECK(differs == 0);
        CHECK(declined == listed);
        CHECK(no_scan == 0);
    }

    // A pre-aggregated counter stands for the events it counts in a trace
    // aggregation, and for one row in a frame.
    TEST_CASE(
        "a trace counts the events of a counter, a frame counts its rows") {
        TestEnvironment env{201};
        REQUIRE(env.is_valid());
        const dstd::String plain = env.get_dir() + "/preagg.pfw";
        {
            dftu_utils_test::FileOut o(plain);
            o << R"({"ph":"C","name":"pwrite","cat":"STDIO","pid":7,"tid":2,)"
                 R"("ts":1000,"args":{"count":4,"dur_sum":40,"dur_min":5,)"
                 R"("dur_max":20,"ret_sum":4096,"offset_sum":8192}})"
              << "\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(compress_file_to_gzip(plain, gz));
        (void)dstd::fs::remove(plain);
        ix::IndexerOptions o;
        o.index_dir = env.get_dir() + "/idx";
        ix::Indexer::open({dstd::String(dstd::StringView(gz))}, o).build();
        const dstd::String index =
            dstd::String(determine_index_path(gz, o.index_dir));

        const dstd::Vector<AggSpec> count{{AggOp::Count, "", "n"}};
        const df::DataFrame trace = scan_only(scan::agg(
            scan::from_file(dstd::String(gz), dstd::String(index)), count));
        REQUIRE(trace.num_rows() == 1);
        CHECK(bnum(trace, 0, "n") == doctest::Approx(4));

        const View v = View::from_file(dstd::String(gz), dstd::String(index));
        const df::DataFrame frame =
            v.with_column("z", df::expr_lit(std::int64_t{1}))
                .group_by(dstd::Vector<dstd::String>{"z"},
                          {{df::Agg::Count, "", "n"}})
                .collect()
                .get();
        REQUIRE(frame.num_rows() == 1);
        CHECK(bnum(frame, 0, "n") == doctest::Approx(1));
    }

    // A from-trace read takes the build's rules from the first index root that
    // has a config, whatever the order of the files: an unindexed file first
    // does not cost the arg group key the build kept, nor the window that
    // spans a pre-aggregated counter.
    TEST_CASE("an unindexed file first keeps the build's rules") {
        constexpr std::uint64_t INTERVAL_US = 250'000;
        constexpr std::uint64_t FILE_GAP = 1'000'000;
        TestEnvironment env{202};
        REQUIRE(env.is_valid());
        dstd::Vector<dstd::String> gz;
        for (int i = 0; i < 2; ++i) {
            const dstd::String dir = env.get_dir() + "/f" + dstd::to_string(i);
            REQUIRE(dstd::fs::create_directories(dir).has_value());
            const dstd::String plain = dir + "/t.pfw";
            {
                dftu_utils_test::FileOut o(plain);
                for (int k = 0; k < 4; ++k)
                    o << R"({"ph":"C","name":"ctr","cat":"sys","pid":7,)"
                      << R"("tid":1,"ts":)"
                      << (FILE_GAP * static_cast<std::uint64_t>(i + 1) +
                          static_cast<std::uint64_t>(k) * 100)
                      << R"(,"args":{"epoch":")" << (k % 2 == 0 ? "a" : "b")
                      << R"(","count":4,"dur_sum":40,"dur_min":5,)"
                      << R"("dur_max":20}})" << "\n";
            }
            const dstd::String out = plain + ".gz";
            REQUIRE(compress_file_to_gzip(plain, out));
            (void)dstd::fs::remove(plain);
            gz.push_back(out);
        }
        ix::IndexerOptions opts;
        opts.index_dir = env.get_dir() + "/idx";
        opts.aggregation.time_interval_us = INTERVAL_US;
        opts.aggregation.extra_group_keys = {"epoch"};
        ix::Indexer::open(dftu_utils_test::dstd_paths(gz), opts).build();
        const dstd::String index =
            dstd::String(determine_index_path(gz.front(), opts.index_dir));

        const dstd::Vector<ViewFile> mixed{
            {dstd::String(gz[0]), ""},
            {dstd::String(gz[1]), dstd::String(index)}};
        const dstd::Vector<ViewFile> indexed{
            {dstd::String(gz[0]), dstd::String(index)},
            {dstd::String(gz[1]), dstd::String(index)}};
        const dstd::Vector<GroupKey> keys{GroupKey::of_arg("epoch")};
        const dstd::Vector<dstd::String> columns{"epoch"};
        const AggSpec count{AggOp::Count, "", "n"};
        const AggSpec max_te{AggOp::Max, "te", "max_te"};

        const Cells counts = view_cells(mixed, keys, columns, count);
        const Cells ends = view_cells(mixed, keys, columns, max_te);
        REQUIRE(counts.size() == 2);
        CHECK(dftu_utils_test::to_number<double>(counts.at("a|")) ==
              doctest::Approx(16));
        CHECK(dftu_utils_test::to_number<double>(counts.at("b|")) ==
              doctest::Approx(16));
        CHECK(dftu_utils_test::to_number<double>(ends.at("a|")) ==
              doctest::Approx(2 * FILE_GAP + 200 + INTERVAL_US));
        CHECK(dftu_utils_test::to_number<double>(ends.at("b|")) ==
              doctest::Approx(2 * FILE_GAP + 300 + INTERVAL_US));
        for (const auto& g : gz) dstd::fs::remove(g).has_value();
        check_same_cells(view_cells(indexed, keys, columns, count), counts);
        check_same_cells(view_cells(indexed, keys, columns, max_te), ends);
    }

    // The first aggregate over a trace with no index builds the index as it
    // reads, and folds a pre-aggregated counter as the index will afterwards.
    TEST_CASE("the first aggregate of an unindexed trace equals the next") {
        TestEnvironment env{206};
        REQUIRE(env.is_valid());
        const dstd::String plain = env.get_dir() + "/t.pfw";
        {
            dftu_utils_test::FileOut o(plain);
            for (int k = 0; k < 2; ++k)
                o << R"({"ph":"C","name":"pwrite","cat":"STDIO","pid":7,)"
                  << R"("tid":2,"ts":)" << (1000 + k * 100)
                  << R"(,"args":{"count":4,"dur_sum":40,"dur_min":5,)"
                  << R"("dur_max":20}})" << "\n";
            for (int k = 0; k < 2; ++k)
                o << R"({"ph":"X","name":"read","cat":"POSIX","pid":7,)"
                  << R"("tid":1,"ts":)" << (1200 + k * 100) << R"(,"dur":)"
                  << (10 + k * 20) << R"(,"args":{}})" << "\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(compress_file_to_gzip(plain, gz));
        (void)dstd::fs::remove(plain);
        const dstd::String index_dir = env.get_dir() + "/idx";
        const dstd::String index =
            dstd::String(determine_index_path(gz, index_dir));
        REQUIRE_FALSE(dstd::fs::exists(index).value());

        const dstd::Vector<ViewFile> files{
            {dstd::String(gz), dstd::String(index)}};
        const dstd::Vector<GroupKey> keys{GroupKey::name()};
        const dstd::Vector<dstd::String> columns{"name"};
        const dstd::Vector<AggSpec> specs{{AggOp::Count, "", "n"},
                                          {AggOp::Sum, "dur", "sum_dur"},
                                          {AggOp::Min, "dur", "min_dur"},
                                          {AggOp::Max, "dur", "max_dur"}};
        const auto ask = [&] {
            return View::from_files(files)
                .group_by(keys)
                .agg(specs)
                .collect()
                .get();
        };
        const dstd::Map<dstd::String, dstd::Map<dstd::String, double>> want{
            {"n", {{"pwrite|", 8}, {"read|", 2}}},
            {"sum_dur", {{"pwrite|", 80}, {"read|", 40}}},
            {"min_dur", {{"pwrite|", 5}, {"read|", 10}}},
            {"max_dur", {{"pwrite|", 20}, {"read|", 30}}}};

        const df::DataFrame first = ask();
        CHECK(dstd::fs::exists(index).value());
        const df::DataFrame next = ask();
        for (const auto& [column, cells] : want) {
            INFO(column);
            const Cells got = cells_of(first, columns, column);
            REQUIRE(got.size() == cells.size());
            for (const auto& [key, value] : cells)
                CHECK(dftu_utils_test::to_number<double>(got.at(key)) ==
                      doctest::Approx(value));
            check_same_cells(cells_of(next, columns, column), got);
        }
    }

    // An index root that is missing or cannot be read leaves the whole file to
    // be read, and the answer is the index's.
    TEST_CASE("a missing or unreadable index root reads the trace") {
        TestEnvironment env{204};
        REQUIRE(env.is_valid());
        const dstd::String plain = env.get_dir() + "/t.pfw";
        {
            dftu_utils_test::FileOut o(plain);
            for (int i = 0; i < 12; ++i)
                o << R"({"ph":"X","name":")" << (i % 3 == 0 ? "read" : "write")
                  << R"(","cat":"POSIX",)"
                  << R"("pid":7,"tid":1,"ts":)" << (1000 + i * 10)
                  << R"(,"dur":)" << (10 + i) << R"(,"args":{"ret":)"
                  << (i * 512) << "}}\n";
        }
        const dstd::String junk = env.get_dir() + "/junk.idx";
        {
            dftu_utils_test::FileOut o(junk);
            o << "not a database\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            plain, gz, MEMBER_BYTES));
        (void)dstd::fs::remove(plain);
        ix::IndexerOptions opts;
        opts.index_dir = env.get_dir() + "/idx";
        opts.checkpoint_size = MEMBER_BYTES;
        ix::Indexer::open({dstd::String(dstd::StringView(gz))}, opts).build();
        const dstd::String index =
            dstd::String(determine_index_path(gz, opts.index_dir));

        const dstd::Vector<GroupKey> keys{GroupKey::cat(), GroupKey::name()};
        const dstd::Vector<dstd::String> columns{"cat", "name"};
        const AggSpec sum_dur{AggOp::Sum, "dur", "sum_dur"};

        const dstd::Vector<ViewFile> missing{
            {dstd::String(gz), dstd::String(env.get_dir() + "/no-such")}};
        const dstd::Vector<ViewFile> unreadable{
            {dstd::String(gz), dstd::String(junk)}};
        const Cells want = view_cells({{dstd::String(gz), dstd::String(index)}},
                                      keys, columns, sum_dur);
        const Cells from_missing = view_cells(missing, keys, columns, sum_dur);
        const Cells from_unreadable =
            view_cells(unreadable, keys, columns, sum_dur);
        CHECK(from_missing.size() > 1);
        check_same_cells(want, from_missing);
        check_same_cells(want, from_unreadable);
    }

    // A fused scan answers each aggregation branch the way the branch answers
    // alone, so a pre-aggregated counter stands for its events in both.
    TEST_CASE("a fused scan folds a branch like the branch alone") {
        TestEnvironment env{205};
        REQUIRE(env.is_valid());
        dstd::Vector<dstd::String> gz;
        for (int i = 0; i < 2; ++i) {
            const dstd::String dir = env.get_dir() + "/f" + dstd::to_string(i);
            REQUIRE(dstd::fs::create_directories(dir).has_value());
            const dstd::String plain = dir + "/t.pfw";
            {
                dftu_utils_test::FileOut o(plain);
                write_trace(o, i);
            }
            const dstd::String out = plain + ".gz";
            REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
                plain, out, MEMBER_BYTES));
            (void)dstd::fs::remove(plain);
            gz.push_back(out);
        }
        ix::IndexerOptions opts;
        opts.index_dir = env.get_dir() + "/idx";
        opts.checkpoint_size = MEMBER_BYTES;
        ix::Indexer::open(dftu_utils_test::dstd_paths(gz), opts).build();
        const dstd::Vector<ViewFile> files{
            {dstd::String(gz[0]),
             dstd::String(
                 dstd::String(determine_index_path(gz[0], opts.index_dir)))},
            {dstd::String(gz[1]),
             dstd::String(
                 dstd::String(determine_index_path(gz[1], opts.index_dir)))}};
        const Query stdio = Query::from_string(R"(cat == "STDIO")").value();
        const Query posix = Query::from_string(R"(cat == "POSIX")").value();
        const dstd::Vector<GroupKey> by_name{GroupKey::name()};
        const dstd::Vector<dstd::String> columns{"name"};
        const auto aggs = [] {
            return dstd::Vector<AggSpec>{{AggOp::Count, "", "n"},
                                         {AggOp::Sum, "dur", "sum_dur"}};
        };

        static Runtime rt;
        ExportStats stats;
        auto alone = [&](const Query& q) {
            df::DataFrame out;
            rt.block_on(coro::named(
                "alone",
                coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                    df::LazyFrame plan =
                        View::from_files(files).branch([&](ViewSession& s) {
                            auto c = s.collect(q, by_name, aggs());
                            return dstd::Function<void(const ExportStats&)>(
                                [&, c](const ExportStats&) {
                                    out = std::move(c.get());
                                });
                        });
                    co_await plan.collect();
                })));
            return out;
        };

        df::DataFrame fused_stdio;
        df::DataFrame fused_posix;
        rt.block_on(coro::named(
            "fused",
            coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                df::LazyFrame plan =
                    View::from_files(files).branch([&](ViewSession& s) {
                        auto a = s.collect(stdio, by_name, aggs());
                        auto b = s.collect(posix, by_name, aggs());
                        return dstd::Function<void(const ExportStats&)>(
                            [&, a, b](const ExportStats& st) {
                                stats = st;
                                fused_stdio = std::move(a.get());
                                fused_posix = std::move(b.get());
                            });
                    });
                co_await plan.collect();
            })));

        // Both branches scanned the traces: no rollup or tier answered them.
        CHECK(stats.events_scanned > 0);
        for (const char* column : {"n", "sum_dur"}) {
            check_same_cells(cells_of(alone(stdio), columns, column),
                             cells_of(fused_stdio, columns, column));
            check_same_cells(cells_of(alone(posix), columns, column),
                             cells_of(fused_posix, columns, column));
        }
    }
}
