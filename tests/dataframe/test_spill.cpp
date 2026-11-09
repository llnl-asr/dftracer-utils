// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/series.h>
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
#include <sys/stat.h>
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
dstd::Optional<Morsel> run(
    dftracer::utils::coro::CoroTask<dstd::Optional<Morsel>> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
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

        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 4);
            w.write(cols, 4);  // two morsels in one run
            w.close();
        }

        spill::Reader r(path);
        int morsels = 0;
        while (auto m = run(r.next(0))) {
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
            s[i] = "name" + dstd::to_string(i % 31);
            if (i % 13 == 0)
                valid[i / 8] =
                    static_cast<std::uint8_t>(valid[i / 8] & ~(1u << (i % 8)));
        }
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), N, valid.data()));
        cols.push_back(Series::strings(s));

        auto round_trip = [&](const char* mode, std::uintmax_t* bytes) {
            Env env(mode);
            spill::Dir dir;
            const dstd::String path = dir.run_path(0);
            {
                spill::Writer w(path);
                w.write(cols, N);
                w.write(cols, N);
                w.close();
            }
            *bytes = dstd::fs::file_size(path).value();
            spill::Reader r(path);
            int morsels = 0;
            while (auto m = run(r.next(0))) {
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
        spill::Dir dir;
        const dstd::String path = dir.run_path(1);
        {
            spill::Writer w(path);
            w.close();
        }
        spill::Reader r(path);
        CHECK_FALSE(run(r.next(0)).has_value());
    }

    TEST_CASE("a JSON column keeps its flag through a run file") {
        Series text = Series::strings(dstd::Vector<dstd::String>{"1", "\"a\""});
        dstd::Vector<Series> cols;
        cols.push_back(text.as_json());
        cols.push_back(Series::strings(dstd::Vector<dstd::String>{"x", "y"}));
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 2);
            w.close();
        }
        spill::Reader r(path);
        auto m = run(r.next(0));
        REQUIRE(m);
        CHECK(m->columns[0].is_json());
        CHECK_FALSE(m->columns[1].is_json());
        CHECK(m->columns[0].string_at(1) == "\"a\"");
    }
}

TEST_SUITE("spill errors") {
    TEST_CASE("a run in a missing directory cannot be opened") {
        CHECK_THROWS(spill::Writer("/dftu_no_such_dir/run0"));
        CHECK_THROWS(spill::Reader("/dftu_no_such_dir/run0"));
        CHECK_THROWS(spill::AggRunReader("/dftu_no_such_dir/run0"));
    }

    TEST_CASE("a truncated run is an error, not the end of the run") {
        dstd::Vector<std::int64_t> a{1, 2, 3, 4, 5, 6, 7, 8};
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 8));
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 8);
            w.write(cols, 8);
            w.close();
        }
        const std::uintmax_t full = dstd::fs::file_size(path).value();
        auto drain = [&] {
            spill::Reader r(path);
            while (auto m = run(r.next(0))) {
            }
        };
        CHECK_NOTHROW(drain());
        // Cut inside the second morsel's payload, then inside its header.
        dstd::fs::resize_file(path, full - 5).has_value();
        CHECK_THROWS(drain());
        REQUIRE(dstd::fs::resize_file(path, full / 2 + 5).has_value());
        CHECK_THROWS(drain());
    }

    TEST_CASE("a truncated aggregate run is an error") {
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        {
            dstd::String os;
            const std::uint32_t len = 100;
            os.append(reinterpret_cast<const char*>(&len), sizeof(len));
            os.append("abcd", 4);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        CHECK_THROWS(spill::AggRunReader(path));
        {
            dstd::String os;
            os.append("ab", 2);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        CHECK_THROWS(spill::AggRunReader(path));
        {
            dstd::String os;
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        spill::AggRunReader empty(path);
        CHECK_FALSE(empty.valid());
    }

    TEST_CASE("a morsel size that is not in the file is an error") {
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        {
            dstd::String os;
            const std::int64_t sizes[2] = {std::int64_t{1} << 40,
                                           std::int64_t{1} << 40};
            os.append(reinterpret_cast<const char*>(sizes), sizeof(sizes));
            os.append("abcd", 4);
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        spill::Reader big(path);
        CHECK_THROWS(run(big.next(0)));
        {
            dstd::String os;
            const std::int64_t sizes[2] = {-5, -5};
            os.append(reinterpret_cast<const char*>(sizes), sizeof(sizes));
            dstd::fs::write_file(dstd::PathView(path), os).discard();
        }
        spill::Reader negative(path);
        CHECK_THROWS(run(negative.next(0)));
    }

    TEST_CASE("an aggregate state length past the file is an error") {
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        dstd::String os;
        const std::uint32_t len = 0xFFFFFFF0u;
        os.append(reinterpret_cast<const char*>(&len), sizeof(len));
        os.append("abcdefgh", 8);
        dstd::fs::write_file(dstd::PathView(path), os).discard();
        CHECK_THROWS(spill::AggRunReader(path));
    }

    TEST_CASE("write_run with a zero morsel size still writes every row") {
        dstd::Vector<std::int64_t> a{1, 2, 3};
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 3));
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        std::uint64_t largest = 0;
        {
            spill::Writer w(path);
            largest = spill::write_run(w, cols, 3, 0);
            w.close();
        }
        CHECK(largest > 0);
        spill::Reader r(path);
        std::int64_t rows = 0;
        while (auto m = run(r.next(0))) rows += m->rows;
        CHECK(rows == 3);
    }

    TEST_CASE("a spool takes no morsel after a reader, in memory or on disk") {
        for (const std::uint64_t budget :
             {std::uint64_t{1} << 20, std::uint64_t{1}}) {
            dstd::Vector<std::int64_t> a{1, 2, 3};
            spill::Spool spool(budget);
            dstd::Vector<Series> cols;
            cols.push_back(Series::flat_i64(a.data(), 3));
            spool.add(std::move(cols), 3);
            dstd::UniquePtr<dftracer::utils::dataframe::Cursor> first =
                spool.reader();
            dstd::UniquePtr<dftracer::utils::dataframe::Cursor> second =
                spool.reader();
            dstd::Vector<Series> more;
            more.push_back(Series::flat_i64(a.data(), 3));
            CHECK_THROWS_AS(spool.add(std::move(more), 3), std::logic_error);
            // Both readers read the three rows.
            for (auto* r : {first.get(), second.get()}) {
                auto m = run(r->next(0));
                REQUIRE(m);
                CHECK(m->rows == 3);
            }
        }
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
                spool.add(std::move(cols), 3);
            }
            for (int pass = 0; pass < 2; ++pass) {
                auto back = spool.reverse_reader();
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
        spill::Dir dir;
        const dstd::String path = dir.run_path(0);
        REQUIRE(::mkfifo(path.c_str(), 0600) == 0);

        // One worker: the open of a FIFO blocks until a writer appears, so the
        // second task runs only if the open handed its permit away.
        Runtime rt(1);
        std::atomic<bool> opened{false};
        std::atomic<bool> other_ran{false};
        const auto open_task = [&]() -> dftracer::utils::coro::CoroTask<void> {
            spill::Reader r(path);
            opened.store(true);
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
