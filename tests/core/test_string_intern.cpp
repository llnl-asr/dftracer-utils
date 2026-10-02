#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
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

    TEST_CASE("StringIntern - a chunk outlives the intern") {
        std::shared_ptr<StringIntern::Chunk> chunk;
        std::uint32_t len, off;
        {
            StringIntern intern;
            auto id = intern.get_or_insert("kept alive");
            auto loc = intern.locate(id);
            REQUIRE(loc.data != nullptr);
            chunk = intern.chunk(loc.chunk);
            len = loc.len;
            off = loc.offset;
        }
        CHECK(std::string_view(chunk->data() + off, len) == "kept alive");
    }

    TEST_CASE("StringIntern - locate agrees with resolve under threads") {
        StringIntern intern;
        constexpr int THREADS = 8;
        constexpr int PER_THREAD = 100000;
        std::vector<std::thread> threads;
        std::vector<std::vector<std::uint32_t>> ids(THREADS);
        auto name = [](int t, int i) {
            return "s" + std::to_string((t % 2) * 1000000 + i);
        };
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < PER_THREAD; ++i) {
                    auto id = intern.get_or_insert(name(t, i));
                    ids[t].push_back(id);
                    auto loc = intern.locate(id);
                    if (std::string_view(loc.data, loc.len) != name(t, i))
                        ids[t].push_back(StringIntern::NO_ID);
                }
            });
        }
        for (auto& th : threads) th.join();
        for (int t = 0; t < THREADS; ++t) {
            std::size_t k = 0;
            for (int i = 0; i < PER_THREAD; ++i) {
                auto id = ids[t][k++];
                REQUIRE(id != StringIntern::NO_ID);
                auto loc = intern.locate(id);
                CHECK(std::string_view(loc.data, loc.len) == name(t, i));
                CHECK(intern.resolve(id) == name(t, i));
                auto c = intern.chunk(loc.chunk);
                CHECK(c->data() + loc.offset == loc.data);
            }
        }
        CHECK(intern.size() == 2u * PER_THREAD);
    }

    TEST_CASE("StringIntern - a string over a chunk gets its own chunk") {
        StringIntern intern;
        auto small = intern.get_or_insert("small");
        std::string big(StringIntern::CHUNK_BYTES + 1000, 'x');
        big[12345] = 'y';
        auto big_id = intern.get_or_insert(big);
        auto after = intern.get_or_insert("after");
        auto lb = intern.locate(big_id);
        CHECK(lb.len == big.size());
        CHECK(lb.offset == 8);
        CHECK(std::string_view(lb.data, lb.len) == big);
        CHECK(intern.chunk(lb.chunk)->size() >= big.size());
        CHECK(lb.chunk != intern.locate(small).chunk);
        CHECK(intern.resolve(after) == "after");
        CHECK(intern.resolve(small) == "small");
        CHECK(intern.get_or_insert(big) == big_id);
    }

    TEST_CASE("StringIntern - records stay inside their chunk") {
        StringIntern intern;
        std::string v(1000, 'a');
        std::vector<std::uint32_t> ids;
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
        StringIntern intern;
        constexpr int THREADS = 14;
        constexpr int PER_THREAD = 100000;
        constexpr int STRIDE = 5000;
        auto name = [](int k) { return "overlap_" + std::to_string(k); };
        std::vector<std::thread> threads;
        std::vector<std::vector<std::uint32_t>> ids(THREADS);
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&, t] {
                ids[t].reserve(PER_THREAD);
                for (int i = 0; i < PER_THREAD; ++i)
                    ids[t].push_back(
                        intern.get_or_insert(name(t * STRIDE + i)));
            });
        }
        for (auto& th : threads) th.join();

        std::map<int, std::uint32_t> id_of;
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < PER_THREAD; ++i) {
                const int k = t * STRIDE + i;
                const auto [it, fresh] = id_of.emplace(k, ids[t][i]);
                if (!fresh) REQUIRE(it->second == ids[t][i]);
                REQUIRE(intern.resolve(ids[t][i]) == name(k));
            }
        }
        std::set<std::uint32_t> distinct_ids;
        for (const auto& [k, id] : id_of) distinct_ids.insert(id);
        CHECK(distinct_ids.size() == id_of.size());
        CHECK(intern.entry_count() == id_of.size());
        CHECK(intern.size() == id_of.size());

        std::set<std::uint32_t> logged;
        for (std::size_t n = 0; n < intern.entry_count(); ++n)
            logged.insert(intern.entry_id(n));
        CHECK(logged == distinct_ids);
    }

    TEST_CASE("StringIntern - deterministic ids keep their fixed values") {
        StringIntern intern;
        intern.enable_deterministic_ids();
        const std::pair<const char*, std::uint32_t> expected[] = {
            {"hello", 229782084},
            {"world", 62348693},
            {"read", 100861361},
            {"write", 33496659},
            {"args.size", 97378550},
            {"cat", 208581076},
            {"POSIX", 130800590},
            {"/UqBWr9S8D8AAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAAAAAAAAAAA",
             218384236},
        };
        for (const auto& [text, id] : expected) {
            CHECK(intern.get_or_insert(text) == id);
            CHECK(intern.resolve(id) == text);
        }
    }

    TEST_CASE("StringIntern - deterministic ids agree under threads") {
        StringIntern intern;
        intern.enable_deterministic_ids();
        StringIntern serial;
        serial.enable_deterministic_ids();
        constexpr int N = 20000;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < N; ++i)
                    intern.get_or_insert("det_" + std::to_string(i));
            });
        }
        for (auto& th : threads) th.join();
        for (int i = 0; i < N; ++i) {
            const auto key = "det_" + std::to_string(i);
            CHECK(intern.get_or_insert(key) == serial.get_or_insert(key));
        }
        CHECK(intern.entry_count() == static_cast<std::size_t>(N));
    }

    TEST_CASE("StringIntern - a deterministic id collision throws") {
        StringIntern intern;
        intern.enable_deterministic_ids();
        std::vector<std::string> kept;
        bool threw = false;
        for (int i = 0; i < 2000000 && !threw; ++i) {
            const auto key = "collide_" + std::to_string(i);
            try {
                intern.get_or_insert(key);
                kept.push_back(key);
            } catch (const DFTUtilsException&) {
                threw = true;
            }
        }
        REQUIRE(threw);
        for (const auto& key : kept)
            CHECK(intern.resolve(intern.get_or_insert(key)) == key);
        CHECK(intern.entry_count() == kept.size());
    }

    TEST_CASE("StringIntern - insert_at_id aliases a known string") {
        StringIntern intern;
        const auto first = intern.get_or_insert("shared");
        intern.insert_at_id(first + 7, "shared");
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
        StringIntern intern;
        std::vector<std::uint32_t> ids;
        for (int i = 0; i < 5000; ++i)
            ids.push_back(intern.get_or_insert("ordered_" + std::to_string(i)));
        intern.get_or_insert("ordered_3");
        REQUIRE(intern.entry_count() == ids.size());
        for (std::size_t n = 0; n < ids.size(); ++n)
            CHECK(intern.entry_id(n) == ids[n]);
    }

    TEST_CASE("StringIntern - readers see every logged entry resolve") {
        StringIntern intern;
        std::atomic<bool> done{false};
        std::atomic<int> bad{0};
        std::thread reader([&] {
            while (!done.load()) {
                const auto count = intern.entry_count();
                for (std::size_t n = 0; n < count; ++n)
                    if (intern.resolve(intern.entry_id(n)).empty()) ++bad;
            }
        });
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t) {
            writers.emplace_back([&, t] {
                for (int i = 0; i < 5000; ++i)
                    intern.get_or_insert(std::to_string(t) + "_w_" +
                                         std::to_string(i));
            });
        }
        for (auto& w : writers) w.join();
        done = true;
        reader.join();
        CHECK(bad.load() == 0);
        CHECK(intern.entry_count() == 20000u);
    }
}
