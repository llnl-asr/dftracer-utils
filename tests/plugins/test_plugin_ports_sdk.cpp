// SPDX-License-Identifier: MIT
// The C++ SDK over the dftu.svc.ports members (plugins/plugin/ports.h):
// `co_await ports.recv(ch, buf)` gives a Result<uint32_t>, Channel<T> carries
// a fixed-size value, and latest and phase calls give Results whose errors
// carry a PortError code. Two folds share one hub, as a scan wires them.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/plugins_internal.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/plugins/plugin.h>
#include <doctest/doctest.h>
#include <plugins/test_plugin_common.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using plugins::host::InjectedPlugin;
using plugins::host::PluginFold;
using plugins::host::PortHub;

namespace {

struct Reading {
    std::int32_t id;
    double value;
};

dftu_status declare_owner(dftu_ctx* ctx, const dftu_plugin_host* host) {
    plugins::Ports ports(host, ctx);
    plugins::ChannelOptions bytes;
    bytes.credits = 2;
    bytes.max_msg_bytes = 8;
    auto q = ports.open("q", bytes);
    if (!q) return plugins::status_of(q.error());
    plugins::ChannelOptions fixed;
    fixed.credits = 4;
    fixed.max_msg_bytes = sizeof(Reading);
    auto readings = ports.open("readings", fixed);
    if (!readings) return plugins::status_of(readings.error());
    auto gen = ports.phase_declare("total", plugins::MergeOp::Sum,
                                   plugins::PhaseType::I64);
    if (!gen) return plugins::status_of(gen.error());
    return DFTU_OK;
}

dftu_status declare_reader(dftu_ctx* ctx, const dftu_plugin_host* host) {
    plugins::Ports ports(host, ctx);
    plugins::ChannelOptions bytes;
    bytes.credits = 2;
    bytes.max_msg_bytes = 8;
    auto q = ports.open("plugin.owner.q", bytes);
    if (!q) return plugins::status_of(q.error());
    plugins::ChannelOptions fixed;
    fixed.credits = 4;
    fixed.max_msg_bytes = sizeof(Reading);
    auto readings = ports.open("plugin.owner.readings", fixed);
    if (!readings) return plugins::status_of(readings.error());
    return DFTU_OK;
}

dftu_status make_empty_slice(dftu_ctx*, void*, void** out) {
    *out = new int(0);
    return DFTU_OK;
}
void destroy_empty_slice(void* slice) { delete static_cast<int*>(slice); }
dftu_status no_merge(dftu_ctx*, void*, void*) { return DFTU_OK; }
void no_destroy(void*) {}
dftu_status no_finalize(dftu_ctx*, void*, const dftu_plugin_host*,
                        dftu_task**) {
    return DFTU_OK;
}

dftu_plugin make_bare_plugin() {
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.make_slice = make_empty_slice;
    p.merge = no_merge;
    p.on_finalize = no_finalize;
    p.destroy_slice = destroy_empty_slice;
    p.destroy = no_destroy;
    return p;
}

struct Scan {
    dstd::StringIntern intern;
    dftu_plugin po = make_bare_plugin();
    dftu_plugin pr = make_bare_plugin();
    dstd::Optional<plugins::host::Plugins> set;
    dstd::UniquePtr<PortHub> hub;
    dstd::UniquePtr<PluginFold> owner;
    dstd::UniquePtr<PluginFold> reader;

    Scan() {
        auto built = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&po, "owner", declare_owner},
             InjectedPlugin{&pr, "reader", declare_reader}});
        REQUIRE(built.has_value());
        set.emplace(std::move(*built));
        hub = plugins::host::make_port_hub(*set);
        owner = dstd::make_unique<PluginFold>(&po, intern);
        reader = dstd::make_unique<PluginFold>(&pr, intern);
        owner->bind_ports(hub.get(), "owner");
        reader->bind_ports(hub.get(), "reader");
    }
};

struct Outcome {
    dstd::Optional<dstd::Error> error;
    std::uint32_t length = 0;
    std::uint32_t needed = 0;
    dstd::String text;
};

plugins::Task recv_into(plugins::Host h, plugins::ChannelId ch,
                        std::uint32_t cap, Outcome* out) {
    plugins::Ports ports = h.ports();
    dstd::String buf(cap, '\0');
    dstd::MutableByteView view(buf.data(), buf.size());
    auto op = ports.recv(ch, view, &out->needed);
    dstd::Result<std::uint32_t> got = co_await op;
    if (!got) {
        out->error = got.error();
    } else {
        out->length = *got;
        out->text = buf.substr(0, *got);
    }
}

plugins::Task send_text(plugins::Host h, plugins::ChannelId ch,
                        const char* text, Outcome* out) {
    plugins::Ports ports = h.ports();
    dstd::ByteView view(text, std::strlen(text));
    auto op = ports.send(ch, view);
    dstd::Result<void> sent = co_await op;
    if (!sent) out->error = sent.error();
}

