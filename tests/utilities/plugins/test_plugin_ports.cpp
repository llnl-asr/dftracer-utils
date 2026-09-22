// Batch-scoped port bus: the FoldPortBus mechanics plus the through-on_batch
// producer -> consumer handoff wired through real PluginFolds sharing one bus,
// covering the same-batch delivery, the ordering rule, and the per-batch reset.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace views = dftracer::utils::trace::views;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::PluginFold;
using views::detail::FoldBatch;
using views::detail::FoldEvent;
using views::detail::FoldPortBus;
using views::detail::ScanUnit;

namespace {

constexpr const char* PORT_CAP = "com.example.perbatch";

// The consumer records what it read so the test can observe the handoff; a
// single-threaded harness steps one consumer, so a file-scope sink is enough.
std::uint64_t g_consumed = 0;
bool g_present = false;

struct ProducerSlice {
    explicit ProducerSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch& b,
              dftracer::utils::plugins::Host h) {
        std::uint64_t with_dur = 0;
        for (const dftracer::utils::plugins::Event& e : b)
            if (e.has_dur()) ++with_dur;
        h.publish(h.port_key(PORT_CAP), &with_dur, sizeof with_dur);
    }
    void merge(ProducerSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

struct ConsumerSlice {
    explicit ConsumerSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftracer::utils::plugins::Batch&,
              dftracer::utils::plugins::Host h) {
        std::uint32_t len = 0;
        const void* p = h.consume(h.port_key(PORT_CAP), &len);
        g_present = p != nullptr && len == sizeof(std::uint64_t);
        g_consumed = g_present ? *static_cast<const std::uint64_t*>(p) : 0;
    }
    void merge(ConsumerSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

// Owns a plugin plus a PluginFold bound to `bus`, torn down in the right order.
struct FoldHolder {
    dftu_plugin* plugin;
    std::unique_ptr<PluginFold> fold;
    FoldHolder(dftu_plugin* p, StringIntern& intern, FoldPortBus* bus)
        : plugin(p), fold(std::make_unique<PluginFold>(p, intern)) {
        fold->bind_port_bus(bus);
    }
    ~FoldHolder() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::vector<FoldEvent> make_events(std::uint64_t with_dur,
                                   std::uint64_t total) {
    std::vector<FoldEvent> evs(total);
    for (std::uint64_t i = 0; i < total; ++i) {
        // step() drops METADATA/UNKNOWN-phase events before it ever reaches
        // on_batch, and plugins::Event::has_dur() is phase() == Complete;
        // COUNTER survives the filter without counting as "has a duration".
        evs[i].has_dur = i < with_dur;
        evs[i].phase = evs[i].has_dur
                           ? dftracer::utils::trace::RecordPhase::COMPLETE
                           : dftracer::utils::trace::RecordPhase::COUNTER;
    }
    return evs;
}

}  // namespace

TEST_SUITE("FoldPortBus") {
    TEST_CASE("publish then consume round-trips; absent key is NULL") {
        FoldPortBus bus;
        std::uint32_t len = 99;
        CHECK(bus.consume(1, &len) == nullptr);
        CHECK(len == 0);

        std::uint64_t v = 42;
        bus.publish(7, &v, sizeof v);
        const void* p = bus.consume(7, &len);
        REQUIRE(p != nullptr);
        CHECK(len == sizeof v);
        CHECK(*static_cast<const std::uint64_t*>(p) == 42);
        CHECK(bus.consume(8, &len) == nullptr);
    }

    TEST_CASE("a second key coexists and clear resets the bus") {
        FoldPortBus bus;
        std::uint64_t a = 1, b = 2;
        bus.publish(10, &a, sizeof a);
        bus.publish(20, &b, sizeof b);
        std::uint32_t len = 0;
        CHECK(*static_cast<const std::uint64_t*>(bus.consume(10, &len)) == 1);
        CHECK(*static_cast<const std::uint64_t*>(bus.consume(20, &len)) == 2);

        bus.clear();
        CHECK(bus.consume(10, &len) == nullptr);
        CHECK(bus.consume(20, &len) == nullptr);
    }
}

TEST_SUITE("PluginPorts") {
    TEST_CASE("producer before consumer delivers the value for the batch") {
        StringIntern intern;
        FoldPortBus bus;
        FoldHolder prod(
            dftracer::utils::plugins::make_plugin<ProducerSlice>(nullptr),
            intern, &bus);
        FoldHolder cons(
            dftracer::utils::plugins::make_plugin<ConsumerSlice>(nullptr),
            intern, &bus);

        auto evs = make_events(/*with_dur=*/3, /*total=*/5);
        ScanUnit unit;
        FoldBatch fb{std::span<const FoldEvent>(evs), unit};

        g_present = false;
        g_consumed = 0;
        bus.clear();
        prod.fold->step(fb);
        cons.fold->step(fb);
        CHECK(g_present);
        CHECK(g_consumed == 3);
    }

    TEST_CASE("consumer before producer reads NULL (the ordering rule)") {
        StringIntern intern;
        FoldPortBus bus;
        FoldHolder prod(
            dftracer::utils::plugins::make_plugin<ProducerSlice>(nullptr),
            intern, &bus);
        FoldHolder cons(
            dftracer::utils::plugins::make_plugin<ConsumerSlice>(nullptr),
            intern, &bus);

        auto evs = make_events(3, 5);
        ScanUnit unit;
        FoldBatch fb{std::span<const FoldEvent>(evs), unit};

        g_present = true;
        g_consumed = 123;
        bus.clear();
        cons.fold->step(fb);  // producer has not published yet
        prod.fold->step(fb);
        CHECK_FALSE(g_present);
        CHECK(g_consumed == 0);
    }

    TEST_CASE("a value published in one batch is gone in the next") {
        StringIntern intern;
        FoldPortBus bus;
        FoldHolder prod(
            dftracer::utils::plugins::make_plugin<ProducerSlice>(nullptr),
            intern, &bus);
        FoldHolder cons(
            dftracer::utils::plugins::make_plugin<ConsumerSlice>(nullptr),
            intern, &bus);

        auto evs = make_events(2, 4);
        ScanUnit unit;
        FoldBatch fb{std::span<const FoldEvent>(evs), unit};

        bus.clear();
        prod.fold->step(fb);
        cons.fold->step(fb);
        REQUIRE(g_present);
        REQUIRE(g_consumed == 2);

        // Next batch: the driver clears the bus and the producer stays silent.
        bus.clear();
        cons.fold->step(fb);
        CHECK_FALSE(g_present);
        CHECK(g_consumed == 0);
    }
}
