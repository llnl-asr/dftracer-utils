#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

using dftracer::utils::DFTUtilsException;
using dftracer::utils::StringIntern;

TEST_SUITE("StringIntern") {
    TEST_CASE("StringIntern - round trips every string it is given") {
        StringIntern intern;
        std::vector<std::uint32_t> ids;
        constexpr int N = 200000;
        for (int i = 0; i < N; ++i)
            ids.push_back(intern.get_or_insert("value_" + std::to_string(i)));
        for (int i = 0; i < N; ++i) {
            CHECK(intern.resolve(ids[i]) == "value_" + std::to_string(i));
        }
        CHECK(intern.get_or_insert("value_7") == ids[7]);
    }

    TEST_CASE("StringIntern - ids beyond the table are reported, not empty") {
        StringIntern intern;
        auto id = intern.get_or_insert("a");
        CHECK(intern.resolve(id) == "a");
        // A free slot inside the table is a genuine absent string.
        CHECK(intern.resolve(id + 1).empty());
        CHECK_THROWS_AS(intern.resolve(StringIntern::FAST_CAPACITY),
                        DFTUtilsException);
    }

    TEST_CASE("StringIntern - insert_at_id preserves loaded ids") {
        StringIntern intern;
        intern.insert_at_id(5000000, "loaded");
        CHECK(intern.resolve(5000000) == "loaded");
        CHECK(intern.get_or_insert("loaded") == 5000000);
        intern.insert_at_id(5000000, "loaded");
        CHECK(intern.resolve(5000000) == "loaded");
        // Ids are index-local, so a second dictionary claiming the same id
        // must not silently take over or be silently dropped.
        CHECK_THROWS_AS(intern.insert_at_id(5000000, "other"),
                        DFTUtilsException);
        CHECK(intern.resolve(5000000) == "loaded");
    }

    TEST_CASE("StringIntern - new strings never reuse a loaded id") {
        StringIntern intern;
        intern.insert_at_id(42, "loaded");
        for (int i = 0; i < 100; ++i) {
            auto id = intern.get_or_insert("fresh_" + std::to_string(i));
            CHECK(id != 42u);
            CHECK(intern.resolve(id) == "fresh_" + std::to_string(i));
        }
        CHECK(intern.resolve(42) == "loaded");
    }

    TEST_CASE("StringIntern - deterministic ids are content derived") {
        StringIntern a, b;
        a.enable_deterministic_ids();
        b.enable_deterministic_ids();
        b.get_or_insert("filler");
        for (const char* s : {"read", "write", "POSIX", "cat"}) {
            CHECK(a.get_or_insert(s) == b.get_or_insert(s));
            CHECK(a.resolve(a.get_or_insert(s)) == s);
        }
    }

    TEST_CASE("StringIntern - insertion log enumerates only real entries") {
        // Content-derived ids are sparse over the whole id space, so anything
        // persisting the dictionary must walk the log, not the id range.
        StringIntern intern;
        intern.enable_deterministic_ids();
        std::vector<std::string> inserted;
        for (int i = 0; i < 1000; ++i) {
            inserted.push_back("value_" + std::to_string(i));
            intern.get_or_insert(inserted.back());
        }
        intern.get_or_insert("value_0");
        REQUIRE(intern.entry_count() == inserted.size());
        for (std::size_t n = 0; n < intern.entry_count(); ++n) {
            CHECK(intern.resolve(intern.entry_id(n)) == inserted[n]);
        }
    }

    TEST_CASE("StringIntern - concurrent inserts all resolve") {
        StringIntern intern;
        constexpr int THREADS = 8;
        constexpr int PER_THREAD = 20000;
        std::vector<std::thread> threads;
        std::vector<std::vector<std::uint32_t>> ids(THREADS);
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < PER_THREAD; ++i)
                    ids[t].push_back(intern.get_or_insert(
                        std::to_string(t) + "_" + std::to_string(i)));
            });
        }
        for (auto& th : threads) th.join();
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < PER_THREAD; ++i) {
                CHECK(intern.resolve(ids[t][i]) ==
                      std::to_string(t) + "_" + std::to_string(i));
            }
        }
    }
}