plugins::Task send_reading(plugins::Host h, plugins::ChannelId ch,
                           Reading reading, Outcome* out) {
    plugins::Channel<Reading> channel(h.ports(), ch);
    auto op = channel.send(reading);
    dstd::Result<void> sent = co_await op;
    if (!sent) out->error = sent.error();
}

plugins::Task recv_reading(plugins::Host h, plugins::ChannelId ch,
                           Reading* reading, Outcome* out) {
    plugins::Channel<Reading> channel(h.ports(), ch);
    auto op = channel.recv();
    dstd::Result<Reading> got = co_await op;
    if (!got) {
        out->error = got.error();
    } else {
        *reading = *got;
    }
}

template <class T>
plugins::Task recv_value(plugins::Host h, plugins::ChannelId ch, T* value,
                         Outcome* out) {
    plugins::Channel<T> channel(h.ports(), ch);
    auto op = channel.recv();
    dstd::Result<T> got = co_await op;
    if (!got) {
        out->error = got.error();
    } else {
        *value = *got;
    }
}

}  // namespace

TEST_CASE("sdk: co_await recv gives the length, and TooBig reports the size") {
    Scan scan;
    test_plugin_common::TaskDriver send_driver(scan.owner->host());
    test_plugin_common::TaskDriver short_driver(scan.reader->host());
    test_plugin_common::TaskDriver whole_driver(scan.reader->host());

    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto q = ports.open("q");
    REQUIRE(q.has_value());

    Outcome sent;
    CHECK(send_driver.run(send_text(send_driver.host(), *q, "abcd", &sent)) ==
          DFTU_OK);
    CHECK_FALSE(sent.error.has_value());

    Outcome tight;
    CHECK(short_driver.run(recv_into(short_driver.host(), *q, 2, &tight)) ==
          DFTU_OK);
    REQUIRE(tight.error.has_value());
    CHECK(tight.error->is_code(plugins::PortError::TooBig));
    CHECK(tight.needed == 4);

    Outcome whole;
    CHECK(whole_driver.run(recv_into(whole_driver.host(), *q, 8, &whole)) ==
          DFTU_OK);
    CHECK_FALSE(whole.error.has_value());
    CHECK(whole.length == 4);
    CHECK(whole.text == "abcd");
}

TEST_CASE("sdk: a send over max_msg_bytes is a TooBig error") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto q = ports.open("q");
    REQUIRE(q.has_value());
    test_plugin_common::TaskDriver driver(scan.owner->host());
    Outcome sent;
    CHECK(driver.run(send_text(driver.host(), *q, "0123456789", &sent)) ==
          DFTU_OK);
    REQUIRE(sent.error.has_value());
    CHECK(sent.error->is_code(plugins::PortError::TooBig));
}

TEST_CASE("sdk: try_send reports Full once the credits are used") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto q = ports.open("q");
    REQUIRE(q.has_value());
    const dstd::ByteView one("x", 1);
    CHECK(ports.try_send(*q, one).has_value());
    CHECK(ports.try_send(*q, one).has_value());
    auto full = ports.try_send(*q, one);
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().is_code(plugins::PortError::Full));
}

TEST_CASE("sdk: Channel<T> carries a fixed-size value and ends with Closed") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto id = ports.open("readings");
    REQUIRE(id.has_value());

    test_plugin_common::TaskDriver send_driver(scan.owner->host());
    Outcome sent;
    CHECK(send_driver.run(send_reading(send_driver.host(), *id, Reading{7, 2.5},
                                       &sent)) == DFTU_OK);
    CHECK_FALSE(sent.error.has_value());
    REQUIRE(ports.close(*id).has_value());

    test_plugin_common::TaskDriver first_driver(scan.reader->host());
    Reading got{};
    Outcome first;
    CHECK(first_driver.run(
              recv_reading(first_driver.host(), *id, &got, &first)) == DFTU_OK);
    CHECK_FALSE(first.error.has_value());
    CHECK(got.id == 7);
    CHECK(got.value == doctest::Approx(2.5));

    test_plugin_common::TaskDriver end_driver(scan.reader->host());
    Outcome end;
    CHECK(end_driver.run(recv_reading(end_driver.host(), *id, &got, &end)) ==
          DFTU_OK);
    REQUIRE(end.error.has_value());
    CHECK(end.error->is_code(plugins::PortError::Closed));
}

TEST_CASE("sdk: latest is NotReady before the first set") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports setter(&scan.owner->host(), ctx.get());
    plugins::Ports getter(&scan.reader->host(), ctx.get());
    char buf[8];
    const dstd::MutableByteView view(buf, sizeof buf);
    auto early = getter.latest_get("plugin.owner.hint", view);
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().is_code(plugins::PortError::NotReady));

    auto first = setter.latest_set("hint", dstd::ByteView("ab", 2));
    auto second = setter.latest_set("hint", dstd::ByteView("xyz", 3));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 1);
    CHECK(*second == 2);
    auto got = getter.latest_get("plugin.owner.hint", view);
    REQUIRE(got.has_value());
    CHECK(got->generation == 2);
    CHECK(got->size == 3);
    CHECK(dstd::String(buf, got->size) == "xyz");
}

