// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/catalog_fold.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

using Leaves = dstd::Vector<dstd::Pair<std::uint32_t, std::uint8_t>>;

constexpr auto INT = static_cast<std::uint8_t>(index::store::PathType::Int);
constexpr auto STR = static_cast<std::uint8_t>(index::store::PathType::String);
constexpr auto NUL =
    static_cast<std::uint8_t>(index::store::PathType::NullValue);

struct Fixture {
    dstd::StringIntern intern;
    dstd::Vector<Leaves> shapes;
    dstd::Vector<std::size_t> order;

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

    dstd::Vector<trace::views::detail::FoldEvent> events(std::size_t from,
                                                         std::size_t to) const {
        dstd::Vector<trace::views::detail::FoldEvent> out(to - from);
        for (std::size_t i = from; i < to; ++i) {
            out[i - from].phase = trace::RecordPhase::Unknown;
            out[i - from].schema_leaves.assign(shapes[order[i]].begin(),
                                               shapes[order[i]].end());
        }
        return out;
    }

    dstd::Map<dstd::String, index::store::PathStat> expected() const {
        dstd::Map<std::uint32_t, index::store::PathStat> by_id;
        for (std::size_t s : order)
            for (const auto& [id, tag] : shapes[s])
                index::extensions::observe_catalog_path(by_id[id], tag);
        dstd::Map<dstd::String, index::store::PathStat> out;
        for (const auto& [id, stat] : by_id)
            out.emplace(dstd::String(intern.resolve(id)), stat);
        return out;
    }

    dstd::Set<dstd::Pair<std::uint32_t, std::uint8_t>> distinct_pairs() const {
        dstd::Set<dstd::Pair<std::uint32_t, std::uint8_t>> out;
        for (const auto& s : shapes) out.insert(s.begin(), s.end());
        return out;
    }
};

void feed(extensions::BloomFold& fold,
          const dstd::Vector<trace::views::detail::FoldEvent>& events,
          const trace::views::detail::ScanUnit& unit) {
    dftu_utils_test::ok(
        fold.step(trace::views::detail::FoldBatch{events, unit}));
}

dstd::Map<dstd::String, index::store::PathStat> catalog_of(
    extensions::BloomFold& fold, const dstd::String& file) {
    dstd::Map<dstd::String, index::store::PathStat> out;
    for (const auto& [path, stat] : fold.file_catalog(file))
        out.emplace(dstd::String(path), stat);
    return out;
}

void check_same(const dstd::Map<dstd::String, index::store::PathStat>& got,
                const dstd::Map<dstd::String, index::store::PathStat>& want) {
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

trace::views::detail::ScanUnit unit_of(std::uint64_t checkpoint) {
    trace::views::detail::ScanUnit u;
    u.file_path = "/t/a.pfw.gz";
    u.index_path = "/t/a.idx";
    u.checkpoint_idx = checkpoint;
    return u;
}

}  // namespace

TEST_CASE("catalog observations follow the distinct pairs, not the events") {
    Fixture fx;
    extensions::BloomFold fold(fx.intern);
    const std::size_t half = fx.order.size() / 2;
    feed(fold, fx.events(0, half), unit_of(0));
    feed(fold, fx.events(half, fx.order.size()), unit_of(1));

    check_same(catalog_of(fold, "/t/a.pfw.gz"), fx.expected());
    CHECK(fold.catalog_observations() <= fx.distinct_pairs().size());
    CHECK(fold.catalog_observations() > 0);
}

TEST_CASE("merged slices give the catalog of one fold") {
    Fixture fx;
    extensions::BloomFold a(fx.intern);
    auto b = a.slice();
    const std::size_t third = fx.order.size() / 3;
    feed(a, fx.events(0, third), unit_of(0));
    feed(static_cast<extensions::BloomFold&>(*b),
         fx.events(third, fx.order.size()), unit_of(1));
    a.merge(*b);

    check_same(catalog_of(a, "/t/a.pfw.gz"), fx.expected());
}
