// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_spill.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <memory/fresh_process.h>
#include <sys/stat.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {
dstd::Optional<Morsel> run(dftracer::utils::coro::CoroTask<MorselResult> t) {
    return dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(t)).get());
}

dstd::SharedPtr<spill::Dir> make_dir() {
    return dftu_utils_test::ok(spill::Dir::create());
}

spill::Writer open_writer(const dstd::String& path) {
    return dftu_utils_test::ok(spill::Writer::open(path));
}

dstd::UniquePtr<spill::Reader> open_reader(const dstd::String& path) {
    return dftu_utils_test::ok(spill::Reader::open(path));
}

dstd::Result<void> drain_run(const dstd::String& path) {
    auto reader = spill::Reader::open(path);
    if (!reader) return reader.error();
    for (;;) {
        auto next =
            dftracer::utils::default_runtime().submit((*reader)->next(0)).get();
        if (!next) return next.error();
        if (!*next) return {};
    }
}
}  // namespace

TEST_SUITE("spill") {
    TEST_CASE("round-trip mixed-type morsels through a run file") {
        dstd::Vector<std::int64_t> a{1, 2, 3, 4};
        std::uint8_t bm = 0b1011;  // row 2 null
        dstd::Vector<double> b{1.5, 2.5, 3.5, 4.5};
        dstd::Vector<dstd::String> s{"foo", "", "bar", "baz"};

        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 4, &bm));
        cols.push_back(Series::flat_f64(b.data(), 4));
        cols.push_back(Series::strings(s));

        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        {
            auto w = open_writer(path);
            dftu_utils_test::ok(w.write(cols, 4));
            dftu_utils_test::ok(w.write(cols, 4));  // two morsels in one run
            dftu_utils_test::ok(w.close());
        }

        auto r = open_reader(path);
        int morsels = 0;
        while (auto m = run(r->next(0))) {
            ++morsels;
            REQUIRE(m->rows == 4);
            REQUIRE(m->columns.size() == 3);
            const std::int64_t* ap = m->columns[0].data<std::int64_t>();
            CHECK(ap[0] == 1);
            CHECK(ap[3] == 4);
            CHECK(m->columns[0].is_null(2));
            CHECK_FALSE(m->columns[0].is_null(0));
            const double* bp = m->columns[1].data<double>();
            CHECK(bp[2] == doctest::Approx(3.5));
            CHECK(m->columns[2].string_at(0) == "foo");
            CHECK(m->columns[2].string_at(1) == "");
            CHECK(m->columns[2].string_at(2) == "bar");
        }
        CHECK(morsels == 2);
    }

    TEST_CASE("compressed morsels read back exactly and take less room") {
        struct Env {
            explicit Env(const char* v) {
                ::setenv("DFTRACER_UTILS_SPILL_COMPRESS", v, 1);
            }
            ~Env() { ::unsetenv("DFTRACER_UTILS_SPILL_COMPRESS"); }
        };
        constexpr std::int64_t N = 20000;
        dstd::Vector<std::int64_t> a(N);
        dstd::Vector<dstd::String> s(N);
        dstd::Vector<std::uint8_t> valid((N + 7) / 8, 0xFF);
        std::uint64_t x = 88172645463325252ULL;
        for (std::int64_t i = 0; i < N; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            a[i] = i < N / 2 ? i % 97 : static_cast<std::int64_t>(x);
            s[i] = dstd::fmt<"name{}">(i % 31);
            if (i % 13 == 0)
                valid[i / 8] =
                    static_cast<std::uint8_t>(valid[i / 8] & ~(1u << (i % 8)));
        }
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), N, valid.data()));
        cols.push_back(Series::strings(s));

        auto round_trip = [&](const char* mode, std::uintmax_t* bytes) {
            Env env(mode);
            const auto dir_owner = make_dir();
            spill::Dir& dir = *dir_owner;
            const dstd::String path = dir.run_path(0);
            {
                auto w = open_writer(path);
                dftu_utils_test::ok(w.write(cols, N));
                dftu_utils_test::ok(w.write(cols, N));
                dftu_utils_test::ok(w.close());
            }
            *bytes = dstd::fs::file_size(path).value();
            auto r = open_reader(path);
            int morsels = 0;
            while (auto m = run(r->next(0))) {
                ++morsels;
                REQUIRE(m->rows == N);
                for (std::int64_t i = 0; i < N; ++i) {
                    REQUIRE(m->columns[0].is_null(i) == (i % 13 == 0));
                    if (i % 13 != 0)
                        REQUIRE(m->columns[0].data<std::int64_t>()[i] == a[i]);
                    REQUIRE(m->columns[1].string_at(i) == s[i]);
                }
            }
            CHECK(morsels == 2);
        };
        std::uintmax_t raw_bytes = 0, packed_bytes = 0;
        round_trip("off", &raw_bytes);
        round_trip("on", &packed_bytes);
        CHECK(packed_bytes < raw_bytes);
    }

    TEST_CASE("empty run reads back as no morsels") {
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(1);
        {
            auto w = open_writer(path);
            dftu_utils_test::ok(w.close());
        }
        auto r = open_reader(path);
        CHECK_FALSE(run(r->next(0)).has_value());
    }

    TEST_CASE("a JSON column keeps its flag through a run file") {
        Series text = Series::strings(dstd::Vector<dstd::String>{"1", "\"a\""});
        dstd::Vector<Series> cols;
        cols.push_back(text.as_json());
        cols.push_back(Series::strings(dstd::Vector<dstd::String>{"x", "y"}));
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        {
            auto w = open_writer(path);
            dftu_utils_test::ok(w.write(cols, 2));
            dftu_utils_test::ok(w.close());
        }
        auto r = open_reader(path);
        auto m = run(r->next(0));
        REQUIRE(m);
        CHECK(m->columns[0].is_json());
        CHECK_FALSE(m->columns[1].is_json());
        CHECK(m->columns[0].string_at(1) == "\"a\"");
    }
}