TEST_CASE("sdk: phase results merge and seal") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports owner(&scan.owner->host(), ctx.get());
    plugins::Ports reader(&scan.reader->host(), ctx.get());
    const std::int64_t parts[3] = {4, 5, -1};
    for (const std::int64_t part : parts) {
        const dstd::ByteView data(reinterpret_cast<const char*>(&part), 8);
        CHECK(owner
                  .phase_contribute("total", 1, plugins::MergeOp::Sum,
                                    plugins::PhaseType::I64, data)
                  .has_value());
    }
    std::int64_t total = 0;
    const dstd::MutableByteView out(reinterpret_cast<char*>(&total), 8);
    auto open = reader.phase_result("plugin.owner.total", 1, out);
    REQUIRE_FALSE(open.has_value());
    CHECK(open.error().is_code(plugins::PortError::NotReady));
    REQUIRE(owner.phase_seal("total", 1).has_value());
    auto sealed = reader.phase_result("plugin.owner.total", 1, out);
    REQUIRE(sealed.has_value());
    CHECK(*sealed == 8);
    CHECK(total == 8);
    const dstd::ByteView late(reinterpret_cast<const char*>(&total), 8);
    auto after = owner.phase_contribute("total", 1, plugins::MergeOp::Sum,
                                        plugins::PhaseType::I64, late);
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().is_code(plugins::PortError::Closed));
}

TEST_CASE(
    "sdk: Channel<T> drops a message shorter than T and names both sizes") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto id = ports.open("readings");
    REQUIRE(id.has_value());

    test_plugin_common::TaskDriver send_driver(scan.owner->host());
    Outcome sent;
    CHECK(send_driver.run(send_text(send_driver.host(), *id, "abcd", &sent)) ==
          DFTU_OK);
    CHECK_FALSE(sent.error.has_value());

    test_plugin_common::TaskDriver short_driver(scan.reader->host());
    Reading got{-1, -1.0};
    Outcome bad;
    CHECK(short_driver.run(recv_value<Reading>(short_driver.host(), *id, &got,
                                               &bad)) == DFTU_OK);
    REQUIRE(bad.error.has_value());
    CHECK(bad.error->is_code(plugins::PortError::TooBig));
    CHECK(bad.error->message().find("4 bytes") != dstd::String::npos);
    CHECK(bad.error->message().find("16 bytes") != dstd::String::npos);
    CHECK(got.id == -1);

    // The wrong message is gone, so the next value comes through.
    test_plugin_common::TaskDriver again_driver(scan.owner->host());
    Outcome again;
    CHECK(again_driver.run(send_reading(again_driver.host(), *id,
                                        Reading{3, 1.5}, &again)) == DFTU_OK);
    test_plugin_common::TaskDriver next_driver(scan.reader->host());
    Outcome next;
    CHECK(next_driver.run(recv_value<Reading>(next_driver.host(), *id, &got,
                                              &next)) == DFTU_OK);
    CHECK_FALSE(next.error.has_value());
    CHECK(got.id == 3);
}

TEST_CASE("sdk: Channel<T> drops a message longer than T and keeps flowing") {
    Scan scan;
    test_plugin_common::Ctx ctx;
    plugins::Ports ports(&scan.owner->host(), ctx.get());
    auto id = ports.open("q");
    REQUIRE(id.has_value());

    test_plugin_common::TaskDriver send_driver(scan.owner->host());
    Outcome sent;
    CHECK(send_driver.run(send_text(send_driver.host(), *id, "12345678",
                                    &sent)) == DFTU_OK);
    CHECK_FALSE(sent.error.has_value());

    test_plugin_common::TaskDriver big_driver(scan.reader->host());
    std::int32_t value = -1;
    Outcome bad;
    CHECK(big_driver.run(recv_value<std::int32_t>(big_driver.host(), *id,
                                                  &value, &bad)) == DFTU_OK);
    REQUIRE(bad.error.has_value());
    CHECK(bad.error->is_code(plugins::PortError::TooBig));
    CHECK(bad.error->message().find("8 bytes") != dstd::String::npos);
    CHECK(bad.error->message().find("4 bytes") != dstd::String::npos);

    test_plugin_common::TaskDriver again_driver(scan.owner->host());
    Outcome again;
    CHECK(again_driver.run(
              send_text(again_driver.host(), *id, "wxyz", &again)) == DFTU_OK);
    test_plugin_common::TaskDriver next_driver(scan.reader->host());
    Outcome next;
    CHECK(next_driver.run(recv_value<std::int32_t>(next_driver.host(), *id,
                                                   &value, &next)) == DFTU_OK);
    CHECK_FALSE(next.error.has_value());
    std::int32_t expect = 0;
    std::memcpy(&expect, "wxyz", 4);
    CHECK(value == expect);
}
