// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <duql/eval_json.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cstdint>
#include <random>

using namespace dftracer::utils;

namespace {

const dstd::Vector<dstd::String> NAMES = {
    "read", "write", "open64", "MPI_Allreduce", "a-long-operation-name-xyz"};
const dstd::Vector<dstd::String> CATS = {"POSIX", "STDIO", "MPI"};
const dstd::Vector<std::int64_t> PIDS = {100, 1234, 98765};
const dstd::Vector<dstd::String> OPS = {"rd", "wr", "seek"};

// Optional args that are null, a bool, an empty or one-element array, or
// absent.
dstd::String extra_args(std::mt19937& rng, const char* c, const char* s) {
    dstd::String out;
    switch (rng() % 4) {
        case 0:
            out += dstd::String(s) + "\"retry\"" + c + "null";
            break;
        case 1:
            out +=
                dstd::String(s) + "\"retry\"" + c + dstd::to_string(rng() % 3);
            break;
        default:
            break;
    }
    switch (rng() % 4) {
        case 0:
            out += dstd::String(s) + "\"tags\"" + c + "[]";
            break;
        case 1:
            out += dstd::String(s) + "\"tags\"" + c + "[\"x\"]";
            break;
        case 2:
            out += dstd::String(s) + "\"tags\"" + c + "null";
            break;
        default:
            break;
    }
    if (rng() % 2)
        out += dstd::String(s) + "\"ok\"" + c + (rng() % 2 ? "true" : "false");
    return out;
}

// Data events and metadata records, half of them in spaced JSON, with file
// names holding '/' (never a needle).
dstd::Vector<dstd::String> make_lines(std::mt19937& rng, int n) {
    auto pick = [&](const auto& v) { return v[rng() % v.size()]; };
    dstd::Vector<dstd::String> out;
    for (int i = 0; i < n; ++i) {
        const bool spaced = rng() % 2 == 0;
        const char* c = spaced ? ": " : ":";
        const char* s = spaced ? ", " : ",";
        dstd::String l;
        if (i % 50 == 0) {
            l = dstd::String("{\"name\"") + c + "\"thread_name\"" + s +
                "\"ph\"" + c + "\"M\"" + s + "\"pid\"" + c +
                dstd::to_string(pick(PIDS)) + s + "\"args\"" + c + "{\"name\"" +
                c + "\"" + pick(NAMES) + "\"}}";
        } else {
            l = dstd::String("{\"id\"") + c + dstd::to_string(i) + s +
                "\"name\"" + c + "\"" + pick(NAMES) + "\"" + s + "\"cat\"" + c +
                "\"" + pick(CATS) + "\"" + s + "\"pid\"" + c +
                dstd::to_string(pick(PIDS)) + s + "\"tid\"" + c + "1" + s +
                "\"ts\"" + c + dstd::to_string(1000 + i) + s + "\"dur\"" + c +
                dstd::to_string(rng() % 100) + s + "\"ph\"" + c + "\"X\"" + s +
                "\"args\"" + c + "{\"size\"" + c +
                dstd::to_string(rng() % 5000) + s + "\"fname\"" + c +
                "\"/data/file_" + dstd::to_string(rng() % 4) + "\"" + s +
                "\"op\"" + c + "\"" + pick(OPS) + "\"" + extra_args(rng, c, s) +
                "}}";
        }
        out.push_back(std::move(l));
    }
    return out;
}

dstd::String random_leaf(std::mt19937& rng) {
    auto pick = [&](const auto& v) { return v[rng() % v.size()]; };
    switch (rng() % 27) {
        case 22:
            return "name ~ \"MPI_.*reduce\"";
        case 23:
            return "name ~ \"^(read|write)$\"";
        case 24:
            return "args.fname like \"%file_" + dstd::to_string(rng() % 4) +
                   "\"";
        case 25:
            return "name ilike \"%READ%\"";
        case 26:
            return "name !~ \"open\"";
        case 13:
            return "args.size * 2 > " + dstd::to_string(rng() % 10000);
        case 14:
            return "exists(args.retry)";
        case 15:
            return "args.retry is null";
        case 16:
            return "len(args.tags) == 0";
        case 17:
            return "exists(args.tags)";
        case 18:
            return "args.ok == true";
        case 19:
            return "(args.retry ?? 0) >= 1";
        case 20:
            return "exists(args.nothing)";
        case 21:
            return "dur between " + dstd::to_string(rng() % 50) + " and " +
                   dstd::to_string(50 + rng() % 50);
        case 0:
            return "name == \"" + pick(NAMES) + "\"";
        case 1:
            return "cat == \"" + pick(CATS) + "\"";
        case 2:
            return "pid == " + dstd::to_string(pick(PIDS));
        case 3:
            return "args.size == " + dstd::to_string(rng() % 5000);
        case 4:
            return "args.size > " + dstd::to_string(rng() % 5000);
        case 5:
            return "name != \"" + pick(NAMES) + "\"";
        case 6:
            return "cat in [\"" + pick(CATS) + "\", \"" + pick(CATS) + "\"]";
        case 7:
            return "pid in [" + dstd::to_string(pick(PIDS)) + ", " +
                   dstd::to_string(pick(PIDS)) + "]";
        case 8:
            return "args.fname == \"/data/file_" + dstd::to_string(rng() % 4) +
                   "\"";
        case 9:
            return "name like \"%rea%\"";
        case 11:
            return "ts >= " + dstd::to_string(1000 + rng() % 3000);
        case 12:
            return "dur < " + dstd::to_string(rng() % 100);
        default:
            return "args.op == \"" + pick(OPS) + "\"";
    }
}

dstd::String random_query(std::mt19937& rng, int depth) {
    if (depth == 0 || rng() % 3 == 0) return random_leaf(rng);
    switch (rng() % 3) {
        case 0:
            return "(" + random_query(rng, depth - 1) + ") and (" +
                   random_query(rng, depth - 1) + ")";
        case 1:
            return "(" + random_query(rng, depth - 1) + ") or (" +
                   random_query(rng, depth - 1) + ")";
        default:
            return "not (" + random_query(rng, depth - 1) + ")";
    }
}

// Each member is a scan unit with a fixed stream setup per scan, so the line
// counts stop a little past four members.
constexpr std::size_t MEMBER_BYTES = 16 * 1024;
constexpr int EVENT_LINES = 300;
constexpr int GENERIC_LINES = 450;

// The 4 MB default buffer would zero-fill per scan for no gain.
constexpr std::size_t READ_BUFFER_BYTES = MEMBER_BYTES;

struct ViewCounts {
    std::uint64_t prefiltered = 0;
    std::uint64_t plain = 0;
};

ViewCounts view_counts(const dstd::String& gz, const duql::Query& q) {
    trace::views::detail::ViewPlan vp;
    vp.files.push_back(
        {dstd::String(gz),
         dftracer::utils::trace::internal::determine_index_path(gz, "")});
    vp.query = q;
    auto vdef = trace::views::detail::make_vdef(vp, false);
    auto run = [&]() -> dftracer::utils::coro::CoroTask<ViewCounts> {
        std::uint64_t skipped = 0;
        auto units =
            co_await trace::views::detail::gather_units(vp, vdef, skipped);
        ViewCounts out;
        for (const bool prefilter : {true, false}) {
            vdef.prefilter = prefilter;
            std::uint64_t matched = 0;
            for (const auto& u : units) {
                auto sin = trace::views::detail::make_scanner_input(u, vdef,
                                                                    vdef.query);
                sin.with_batch_size(READ_BUFFER_BYTES);
                trace::views::ViewScannerUtility scanner;
                auto gen = scanner(sin);
                while (auto b = co_await gen.next())
                    matched += b->events_matched;
            }
            (prefilter ? out.prefiltered : out.plain) = matched;
        }
        co_return out;
    };
    return run().get();
}

std::size_t reader_count(dstd::StringView gz, const dstd::String& text) {
    auto run = [&]() -> dftracer::utils::coro::CoroTask<std::size_t> {
        utilities::reader::TraceReader r({.file_path = dstd::String(gz)});
        utilities::reader::ReadConfig cfg;
        cfg.query = text;
        cfg.buffer_size = READ_BUFFER_BYTES;
        auto gen = r.read_json(cfg);
        std::size_t n = 0;
        while (auto line = co_await gen.next()) ++n;
        co_return n;
    };
    return run().get();
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
std::int64_t arrow_count(dstd::StringView gz, const dstd::String& text) {
    auto run = [&]() -> dftracer::utils::coro::CoroTask<std::int64_t> {
        utilities::reader::TraceReader r({.file_path = dstd::String(gz)});
        utilities::reader::ReadConfig cfg;
        cfg.query = text;
        cfg.buffer_size = READ_BUFFER_BYTES;
        auto gen = r.read_arrow(cfg);
        std::int64_t n = 0;
        while (auto b = co_await gen.next()) n += b->num_rows();
        co_return n;
    };
    return run().get();
}
#endif

}  // namespace

TEST_CASE("the pre-filter never drops a match") {
    dftu_utils_test::TestEnvironment env(10);
    std::mt19937 rng(20260925);
    const auto lines = make_lines(rng, EVENT_LINES);
    const dstd::String plain = env.dir() + "/fuzz.pfw";
    {
        dstd::String out;
        for (const auto& l : lines) out += common::str_cat(l, "\n");
        (void)dstd::fs::write_file(dstd::PathView(plain), out);
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               MEMBER_BYTES));
    (void)dstd::fs::remove(plain);
    REQUIRE(dftu_utils_test::build_index(gz));