TEST_SUITE("spill errors") {
    TEST_CASE("a run in a missing directory cannot be opened") {
        dftu_utils_test::expect_error(
            spill::Writer::open("/dftu_no_such_dir/run0"),
            common::ErrorCode::Io);
        dftu_utils_test::expect_error(
            spill::Reader::open("/dftu_no_such_dir/run0"),
            common::ErrorCode::Io);
        dftu_utils_test::expect_error(
            spill::AggRunReader::open("/dftu_no_such_dir/run0"),
            common::ErrorCode::Io);
    }

    TEST_CASE("a failed aggregate spill leaves no partial run behind") {
        if (::geteuid() == 0) return;
        const dstd::Path root = dstd::fs::temp_directory() /
                                dstd::fmt<"dftu_agg_spill_fail_{}">(::getpid());
        REQUIRE(dstd::fs::remove_all(root).has_value());
        REQUIRE(dstd::fs::create_directories(root).has_value());
        ::setenv("DFTRACER_UTILS_SPILL_DIR", root.c_str(), 1);

        dstd::Vector<std::int64_t> keys{1, 2, 3};
        dstd::Vector<std::int64_t> vals{4, 5, 6};
        const Series k = Series::flat_i64(keys.data(), 3);
        const Series v = Series::flat_i64(vals.data(), 3);
        auto fill = []() { return agg_new({AggSpec{AggOp::Count, -1, "n"}}); };
        AggStatePtr state = fill();
        dftu_utils_test::ok(agg_accumulate(*state,
                                           dstd::Vector<const Series*>{&k},
                                           dstd::Vector<const Series*>{&v}));

        AggSpiller spiller(1);
        CHECK(dftu_utils_test::ok(spiller.maybe_spill(state)));
        REQUIRE(spiller.runs() == 1);

        dstd::Path sub;
        for (const auto& e : dftu_utils_test::list_dir(root)) sub = e.path;
        REQUIRE_FALSE(sub.empty());
        REQUIRE(::chmod(sub.c_str(), 0500) == 0);

        state = fill();
        dftu_utils_test::ok(agg_accumulate(*state,
                                           dstd::Vector<const Series*>{&k},
                                           dstd::Vector<const Series*>{&v}));
        dftu_utils_test::expect_error(spiller.maybe_spill(state),
                                      common::ErrorCode::Io);
        CHECK(spiller.runs() == 1);
        CHECK(agg_group_count(*state) == 3);

        ::chmod(sub.c_str(), 0700);
        const auto drained =
            spiller.drain(*state, dstd::Vector<dstd::String>{"k"});
        CHECK(drained.has_value());

        ::unsetenv("DFTRACER_UTILS_SPILL_DIR");
        dstd::fs::remove_all(root).discard();
    }

    TEST_CASE("a truncated run is an error, not the end of the run") {
        dstd::Vector<std::int64_t> a{1, 2, 3, 4, 5, 6, 7, 8};
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 8));
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        {
            auto w = open_writer(path);
            dftu_utils_test::ok(w.write(cols, 8));
            dftu_utils_test::ok(w.write(cols, 8));
            dftu_utils_test::ok(w.close());
        }
        const std::uintmax_t full = dstd::fs::file_size(path).value();
        CHECK(drain_run(path).has_value());
        // Cut inside the second morsel's payload, then inside its header.
        REQUIRE(dstd::fs::resize_file(path, full - 5).has_value());
        dftu_utils_test::expect_error(drain_run(path),
                                      common::ErrorCode::Parse);
        REQUIRE(dstd::fs::resize_file(path, full / 2 + 5).has_value());
        dftu_utils_test::expect_error(drain_run(path), common::ErrorCode::Io);
    }

    TEST_CASE("a truncated aggregate run is an error") {
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        {
            dstd::String os;
            const std::uint32_t len = 100;
            os.append(reinterpret_cast<const char*>(&len), sizeof(len));
            os.append("abcd", 4);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        dftu_utils_test::expect_error(spill::AggRunReader::open(path),
                                      common::ErrorCode::Parse);
        {
            dstd::String os;
            os.append("ab", 2);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        dftu_utils_test::expect_error(spill::AggRunReader::open(path),
                                      common::ErrorCode::Io);
        {
            dstd::String os;
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        const auto empty = dftu_utils_test::ok(spill::AggRunReader::open(path));
        CHECK_FALSE(empty->valid());
    }

    TEST_CASE("a morsel size that is not in the file is an error") {
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        {
            dstd::String os;
            const std::int64_t sizes[2] = {std::int64_t{1} << 40,
                                           std::int64_t{1} << 40};
            os.append(reinterpret_cast<const char*>(sizes), sizeof(sizes));
            os.append("abcd", 4);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        auto big = open_reader(path);
        CHECK_FALSE(dftracer::utils::default_runtime()
                        .submit(big->next(0))
                        .get()
                        .has_value());
        {
            dstd::String os;
            const std::int64_t sizes[2] = {-5, -5};
            os.append(reinterpret_cast<const char*>(sizes), sizeof(sizes));
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        auto negative = open_reader(path);
        CHECK_FALSE(dftracer::utils::default_runtime()
                        .submit(negative->next(0))
                        .get()
                        .has_value());
    }

    TEST_CASE("an aggregate state length past the file is an error") {
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        dstd::String os;
        const std::uint32_t len = 0xFFFFFFF0u;
        os.append(reinterpret_cast<const char*>(&len), sizeof(len));
        os.append("abcdefgh", 8);
        dstd::fs::write_file(dstd::PathView(path), os).discard();
        dftu_utils_test::expect_error(spill::AggRunReader::open(path),
                                      common::ErrorCode::Parse);
    }

    TEST_CASE("write_run with a zero morsel size still writes every row") {
        dstd::Vector<std::int64_t> a{1, 2, 3};
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 3));
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        std::uint64_t largest = 0;
        {
            auto w = open_writer(path);
            largest = dftu_utils_test::ok(spill::write_run(w, cols, 3, 0));
            dftu_utils_test::ok(w.close());
        }
        CHECK(largest > 0);
        auto r = open_reader(path);
        std::int64_t rows = 0;
        while (auto m = run(r->next(0))) rows += m->rows;
        CHECK(rows == 3);
    }

    TEST_CASE(
        "two readers of a spool each read every morsel, in memory or on disk") {
        for (const std::uint64_t budget :
             {std::uint64_t{1} << 20, std::uint64_t{1}}) {
            dstd::Vector<std::int64_t> a{1, 2, 3};
            spill::Spool spool(budget);
            dstd::Vector<Series> cols;
            cols.push_back(Series::flat_i64(a.data(), 3));
            dftu_utils_test::ok(spool.add(std::move(cols), 3));
            dstd::UniquePtr<dftracer::utils::dataframe::Cursor> first =
                dftu_utils_test::ok(spool.reader());
            dstd::UniquePtr<dftracer::utils::dataframe::Cursor> second =
                dftu_utils_test::ok(spool.reader());
            // Both readers read the three rows.
            for (auto* r : {first.get(), second.get()}) {
                auto m = run(r->next(0));
                REQUIRE(m);
                CHECK(m->rows == 3);
            }
        }
    }

    TEST_CASE("a spool that takes a morsel after a reader ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        dstd::Vector<std::int64_t> a{1, 2, 3};
        spill::Spool spool(1);
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 3));
        dftu_utils_test::ok(spool.add(std::move(cols), 3));
        auto reader = dftu_utils_test::ok(spool.reader());
        dstd::Vector<Series> more;
        more.push_back(Series::flat_i64(a.data(), 3));
        (void)spool.add(std::move(more), 3);
    }

    TEST_CASE("a spool that takes a morsel after a reader is fatal") {
        auto result = fresh_process::run(
            "a spool that takes a morsel after a reader ends the process");
        CHECK(result.aborted_with("spool: a morsel was added after a reader"));
    }

    TEST_CASE("a spool read backwards returns the morsels last first") {
        // A budget of one byte puts the first morsel in memory and the rest on
        // disk; a large one keeps all of them in memory.
        for (const std::uint64_t budget :
             {std::uint64_t{1} << 20, std::uint64_t{1}}) {
            spill::Spool spool(budget);
            for (std::int64_t m = 0; m < 5; ++m) {
                dstd::Vector<std::int64_t> a{m * 10, m * 10 + 1, m * 10 + 2};
                dstd::Vector<Series> cols;
                cols.push_back(Series::flat_i64(a.data(), 3));
                dftu_utils_test::ok(spool.add(std::move(cols), 3));
            }
            for (int pass = 0; pass < 2; ++pass) {
                auto back = dftu_utils_test::ok(spool.reverse_reader());
                dstd::Vector<std::int64_t> firsts;
                while (auto m = run(back->next(0))) {
                    REQUIRE(m->rows == 3);
                    const Series c = m->columns[0].materialize();
                    CHECK(c.data<std::int64_t>()[1] ==
                          c.data<std::int64_t>()[0] + 1);
                    firsts.push_back(c.data<std::int64_t>()[0]);
                }
                CHECK(firsts == dstd::Vector<std::int64_t>{40, 30, 20, 10, 0});
            }
        }
    }

    TEST_CASE(
        "opening a run file that blocks leaves the worker to other tasks") {
        const auto dir_owner = make_dir();
        spill::Dir& dir = *dir_owner;
        const dstd::String path = dir.run_path(0);
        REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

        // One worker: the open of a FIFO blocks until a writer appears, so the
        // second task runs only if the open handed its permit away.
        Runtime rt(1);
        std::atomic<bool> opened{false};
        std::atomic<bool> other_ran{false};
        const auto open_task = [&]() -> dftracer::utils::coro::CoroTask<void> {
            const auto r = spill::Reader::open(path);
            opened.store(r.has_value());
            co_return;
        };
        const auto other_task = [&]() -> dftracer::utils::coro::CoroTask<void> {
            other_ran.store(true);
            co_return;
        };
        rt.submit(open_task(), "open");
        rt.submit(other_task(), "other");

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!other_ran.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool ran_while_blocked = other_ran.load() && !opened.load();

        const int writer = ::open(path.c_str(), O_WRONLY);
        rt.wait_all();
        if (writer >= 0) ::close(writer);
        ::unlink(path.c_str());

        CHECK(ran_while_blocked);
        CHECK(opened.load());
    }
}
