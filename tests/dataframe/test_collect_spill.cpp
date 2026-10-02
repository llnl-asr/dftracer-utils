// A streamed collect past its memory budget keeps the later parts in a mapped,
// unlinked spill file, and every spill file goes to DFTRACER_UTILS_SPILL_DIR.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t N = 3000;
constexpr std::int64_t MORSEL = 100;
constexpr const char* DIR_VAR = "DFTRACER_UTILS_SPILL_DIR";

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

struct SpillDirVar {
    fs::path dir;
    explicit SpillDirVar(const std::string& name) {
        dir = fs::temp_directory_path() /
              (name + "_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        ::setenv(DIR_VAR, dir.c_str(), 1);
    }
    ~SpillDirVar() {
        ::unsetenv(DIR_VAR);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::size_t entries() const {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            (void)e;
            ++n;
        }
        return n;
    }
};

DataFrame make_frame() {
    std::vector<std::int64_t> i64;
    std::vector<double> f64;
    std::vector<std::uint8_t> bits((N + 7) / 8, 0);
    std::vector<std::uint8_t> valid((N + 7) / 8, 0);
    std::vector<std::uint8_t> sv((N + 7) / 8, 0);
    std::vector<std::string> text, json;
    std::vector<std::int32_t> offs{0};
    std::vector<std::int64_t> inner;
    for (std::int64_t i = 0; i < N; ++i) {
        i64.push_back(i * 37 - 100);
        f64.push_back(static_cast<double>(i) * 1.5);
        if (i % 3 != 1)
            bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (i % 5 != 2)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (i % 7 != 3) sv[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        text.push_back(i % 7 == 3 ? "" : "name-" + std::to_string(i % 11));
        json.push_back("{\"k\":" + std::to_string(i) + "}");
        inner.push_back(i);
        offs.push_back(static_cast<std::int32_t>(inner.size()));
    }
    std::vector<std::string_view> tv(text.begin(), text.end());
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
    return LazyFrame::scan(std::make_shared<InMemorySource>(std::move(copy)))
        .memory_budget(budget);
}

std::string cell(const Series& s, std::int64_t i) {
    if (s.is_null(i)) return "<null>";
    switch (s.type()) {
        case TypeId::Int64:
            return std::to_string(s.data<std::int64_t>()[i]);
        case TypeId::Float64:
            return std::to_string(s.data<double>()[i]);
        case TypeId::Bool:
            return ((s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "T"
                                                                     : "F";
        default:
            return std::string(s.string_at(i));
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
            run(lazy_of(df, dftracer::utils::NO_SPILL_BUDGET).collect(MORSEL));
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
            run(lazy_of(df, dftracer::utils::NO_SPILL_BUDGET).collect(MORSEL));
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

    TEST_CASE("NO_SPILL_BUDGET never creates a spill file") {
        SpillDirVar var("dftu_cs_never");
        ::setenv(DIR_VAR, (var.dir / "missing" / "x").c_str(), 1);
        const DataFrame df = make_frame();
        const DataFrame got =
            run(lazy_of(df, dftracer::utils::NO_SPILL_BUDGET).collect(MORSEL));
        CHECK(got.num_rows() == N);
        CHECK(!fs::exists(var.dir / "missing"));
    }

    TEST_CASE("a spill file goes to the directory the variable names") {
        SpillDirVar var("dftu_cs_dir");
        const fs::path run_path = [] {
            dftracer::utils::dataframe::spill::Dir d;
            return fs::path(d.run_path(0));
        }();
        CHECK(run_path.string().rfind(var.dir.string(), 0) == 0);
        {
            dftracer::utils::dataframe::spill::Dir d;
            std::ofstream(d.run_path(0)) << "x";
            CHECK(var.entries() == 1);
        }
        CHECK(var.entries() == 0);
    }

    TEST_CASE("an unusable directory fails naming it and the variable") {
        SpillDirVar var("dftu_cs_bad");
        const fs::path blocker = var.dir / "file";
        std::ofstream(blocker) << "x";
        const fs::path bad = blocker / "sub";
        ::setenv(DIR_VAR, bad.c_str(), 1);
        const DataFrame df = make_frame();
        const auto msg_of = [](auto&& fn) {
            try {
                fn();
            } catch (const std::exception& e) {
                return std::string(e.what());
            }
            return std::string();
        };
        const std::string collect_msg =
            msg_of([&] { run(lazy_of(df, 1).collect(MORSEL)); });
        CHECK(collect_msg.find(bad.string()) != std::string::npos);
        CHECK(collect_msg.find(DIR_VAR) != std::string::npos);
        const std::string dir_msg =
            msg_of([] { dftracer::utils::dataframe::spill::Dir d; });
        CHECK(dir_msg.find(bad.string()) != std::string::npos);
        CHECK(dir_msg.find(DIR_VAR) != std::string::npos);
    }
}
