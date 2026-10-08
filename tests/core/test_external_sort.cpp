#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/external_sort.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using dftracer::utils::ExternalSorter;
using dftracer::utils::SpillFile;

namespace {

struct Rec {
    std::int64_t key;
    std::int64_t seq;
};

struct Less {
    bool operator()(const Rec& a, const Rec& b) const {
        return a.key != b.key ? a.key < b.key : a.seq < b.seq;
    }
};

std::vector<Rec> shuffled(std::size_t n) {
    std::mt19937_64 rng(7);
    std::vector<Rec> v;
    for (std::size_t i = 0; i < n; ++i)
        v.push_back({static_cast<std::int64_t>(rng() % 50),
                     static_cast<std::int64_t>(i)});
    return v;
}

std::vector<Rec> sorted_through(const std::vector<Rec>& in,
                                std::uint64_t budget, std::size_t* runs) {
    ExternalSorter<Rec, Less> s(Less{}, budget);
    for (const Rec& r : in) REQUIRE(s.add(r).has_value());
    std::vector<Rec> out;
    REQUIRE(s.drain([&](const Rec& r) { out.push_back(r); }).has_value());
    if (runs) *runs = s.runs();
    return out;
}

}  // namespace

TEST_CASE("input within the budget is sorted without a spill file") {
    const auto in = shuffled(1000);
    std::size_t runs = 99;
    auto out = sorted_through(in, 1 << 20, &runs);
    CHECK(runs == 0);
    auto want = in;
    std::sort(want.begin(), want.end(), Less{});
    REQUIRE(out.size() == want.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i].seq == want[i].seq);
}

TEST_CASE("input above the budget spills runs and merges to the same order") {
    const auto in = shuffled(100000);
    std::size_t runs = 0;
    auto out = sorted_through(in, 64 * sizeof(Rec) * 16, &runs);
    CHECK(runs > 50);
    auto want = in;
    std::sort(want.begin(), want.end(), Less{});
    REQUIRE(out.size() == want.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i].key == want[i].key);
        REQUIRE(out[i].seq == want[i].seq);
    }
}

TEST_CASE("an empty sorter drains nothing") {
    CHECK(sorted_through({}, 1024, nullptr).empty());
}

TEST_CASE("a spill file returns what was appended at the returned offsets") {
    auto made = SpillFile::create();
    REQUIRE(made.has_value());
    SpillFile& f = **made;
    const char a[] = "abcdef";
    const char b[] = "XYZ";
    const auto oa = *f.append(a, 6);
    const auto ob = *f.append(b, 3);
    char buf[7] = {};
    REQUIRE(f.read(ob, buf, 3).has_value());
    CHECK(std::string(buf, 3) == "XYZ");
    REQUIRE(f.read(oa, buf, 6).has_value());
    CHECK(std::string(buf, 6) == "abcdef");
    CHECK_FALSE(f.read(ob, buf, 7).has_value());
}
