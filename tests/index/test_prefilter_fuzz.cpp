#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/json/json.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace views = dftracer::utils::trace::views;
namespace reader = dftracer::utils::utilities::reader;
using dftracer::utils::query::Query;

namespace {

const std::vector<std::string> NAMES = {
    "read", "write", "open64", "MPI_Allreduce", "a-long-operation-name-xyz"};
const std::vector<std::string> CATS = {"POSIX", "STDIO", "MPI"};
const std::vector<std::int64_t> PIDS = {100, 1234, 98765};
const std::vector<std::string> OPS = {"rd", "wr", "seek"};

// Data events and metadata records, half of them in spaced JSON, with file
// names holding '/' (never a needle).
std::vector<std::string> make_lines(std::mt19937& rng, int n) {
    auto pick = [&](const auto& v) { return v[rng() % v.size()]; };
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        const bool spaced = rng() % 2 == 0;
        const char* c = spaced ? ": " : ":";
        const char* s = spaced ? ", " : ",";
        std::string l;
        if (i % 50 == 0) {
            l = std::string("{\"name\"") + c + "\"thread_name\"" + s +
                "\"ph\"" + c + "\"M\"" + s + "\"pid\"" + c +
                std::to_string(pick(PIDS)) + s + "\"args\"" + c + "{\"name\"" +
                c + "\"" + pick(NAMES) + "\"}}";
        } else {
            l = std::string("{\"id\"") + c + std::to_string(i) + s +
                "\"name\"" + c + "\"" + pick(NAMES) + "\"" + s + "\"cat\"" + c +
                "\"" + pick(CATS) + "\"" + s + "\"pid\"" + c +
                std::to_string(pick(PIDS)) + s + "\"tid\"" + c + "1" + s +
                "\"ts\"" + c + std::to_string(1000 + i) + s + "\"dur\"" + c +
                std::to_string(rng() % 100) + s + "\"ph\"" + c + "\"X\"" + s +
                "\"args\"" + c + "{\"size\"" + c +
                std::to_string(rng() % 5000) + s + "\"fname\"" + c +
                "\"/data/file_" + std::to_string(rng() % 4) + "\"" + s +
                "\"op\"" + c + "\"" + pick(OPS) + "\"}}";
        }
        out.push_back(std::move(l));
    }
    return out;
}

std::string random_leaf(std::mt19937& rng) {
    auto pick = [&](const auto& v) { return v[rng() % v.size()]; };
    switch (rng() % 13) {
        case 0:
            return "name == \"" + pick(NAMES) + "\"";
        case 1:
            return "cat == \"" + pick(CATS) + "\"";
        case 2:
            return "pid == " + std::to_string(pick(PIDS));
        case 3:
            return "args.size == " + std::to_string(rng() % 5000);
        case 4:
            return "args.size > " + std::to_string(rng() % 5000);
        case 5:
            return "name != \"" + pick(NAMES) + "\"";
        case 6:
            return "cat in [\"" + pick(CATS) + "\", \"" + pick(CATS) + "\"]";
        case 7:
            return "pid in [" + std::to_string(pick(PIDS)) + ", " +
                   std::to_string(pick(PIDS)) + "]";
        case 8:
            return "args.fname == \"/data/file_" + std::to_string(rng() % 4) +
                   "\"";
        case 9:
            return "name like \"%rea%\"";
        case 11:
            return "ts >= " + std::to_string(1000 + rng() % 3000);
        case 12:
            return "dur < " + std::to_string(rng() % 100);
        default:
            return "args.op == \"" + pick(OPS) + "\"";
    }
}

