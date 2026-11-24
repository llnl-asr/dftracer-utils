// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/reader/trace_reader.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/writer/trace_writer.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::writer;
using namespace dftracer::utils::trace::reader;
using namespace dftu_utils_test;

namespace {

coro::CoroTask<dstd::Vector<dstd::String>> collect_lines(
    coro::AsyncGenerator<fileio::lines::Line> gen) {
    dstd::Vector<dstd::String> lines;
    while (true) {
        auto line = co_await gen.next();
        if (!line) break;
        lines.emplace_back(line->content);
    }
    co_return lines;
}

dstd::Vector<dstd::String> read_gzip_lines(dstd::StringView path) {
    TraceReader reader({.file_path = dstd::String(path)});
    return dftu_utils_test::ok(
        collect_lines(dftu_utils_test::ok_items(reader.read_lines())).get());
}

dstd::Vector<dstd::String> read_plain_lines(dstd::StringView path) {
    const dstd::String text = dftu_utils_test::ok(dstd::fs::read_file(path));
    dstd::Vector<dstd::String> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == dstd::String::npos) end = text.size();
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

TraceWriter open_writer(dstd::StringView path, bool compress = true) {
    TraceWriterOptions opts;
    opts.compress = compress;
    return dftu_utils_test::ok(TraceWriter::open(path, opts));
}

void write_one_of_each(TraceWriter& w) {
    const Arg ev_args[] = {{"level", 2}, {"fhash", "ab"}};
    REQUIRE(w.write_event("read", "POSIX", 100, 7, 11, 12, ev_args));
    const Arg ctr_args[] = {{"cpu", 12.5}};
    REQUIRE(w.write_counter("cpu", "sys", 200, 11, 12, ctr_args));
    const Arg agg_args[] = {{"count", std::uint64_t{3}}};
    REQUIRE(w.write_aggregated("read", "POSIX", 300, 11, 12, agg_args));
    const Arg meta_args[] = {{"name", "host"}, {"value", "n0"}};
    REQUIRE(w.write_metadata("HH", 11, 12, meta_args));
}

std::int64_t int_field(json::Value& v, dstd::StringView key) {
    auto f = v.find(key);
    REQUIRE(f.has_value());
    auto n = f->as_int();
    REQUIRE(n.has_value());
    return *n;
}

dstd::String str_field(json::Value& v, dstd::StringView key) {
    auto f = v.find(key);
    REQUIRE(f.has_value());
    auto s = f->as_string();
    REQUIRE(s.has_value());
    return dstd::String(*s);
}

}  // namespace

