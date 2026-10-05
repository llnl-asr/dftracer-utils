#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/schemas/dft/agg/aggregation_runner.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;
namespace agg = dftracer::utils::index::schemas::dft::agg;

namespace {

void write_trace(const std::string& path) {
    GzipWriterOptions o;
    o.member_size = 64 * 1024;
    o.part_header = "[\n";
    auto w = GzipLineWriterBlocking::open(path, o);
    REQUIRE(w.has_value());
    const char* names[] = {"read", "write", "open", "close", "fsync"};
    std::string batch;
    for (int i = 0; i < 20000; ++i) {
        batch += "{\"ph\":\"X\",\"name\":\"" + std::string(names[i % 5]) +
                 "\",\"cat\":\"POSIX\",\"pid\":" + std::to_string(i % 7) +
                 ",\"tid\":" + std::to_string(i % 3) +
                 ",\"ts\":" + std::to_string(1000 + i * 10) +
                 ",\"dur\":" + std::to_string(5 + i % 40) +
                 ",\"args\":{\"fname\":\"/data/f" + std::to_string(i % 13) +
                 "\",\"size\":" + std::to_string(i % 4096) + "}}\n";
    }
    REQUIRE(w->append(batch));
    REQUIRE(w->close());
}

// Decodes every gzip member of `path` and checks that each ends in '\n'.
std::string decode_members(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string in((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    z_stream z{};
    REQUIRE(inflateInit2(&z, 31) == Z_OK);
    z.next_in = reinterpret_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    std::string out;
    std::string member;
    char buf[65536];
    int rc = Z_OK;
    while (z.avail_in > 0 || rc == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof(buf);
        rc = inflate(&z, Z_NO_FLUSH);
        REQUIRE((rc == Z_OK || rc == Z_STREAM_END));
        member.append(buf, sizeof(buf) - z.avail_out);
        if (rc == Z_STREAM_END) {
            REQUIRE(!member.empty());
            CHECK(member.back() == '\n');
            out += member;
            member.clear();
            if (z.avail_in == 0) break;
            REQUIRE(inflateReset(&z) == Z_OK);
            rc = Z_OK;
        }
    }
    inflateEnd(&z);
    return out;
}

struct Run {
    std::string text;
    std::size_t keys = 0;
};

Run aggregate(const std::string& dir, const std::string& tag,
              std::size_t threads) {
    agg::AggregationRunInput in;
    in.log_dir = dir + "/in";
    in.index_dir = dir + "/idx_" + tag;
    in.output_file = dir + "/out_" + tag + ".pfw.gz";
    in.compress_output = true;
    in.verbose = false;
    in.checkpoint_size = 64 * 1024;
    in.agg_config.time_interval_us = 100;
    in.pipeline_config.executor_threads = threads;
    auto res = agg::run_aggregation(std::move(in)).get();
    REQUIRE(res.has_value());
    return {decode_members(dir + "/out_" + tag + ".pfw.gz"), res->total_keys};
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> v;
    std::size_t p = 0;
    while (p < text.size()) {
        const std::size_t e = text.find('\n', p);
        v.push_back(text.substr(p, e - p));
        p = e + 1;
    }
    return v;
}

}  // namespace

TEST_CASE("aggregate trace writer output is a complete JSON array") {
    dftu_utils_test::ScopedTestDir dir("dft_trace_writer");
    dir.create_dir("in");
    write_trace(dir.file("in/t.pfw.gz"));

    Run many = aggregate(dir.str(), "many", 4);
    Run one = aggregate(dir.str(), "one", 1);

    REQUIRE(many.keys > 0);
    CHECK(many.text.rfind("[\n", 0) == 0);
    REQUIRE(many.text.size() > 2);
    CHECK(many.text.substr(many.text.size() - 2) == "]\n");

    auto ls = lines_of(many.text);
    const std::size_t events = static_cast<std::size_t>(
        std::count_if(ls.begin(), ls.end(), [](const std::string& l) {
            return l.find("\"ph\":3") != std::string::npos;
        }));
    CHECK(events == many.keys);

    auto a = ls;
    auto b = lines_of(one.text);
    CHECK(one.keys == many.keys);
    std::sort(a.begin() + 1, a.end() - 1);
    std::sort(b.begin() + 1, b.end() - 1);
    CHECK(a == b);
}
