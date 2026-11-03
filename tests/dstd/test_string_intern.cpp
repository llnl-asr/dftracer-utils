// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;

using Status = dstd::StringIntern::InsertStatus;

TEST_SUITE("StringIntern") {
    TEST_CASE("StringIntern - a table of a few strings costs a few KB") {
        // A build makes one table per file, so a fixed cost per table grows
        // with the file count.
        memory::detail::PoolHandle pool =
            memory::detail::Manager::get().general().child("intern size");
        {
            dstd::StringIntern intern(pool.get());
            for (int i = 0; i < 100; ++i)
                intern.get_or_insert("s" + dstd::to_string(i));
            MESSAGE("bytes for 100 strings: " << pool->live());
            CHECK(pool->live() < 128 * 1024);
        }
        CHECK(pool->live() == 0);
    }

    TEST_CASE("StringIntern - round trips every string it is given") {
        dstd::StringIntern intern;
        dstd::Vector<std::uint32_t> ids;
        constexpr int N = 200000;
        for (int i = 0; i < N; ++i)
            ids.push_back(intern.get_or_insert("value_" + dstd::to_string(i)));
        for (int i = 0; i < N; ++i) {
            CHECK(intern.resolve(ids[i]) == "value_" + dstd::to_string(i));
        }
        CHECK(intern.get_or_insert("value_7") == ids[7]);
    }

    TEST_CASE("StringIntern - find gives the id of a held string only") {
        dstd::StringIntern intern;
        CHECK(intern.find("dur") == dstd::StringIntern::NO_ID);
        const auto dur = intern.get_or_insert("dur");
        const auto size = intern.get_or_insert("size");
        CHECK(intern.find("dur") == dur);
        CHECK(intern.find("size") == size);
        CHECK(intern.find("du") == dstd::StringIntern::NO_ID);
        CHECK(intern.find("dur_sum") == dstd::StringIntern::NO_ID);
        CHECK(intern.find("") == dstd::StringIntern::NO_ID);
        CHECK(intern.get_or_insert("dur") == dur);
    }

    TEST_CASE("StringIntern - ids beyond the table are reported, not empty") {
        dstd::StringIntern intern;
        auto id = intern.get_or_insert("a");
        CHECK(intern.resolve(id) == "a");
        // A free slot inside the table is a genuine absent string.
        CHECK(intern.resolve(id + 1).empty());
        CHECK_THROWS_AS(intern.resolve(dstd::StringIntern::FAST_CAPACITY),
                        std::out_of_range);
    }

    TEST_CASE("StringIntern - insert_at_id preserves loaded ids") {
        dstd::StringIntern intern;
        CHECK(intern.insert_at_id(5000000, "loaded") == Status::Ok);
        CHECK(intern.resolve(5000000) == "loaded");
        CHECK(intern.get_or_insert("loaded") == 5000000);
        CHECK(intern.insert_at_id(5000000, "loaded") == Status::Ok);
        CHECK(intern.resolve(5000000) == "loaded");
        // Ids are index-local, so a second dictionary claiming the same id
        // must not silently take over or be silently dropped.
        CHECK(intern.insert_at_id(5000000, "other") == Status::Conflict);
        CHECK(intern.resolve(5000000) == "loaded");
    }

    TEST_CASE("StringIntern - insert_at_id reports an id past the table") {
        dstd::StringIntern intern;
        CHECK(intern.insert_at_id(dstd::StringIntern::FAST_CAPACITY, "x") ==
              Status::OutOfRange);
        CHECK(intern.size() == 0u);
    }

    TEST_CASE("StringIntern - new strings never reuse a loaded id") {
        dstd::StringIntern intern;
        CHECK(intern.insert_at_id(42, "loaded") == Status::Ok);
        for (int i = 0; i < 100; ++i) {
            auto id = intern.get_or_insert("fresh_" + dstd::to_string(i));
            CHECK(id != 42u);
            CHECK(intern.resolve(id) == "fresh_" + dstd::to_string(i));
        }
        CHECK(intern.resolve(42) == "loaded");
    }

    TEST_CASE("StringIntern - deterministic ids are content derived") {
        dstd::StringIntern a, b;
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
        dstd::StringIntern intern;
        intern.enable_deterministic_ids();
        dstd::Vector<dstd::String> inserted;
        for (int i = 0; i < 1000; ++i) {
            inserted.push_back("value_" + dstd::to_string(i));
            intern.get_or_insert(inserted.back());
        }
        intern.get_or_insert("value_0");
        REQUIRE(intern.entry_count() == inserted.size());
        for (std::size_t n = 0; n < intern.entry_count(); ++n) {
            CHECK(intern.resolve(intern.entry_id(n)) == inserted[n]);
        }
    }

    TEST_CASE("StringIntern - concurrent inserts all resolve") {
        dstd::StringIntern intern;
        constexpr int THREADS = 8;
        constexpr int PER_THREAD = 20000;
        dstd::Vector<dstd::Thread> threads;
        dstd::Vector<dstd::Vector<std::uint32_t>> ids(THREADS);
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < PER_THREAD; ++i)
                    ids[t].push_back(intern.get_or_insert(
                        dstd::to_string(t) + "_" + dstd::to_string(i)));
            });
        }
        for (auto& th : threads) th.join();
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < PER_THREAD; ++i) {
                CHECK(intern.resolve(ids[t][i]) ==
                      dstd::to_string(t) + "_" + dstd::to_string(i));
            }
        }
    }

    TEST_CASE("StringIntern - a chunk outlives the intern") {
        dstd::SharedPtr<dstd::StringIntern::Chunk> chunk;
        std::uint32_t len, off;
        {
            dstd::StringIntern intern;
            auto id = intern.get_or_insert("kept alive");
            auto loc = intern.locate(id);
            REQUIRE(loc.data != nullptr);
            chunk = intern.chunk(loc.chunk);
            len = loc.len;
            off = loc.offset;
        }
        CHECK(dstd::StringView(chunk->data() + off, len) == "kept alive");
    }

    TEST_CASE("StringIntern - locate agrees with resolve under threads") {
        dstd::StringIntern intern;
        constexpr int THREADS = 8;
        constexpr int PER_THREAD = 100000;
        dstd::Vector<dstd::Thread> threads;
        dstd::Vector<dstd::Vector<std::uint32_t>> ids(THREADS);
        auto name = [](int t, int i) {
            return "s" + dstd::to_string((t % 2) * 1000000 + i);
        };
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < PER_THREAD; ++i) {
                    auto id = intern.get_or_insert(name(t, i));
                    ids[t].push_back(id);
                    auto loc = intern.locate(id);
                    if (dstd::StringView(loc.data, loc.len) != name(t, i))
                        ids[t].push_back(dstd::StringIntern::NO_ID);
                }
            });
        }
        for (auto& th : threads) th.join();
        for (int t = 0; t < THREADS; ++t) {
            std::size_t k = 0;
            for (int i = 0; i < PER_THREAD; ++i) {
                auto id = ids[t][k++];
                REQUIRE(id != dstd::StringIntern::NO_ID);
                auto loc = intern.locate(id);
                CHECK(dstd::StringView(loc.data, loc.len) == name(t, i));
                CHECK(intern.resolve(id) == name(t, i));
                auto c = intern.chunk(loc.chunk);
                CHECK(c->data() + loc.offset == loc.data);
            }
        }
        CHECK(intern.size() == 2u * PER_THREAD);
    }

    TEST_CASE("StringIntern - a string over a chunk gets its own chunk") {
        dstd::StringIntern intern;
        auto small = intern.get_or_insert("small");
        dstd::String big(dstd::StringIntern::CHUNK_BYTES + 1000, 'x');
        big[12345] = 'y';
        auto big_id = intern.get_or_insert(big);
        auto after = intern.get_or_insert("after");
        auto lb = intern.locate(big_id);
        CHECK(lb.len == big.size());
        CHECK(lb.offset == 8);
        CHECK(dstd::StringView(lb.data, lb.len) == big);
        CHECK(intern.chunk(lb.chunk)->size() >= big.size());
        CHECK(lb.chunk != intern.locate(small).chunk);
        CHECK(intern.resolve(after) == "after");
        CHECK(intern.resolve(small) == "small");
        CHECK(intern.get_or_insert(big) == big_id);
    }

    TEST_CASE("StringIntern - records stay inside their chunk") {
        dstd::StringIntern intern;
        dstd::String v(1000, 'a');
        dstd::Vector<std::uint32_t> ids;
        for (int i = 0; i < 3000; ++i) {
            v[0] = static_cast<char>('a' + i % 26);
            v[1] = static_cast<char>('a' + (i / 26) % 26);
            v[2] = static_cast<char>('a' + (i / 676) % 26);
            ids.push_back(intern.get_or_insert(v));
        }
        std::uint32_t max_chunk = 0;
        for (auto id : ids) {
            auto loc = intern.locate(id);
            CHECK(loc.offset + loc.len <= intern.chunk(loc.chunk)->size());
            max_chunk = std::max(max_chunk, loc.chunk);
        }
        CHECK(max_chunk >= 2);
    }

    TEST_CASE("StringIntern - overlapping concurrent inserts agree on ids") {
        dstd::StringIntern intern;
        constexpr int THREADS = 14;
        constexpr int PER_THREAD = 100000;
        constexpr int STRIDE = 5000;
        auto name = [](int k) { return "overlap_" + dstd::to_string(k); };
        dstd::Vector<dstd::Thread> threads;
        dstd::Vector<dstd::Vector<std::uint32_t>> ids(THREADS);
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                ids[t].reserve(PER_THREAD);
                for (int i = 0; i < PER_THREAD; ++i)
                    ids[t].push_back(
                        intern.get_or_insert(name(t * STRIDE + i)));
            });
        }
        for (auto& th : threads) th.join();

        dstd::Map<int, std::uint32_t> id_of;
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < PER_THREAD; ++i) {
                const int k = t * STRIDE + i;
                const auto [it, fresh] = id_of.emplace(k, ids[t][i]);
                if (!fresh) REQUIRE(it->second == ids[t][i]);
                REQUIRE(intern.resolve(ids[t][i]) == name(k));
            }
        }
        dstd::Set<std::uint32_t> distinct_ids;
        for (const auto& [k, id] : id_of) distinct_ids.insert(id);
        CHECK(distinct_ids.size() == id_of.size());
        CHECK(intern.entry_count() == id_of.size());
        CHECK(intern.size() == id_of.size());

        dstd::Set<std::uint32_t> logged;
        for (std::size_t n = 0; n < intern.entry_count(); ++n)
            logged.insert(intern.entry_id(n));
        CHECK(logged == distinct_ids);
    }

    TEST_CASE("StringIntern - deterministic ids keep their fixed values") {
        dstd::StringIntern intern;
        intern.enable_deterministic_ids();
        const dstd::Pair<const char*, std::uint32_t> expected[] = {
            {"hello", 207805693},
            {"world", 138242494},
            {"read", 32689650},
            {"write", 9196506},
            {"args.size", 263050526},
            {"cat", 18633966},
            {"POSIX", 209986609},
            {"/UqBWr9S8D8AAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAAAAAAAA"
             "AAA",
             259052688},
        };
        for (const auto& [text, id] : expected) {
            CHECK(intern.get_or_insert(text) == id);
            CHECK(intern.resolve(id) == text);
        }
    }

    TEST_CASE("StringIntern - deterministic ids agree under threads") {
        dstd::StringIntern intern;
        intern.enable_deterministic_ids();
        dstd::StringIntern serial;
        serial.enable_deterministic_ids();
        constexpr int N = 20000;
        dstd::Vector<dstd::Thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < N; ++i)
                    intern.get_or_insert("det_" + dstd::to_string(i));
            });
        }
        for (auto& th : threads) th.join();
        for (int i = 0; i < N; ++i) {
            const auto key = "det_" + dstd::to_string(i);
            CHECK(intern.get_or_insert(key) == serial.get_or_insert(key));
        }
        CHECK(intern.entry_count() == static_cast<std::size_t>(N));
    }

    TEST_CASE("StringIntern - a deterministic id collision throws") {
        dstd::StringIntern intern;
        intern.enable_deterministic_ids();
        dstd::Vector<dstd::String> kept;
        bool threw = false;
        for (int i = 0; i < 2000000 && !threw; ++i) {
            const auto key = "collide_" + dstd::to_string(i);
            try {
                intern.get_or_insert(key);
                kept.push_back(key);
            } catch (const std::logic_error&) {
                threw = true;
            }
        }
        REQUIRE(threw);
        for (const auto& key : kept)
            CHECK(intern.resolve(intern.get_or_insert(key)) == key);
        CHECK(intern.entry_count() == kept.size());
    }

    TEST_CASE("StringIntern - insert_at_id aliases a known string") {
        dstd::StringIntern intern;
        const auto first = intern.get_or_insert("shared");
        CHECK(intern.insert_at_id(first + 7, "shared") == Status::Ok);
        CHECK(intern.resolve(first) == "shared");
        CHECK(intern.resolve(first + 7) == "shared");
        CHECK(intern.get_or_insert("shared") == first);
        CHECK(intern.size() == first + 8);
        const auto next = intern.get_or_insert("after the loaded id");
        CHECK(next != first);
        CHECK(next != first + 7);
        CHECK(intern.resolve(next) == "after the loaded id");
    }

    TEST_CASE("StringIntern - the log is in insertion order") {
        dstd::StringIntern intern;
        dstd::Vector<std::uint32_t> ids;
        for (int i = 0; i < 5000; ++i)
            ids.push_back(
                intern.get_or_insert("ordered_" + dstd::to_string(i)));
        intern.get_or_insert("ordered_3");
        REQUIRE(intern.entry_count() == ids.size());
        for (std::size_t n = 0; n < ids.size(); ++n)
            CHECK(intern.entry_id(n) == ids[n]);
    }

    TEST_CASE("StringIntern - readers see every logged entry resolve") {
        dstd::StringIntern intern;
        std::atomic<bool> done{false};
        std::atomic<int> bad{0};
        dstd::Thread reader([&] {
            while (!done.load()) {
                const auto count = intern.entry_count();
                for (std::size_t n = 0; n < count; ++n)
                    if (intern.resolve(intern.entry_id(n)).empty()) ++bad;
            }
        });
        dstd::Vector<dstd::Thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < 5000; ++i)
                    intern.get_or_insert(dstd::to_string(t) + "_w_" +
                                         dstd::to_string(i));
            });
        }
        for (auto& w : writers) w.join();
        done = true;
        reader.join();
        CHECK(bad.load() == 0);
        CHECK(intern.entry_count() == 20000u);
    }
}
