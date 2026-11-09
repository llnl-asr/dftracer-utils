// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr int RECORDS = 3000;

dstd::String gzip_of(TestEnvironment& env, const dstd::String& stem,
                     const dstd::String& body) {
    const dstd::String plain = env.dir() + "/" + stem + ".ndjson";
    {
        dftu_utils_test::FileOut out(plain);
        out << body;
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

dstd::String genesis_trace(TestEnvironment& env) {
    dstd::String body;
    for (int i = 0; i < RECORDS; ++i)
        body += R"({"gtype":"func","run":"ab","ts":)" + dstd::to_string(i) +
                R"(,"v":{"p50":)" + dstd::to_string(i / 30) + R"(,"p99":)" +
                dstd::to_string(i / 30 + 5) + R"(},"count":)" +
                dstd::to_string(i % 7) + "}\n";
    return gzip_of(env, "genesis", body);
}

// A field of text, one that is an int in the first half of the trace and a
// float in the second, and a timestamp-like int.
dstd::String shapes_trace(TestEnvironment& env) {
    dstd::String body;
    for (int i = 0; i < RECORDS; ++i) {
        const dstd::String mix = i < RECORDS / 2
                                     ? dstd::to_string(i % 40)
                                     : dstd::to_string(i % 40) + ".5";
        body += R"({"gtype":"func","run":"ab","ts":)" + dstd::to_string(i) +
                R"(,"sv":"s)" + dstd::to_string(i % 9) + R"(","mix":)" + mix +
                R"(,"t0":)" + dstd::to_string(1700000000000000LL + i * 1000) +
                "}\n";
    }
    return gzip_of(env, "shapes", body);
}

dstd::String dft_trace(TestEnvironment& env) {
    dstd::String body;
    for (int i = 0; i < RECORDS; ++i)
        body +=
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
            dstd::to_string(1000 + i * 100) + R"(,"dur":)" +
            dstd::to_string(10 + i) + R"(,"args":{"size":)" +
            dstd::to_string(i % 13) + R"(,"offset":)" + dstd::to_string(i) +
            R"(}})" + "\n";
    return gzip_of(env, "dft", body);
}

View query_indexed(const dstd::String& gz, const dstd::String& schema) {
    return View::from_file(dstd::String(gz))
        .record_schema(dstd::String(schema));
}

View full_index(const dstd::String& gz, const dstd::String& schema) {
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))})
        .build();
    return View::from_file(
               dstd::String(gz),
               dstd::String(dstd::String(determine_index_path(gz, ""))))
        .record_schema(dstd::String(schema));
}

dataframe::DataFrame frame(const View& v, const dstd::String& q) {
    return dftu_utils_test::ok(v.duql(q).collect().get());
}

bool has(const dataframe::DataFrame& f, const dstd::String& n) {
    return std::find(f.names.begin(), f.names.end(), n) != f.names.end();
}

// Reads the catalog as it is: schema_tree() would build a missing one.
bool declares(const View& v, const dstd::String& path) {
    for (const auto& leaf :
         dftracer::utils::trace::views::detail::catalog_leaves(v))
        if (dstd::String(leaf.path) == path) return true;
    return false;
}

dstd::Vector<dstd::String> sorted(dstd::Vector<dstd::String> n) {
    std::sort(n.begin(), n.end());
    return n;
}

void same_rows(const dataframe::DataFrame& a, const dataframe::DataFrame& b,
               const dstd::Vector<dstd::String>& cols) {
    REQUIRE(a.num_rows() == b.num_rows());
    CHECK(sorted_row_keys(a, cols) == sorted_row_keys(b, cols));
}

void same_rows(const dataframe::DataFrame& a, const dataframe::DataFrame& b) {
    REQUIRE(a.num_rows() == b.num_rows());
    CHECK(sorted_row_keys(a) == sorted_row_keys(b));
}

dstd::String join_names(const dataframe::DataFrame& f) {
    dstd::String out;
    for (const auto& n : sorted(f.names)) out += n + " ";
    return out;
}

