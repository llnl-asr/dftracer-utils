#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace std::string_view_literals;

namespace {

memory::Pool& root() { return memory::Manager::get().root(); }

// The longest string libc++ and libstdc++ both keep in the object.
constexpr std::size_t SHORT_STRING = 15;

struct Counted {
    explicit Counted(int v) : value(v) { ++alive; }
    Counted(const Counted& other) : value(other.value) { ++alive; }
    ~Counted() { --alive; }
    int value;
    static int alive;
};
int Counted::alive = 0;

struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {
    std::uint64_t extra = 0;
};

struct Throws {
    explicit Throws(bool fail) {
        if (fail) throw std::runtime_error("constructor failed");
    }
};

}  // namespace

static_assert(sizeof(dstd::Vector<int>) == sizeof(std::vector<int>));
static_assert(sizeof(dstd::String) == sizeof(std::string));
static_assert(std::is_same_v<dstd::StringView, std::string_view>);
static_assert(std::is_same_v<dstd::Span<const int>, std::span<const int>>);
static_assert(sizeof(dstd::UniquePtr<int>) == sizeof(int*));
static_assert(
    !std::is_convertible_v<dstd::UniquePtr<Derived>, dstd::UniquePtr<Base>>);

TEST_CASE("Vector copies, moves and grows like std::vector") {
    dstd::Vector<int> v{1, 2, 3};
    for (int i = 4; i <= 1000; ++i) v.push_back(i);
    CHECK(v.size() == 1000);
    CHECK(v.front() == 1);
    CHECK(v.back() == 1000);

    dstd::Vector<int> copy = v;
    CHECK(copy == v);
    copy[0] = -1;
    CHECK(v[0] == 1);

    const int* data = v.data();
    dstd::Vector<int> moved = std::move(v);
    CHECK(moved.data() == data);
    CHECK(moved.size() == 1000);

    moved.resize(10);
    moved.shrink_to_fit();
    CHECK(moved.size() == 10);
    CHECK(moved.back() == 10);

    dstd::Span<const int> view{moved};
    CHECK(view.size() == 10);
    CHECK(view[9] == 10);
}

