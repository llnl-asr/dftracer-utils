// SPDX-License-Identifier: MIT
// A streamed collect past its memory budget keeps the later parts in a mapped,
// unlinked spill file, and every spill file goes to DFTRACER_UTILS_SPILL_DIR.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <stdlib.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;

using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int64_t N = 3000;
constexpr std::int64_t MORSEL = 100;
constexpr const char* DIR_VAR = "DFTRACER_UTILS_SPILL_DIR";

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

struct SpillDirVar {
    dstd::Path dir;
    explicit SpillDirVar(const dstd::String& name) {
        dir = dstd::fs::temp_directory() /
              (name + "_" + dstd::to_string(::getpid()));
        REQUIRE(dstd::fs::remove_all(dir).has_value());
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        ::setenv(DIR_VAR, dir.c_str(), 1);
    }
    ~SpillDirVar() {
        ::unsetenv(DIR_VAR);
        (void)dstd::fs::remove_all(dir);
    }
    std::size_t entries() const {
        std::size_t n = 0;
        for (const auto& e : dftu_utils_test::list_dir(dir)) {
            (void)e;
            ++n;
        }
        return n;
    }
};

DataFrame make_frame() {
    dstd::Vector<std::int64_t> i64;
    dstd::Vector<double> f64;
    dstd::Vector<std::uint8_t> bits((N + 7) / 8, 0);
    dstd::Vector<std::uint8_t> valid((N + 7) / 8, 0);
    dstd::Vector<std::uint8_t> sv((N + 7) / 8, 0);
    dstd::Vector<dstd::String> text, json;
    dstd::Vector<std::int32_t> offs{0};
    dstd::Vector<std::int64_t> inner;
    for (std::int64_t i = 0; i < N; ++i) {
        i64.push_back(i * 37 - 100);
        f64.push_back(static_cast<double>(i) * 1.5);
        if (i % 3 != 1)
            bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (i % 5 != 2)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (i % 7 != 3) sv[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        text.push_back(i % 7 == 3 ? "" : "name-" + dstd::to_string(i % 11));
        json.push_back("{\"k\":" + dstd::to_string(i) + "}");
        inner.push_back(i);
        offs.push_back(static_cast<std::int32_t>(inner.size()));
    }
    dstd::Vector<dstd::StringView> tv(text.begin(), text.end());
    DataFrame df;
    df.names = {"i", "f", "b", "s", "j", "l"};
    df.columns.push_back(Series::flat_i64(i64.data(), N, valid.data()));
    df.columns.push_back(Series::flat_f64(f64.data(), N));
    df.columns.push_back(
        Series::flat(TypeId::Bool, bits.data(), N, valid.data()));
    df.columns.push_back(Series::strings(tv, sv.data()));
    df.columns.push_back(Series::strings(json).as_json());
    df.columns.push_back(Series::list(offs, Series::flat_i64(inner.data(), N)));
    return df;
}

LazyFrame lazy_of(const DataFrame& df, std::uint64_t budget) {
    DataFrame copy;
    copy.names = df.names;
    for (const Series& c : df.columns) copy.columns.push_back(c.share());
    return LazyFrame::scan(dstd::make_shared<InMemorySource>(std::move(copy)))
        .memory_budget(budget);
}

dstd::String cell(const Series& s, std::int64_t i) {
    if (s.is_null(i)) return "<null>";
    switch (s.type()) {
        case TypeId::Int64:
            return dstd::to_string(s.data<std::int64_t>()[i]);
        case TypeId::Float64:
            return dstd::to_string(s.data<double>()[i]);
        case TypeId::Bool:
            return ((s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "T"
                                                                     : "F";
        default:
            return dstd::String(s.string_at(i));
    }
}

void same_columns(const DataFrame& got, const DataFrame& want,
                  std::size_t ncols) {
    REQUIRE(got.names == want.names);
    for (std::size_t c = 0; c < ncols; ++c) {
        CAPTURE(want.names[c]);
        const Series& g = got.columns[c];
        const Series& w = want.columns[c];
        REQUIRE(g.length() == w.length());
        CHECK(g.type() == w.type());
        CHECK(g.null_count() == w.null_count());
        CHECK(g.is_json() == w.is_json());
        const Series gm = g.materialize();
        const Series wm = w.materialize();
        for (std::int64_t i = 0; i < w.length(); ++i) {
            const bool text = g.type() == TypeId::String;
            if (cell(wm, i) != cell(gm, i) || g.is_null(i) != w.is_null(i) ||
                (text && g.string_at(i) != w.string_at(i))) {
                FAIL_CHECK("row " << i << ": " << cell(gm, i) << " vs "
                                  << cell(wm, i));
                break;
            }
        }
    }
}

}  // namespace

TEST_SUITE("collect spill") {
    TEST_CASE("a budget below the result gives the unbudgeted columns") {
        SpillDirVar var("dftu_cs_equal");
        const DataFrame df = make_frame();
        const DataFrame want =
            run(lazy_of(df, dftracer::utils::memory::NO_SPILL_BUDGET)
                    .collect(MORSEL));
        const DataFrame got = run(lazy_of(df, 1).collect(MORSEL));
        same_columns(got, want, 5);
        const Series& list = got.columns[5];
        CHECK(list.type() == TypeId::List);
        CHECK(list.length() == N);
        CHECK(var.entries() == 0);
    }

    TEST_CASE("spilled columns outlive the frame they came from") {
        SpillDirVar var("dftu_cs_outlive");
        const DataFrame df = make_frame();
        const DataFrame want =
            run(lazy_of(df, dftracer::utils::memory::NO_SPILL_BUDGET)
                    .collect(MORSEL));
        Series kept_i, kept_s;
        {
            DataFrame got = run(lazy_of(df, 1).collect(MORSEL));
            kept_i = got.columns[0].share();
            kept_s = got.columns[3].share();
        }
        const Series want_i = want.columns[0].materialize();
        const Series kept_flat = kept_i.materialize();
        for (std::int64_t i = 0; i < N; ++i) {
            REQUIRE(cell(kept_flat, i) == cell(want_i, i));
            REQUIRE(kept_s.string_at(i) == want.columns[3].string_at(i));
        }
        CHECK(var.entries() == 0);
    }

    TEST_CASE("memory::NO_SPILL_BUDGET never creates a spill file") {
        SpillDirVar var("dftu_cs_never");
        ::setenv(DIR_VAR, (var.dir / "missing" / "x").c_str(), 1);
        const DataFrame df = make_frame();
        const DataFrame got =
            run(lazy_of(df, dftracer::utils::memory::NO_SPILL_BUDGET)
                    .collect(MORSEL));
        CHECK(got.num_rows() == N);
        CHECK(!dstd::fs::exists(var.dir / "missing").value());
    }

    TEST_CASE("a spill file goes to the directory the variable names") {
        SpillDirVar var("dftu_cs_dir");
        const dstd::Path run_path = [] {
            dftracer::utils::dataframe::spill::Dir d;
            return dstd::Path(d.run_path(0));
        }();
        CHECK(run_path.str().rfind(var.dir.str(), 0) == 0);
        {
            dftracer::utils::dataframe::spill::Dir d;
            (void)dstd::fs::write_file(dstd::PathView(d.run_path(0)), "x");
            CHECK(var.entries() == 1);
        }
        CHECK(var.entries() == 0);
    }

    TEST_CASE("an unusable directory fails naming it and the variable") {
        SpillDirVar var("dftu_cs_bad");
        const dstd::Path blocker = var.dir / "file";
        (void)dstd::fs::write_file(dstd::PathView(blocker), "x");
        const dstd::Path bad = blocker / "sub";
        ::setenv(DIR_VAR, bad.c_str(), 1);
        const DataFrame df = make_frame();
        const auto msg_of = [](auto&& fn) {
            try {
                fn();
            } catch (const std::exception& e) {
                return dstd::String(e.what());
            }
            return dstd::String();
        };
        const dstd::String collect_msg =
            msg_of([&] { run(lazy_of(df, 1).collect(MORSEL)); });
        CHECK(collect_msg.find(bad.str()) != dstd::String::npos);
        CHECK(collect_msg.find(DIR_VAR) != dstd::String::npos);
        const dstd::String dir_msg =
            msg_of([] { dftracer::utils::dataframe::spill::Dir d; });
        CHECK(dir_msg.find(bad.str()) != dstd::String::npos);
        CHECK(dir_msg.find(DIR_VAR) != dstd::String::npos);
    }
}