bool has_catalog(const dstd::String& gz) {
    try {
        const auto snapshot = dftu_utils_test::index_snapshot(
            dstd::String(determine_index_path(gz, "")));
        const auto file = dftu_utils_test::find_indexed_file(snapshot, gz);
        return file &&
               dftracer::utils::index::extension_current(
                   snapshot, *file, index::store::IndexExtension::Catalog);
    } catch (...) {
        return false;
    }
}

// A trace whose index holds no catalog: a full scan writes one, so each
// query gets its own copy.
struct Fresh {
    dstd::UniquePtr<TestEnvironment> env;
    dstd::String gz;
    View view;
};

using Gen = dstd::String (*)(TestEnvironment&);

Fresh fresh(Gen gen, const dstd::String& schema) {
    Fresh f;
    f.env = dstd::make_unique<TestEnvironment>(10);
    f.gz = gen(*f.env);
    REQUIRE_FALSE(has_catalog(f.gz));
    f.view = query_indexed(f.gz, schema);
    return f;
}

// The query on a fresh catalog-less view equals the full-index result; an
// error on one must be the same error on the other.
void same_as_full(Gen gen, const dstd::String& schema, const dstd::String& q) {
    TestEnvironment full_env(10);
    const View f = full_index(gen(full_env), schema);
    const Fresh p = fresh(gen, schema);
    dstd::String want_error;
    dataframe::DataFrame want;
    try {
        want = frame(f, q);
    } catch (const std::exception& e) {
        want_error = e.what();
    }
    if (!want_error.empty()) {
        INFO(q);
        CHECK_THROWS_WITH(frame(p.view, q), want_error.c_str());
        return;
    }
    const auto got = frame(p.view, q);
    INFO(q);
    INFO("got " << join_names(got) << " want " << join_names(want));
    REQUIRE(sorted(got.names) == sorted(want.names));
    same_rows(got, want);
}

struct Case {
    const char* name;
    const char* genesis;
    const char* dft;
};

const Case CASES[] = {
    {"sort and take", "sort -count, ts | take 50", "sort -dur, ts | take 50"},
    {"take after a filter", "where count > 2 | take 100",
     "where dur > 20 | take 100"},
    {"derive then sort", "derive d = count * 2 | sort -d, ts",
     "derive d = dur * 2 | sort -d, ts"},
    // Without a catalog `count` has no type until a batch gives it one: the
    // filter and the sort compare its values, not nulls.
    {"filter and sort on a field typed per batch",
     "derive d = count * 2 | where count > 5 | sort -count, ts",
     "derive d = dur * 2 | where dur > 25 | sort -dur, ts"},
    {"distinct", "distinct | sort ts", "distinct | sort ts"},
    {"window",
     "window run sort ts { rn = row_number(), s = sum(count) over 5 "
     "rows } | sort ts",
     "window pid sort ts { rn = row_number(), g = ts - lag(ts) } | sort ts"},
    {"group", "group count { n = count(), m = sum(ts) } | sort count",
     "group args.size { n = count(), d = sum(dur) } | sort args.size"},
};

}  // namespace

