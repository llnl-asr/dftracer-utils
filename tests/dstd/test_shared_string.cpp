// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>

#include <atomic>
#include <type_traits>
#include <utility>

using namespace dftracer::utils;

namespace {
memory::Pool& root() { return memory::detail::Manager::get().root(); }
}  // namespace

static_assert(sizeof(dstd::SharedString) == sizeof(void*));
static_assert(std::is_nothrow_move_constructible_v<dstd::SharedString>);
static_assert(std::is_nothrow_copy_constructible_v<dstd::SharedString>);

TEST_CASE("the empty state allocates nothing") {
    memory::PoolHandle pool = root().child("shared_string_empty");
    memory::detail::Scope s{*pool};
    dstd::SharedString none;
    dstd::SharedString from_empty{dstd::StringView{}};
    dstd::SharedString copy = none;
    CHECK(none.empty());
    CHECK(from_empty.empty());
    CHECK(copy.size() == 0);
    CHECK(none.data() == nullptr);
    CHECK(none == from_empty);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("construct, copy and move") {
    memory::PoolHandle pool = root().child("shared_string_copy");
    memory::detail::Scope s{*pool};
    {
        dstd::SharedString a{dstd::StringView{"duration"}};
        CHECK(a.size() == 8);
        CHECK(a.view() == "duration");
        CHECK(pool->blocks() == 1);

        dstd::SharedString b = a;
        CHECK(b.data() == a.data());
        CHECK(pool->blocks() == 1);

        dstd::SharedString c = std::move(a);
        CHECK(a.empty());
        CHECK(c.data() == b.data());

        b = dstd::SharedString{};
        CHECK(c == "duration");
        CHECK(pool->blocks() == 1);
    }
    CHECK(pool->blocks() == 0);
    CHECK(pool->live() == 0);
}

TEST_CASE("equality with views, C strings and strings") {
    dstd::SharedString a{dstd::StringView{"abc"}};
    dstd::SharedString b{dstd::StringView{"abc"}};
    CHECK(a == b);
    CHECK(a == dstd::StringView{"abc"});
    CHECK(a == "abc");
    CHECK(a == dstd::String{"abc"});
    CHECK(a != dstd::StringView{"abd"});
    CHECK(a != dstd::SharedString{});
    CHECK(dstd::SharedString{} == dstd::StringView{});
}

TEST_CASE("a HashMap keyed by SharedString finds by view") {
    dstd::HashMap<dstd::SharedString, int> m;
    m.emplace(dstd::SharedString{dstd::StringView{"one"}}, 1);
    m.emplace(dstd::SharedString{dstd::StringView{"two"}}, 2);
    auto it = m.find(dstd::StringView{"two"});
    REQUIRE(it != m.end());
    CHECK(it->second == 2);
    CHECK(m.find(dstd::StringView{"three"}) == m.end());
}

TEST_CASE("copies on several threads end with no block left") {
    memory::PoolHandle pool = root().child("shared_string_threads");
    memory::detail::Scope s{*pool};
    {
        const dstd::SharedString shared{
            dstd::StringView{"shared across threads"}};
        std::atomic<int> bad{0};
        {
            dstd::Vector<dstd::Thread> threads;
            for (int t = 0; t < 8; ++t) {
                threads.emplace_back([&shared, &bad] {
                    for (int i = 0; i < 20000; ++i) {
                        dstd::SharedString copy = shared;
                        dstd::SharedString again = copy;
                        if (again.data() != shared.data() ||
                            again !=
                                dstd::StringView{"shared across threads"}) {
                            bad.fetch_add(1);
                        }
                    }
                });
            }
        }
        CHECK(bad.load() == 0);
        CHECK(pool->blocks() == 1);
    }
    CHECK(pool->blocks() == 0);
    CHECK(pool->live() == 0);
}