TEST_CASE("String behaves as std::string and keeps the small buffer") {
    memory::PoolHandle pool = root().child("strings");
    memory::Scope s{*pool};
    {
        dstd::String small(SHORT_STRING, 'x');
        dstd::String copy = small;
        copy += "";
        CHECK(copy == small);
        CHECK(pool->blocks() == 0);

        dstd::String text = "a string that does not fit the small buffer";
        CHECK(pool->blocks() == 1);
        dstd::String copied = text;
        CHECK(copied == text);
        CHECK(pool->blocks() == 2);
        dstd::String moved = std::move(copied);
        CHECK(moved == text);
        CHECK(pool->blocks() == 2);

        std::size_t before = text.size();
        text.append(1000, 'y');
        CHECK(text.size() == before + 1000);
        CHECK(text.find("that") == 9);
        CHECK(dstd::StringView{text}.substr(0, 8) == "a string");
        CHECK(text.compare(0, 1, "a") == 0);
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("HashMap copies, moves, grows and finds like a hash map") {
    dstd::HashMap<std::uint64_t, int> m;
    for (std::uint64_t i = 0; i < 10000; ++i)
        m.emplace(i * 7, static_cast<int>(i));
    CHECK(m.size() == 10000);
    CHECK(m.at(700) == 100);
    CHECK(m.find(701) == m.end());
    CHECK(m.count(14) == 1);

    dstd::HashMap<std::uint64_t, int> copy = m;
    CHECK(copy == m);
    copy[0] = -1;
    CHECK(m[0] == 0);
    dstd::HashMap<std::uint64_t, int> moved = std::move(copy);
    CHECK(moved.size() == 10000);
    CHECK(moved.at(0) == -1);
    CHECK(moved.erase(7) == 1);
    CHECK(moved.size() == 9999);

    dstd::HashSet<int> set;
    for (int i = 0; i < 100; ++i) set.insert(i % 10);
    CHECK(set.size() == 10);
    CHECK(set.contains(9));
    CHECK_FALSE(set.contains(10));
}

TEST_CASE("HashMap with String keys finds by std::string_view") {
    dstd::HashMap<dstd::String, int> m;
    m.emplace("alpha", 1);
    m["a key long enough to leave the small buffer"] = 2;
    CHECK(m.find("alpha"sv)->second == 1);
    CHECK(
        m.find(std::string_view{"a key long enough to leave the small buffer"})
            ->second == 2);
    CHECK(m.contains("alpha"));
    CHECK(m.count("beta"sv) == 0);
    CHECK(m.find("alph"sv) == m.end());

    dstd::HashSet<dstd::String> set{"x", "y"};
    CHECK(set.contains("y"sv));
    CHECK_FALSE(set.contains("z"sv));
}

TEST_CASE("HashMap with const char* keys compares the text") {
    char stored[] = "key";
    char probe[] = "key";
    dstd::HashMap<const char*, int> m;
    m.emplace(stored, 1);
    REQUIRE(m.find(probe) != m.end());
    CHECK(m.find(probe)->second == 1);
    CHECK(m.find("key"sv)->second == 1);
}

namespace {
struct Point {
    int x;
    int y;
    bool operator==(const Point&) const = default;
};
struct PointHash {
    std::size_t operator()(const Point& p) const noexcept {
        return dstd::Hash<int>{}(p.x) ^ dstd::Hash<int>{}(p.y + 1);
    }
};
}  // namespace

TEST_CASE("HashMap takes a hasher for a key type without a Hash") {
    dstd::HashMap<Point, int, PointHash> m;
    m[{1, 2}] = 3;
    CHECK(m.at({1, 2}) == 3);
    CHECK(m.find({2, 1}) == m.end());
}

TEST_CASE("Hash agrees across the string types and avalanches integers") {
    dstd::Hash<dstd::String> strings;
    const char* text = "same text";
    std::size_t h = strings(dstd::String{text});
    CHECK(h == dstd::Hash<std::string_view>{}(std::string_view{text}));
    CHECK(h == dstd::Hash<const char*>{}(text));
    CHECK(h == strings(std::string_view{text}));
    CHECK(strings("other"sv) != h);

    dstd::Hash<std::uint32_t> ints;
    CHECK(ints(1) != ints(2));
    CHECK((ints(1) >> 32) != 0);
    CHECK(dstd::Hash<int>{}(-1) == dstd::Hash<std::int64_t>{}(-1));
}

TEST_CASE("Map and Set keep their order like std::map and std::set") {
    dstd::Map<dstd::String, int> m;
    m["b"] = 2;
    m["a"] = 1;
    m.emplace("c", 3);
    CHECK(m.begin()->first == "a");
    CHECK(m.find("b"sv)->second == 2);
    CHECK(m.lower_bound("bb"sv)->first == "c");

    dstd::Map<dstd::String, int> copy = m;
    CHECK(copy == m);
    dstd::Map<dstd::String, int> moved = std::move(copy);
    CHECK(moved.size() == 3);

    dstd::Set<int> set{5, 1, 3, 1};
    CHECK(set.size() == 3);
    CHECK(*set.begin() == 1);
    CHECK(*set.rbegin() == 5);
    CHECK(set.count(3) == 1);
}

TEST_CASE("Deque grows at both ends like std::deque") {
    dstd::Deque<int> d;
    for (int i = 0; i < 5000; ++i) {
        d.push_back(i);
        d.push_front(-i);
    }
    CHECK(d.size() == 10000);
    CHECK(d.front() == -4999);
    CHECK(d.back() == 4999);
    dstd::Deque<int> copy = d;
    CHECK(copy == d);
    dstd::Deque<int> moved = std::move(copy);
    CHECK(moved[5000] == 0);
    moved.pop_front();
    CHECK(moved.front() == -4998);
}

TEST_CASE("UniquePtr destroys and frees its object") {
    memory::PoolHandle pool = root().child("unique");
    memory::Scope s{*pool};
    {
        dstd::UniquePtr<Counted> p = dstd::make_unique<Counted>(7);
        CHECK(p->value == 7);
        CHECK(Counted::alive == 1);
        CHECK(pool->blocks() == 1);
        dstd::UniquePtr<Counted> q = std::move(p);
        CHECK(p == nullptr);
        CHECK(q->value == 7);
        q.reset();
        CHECK(Counted::alive == 0);
        CHECK(pool->blocks() == 0);

        dstd::UniquePtr<const Counted> c = dstd::make_unique<const Counted>(3);
        CHECK(c->value == 3);
    }
    CHECK(Counted::alive == 0);
    CHECK_THROWS_AS(dstd::make_unique<Throws>(true), std::runtime_error);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("SharedPtr shares one object and frees it with the last owner") {
    memory::PoolHandle pool = root().child("shared");
    memory::Scope s{*pool};
    {
        dstd::SharedPtr<Counted> a = dstd::make_shared<Counted>(5);
        CHECK(pool->blocks() == 1);
        dstd::SharedPtr<Counted> b = a;
        CHECK(a.use_count() == 2);
        CHECK(b->value == 5);
        a.reset();
        CHECK(Counted::alive == 1);
        std::weak_ptr<Counted> weak = b;
        b.reset();
        CHECK(Counted::alive == 0);
        CHECK(weak.expired());
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("every container charges the scope pool and frees to zero") {
    memory::PoolHandle pool = root().child("containers");
    {
        memory::Scope s{*pool};
        dstd::Vector<int> v(1000, 1);
        dstd::String text(1000, 'z');
        dstd::HashMap<dstd::String, int> hm;
        for (int i = 0; i < 100; ++i) {
            hm.emplace(dstd::String(40, static_cast<char>('a' + i % 26)) +
                           std::to_string(i).c_str(),
                       i);
        }
        dstd::HashSet<int> hs{1, 2, 3};
        dstd::Map<int, int> m{{1, 1}, {2, 2}};
        dstd::Set<int> set{1, 2};
        dstd::Deque<int> d(100, 0);
        auto u = dstd::make_unique<int>(1);
        auto sp = dstd::make_shared<int>(2);
        CHECK(pool->live() >= 1000 * sizeof(int) + 1000);
        CHECK(pool->blocks() >= 9);
        CHECK(memory::current_pool().id() == pool->id());
    }
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}