TEST_SUITE("trace::writer::TraceWriter") {
    TEST_CASE("each record kind reads back with integer ph") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/kinds.pfw";
        {
            auto w = open_writer(path, false);
            write_one_of_each(w);
            REQUIRE(w.close());
        }
        auto lines = read_plain_lines(path);
        REQUIRE(lines.size() == 4);

        json::Document doc;
        const int phases[] = {1, 2, 3, 4};
        const char* names[] = {"read", "cpu", "read", "HH"};
        for (std::size_t i = 0; i < 4; ++i) {
            auto root = doc.parse(lines[i]);
            REQUIRE(root.has_value());
            CHECK(int_field(*root, "ph") == phases[i]);
            CHECK(str_field(*root, "name") == names[i]);
            CHECK(int_field(*root, "pid") == 11);
            CHECK(int_field(*root, "tid") == 12);
        }
        {
            auto root = doc.parse(lines[0]);
            CHECK(int_field(*root, "ts") == 100);
            CHECK(int_field(*root, "dur") == 7);
            CHECK(str_field(*root, "cat") == "POSIX");
            auto args = root->find("args");
            REQUIRE(args.has_value());
            CHECK(int_field(*args, "level") == 2);
            CHECK(str_field(*args, "fhash") == "ab");
        }
        {
            auto root = doc.parse(lines[3]);
            CHECK(int_field(*root, "ts") == 0);
            CHECK_FALSE(root->find("dur").has_value());
        }
    }

    TEST_CASE("a gzip file holds the same lines as a plain one") {
        TestEnvironment env(0);
        const dstd::String plain = env.dir() + "/same.pfw";
        const dstd::String gz = env.dir() + "/same.pfw.gz";
        for (bool compress : {false, true}) {
            auto w = open_writer(compress ? gz : plain, compress);
            write_one_of_each(w);
            REQUIRE(w.close());
        }
        CHECK(read_gzip_lines(gz) == read_plain_lines(plain));
    }

    TEST_CASE("id and type go out only when given") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/extras.pfw";
        {
            auto w = open_writer(path, false);
            RecordExtras extras;
            extras.has_id = true;
            extras.id = 9;
            extras.type = trace::EventType::LibcIo;
            REQUIRE(w.write_event("read", "POSIX", 1, 2, 3, 4, {}, extras));
            REQUIRE(w.write_event("read", "POSIX", 1, 2, 3, 4));
            REQUIRE(w.close());
        }
        auto lines = read_plain_lines(path);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] ==
              R"({"id":9,"name":"read","cat":"POSIX","type":3,"pid":3,)"
              R"("tid":4,"ts":1,"dur":2,"ph":1})");
        CHECK(lines[1] ==
              R"({"name":"read","cat":"POSIX","pid":3,"tid":4,"ts":1,)"
              R"("dur":2,"ph":1})");
    }

    TEST_CASE("NaN and infinities are strings, nested raw args pass through") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/special.pfw";
        {
            auto w = open_writer(path, false);
            const double nan = std::numeric_limits<double>::quiet_NaN();
            const double inf = std::numeric_limits<double>::infinity();
            const Arg args[] = {
                {"nan", nan},
                {"inf", inf},
                {"ninf", -inf},
                {"none", ArgValue()},
                {"flag", true},
                {"neg", std::int64_t{-5}},
                {"big", std::uint64_t{18446744073709551615ULL}},
                {"nested", ArgValue::raw(R"({"a":[1,2.5,"x"],"b":null})")},
                {"text", "a\"b\nc"},
            };
            REQUIRE(w.write_counter("c", "sys", 1, 2, 3, args));
            REQUIRE(w.close());
        }
        auto lines = read_plain_lines(path);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find(R"("nan":"NaN","inf":"Infinity",)"
                            R"("ninf":"-Infinity","none":null,"flag":true,)"
                            R"("neg":-5,"big":18446744073709551615,)"
                            R"("nested":{"a":[1,2.5,"x"],"b":null},)"
                            R"("text":"a\"b\nc")") != dstd::String::npos);
        json::Document doc;
        REQUIRE(doc.parse(lines[0]).has_value());
    }

    TEST_CASE("finite doubles round-trip bit for bit") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/doubles.pfw";
        std::mt19937_64 rng(42);
        dstd::Vector<double> sent;
        while (sent.size() < 2000) {
            const std::uint64_t bits = rng();
            double d;
            std::memcpy(&d, &bits, sizeof d);
            if (std::isfinite(d)) sent.push_back(d);
        }
        sent.push_back(0.0);
        sent.push_back(-0.0);
        sent.push_back(1.0);
        sent.push_back(std::numeric_limits<double>::denorm_min());
        sent.push_back(std::numeric_limits<double>::max());
        {
            auto w = open_writer(path, false);
            for (double d : sent) {
                const Arg args[] = {{"v", d}};
                REQUIRE(w.write_counter("c", "sys", 1, 2, 3, args));
            }
            REQUIRE(w.close());
        }
        auto lines = read_plain_lines(path);
        REQUIRE(lines.size() == sent.size());
        json::Document doc;
        for (std::size_t i = 0; i < sent.size(); ++i) {
            auto root = doc.parse(lines[i]);
            REQUIRE(root.has_value());
            auto args = root->find("args");
            auto v = args->find("v");
            auto d = v->as_double();
            REQUIRE(d.has_value());
            std::uint64_t a, b;
            std::memcpy(&a, &*d, sizeof a);
            std::memcpy(&b, &sent[i], sizeof b);
            CHECK(a == b);
        }
    }

    TEST_CASE("write_raw appends whole lines and rejects a partial one") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/raw.pfw";
        {
            auto w = open_writer(path, false);
            REQUIRE(w.write_raw("[\n"));
            auto bad = w.write_raw("{\"x\":1}");
            REQUIRE_FALSE(bad.has_value());
            CHECK(common::errorcode_of(bad.error()) ==
                  common::ErrorCode::InvalidArgument);
            REQUIRE(w.write_raw("{\"x\":1}\n{\"x\":2}\n"));
            REQUIRE(w.write_raw(""));
            REQUIRE(w.close());
        }
        auto lines = read_plain_lines(path);
        REQUIRE(lines.size() == 3);
        CHECK(lines[0] == "[");
        CHECK(lines[2] == R"({"x":2})");
    }

    TEST_CASE("a large raw write goes through the file") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/big.pfw.gz";
        TraceWriterOptions opts;
        opts.member_size = 4096;
        auto w = dftu_utils_test::ok(TraceWriter::open(path, opts));
        dstd::String big;
        for (int i = 0; i < 1000; ++i) {
            big += "{\"i\":";
            big += dstd::to_string(i);
            big += "}\n";
        }
        REQUIRE(w.write_event("a", "b", 1, 2, 3, 4));
        REQUIRE(w.write_raw(big));
        REQUIRE(w.write_event("c", "d", 1, 2, 3, 4));
        REQUIRE(w.close());
        CHECK(read_gzip_lines(path).size() == 1002);
    }

    TEST_CASE("an unwritable path fails to open") {
        TestEnvironment env(0);
        auto opened = TraceWriter::open(env.dir() + "/missing_dir/x.pfw.gz");
        CHECK_FALSE(opened.has_value());
    }

    TEST_CASE("a closed writer refuses records, and close is repeatable") {
        TestEnvironment env(0);
        auto w = open_writer(env.dir() + "/closed.pfw", false);
        REQUIRE(w.close());
        REQUIRE(w.close());
        auto late = w.write_event("a", "b", 1, 2, 3, 4);
        REQUIRE_FALSE(late.has_value());
        CHECK(common::errorcode_of(late.error()) ==
              common::ErrorCode::InvalidArgument);
        CHECK_FALSE(w.write_raw("x\n").has_value());
    }

    TEST_CASE("a writer destroyed without close leaves no file") {
        TestEnvironment env(0);
        const dstd::String path = env.dir() + "/discard.pfw.gz";
        {
            auto w = open_writer(path);
            REQUIRE(w.write_event("a", "b", 1, 2, 3, 4));
        }
        CHECK_FALSE(dstd::fs::exists(path).value());
    }

    TEST_CASE("the view reads back the phase counts") {
        TestEnvironment env(0);
        const dstd::String gz = env.dir() + "/view.pfw.gz";
        {
            auto w = open_writer(gz);
            REQUIRE(w.write_raw("[\n"));
            for (int i = 0; i < 5; ++i)
                REQUIRE(w.write_event("read", "POSIX", 1000 + i * 10, 5, 1, 2));
            for (int i = 0; i < 3; ++i) {
                const Arg args[] = {{"cpu", 1.5}};
                REQUIRE(w.write_counter("cpu", "sys", 2000 + i, 1, 2, args));
            }
            for (int i = 0; i < 2; ++i) {
                const Arg args[] = {{"count", 4}};
                REQUIRE(
                    w.write_aggregated("agg", "POSIX", 3000 + i, 1, 2, args));
            }
            REQUIRE(w.close());
        }
        using namespace trace::views;
        const dstd::String idx =
            dstd::String(trace::internal::determine_index_path(gz, ""));
        auto count = [&](Phase p) {
            test_view_common::StringSink sink;
            dftu_utils_test::ok(
                View::from_file(dstd::String(gz), dstd::String(idx))
                    .phase(p)
                    .sink_json(sink)
                    .get());
            return sink.lines().size();
        };
        CHECK(count(Phase::Events) == 5);
        CHECK(count(Phase::Counters) == 3);
        CHECK(count(Phase::Aggregated) == 2);
    }
}
