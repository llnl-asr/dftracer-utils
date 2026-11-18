// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/tables.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>

using namespace dftracer::utils;

namespace {

constexpr int EVENTS = 240;

// `size` rises with the event number and `m` repeats in runs of ten, so each
// gzip member holds its own range of `size` and its own few values of `m`.
dstd::String write_trace(dstd::CStringView dir) {
    dstd::String body = "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        dstd::fmt_append<
            "{\"id\":{},\"name\":\"{}\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":2,"
            "\"ph\":\"X\",\"ts\":{},\"dur\":5,\"args\":{\"size\":{},\"m\":\"m{}"
            "\"}}\n">(body, i, i % 3 ? "read" : "write", 1000 + i, i * 10,
                      i / 10);
    }
    body += "]\n";
    const auto plain = dstd::String(dir) + "/summaries.pfw";
    dstd::fs::write_file(dstd::PathView(plain), body).discard();
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

trace::index::IndexerOptions options(
    dstd::CStringView index_dir,
    dstd::Vector<trace::index::SummaryChoice> summaries = {}) {
    trace::index::IndexerOptions o;
    o.index_dir = index_dir;
    o.summaries = std::move(summaries);
    return o;
}

trace::index::Indexer open(dstd::CStringView trace,
                           trace::index::IndexerOptions o) {
    return dftu_utils_test::ok(
        trace::index::Indexer::open({dstd::String(trace)}, std::move(o)));
}

dstd::Vector<trace::index::SummaryRow> rows_of(dstd::CStringView trace,
                                               dstd::CStringView index_path) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    return dftu_utils_test::index_take(
        trace::index::summary_rows(snapshot, file));
}

dstd::Set<dstd::String> kinds_of(
    dstd::Span<const trace::index::SummaryRow> rows, dstd::StringView path) {
    dstd::Set<dstd::String> out;
    for (const auto& r : rows)
        if (r.path == path) out.insert(r.summary);
    return out;
}

std::uint64_t chunk_count(const trace::index::Indexer& ix) {
    return dftu_utils_test::ok(ix.explain(R"(name == "read")"))[0].chunks;
}

}  // namespace