TEST_SUITE("DropRenameViews") {
    TEST_CASE("path trace without a catalog") {
        TestEnvironment query_env(10);
        TestEnvironment full_env(10);
        const View q = query_indexed(genesis_trace(query_env), "genesis");
        const View f = full_index(genesis_trace(full_env), "genesis");
        REQUIRE_FALSE(declares(q, "v.p50"));
        REQUIRE(declares(f, "v.p50"));

        const dstd::String base = "gtype == \"func\"";
        const auto all = frame(q, base);
        REQUIRE(has(all, "v.p50"));
        REQUIRE(has(all, "count"));
        CHECK(sorted(all.names) == sorted(frame(f, base).names));

        const auto dropped = frame(q, base + " | drop v.p50");
        CHECK_FALSE(has(dropped, "v.p50"));
        CHECK(dropped.names.size() == all.names.size() - 1);
        const auto dropped_full = frame(f, base + " | drop v.p50");
        REQUIRE(sorted(dropped.names) == sorted(dropped_full.names));
        same_rows(dropped, dropped_full);

        const dstd::String rename = base + " | rename cnt = count, p = v.p99";
        const auto renamed = frame(q, rename);
        CHECK_FALSE(has(renamed, "count"));
        CHECK_FALSE(has(renamed, "v.p99"));
        CHECK(has(renamed, "cnt"));
        CHECK(has(renamed, "p"));
        CHECK(renamed.names.size() == all.names.size());
        const auto renamed_full = frame(f, rename);
        REQUIRE(sorted(renamed.names) == sorted(renamed_full.names));
        same_rows(renamed, renamed_full);
    }

    TEST_CASE("dftracer trace without a catalog") {
        TestEnvironment query_env(10);
        TestEnvironment full_env(10);
        const View q = query_indexed(dft_trace(query_env), "dftracer");
        const View f = full_index(dft_trace(full_env), "dftracer");
        REQUIRE_FALSE(declares(q, "args.offset"));
        REQUIRE(declares(f, "args.offset"));

        const dstd::String base = "ph == \"X\"";
        const auto renamed = frame(q, base + " | rename off = args.offset");
        const auto renamed_full =
            frame(f, base + " | rename off = args.offset");
        CHECK(has(renamed, "off"));
        CHECK_FALSE(has(renamed, "args.offset"));
        CHECK(has(renamed, "dur"));
        same_rows(renamed, renamed_full, {"off", "dur"});

        const auto dropped = frame(q, base + " | drop dur");
        CHECK_FALSE(has(dropped, "dur"));
        CHECK(has(dropped, "name"));
        CHECK(has(dropped, "ts"));
    }

    TEST_CASE("lazy ops equal the full index without a catalog") {
        for (const auto& c : CASES) {
            INFO(c.name);
            same_as_full(genesis_trace, "genesis", c.genesis);
            same_as_full(dft_trace, "dftracer", c.dft);
        }
    }

    TEST_CASE("fields of mixed or text types equal the full index") {
        for (const char* q :
             {"derive a = sv * 2 | sort ts", "where sv > 3 | sort ts",
              "derive m = mix * 2 | where mix > 10 | sort ts",
              "derive f = format_time(t0, \"%Y-%m\") | sort ts",
              "derive u = upper(sv), n = len(sv) | sort ts",
              "derive r = round(mix, 0) | sort -r, ts | take 20"}) {
            INFO(q);
            same_as_full(shapes_trace, "genesis", q);
        }
    }

    TEST_CASE("lookup keeps every left and right column") {
        const dstd::String g =
            "let r = where gtype == \"func\" | select ts, rc = count; where "
            "count "
            "> 1 | lookup r on ts | sort ts";
        const dstd::String d =
            "let r = where ph == \"X\" | select ts, rd = dur; where dur > 20 | "
            "lookup r on ts | sort ts";
        same_as_full(genesis_trace, "genesis", g);
        same_as_full(dft_trace, "dftracer", d);
    }

    TEST_CASE("a self lookup refuses the same clash without a catalog") {
        // The rows and `r` share gtype, run and count: a lookup that fills a
        // column the rows already hold is the query's error, with or
        // without a catalog.
        const dstd::String g =
            "let r = where gtype == \"func\"; where count > 1 | lookup r on "
            "ts | sort ts";
        TestEnvironment full_env(10);
        const View f = full_index(genesis_trace(full_env), "genesis");
        CHECK_THROWS_WITH(frame(f, g), doctest::Contains("lookup fills"));
        const Fresh p = fresh(genesis_trace, "genesis");
        CHECK_THROWS_WITH(frame(p.view, g), doctest::Contains("lookup fills"));
    }

    TEST_CASE("fill_null keeps the undeclared columns") {
        TestEnvironment full_env(10);
        const View f = full_index(genesis_trace(full_env), "genesis");
        const auto want =
            dftu_utils_test::ok(f.lazy().fill_null(0).collect().get());
        const Fresh p = fresh(genesis_trace, "genesis");
        const auto got =
            dftu_utils_test::ok(p.view.lazy().fill_null(0).collect().get());
        REQUIRE(sorted(got.names) == sorted(want.names));
        CHECK(has(got, "v.p50"));
        same_rows(got, want);
    }
}
