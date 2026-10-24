#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/catalog_fold.h>
#include <dftracer/utils/index/store/index_database.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace store = dftracer::utils::index::store;
namespace views = dftracer::utils::trace::views::detail;
using index::extensions::BloomFold;

namespace {

using Leaves = std::vector<std::pair<std::uint32_t, std::uint8_t>>;

constexpr auto INT = static_cast<std::uint8_t>(store::PathType::Int);
constexpr auto STR = static_cast<std::uint8_t>(store::PathType::String);
constexpr auto NUL = static_cast<std::uint8_t>(store::PathType::NullValue);

struct Fixture {
    dstd::StringIntern intern;
    std::vector<Leaves> shapes;
    std::vector<std::size_t> order;

    Fixture() {
        auto id = [&](const char* p) { return intern.get_or_insert(p); };
        shapes = {
            {{id("args.size"), INT}, {id("args.io.off"), INT}},
            {{id("args.size"), INT}, {id("args.io.off"), STR}},
            {{id("args.size"), INT},
             {id("args.retry"), NUL},
             {id("args.hosts.0"), STR}},
            {{id("args.size"), INT}},
        };
        for (std::size_t i = 0; i < 4000; ++i)
            order.push_back((i % 7 == 0 ? i / 7 : i / 50) % shapes.size());
    }

    std::vector<views::FoldEvent> events(std::size_t from,
                                         std::size_t to) const {
        std::vector<views::FoldEvent> out(to - from);
        for (std::size_t i = from; i < to; ++i) {
            out[i - from].phase = trace::RecordPhase::Unknown;
            out[i - from].schema_leaves = shapes[order[i]];
        }
        return out;
    }

    std::map<std::string, store::PathStat> expected() const {
        std::map<std::uint32_t, store::PathStat> by_id;
        for (std::size_t s : order)
            for (const auto& [id, tag] : shapes[s])
                index::extensions::observe_catalog_path(by_id[id], tag);
        std::map<std::string, store::PathStat> out;
        for (const auto& [id, stat] : by_id)
            out.emplace(std::string(intern.resolve(id)), stat);
        return out;
    }

    std::set<std::pair<std::uint32_t, std::uint8_t>> distinct_pairs() const {
        std::set<std::pair<std::uint32_t, std::uint8_t>> out;
        for (const auto& s : shapes) out.insert(s.begin(), s.end());
        return out;
    }
};

void feed(BloomFold& fold, const std::vector<views::FoldEvent>& events,
          const views::ScanUnit& unit) {
    fold.step(views::FoldBatch{events, unit});
}

std::map<std::string, store::PathStat> catalog_of(BloomFold& fold,
                                                  const std::string& file) {
    std::map<std::string, store::PathStat> out;
    for (const auto& [path, stat] : fold.file_catalog(file))
        out.emplace(std::string(path), stat);
    return out;
}

void check_same(const std::map<std::string, store::PathStat>& got,
                const std::map<std::string, store::PathStat>& want) {
    REQUIRE(got.size() == want.size());
    for (const auto& [path, stat] : want) {
        CAPTURE(path);
        auto it = got.find(path);
        REQUIRE(it != got.end());
        CHECK(it->second.type == stat.type);
        CHECK(it->second.seen == stat.seen);
        CHECK(it->second.count == stat.count);
    }
}

views::ScanUnit unit_of(std::uint64_t checkpoint) {
    views::ScanUnit u;
    u.file_path = "/t/a.pfw.gz";
    u.index_path = "/t/a.idx";
    u.checkpoint_idx = checkpoint;
    return u;
}

}  // namespace

TEST_CASE("catalog observations follow the distinct pairs, not the events") {
    Fixture fx;
    BloomFold fold(fx.intern);
    const std::size_t half = fx.order.size() / 2;
    feed(fold, fx.events(0, half), unit_of(0));
    feed(fold, fx.events(half, fx.order.size()), unit_of(1));

    check_same(catalog_of(fold, "/t/a.pfw.gz"), fx.expected());
    CHECK(fold.catalog_observations() <= fx.distinct_pairs().size());
    CHECK(fold.catalog_observations() > 0);
}

TEST_CASE("merged slices give the catalog of one fold") {
    Fixture fx;
    BloomFold a(fx.intern);
    auto b = a.slice();
    const std::size_t third = fx.order.size() / 3;
    feed(a, fx.events(0, third), unit_of(0));
    feed(static_cast<BloomFold&>(*b), fx.events(third, fx.order.size()),
         unit_of(1));
    a.merge(*b);

    check_same(catalog_of(a, "/t/a.pfw.gz"), fx.expected());
}