TEST_SUITE("Summaries") {
    TEST_CASE("a bad summary choice is an error that names it") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        const auto message = [&](trace::index::IndexerOptions o) {
            auto opened = trace::index::Indexer::open({trace}, std::move(o));
            REQUIRE_FALSE(opened.has_value());
            return dstd::String(opened.error().message());
        };
        CHECK(message(options(idx, {{"size", {"sketchy"}}})).find("sketchy") !=
              dstd::String::npos);
        CHECK(
            message(options(idx, {{"size", {"postings"}}})).find("postings") !=
            dstd::String::npos);
        CHECK(message(options(idx, {{"", {"bloom"}}})).find("empty") !=
              dstd::String::npos);
        CHECK(message(options(idx, {{"size", {}}})).find("size") !=
              dstd::String::npos);
        CHECK(message(
                  options(idx, {{"size", {"bloom"}}, {"args.size", {"bloom"}}}))
                  .find("size") != dstd::String::npos);
        CHECK(message(options(idx, {{"size", {"bloom", "bloom"}}}))
                  .find("bloom") != dstd::String::npos);
        auto narrow = options(idx, {{"size", {"bloom"}}});
        narrow.extensions = {"zonemap"};
        CHECK(message(std::move(narrow)).find("bloom") != dstd::String::npos);
    }

    TEST_CASE("a path keeps only the summaries it is given") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        auto ix = open(trace, options(idx, {{"args.size", {"zonemap"}},
                                            {"m", {"bloom", "zonemap"}}}));
        const auto built = dftu_utils_test::ok(ix.build());
        const auto rows = rows_of(trace, built.index_path);

        CHECK(kinds_of(rows, "size") == dstd::Set<dstd::String>{"zonemap"});
        CHECK(kinds_of(rows, "m") ==
              dstd::Set<dstd::String>{"bloom", "zonemap"});
        // A path that is not listed keeps every summary.
        const auto name = kinds_of(rows, "name");
        CHECK(name.count("zonemap"));
        CHECK(name.count("bloom"));
        CHECK(name.count("counts"));
        CHECK(name.count("postings"));

        const auto chunks = chunk_count(ix);
        REQUIRE(chunks > 3);
        std::uint64_t size_rows = 0;
        for (const auto& r : rows) {
            CHECK(r.bytes > 0);
            if (r.path == "size") {
                ++size_rows;
                CHECK_FALSE(r.hash_version);
            }
            if (r.path == "m" && r.summary == "bloom")
                CHECK(r.hash_version == dstd::hash::VALUE_HASH_VERSION);
        }
        CHECK(size_rows == chunks);
    }

    TEST_CASE(
        "a changed summary choice rebuilds the index and the same one does "
        "not") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        const dstd::Vector<dstd::String> pending{trace};
        auto a = [&] { return options(idx, {{"size", {"zonemap"}}}); };
        auto b = [&] { return options(idx, {{"size", {"bloom"}}}); };

        auto first = open(trace, a());
        CHECK(dftu_utils_test::ok(first.build()).indexed == 1);
        CHECK(
            dftu_utils_test::ok(open(trace, a()).status()).needs_work.empty());
        CHECK(dftu_utils_test::ok(open(trace, a()).build()).indexed == 0);

        auto second = open(trace, b());
        CHECK(dftu_utils_test::ok(second.status()).needs_work == pending);
        const auto built = dftu_utils_test::ok(second.build());
        CHECK(built.indexed == 1);
        CHECK(dftu_utils_test::ok(second.status()).needs_work.empty());
        CHECK(kinds_of(rows_of(trace, built.index_path), "size") ==
              dstd::Set<dstd::String>{"bloom"});

        CHECK(dftu_utils_test::ok(open(trace, options(idx)).status())
                  .needs_work == pending);
        CHECK(
            dftu_utils_test::ok(open(trace, b()).status()).needs_work.empty());
    }

    TEST_CASE("explain counts the chunks each summary skips") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        auto ix = open(trace,
                       options(idx, {{"size", {"zonemap"}}, {"m", {"bloom"}}}));
        dftu_utils_test::ok(ix.build());
        const auto chunks = chunk_count(ix);
        REQUIRE(chunks > 3);

        const auto range = dftu_utils_test::ok(ix.explain("size >= 2000"))[0];
        REQUIRE(range.summaries.size() == 1);
        CHECK(range.summaries[0].path == "size");
        CHECK(range.summaries[0].summary == "zonemap");
        CHECK(range.summaries[0].chunks == chunks);
        CHECK(range.summaries[0].skipped > 0);
        CHECK(range.summaries[0].skipped == chunks - range.read.size());

        const auto equal = dftu_utils_test::ok(ix.explain(R"(m == "m3")"))[0];
        REQUIRE(equal.summaries.size() == 1);
        CHECK(equal.summaries[0].path == "m");
        CHECK(equal.summaries[0].summary == "bloom");
        CHECK(equal.summaries[0].skipped > 0);
        CHECK(equal.summaries[0].skipped == chunks - equal.read.size());

        const auto both = dftu_utils_test::ok(
            ix.explain(R"(size >= 2000 and m == "m23")"))[0];
        REQUIRE(both.summaries.size() == 2);

        const auto text = trace::index::to_text(
            dstd::Span<const trace::index::FileExplain>(&range, 1));
        const auto line =
            dstd::fmt<"  summary size zonemap: skipped {} of {} chunks\n">(
                range.summaries[0].skipped, chunks);
        CHECK(text.find(line) != dstd::String::npos);
        const auto json = trace::index::to_json(
            dstd::Span<const trace::index::FileExplain>(&range, 1));
        CHECK(json.find("\"summaries\":[{\"path\":\"size\",\"summary\":"
                        "\"zonemap\"") != dstd::String::npos);
    }

    TEST_CASE("a choice on a path-decoded schema names the path as it is") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::String body;
        for (int i = 0; i < 60; ++i)
            dstd::fmt_append<
                "{\"op\":\"r\",\"args\":{\"tag\":\"t{}\"},\"io\":{"
                "\"off\":{}}}\n">(body, i / 10, i);
        const auto plain = env.dir() + "/generic.jsonl";
        dstd::fs::write_file(dstd::PathView(plain), body).discard();
        const auto gz = plain + ".gz";
        REQUIRE(
            dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 256));
        const auto kinds = [&](dstd::StringView spelling, dstd::StringView path,
                               dstd::StringView dir) {
            auto o = options(env.dir() + "/" + dstd::String(dir),
                             {{dstd::String(spelling), {"zonemap"}}});
            o.schema = "generic";
            auto ix = open(gz, std::move(o));
            const auto built = dftu_utils_test::ok(ix.build());
            CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
            return kinds_of(rows_of(gz, built.index_path), path);
        };
        // The path is "args.tag": that spelling keeps only the zonemap and the
        // other names a path that holds nothing.
        CHECK(kinds("args.tag", "args.tag", "idx_a") ==
              dstd::Set<dstd::String>{"zonemap"});
        CHECK(kinds("tag", "args.tag", "idx_b").count("bloom") == 1);
    }
}