    simdjson::dom::parser parser;
    dstd::Vector<simdjson::padded_string> padded;
    for (const auto& l : lines) padded.emplace_back(l);

    for (int i = 0; i < 300; ++i) {
        const dstd::String text = random_query(rng, 3);
        CAPTURE(text);
        const duql::Query q = dftracer::utils::duql::parse_or_throw(text);
        std::uint64_t data = 0, all = 0;
        for (std::size_t k = 0; k < lines.size(); ++k) {
            auto root = parser.parse(padded[k]).value();
            if (!eval_json(q, dftracer::utils::json::JsonValue(root))) continue;
            ++all;
            if (lines[k].find("\"M\"") == dstd::String::npos) ++data;
        }
        const ViewCounts v = view_counts(gz, q);
        CHECK(v.prefiltered == data);
        CHECK(v.plain == data);
        CHECK(reader_count(gz, text) == all);
#ifdef DFTRACER_UTILS_ENABLE_ARROW
        CHECK(arrow_count(gz, text) == static_cast<std::int64_t>(all));
#endif
    }
}

TEST_CASE("the pre-filter never drops a match on generic records") {
    dftu_utils_test::TestEnvironment env(10);
    std::mt19937 rng(7);
    auto pick = [&](const auto& v) { return v[rng() % v.size()]; };
    dstd::Vector<dstd::String> lines;
    for (int i = 0; i < GENERIC_LINES; ++i) {
        const char* c = rng() % 2 ? ": " : ":";
        const char* sep = rng() % 2 ? ", " : ",";
        lines.push_back(
            dstd::String("{\"op\"") + c + "\"" + pick(NAMES) + "\",\"lat\"" +
            c + dstd::to_string(rng() % 50) + ",\"io\"" + c + "{\"off\"" + c +
            dstd::to_string(pick(PIDS)) + "},\"tags\"" + c + "[\"" +
            pick(NAMES) + "\"" + sep + "\"" + pick(NAMES) + "\"],\"sizes\"" +
            c + "[" + dstd::to_string(rng() % 50) + sep +
            dstd::to_string(pick(PIDS)) + "]}");
    }
    const dstd::String plain = env.dir() + "/fuzz.ndjson";
    {
        dstd::String out;
        for (const auto& l : lines) out += common::str_cat(l, "\n");
        (void)dstd::fs::write_file(dstd::PathView(plain), out);
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               MEMBER_BYTES));
    (void)dstd::fs::remove(plain);
    REQUIRE(dftu_utils_test::build_index(gz));

    auto leaf = [&]() -> dstd::String {
        switch (rng() % 9) {
            case 6:
                return "any(tags) == \"" + pick(NAMES) + "\"";
            case 7:
                return "any(sizes) == " + dstd::to_string(pick(PIDS));
            case 8:
                return "any(sizes) > " + dstd::to_string(rng() % 50);
            case 4:
                return "lat >= " + dstd::to_string(rng() % 50);
            case 5:
                return "io.off < " + dstd::to_string(pick(PIDS));
            case 0:
                return "op == \"" + pick(NAMES) + "\"";
            case 1:
                return "io.off == " + dstd::to_string(pick(PIDS));
            case 2:
                return "lat == " + dstd::to_string(rng() % 50);
            default:
                return "op in [\"" + pick(NAMES) + "\", \"" + pick(NAMES) +
                       "\"]";
        }
    };
    simdjson::dom::parser parser;
    dstd::Vector<simdjson::padded_string> padded;
    for (const auto& l : lines) padded.emplace_back(l);
    for (int i = 0; i < 100; ++i) {
        const dstd::String text =
            rng() % 2 ? leaf() : "(" + leaf() + ") and (" + leaf() + ")";
        CAPTURE(text);
        const duql::Query q = dftracer::utils::duql::parse_or_throw(text);
        std::uint64_t all = 0;
        for (auto& p : padded)
            all += eval_json(q, dftracer::utils::json::JsonValue(
                                    parser.parse(p).value()))
                       ? 1
                       : 0;
        const ViewCounts v = view_counts(gz, q);
        CHECK(v.prefiltered == all);
        CHECK(v.plain == all);
        CHECK(reader_count(gz, text) == all);
#ifdef DFTRACER_UTILS_ENABLE_ARROW
        CHECK(arrow_count(gz, text) == static_cast<std::int64_t>(all));
#endif
    }
}