std::string random_query(std::mt19937& rng, int depth) {
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

std::uint64_t view_count(const std::string& gz, const Query& q,
                         bool prefilter) {
    views::detail::ViewPlan vp;
    vp.files.push_back(
        {gz, dftracer::utils::trace::internal::determine_index_path(gz, "")});
    vp.query = q;
    vp.include_metadata = false;
    auto vdef = views::detail::make_vdef(vp, false);
    vdef.prefilter = prefilter;
    return views::detail::for_each_scanned_batch(
               vp, vdef, 1, 0,
               [](std::size_t, const std::vector<std::string_view>&) {})
        .get()
        .events_matched;
}

std::size_t reader_count(const std::string& gz, const std::string& text) {
    auto run = [&]() -> dftracer::utils::coro::CoroTask<std::size_t> {
        reader::TraceReader r({.file_path = gz});
        reader::ReadConfig cfg;
        cfg.query = text;
        auto gen = r.read_json(cfg);
        std::size_t n = 0;
        while (auto line = co_await gen.next()) ++n;
        co_return n;
    };
    return run().get();
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
std::int64_t arrow_count(const std::string& gz, const std::string& text) {
    auto run = [&]() -> dftracer::utils::coro::CoroTask<std::int64_t> {
        reader::TraceReader r({.file_path = gz});
        reader::ReadConfig cfg;
        cfg.query = text;
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
    const auto lines = make_lines(rng, 3000);
    const std::string plain = env.get_dir() + "/fuzz.pfw";
    {
        std::ofstream out(plain);
        for (const auto& l : lines) out << l << "\n";
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    REQUIRE(dftu_utils_test::build_index(gz));

    simdjson::dom::parser parser;
    std::vector<simdjson::padded_string> padded;
    for (const auto& l : lines) padded.emplace_back(l);

    for (int i = 0; i < 300; ++i) {
        const std::string text = random_query(rng, 3);
        CAPTURE(text);
        const Query q = dftracer::utils::query::parse_or_throw(text);
        std::uint64_t data = 0, all = 0;
        for (std::size_t k = 0; k < lines.size(); ++k) {
            auto root = parser.parse(padded[k]).value();
            if (!q.evaluate(dftracer::utils::json::JsonValue(root))) continue;
            ++all;
            if (lines[k].find("\"M\"") == std::string::npos) ++data;
        }
        CHECK(view_count(gz, q, true) == data);
        CHECK(view_count(gz, q, false) == data);
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
    std::vector<std::string> lines;
    for (int i = 0; i < 3000; ++i) {
        const char* c = rng() % 2 ? ": " : ":";
        const char* sep = rng() % 2 ? ", " : ",";
        lines.push_back(std::string("{\"op\"") + c + "\"" + pick(NAMES) +
                        "\",\"lat\"" + c + std::to_string(rng() % 50) +
                        ",\"io\"" + c + "{\"off\"" + c +
                        std::to_string(pick(PIDS)) + "},\"tags\"" + c + "[\"" +
                        pick(NAMES) + "\"" + sep + "\"" + pick(NAMES) +
                        "\"],\"sizes\"" + c + "[" + std::to_string(rng() % 50) +
                        sep + std::to_string(pick(PIDS)) + "]}");
    }
    const std::string plain = env.get_dir() + "/fuzz.ndjson";
    {
        std::ofstream out(plain);
        for (const auto& l : lines) out << l << "\n";
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    REQUIRE(dftu_utils_test::build_index(gz));

    auto leaf = [&]() -> std::string {
        switch (rng() % 9) {
            case 6:
                return "any(tags) == \"" + pick(NAMES) + "\"";
            case 7:
                return "any(sizes) == " + std::to_string(pick(PIDS));
            case 8:
                return "any(sizes) > " + std::to_string(rng() % 50);
            case 4:
                return "lat >= " + std::to_string(rng() % 50);
            case 5:
                return "io.off < " + std::to_string(pick(PIDS));
            case 0:
                return "op == \"" + pick(NAMES) + "\"";
            case 1:
                return "io.off == " + std::to_string(pick(PIDS));
            case 2:
                return "lat == " + std::to_string(rng() % 50);
            default:
                return "op in [\"" + pick(NAMES) + "\", \"" + pick(NAMES) +
                       "\"]";
        }
    };
    simdjson::dom::parser parser;
    std::vector<simdjson::padded_string> padded;
    for (const auto& l : lines) padded.emplace_back(l);
    for (int i = 0; i < 100; ++i) {
        const std::string text =
            rng() % 2 ? leaf() : "(" + leaf() + ") and (" + leaf() + ")";
        CAPTURE(text);
        const Query q = dftracer::utils::query::parse_or_throw(text);
        std::uint64_t all = 0;
        for (auto& p : padded)
            all += q.evaluate(dftracer::utils::json::JsonValue(
                       parser.parse(p).value()))
                       ? 1
                       : 0;
        CHECK(view_count(gz, q, true) == all);
        CHECK(view_count(gz, q, false) == all);
        CHECK(reader_count(gz, text) == all);
#ifdef DFTRACER_UTILS_ENABLE_ARROW
        CHECK(arrow_count(gz, text) == static_cast<std::int64_t>(all));
#endif
    }
}
