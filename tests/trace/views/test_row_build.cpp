#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/arg_type.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::StringIntern;
using dftracer::utils::coro::Channel;
using dftracer::utils::coro::CoroSemaphore;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::trace::RecordPhase;
using dftracer::utils::trace::views::detail::ArgKinds;
using dftracer::utils::trace::views::detail::ArgType;
using dftracer::utils::trace::views::detail::build_row_frame;
using dftracer::utils::trace::views::detail::FoldBatch;
using dftracer::utils::trace::views::detail::FoldEvent;
using dftracer::utils::trace::views::detail::ScanUnit;
using dftracer::utils::trace::views::detail::StreamRowFold;

namespace df = dftracer::utils::dataframe;

namespace {

constexpr int EVENTS = 40;

std::vector<FoldEvent> make_events(StringIntern& intern) {
    const auto size = intern.get_or_insert("size");
    const auto lat = intern.get_or_insert("lat");
    const auto op = intern.get_or_insert("op");
    const auto tag = intern.get_or_insert("tag");
    std::vector<FoldEvent> out;
    for (int i = 0; i < EVENTS; ++i) {
        FoldEvent ev;
        ev.phase = RecordPhase::COMPLETE;
        ev.by_path = true;
        ev.args.emplace_back(size, static_cast<std::int64_t>(i * 10));
        if (i % 3 != 0) ev.args.emplace_back(lat, i * 0.25);
        ev.args.emplace_back(op,
                             intern.get_or_insert(i % 2 ? "read" : "write"));
        if (i % 5 == 0)
            ev.args.emplace_back(tag, static_cast<std::uint64_t>(1) << 63);
        out.push_back(std::move(ev));
    }
    return out;
}

struct Built {
    std::vector<std::string> names;
    std::vector<df::Series> columns;
};

CoroTask<Built> run_step(StringIntern& intern_ref,
                         std::shared_ptr<StringIntern> intern,
                         std::vector<FoldEvent> events) {
    (void)intern_ref;
    auto channel = std::make_shared<Channel<df::Morsel>>(4);
    auto budget = std::make_shared<CoroSemaphore>(1ull << 30);
    StreamRowFold fold(channel, budget, intern, {}, 1.0, false, false, nullptr,
                       nullptr, true);
    ScanUnit unit;
    FoldBatch batch{std::span<const FoldEvent>(events), unit};
    fold.step(batch);
    if (auto* t = fold.take_pending())
        co_await *reinterpret_cast<CoroTask<void>*>(t);
    df::Morsel m;
    Built out;
    if (!channel->try_receive(m)) co_return out;
    for (const auto id : m.name_ids())
        out.names.emplace_back(intern->resolve(id));
    out.columns = std::move(m.columns);
    co_return out;
}

Built stepped(const std::shared_ptr<StringIntern>& intern,
              std::vector<FoldEvent> events) {
    return dftracer::utils::default_runtime()
        .submit(run_step(*intern, intern, std::move(events)))
        .get();
}

std::vector<FoldEvent> copy_events(const std::vector<FoldEvent>& events) {
    return events;
}

void check_equal(const Built& got, const df::DataFrame& want) {
    REQUIRE(got.names == want.names);
    REQUIRE(got.columns.size() == want.columns.size());
    for (std::size_t c = 0; c < want.columns.size(); ++c) {
        CAPTURE(want.names[c]);
        const df::Series& g = got.columns[c];
        const df::Series& w = want.columns[c];
        REQUIRE(g.type() == w.type());
        CHECK(g.encoding() == w.encoding());
        REQUIRE(g.length() == w.length());
        for (std::int64_t i = 0; i < w.length(); ++i) {
            REQUIRE(g.is_null(i) == w.is_null(i));
            if (w.is_null(i)) continue;
            if (w.type() == df::TypeId::Int64)
                CHECK(g.values<std::int64_t>()[i] ==
                      w.values<std::int64_t>()[i]);
            else if (w.type() == df::TypeId::Uint64)
                CHECK(g.values<std::uint64_t>()[i] ==
                      w.values<std::uint64_t>()[i]);
            else if (w.type() == df::TypeId::Float64)
                CHECK(g.values<double>()[i] == w.values<double>()[i]);
            else
                CHECK(g.string_at(i) == w.string_at(i));
        }
    }
}

}  // namespace

TEST_SUITE("RowBuild") {
    TEST_CASE("a batch whose events are all kept builds the copied frame") {
        auto intern = std::make_shared<StringIntern>();
        const auto events = make_events(*intern);
        const df::DataFrame want = build_row_frame(copy_events(events), *intern,
                                                   {}, 1.0, /*by_path=*/true);
        REQUIRE(want.columns.size() == 4);
        check_equal(stepped(intern, copy_events(events)), want);
    }

    TEST_CASE("a filtered batch builds exactly the kept events") {
        auto intern = std::make_shared<StringIntern>();
        auto events = make_events(*intern);
        std::vector<FoldEvent> kept;
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (i % 4 == 1) {
                events[i].phase = RecordPhase::UNKNOWN;
            } else {
                kept.push_back(events[i]);
            }
        }
        const df::DataFrame want =
            build_row_frame(kept, *intern, {}, 1.0, /*by_path=*/true);
        const Built got = stepped(intern, std::move(events));
        REQUIRE(!got.columns.empty());
        CHECK(got.columns.front().length() ==
              static_cast<std::int64_t>(kept.size()));
        check_equal(got, want);
    }

    TEST_CASE("a batch with no kept event sends nothing") {
        auto intern = std::make_shared<StringIntern>();
        auto events = make_events(*intern);
        for (auto& ev : events) ev.phase = RecordPhase::UNKNOWN;
        CHECK(stepped(intern, std::move(events)).columns.empty());
    }

    TEST_CASE("the value-kind mask rule gives the sequential rule's type") {
        const std::vector<FoldEvent::ArgValue> alphabet = {
            1.5, std::int64_t{-3}, std::int64_t{7}, std::uint64_t{1} << 63,
            std::uint32_t{5}};
        std::size_t checked = 0;
        for (std::size_t length = 1; length <= 6; ++length) {
            std::size_t total = 1;
            for (std::size_t i = 0; i < length; ++i) total *= alphabet.size();
            for (std::size_t code = 0; code < total; ++code) {
                ArgType sequential;
                ArgKinds kinds;
                std::size_t rest = code;
                for (std::size_t i = 0; i < length; ++i) {
                    const auto& v = alphabet[rest % alphabet.size()];
                    rest /= alphabet.size();
                    sequential.see(v);
                    kinds.add(v);
                }
                REQUIRE(kinds.kind() == sequential.kind);
                ++checked;
            }
        }
        CHECK(checked == 5 + 25 + 125 + 625 + 3125 + 15625);
    }
}
