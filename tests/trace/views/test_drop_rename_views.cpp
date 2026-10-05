#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

constexpr int RECORDS = 3000;

std::string gzip_of(TestEnvironment& env, const std::string& stem,
                    const std::string& body) {
    const std::string plain = env.get_dir() + "/" + stem + ".ndjson";
    {
        std::ofstream out(plain);
        out << body;
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    return gz;
}

std::string genesis_trace(TestEnvironment& env) {
    std::string body;
    for (int i = 0; i < RECORDS; ++i)
        body += R"({"gtype":"func","run":"ab","ts":)" + std::to_string(i) +
                R"(,"v":{"p50":)" + std::to_string(i / 30) + R"(,"p99":)" +
                std::to_string(i / 30 + 5) + R"(},"count":)" +
                std::to_string(i % 7) + "}\n";
    return gzip_of(env, "genesis", body);
}

// A field of text, one that is an int in the first half of the trace and a
// float in the second, and a timestamp-like int.
std::string shapes_trace(TestEnvironment& env) {
    std::string body;
    for (int i = 0; i < RECORDS; ++i) {
        const std::string mix = i < RECORDS / 2 ? std::to_string(i % 40)
                                                : std::to_string(i % 40) + ".5";
        body += R"({"gtype":"func","run":"ab","ts":)" + std::to_string(i) +
                R"(,"sv":"s)" + std::to_string(i % 9) + R"(","mix":)" + mix +
                R"(,"t0":)" + std::to_string(1700000000000000LL + i * 1000) +
                "}\n";
    }
    return gzip_of(env, "shapes", body);
}

std::string dft_trace(TestEnvironment& env) {
    std::string body;
    for (int i = 0; i < RECORDS; ++i)
        body +=
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
            std::to_string(1000 + i * 100) + R"(,"dur":)" +
            std::to_string(10 + i) + R"(,"args":{"size":)" +
            std::to_string(i % 13) + R"(,"offset":)" + std::to_string(i) +
            R"(}})" + "\n";
    return gzip_of(env, "dft", body);
}

View query_indexed(const std::string& gz, const std::string& schema) {
    return View::from_file(gz).record_schema(schema);
}

View full_index(const std::string& gz, const std::string& schema) {
    dftracer::utils::index::Indexer::open({gz}).build();
    return View::from_file(gz, determine_index_path(gz, ""))
        .record_schema(schema);
}

dataframe::DataFrame frame(const View& v, const std::string& q) {
    return v.duql(q).collect().get();
}

bool has(const dataframe::DataFrame& f, const std::string& n) {
    return std::find(f.names.begin(), f.names.end(), n) != f.names.end();
}

// Reads the catalog as it is: schema_tree() would build a missing one.
bool declares(const View& v, const std::string& path) {
    for (const auto& leaf :
         dftracer::utils::trace::views::detail::catalog_leaves(v))
        if (leaf.path == path) return true;
    return false;
}

std::vector<std::string> sorted(std::vector<std::string> n) {
    std::sort(n.begin(), n.end());
    return n;
}

void same_column(const dataframe::DataFrame& a, const dataframe::DataFrame& b,
                 const std::string& n) {
    REQUIRE(a.num_rows() == b.num_rows());
    const auto& ca = a.columns[static_cast<std::size_t>(bcol(a, n))];
    const auto& cb = b.columns[static_cast<std::size_t>(bcol(b, n))];
    for (std::int64_t r = 0; r < a.num_rows(); ++r) {
        REQUIRE(ca.is_null(r) == cb.is_null(r));
        if (ca.is_null(r)) continue;
        if (ca.type() == dataframe::TypeId::String)
            CHECK(bstr(a, r, n) == bstr(b, r, n));
        else
            CHECK(bnum(a, r, n) == bnum(b, r, n));
    }
}

std::string join_names(const dataframe::DataFrame& f) {
    std::string out;
    for (const auto& n : sorted(f.names)) out += n + " ";
    return out;
}

namespace st = dftracer::utils::index::store;

bool has_catalog(const std::string& gz) {
    try {
        st::IndexDatabase db(determine_index_path(gz, ""),
                             st::IndexOpenMode::ReadOnly);
        const int fid = db.get_file_info_id(st::internal::get_logical_path(gz));
        return fid >= 0 &&
               db.extension_current(fid, st::IndexExtension::CATALOG);
    } catch (...) {
        return false;
    }
}

// A trace whose index holds no catalog: a full scan writes one, so each
// query gets its own copy.
struct Fresh {
    std::unique_ptr<TestEnvironment> env;
    std::string gz;
    View view;
};

using Gen = std::string (*)(TestEnvironment&);

Fresh fresh(Gen gen, const std::string& schema) {
    Fresh f;
    f.env = std::make_unique<TestEnvironment>(10);
    f.gz = gen(*f.env);
    REQUIRE_FALSE(has_catalog(f.gz));
    f.view = query_indexed(f.gz, schema);
    return f;
}

// The query on a fresh catalog-less view equals the full-index result; an
// error on one must be the same error on the other.
void same_as_full(Gen gen, const std::string& schema, const std::string& q) {
    TestEnvironment full_env(10);
    const View f = full_index(gen(full_env), schema);
    const Fresh p = fresh(gen, schema);
    std::string want_error;
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
    for (const auto& n : got.names) same_column(got, want, n);
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

        const std::string base = "gtype == \"func\"";
        const auto all = frame(q, base);
        REQUIRE(has(all, "v.p50"));
        REQUIRE(has(all, "count"));
        CHECK(sorted(all.names) == sorted(frame(f, base).names));

        const auto dropped = frame(q, base + " | drop v.p50");
        CHECK_FALSE(has(dropped, "v.p50"));
        CHECK(dropped.names.size() == all.names.size() - 1);
        const auto dropped_full = frame(f, base + " | drop v.p50");
        REQUIRE(sorted(dropped.names) == sorted(dropped_full.names));
        for (const auto& n : dropped.names)
            same_column(dropped, dropped_full, n);

        const std::string rename = base + " | rename cnt = count, p = v.p99";
        const auto renamed = frame(q, rename);
        CHECK_FALSE(has(renamed, "count"));
        CHECK_FALSE(has(renamed, "v.p99"));
        CHECK(has(renamed, "cnt"));
        CHECK(has(renamed, "p"));
        CHECK(renamed.names.size() == all.names.size());
        const auto renamed_full = frame(f, rename);
        REQUIRE(sorted(renamed.names) == sorted(renamed_full.names));
        for (const auto& n : renamed.names)
            same_column(renamed, renamed_full, n);
    }

    TEST_CASE("dftracer trace without a catalog") {
        TestEnvironment query_env(10);
        TestEnvironment full_env(10);
        const View q = query_indexed(dft_trace(query_env), "dftracer");
        const View f = full_index(dft_trace(full_env), "dftracer");
        REQUIRE_FALSE(declares(q, "args.offset"));
        REQUIRE(declares(f, "args.offset"));

        const std::string base = "ph == \"X\"";
        const auto renamed = frame(q, base + " | rename off = args.offset");
        const auto renamed_full =
            frame(f, base + " | rename off = args.offset");
        CHECK(has(renamed, "off"));
        CHECK_FALSE(has(renamed, "args.offset"));
        CHECK(has(renamed, "dur"));
        same_column(renamed, renamed_full, "off");
        same_column(renamed, renamed_full, "dur");

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
        const std::string g =
            "let r = where gtype == \"func\" | select ts, rc = count; where "
            "count "
            "> 1 | lookup r on ts | sort ts";
        const std::string d =
            "let r = where ph == \"X\" | select ts, rd = dur; where dur > 20 | "
            "lookup r on ts | sort ts";
        same_as_full(genesis_trace, "genesis", g);
        same_as_full(dft_trace, "dftracer", d);
    }

    TEST_CASE("a self lookup refuses the same clash without a catalog") {
        // The rows and `r` share gtype, run and count: a lookup that fills a
        // column the rows already hold is the query's error, with or
        // without a catalog.
        const std::string g =
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
        const auto want = f.lazy().fill_null(0).collect().get();
        const Fresh p = fresh(genesis_trace, "genesis");
        const auto got = p.view.lazy().fill_null(0).collect().get();
        REQUIRE(sorted(got.names) == sorted(want.names));
        CHECK(has(got, "v.p50"));
        for (const auto& n : got.names) same_column(got, want, n);
    }
}
