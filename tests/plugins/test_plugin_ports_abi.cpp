// SPDX-License-Identifier: MIT
// The dftu.svc.ports channel, latest and phase members, called from C
// (ports_abi_c_consumer.c) through the host of real folds that share one hub,
// and the plan-time rules the host enforces on what a factory declares.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/abi/agg.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/plugins_internal.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <plugins/abi_c_consumers.h>
#include <plugins/test_plugin_common.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <thread>

using namespace dftracer::utils;
using plugins::host::InjectedPlugin;
using plugins::host::PluginFold;
using plugins::host::PortHub;
using test_plugin_common::Ctx;
using test_plugin_common::run_task;

namespace {

constexpr std::uint32_t CREDITS = DFTU_CHANNEL_F_CREDITS;
constexpr std::uint32_t OVERFLOW_MODE = DFTU_CHANNEL_F_OVERFLOW;
constexpr std::uint32_t MAX_BYTES = DFTU_CHANNEL_F_MAX_MSG_BYTES;
constexpr std::uint32_t TASK_END = DFTU_CHANNEL_F_TASK_END;
constexpr std::uint32_t SENDS = DFTU_CHANNEL_F_SENDS;
constexpr std::uint32_t RECEIVES = DFTU_CHANNEL_F_RECEIVES;

struct ChannelSpec {
    const char* name;
    std::uint32_t mask;
    std::uint32_t credits;
    dftu_overflow overflow;
    std::uint32_t max_bytes;
};

struct PhaseSpec {
    const char* name;
    dftu_merge_op op;
    dftu_type type;
    std::uint64_t want;
};

// A fixed list, since a global dstd::Vector would hold its block past the
// memory exit check.
template <class T, std::size_t N>
struct Fixed {
    T items[N] = {};
    std::size_t count = 0;

    Fixed() = default;
    Fixed(const Fixed&) = delete;
    Fixed& operator=(const Fixed&) = delete;
    Fixed& operator=(std::initializer_list<T> list) {
        count = 0;
        for (const T& item : list) items[count++] = item;
        return *this;
    }
    const T* begin() const { return items; }
    const T* end() const { return items + count; }
};

// What the next plan's factories declare; the declare callbacks carry no data.
struct Decl {
    Fixed<ChannelSpec, 4> channels;
    Fixed<PhaseSpec, 4> phases;
};
Decl g_alpha, g_beta, g_gamma;

dftu_status declare_with(const Decl& d, dftu_ctx* ctx,
                         const dftu_plugin_host* host) {
    for (const ChannelSpec& c : d.channels) {
        std::uint64_t id = 0;
        const dftu_status st = dftu_test_ports_open(
            ctx, host, c.name, c.mask, c.credits, c.overflow, c.max_bytes, &id);
        if (st < 0) return st;
    }
    for (const PhaseSpec& p : d.phases) {
        std::uint64_t got = 0;
        const dftu_status st = dftu_test_ports_phase_declare(
            ctx, host, p.name, p.op, p.type, p.want, &got);
        if (st < 0) return st;
    }
    return DFTU_OK;
}

dftu_status declare_alpha(dftu_ctx* c, const dftu_plugin_host* h) {
    return declare_with(g_alpha, c, h);
}
dftu_status declare_beta(dftu_ctx* c, const dftu_plugin_host* h) {
    return declare_with(g_beta, c, h);
}
dftu_status declare_gamma(dftu_ctx* c, const dftu_plugin_host* h) {
    return declare_with(g_gamma, c, h);
}

void reset_decls() {
    for (Decl* d : {&g_alpha, &g_beta, &g_gamma}) {
        d->channels.count = 0;
        d->phases.count = 0;
    }
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

// Two folds, alpha and beta, wired to one hub the way Plugins::run wires them.
struct Scan {
    dstd::StringIntern intern;
    dftu_plugin pa = make_bare_plugin();
    dftu_plugin pb = make_bare_plugin();
    dstd::Optional<plugins::host::Plugins> set;
    dstd::UniquePtr<PortHub> hub;
    dstd::UniquePtr<PluginFold> alpha;
    dstd::UniquePtr<PluginFold> beta;

    Scan() {
        auto built = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&pa, "alpha", declare_alpha},
             InjectedPlugin{&pb, "beta", declare_beta}});
        REQUIRE(built.has_value());
        set.emplace(std::move(*built));
        hub = plugins::host::make_port_hub(*set);
        alpha = dstd::make_unique<PluginFold>(&pa, intern);
        beta = dstd::make_unique<PluginFold>(&pb, intern);
        alpha->bind_ports(hub.get(), "alpha");
        beta->bind_ports(hub.get(), "beta");
    }
    const dftu_plugin_host* a() { return &alpha->host(); }
    const dftu_plugin_host* b() { return &beta->host(); }
};

std::uint64_t open_channel(const dftu_plugin_host* h, const char* name) {
    Ctx c;
    std::uint64_t id = 0;
    const dftu_status st = dftu_test_ports_open(c.get(), h, name, 0, 0,
                                                DFTU_OVERFLOW_FAIL, 0, &id);
    REQUIRE_MESSAGE(st == DFTU_OK, c.message().c_str());
    return id;
}

// One send, run to its end through the task service.
dftu_port_status send_bytes(const dftu_plugin_host* h, std::uint64_t ch,
                            const char* data, std::uint32_t len) {
    Ctx c;
    dftu_port_status st = DFTU_PORT_FULL;
    dftu_task* t = nullptr;
    REQUIRE(dftu_test_ports_send(c.get(), h, ch, data, len, &st, &t) ==
            DFTU_OK);
    REQUIRE(run_task(t) == DFTU_OK);
    return st;
}

struct Got {
    dftu_port_status status = DFTU_PORT_FULL;
    std::uint32_t len = 0;
    dstd::String text;
};

// The receive without a doctest assertion, so a thread can run it.
dftu_status recv_raw(const dftu_plugin_host* h, std::uint64_t ch,
                     std::uint32_t cap, Got* got) {
    Ctx c;
    dstd::String buf(cap, '\0');
    dftu_task* t = nullptr;
    const dftu_status started = dftu_test_ports_recv(
        c.get(), h, ch, buf.data(), cap, &got->len, &got->status, &t);
    if (started < 0) return started;
    const dftu_status ended = run_task(t);
    if (got->status == DFTU_PORT_OK) got->text = buf.substr(0, got->len);
    return ended;
}

Got recv_bytes(const dftu_plugin_host* h, std::uint64_t ch, std::uint32_t cap) {
    Got got;
    REQUIRE(recv_raw(h, ch, cap, &got) == DFTU_OK);
    return got;
}

dftu_port_status try_send_bytes(const dftu_plugin_host* h, std::uint64_t ch,
                                const char* data, std::uint32_t len) {
    Ctx c;
    dftu_port_status st = DFTU_PORT_OK;
    REQUIRE(dftu_test_ports_try_send(c.get(), h, ch, data, len, &st) ==
            DFTU_OK);
    return st;
}

dstd::String plan_error(const char* alpha_beta_gamma_note = "") {
    (void)alpha_beta_gamma_note;
    dftu_plugin pa = make_bare_plugin();
    dftu_plugin pb = make_bare_plugin();
    dftu_plugin pc = make_bare_plugin();
    auto built = plugins::host::build_injected_plugins_declaring(
        {InjectedPlugin{&pa, "alpha", declare_alpha},
         InjectedPlugin{&pb, "beta", declare_beta},
         InjectedPlugin{&pc, "gamma", declare_gamma}});
    if (built.has_value()) return {};
    return dstd::String(built.error().message());
}

char g_seen_buf[16];
std::uint32_t g_recv_len = 0;
dftu_port_status g_recv_status = DFTU_PORT_FULL;

dftu_status sender_on_batch(dftu_ctx* ctx, void*, const dftu_dataframe*,
                            const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(ctx, host, "events", 0, 0,
                                          DFTU_OVERFLOW_FAIL, 0, &ch);
    if (st < 0) return st;
    const std::uint64_t one = 1;
    dftu_port_status sent = DFTU_PORT_OK;
    return dftu_test_ports_try_send(ctx, host, ch, &one, 8, &sent);
}

dftu_status quiet_on_batch(dftu_ctx*, void*, const dftu_dataframe*,
                           const dftu_plugin_host*) {
    return DFTU_OK;
}

// The receiver hands the host a recv task to await in its finalize.
dftu_status receiver_on_finalize(dftu_ctx* ctx, void*,
                                 const dftu_plugin_host* host,
                                 dftu_task** pending) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(ctx, host, "plugin.sender.events", 0,
                                          0, DFTU_OVERFLOW_FAIL, 0, &ch);
    if (st < 0) return st;
    return dftu_test_ports_recv(ctx, host, ch, g_seen_buf, sizeof g_seen_buf,
                                &g_recv_len, &g_recv_status, pending);
}

dftu_status declare_sender(dftu_ctx* ctx, const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "events", 0, 0, DFTU_OVERFLOW_FAIL,
                                0, &ch);
}

dftu_status declare_receiver(dftu_ctx* ctx, const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "plugin.sender.events", 0, 0,
                                DFTU_OVERFLOW_FAIL, 0, &ch);
}

dstd::String make_trace(dftu_utils_test::TestEnvironment& env, int n) {
    dstd::String dir = env.dir() + "/ports";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::String pfw = dir + "/trace.pfw";
    dstd::String lines;
    for (int i = 0; i < n; ++i) {
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":{}{}\n">(lines, (1000 + i * 100),
                                                  (10 + i), R"(,"args":{}})");
    }
    dstd::fs::write_file(dstd::PathView(pfw), lines).discard();
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

// A receive started in on_finalize on a thread of its own, so nothing but the
// end of the scan can wake it.
std::atomic<bool> g_bg_done{false};
dftu_port_status g_bg_status = DFTU_PORT_FULL;
std::uint32_t g_bg_len = 0;
char g_bg_buf[8];
dstd::Optional<dstd::Thread> g_bg;
std::atomic<dftu_task*> g_bg_task{nullptr};

dftu_status bg_receiver_on_finalize(dftu_ctx* ctx, void*,
                                    const dftu_plugin_host* host, dftu_task**) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(ctx, host, "plugin.sender.events", 0,
                                          0, DFTU_OVERFLOW_FAIL, 0, &ch);
    if (st < 0) return st;
    dftu_task* t = nullptr;
    st = dftu_test_ports_recv(ctx, host, ch, g_bg_buf, sizeof g_bg_buf,
                              &g_bg_len, &g_bg_status, &t);
    if (st < 0) return st;
    g_bg_task.store(t);
    g_bg.emplace([t] {
        run_task(t);
        g_bg_done.store(true);
    });
    return DFTU_OK;
}

// A Block send from on_batch: the host must refuse it, not wait.
dftu_status block_declare_sender(dftu_ctx* ctx, const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "q",
                                CREDITS | OVERFLOW_MODE | TASK_END | SENDS, 1,
                                DFTU_OVERFLOW_BLOCK, 0, &ch);
}

dftu_status block_declare_receiver(dftu_ctx* ctx,
                                   const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "plugin.sender.q",
                                CREDITS | OVERFLOW_MODE | TASK_END | RECEIVES,
                                1, DFTU_OVERFLOW_BLOCK, 0, &ch);
}

// A Block send from a state update: the sender plugin keeps its host and
// channel from on_batch, and the receiver plugin's state update calls the send
// through them, on the thread that runs the state update.
const dftu_plugin_host* g_kept_host = nullptr;
std::uint64_t g_kept_channel = 0;

dftu_status keep_host_on_batch(dftu_ctx* ctx, void*, const dftu_dataframe*,
                               const dftu_plugin_host* host) {
    const dftu_status opened = dftu_test_ports_open(
        ctx, host, "q", CREDITS | OVERFLOW_MODE | TASK_END | SENDS, 1,
        DFTU_OVERFLOW_BLOCK, 0, &g_kept_channel);
    if (opened < 0) return opened;
    g_kept_host = host;
    return DFTU_OK;
}

dftu_status state_init(dftu_ctx*, void*, void** out) {
    *out = new int(0);
    return DFTU_OK;
}

dftu_status state_update(dftu_ctx* ctx, void*, const dftu_dataframe*) {
    if (g_kept_host == nullptr) return DFTU_OK;
    dftu_port_status sent = DFTU_PORT_OK;
    dftu_task* t = nullptr;
    return dftu_test_ports_send(ctx, g_kept_host, g_kept_channel, "x", 1, &sent,
                                &t);
}

dftu_status state_merge(dftu_ctx*, void*, void*) { return DFTU_OK; }

dftu_status state_finalize(dftu_ctx*, void*, dftu_result_value*) {
    return DFTU_OK;
}

void state_destroy(void* state) { delete static_cast<int*>(state); }

dftu_status state_declare_receiver(dftu_ctx* ctx,
                                   const dftu_plugin_host* host) {
    const dftu_status opened = block_declare_receiver(ctx, host);
    if (opened < 0) return opened;
    const auto* agg = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    if (agg == nullptr) return DFTU_E_UNSUPPORTED;
    dftu_state_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = "receiver.counter";
    desc.init = state_init;
    desc.update = state_update;
    desc.merge = state_merge;
    desc.finalize = state_finalize;
    desc.destroy = state_destroy;
    return agg->register_state(ctx, host->h, &desc, nullptr);
}

dftu_status block_sender_on_batch(dftu_ctx* ctx, void*, const dftu_dataframe*,
                                  const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(
        ctx, host, "q", CREDITS | OVERFLOW_MODE | TASK_END | SENDS, 1,
        DFTU_OVERFLOW_BLOCK, 0, &ch);
    if (st < 0) return st;
    dftu_port_status sent = DFTU_PORT_OK;
    dftu_task* t = nullptr;
    return dftu_test_ports_send(ctx, host, ch, "x", 1, &sent, &t);
}

// The attach branch of the scan-end test, as a functor and a function of their
// own: GCC 12 rejects the same shape written as lambdas inside lambdas.
struct WaitForBackground {
    trace::views::Deferred<plugins::host::PluginRun> handle;

    void operator()(const trace::views::ExportStats&) {
        handle.get();
        // The hub lives until the plan is torn down, so the woken receive must
        // end before this callback returns.
        for (int i = 0; i < 500 && !g_bg_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

dstd::Function<void(const trace::views::ExportStats&)> attach_and_wait(
    const plugins::host::Plugins& set, trace::views::ViewSession& session) {
    WaitForBackground done{set.attach(session)};
    return dstd::Function<void(const trace::views::ExportStats&)>(
        std::move(done));
}

// A run to its end, with the error message of a failed run.
coro::CoroTask<void> run_for_message(const plugins::host::Plugins* set,
                                     const trace::views::View* view,
                                     dstd::String* message) {
    auto op = set->run(*view);
    auto run = co_await op;
    if (!run) *message = run.error().message();
}

// Senders that end their finalize without a close. Each sends G_SEND_COUNT
// values; the receiver's finalize receives until the host closes the channel.
constexpr std::uint64_t G_SEND_COUNT = 3;
dftu_port_status g_last_send_status = DFTU_PORT_FULL;
std::uint64_t g_received = 0;
dftu_port_status g_end_status = DFTU_PORT_FULL;

dftu_status send_values_on_finalize(dftu_ctx* ctx, void*,
                                    const dftu_plugin_host* host, dftu_task**) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(ctx, host, "plugin.first.events", 0,
                                          0, DFTU_OVERFLOW_FAIL, 0, &ch);
    if (st < 0) return st;
    for (std::uint64_t i = 0; i < G_SEND_COUNT; ++i) {
        st =
            dftu_test_ports_try_send(ctx, host, ch, &i, 8, &g_last_send_status);
        if (st < 0) return st;
    }
    return DFTU_OK;
}

dftu_status failing_on_finalize(dftu_ctx*, void*, const dftu_plugin_host*,
                                dftu_task**) {
    return DFTU_E_INVALID;
}

dftu_status count_until_closed_on_finalize(dftu_ctx* ctx, void*,
                                           const dftu_plugin_host* host,
                                           dftu_task**) {
    std::uint64_t ch = 0;
    dftu_status st = dftu_test_ports_open(ctx, host, "plugin.first.events", 0,
                                          0, DFTU_OVERFLOW_FAIL, 0, &ch);
    if (st < 0) return st;
    for (;;) {
        Got got;
        st = recv_raw(host, ch, 8, &got);
        if (st < 0) return st;
        g_end_status = got.status;
        if (got.status != DFTU_PORT_OK) return DFTU_OK;
        ++g_received;
    }
}

dftu_status declare_first_sender(dftu_ctx* ctx, const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "events", SENDS, 0,
                                DFTU_OVERFLOW_FAIL, 0, &ch);
}

dftu_status declare_second_sender(dftu_ctx* ctx, const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "plugin.first.events", SENDS, 0,
                                DFTU_OVERFLOW_FAIL, 0, &ch);
}

dftu_status declare_counting_receiver(dftu_ctx* ctx,
                                      const dftu_plugin_host* host) {
    std::uint64_t ch = 0;
    return dftu_test_ports_open(ctx, host, "plugin.first.events", RECEIVES, 0,
                                DFTU_OVERFLOW_FAIL, 0, &ch);
}

void reset_end_state() {
    g_last_send_status = DFTU_PORT_FULL;
    g_received = 0;
    g_end_status = DFTU_PORT_FULL;
}

// A run of `plugins` over a 40 line trace, with the error message of a failed
// run.
dstd::String run_plugins_message(dstd::Vector<InjectedPlugin> plugins) {
    dftu_utils_test::TestEnvironment env(40);
    trace::views::ViewFile file =
        test_plugin_common::index_trace(make_trace(env, 40));
    auto set =
        plugins::host::build_injected_plugins_declaring(std::move(plugins));
    REQUIRE(set.has_value());
    dstd::String message;
    dstd::Vector<trace::views::ViewFile> files;
    files.push_back(file);
    const trace::views::View view = trace::views::View::from_files(files);
    const plugins::host::Plugins* planned = &*set;
    Runtime rt(2);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto body = run_for_message(planned, &view, &message);
        co_await body;
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "ports-senders").wait());
    rt.shutdown();
    return message;
}

}  // namespace

TEST_SUITE("plugin ports: run") {
    TEST_CASE(
        "Plugins::run wires the hub, so a reader gets what an owner sent") {
        dftu_utils_test::TestEnvironment env(40);
        trace::views::ViewFile file =
            test_plugin_common::index_trace(make_trace(env, 40));

        dftu_plugin receiver = make_bare_plugin();
        receiver.on_batch = quiet_on_batch;
        receiver.on_finalize = receiver_on_finalize;
        dftu_plugin sender = make_bare_plugin();
        sender.on_batch = sender_on_batch;
        auto set = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&receiver, "receiver", declare_receiver},
             InjectedPlugin{&sender, "sender", declare_sender}});
        REQUIRE(set.has_value());

        g_recv_len = 0;
        g_recv_status = DFTU_PORT_FULL;
        test_plugin_common::run_set(
            *set, trace::views::View::from_files({file}), "ports-run");
        CHECK(g_recv_status == DFTU_PORT_OK);
        CHECK(g_recv_len == 8);
    }

    TEST_CASE("a channel closes when its sender finalized without closing it") {
        dftu_plugin first = make_bare_plugin();
        first.on_finalize = send_values_on_finalize;
        dftu_plugin reader = make_bare_plugin();
        reader.on_finalize = count_until_closed_on_finalize;
        reset_end_state();
        const dstd::String message = run_plugins_message(
            {InjectedPlugin{&first, "first", declare_first_sender},
             InjectedPlugin{&reader, "reader", declare_counting_receiver}});
        CHECK(message.empty());
        CHECK(g_received == G_SEND_COUNT);
        CHECK(g_end_status == DFTU_PORT_CLOSED);
    }

    TEST_CASE("a channel with two senders closes after both finalized") {
        dftu_plugin first = make_bare_plugin();
        first.on_finalize = send_values_on_finalize;
        dftu_plugin second = make_bare_plugin();
        second.on_finalize = send_values_on_finalize;
        dftu_plugin reader = make_bare_plugin();
        reader.on_finalize = count_until_closed_on_finalize;
        reset_end_state();
        const dstd::String message = run_plugins_message(
            {InjectedPlugin{&first, "first", declare_first_sender},
             InjectedPlugin{&second, "second", declare_second_sender},
             InjectedPlugin{&reader, "reader", declare_counting_receiver}});
        CHECK(message.empty());
        CHECK(g_last_send_status == DFTU_PORT_OK);
        CHECK(g_received == 2 * G_SEND_COUNT);
        CHECK(g_end_status == DFTU_PORT_CLOSED);
    }

    TEST_CASE("a sender whose finalize fails ends the run with its error") {
        dftu_plugin first = make_bare_plugin();
        first.on_finalize = failing_on_finalize;
        dftu_plugin reader = make_bare_plugin();
        reader.on_finalize = count_until_closed_on_finalize;
        reset_end_state();
        const dstd::String message = run_plugins_message(
            {InjectedPlugin{&first, "first", declare_first_sender},
             InjectedPlugin{&reader, "reader", declare_counting_receiver}});
        CHECK(message.find("on_finalize") != dstd::String::npos);
        CHECK(message.find("first") != dstd::String::npos);
    }
}

TEST_SUITE("plugin ports: channels") {
    TEST_CASE("a short recv buffer gives TooBig with the needed size") {
        reset_decls();
        g_alpha.channels = {
            {"q", CREDITS | MAX_BYTES, 4, DFTU_OVERFLOW_FAIL, 8}};
        g_beta.channels = {
            {"plugin.alpha.q", CREDITS | MAX_BYTES, 4, DFTU_OVERFLOW_FAIL, 8}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        CHECK(open_channel(scan.b(), "plugin.alpha.q") == ch);

        CHECK(send_bytes(scan.a(), ch, "abcd", 4) == DFTU_PORT_OK);
        const Got shortened = recv_bytes(scan.b(), ch, 2);
        CHECK(shortened.status == DFTU_PORT_TOO_BIG);
        CHECK(shortened.len == 4);
        // The message stayed queued, so a big enough buffer gets it.
        const Got whole = recv_bytes(scan.b(), ch, 8);
        CHECK(whole.status == DFTU_PORT_OK);
        CHECK(whole.text == "abcd");
        // A message over max_msg_bytes is refused, never queued.
        CHECK(send_bytes(scan.a(), ch, "0123456789", 10) == DFTU_PORT_TOO_BIG);
    }

    TEST_CASE("Fail: a full channel reports Full and keeps its messages") {
        reset_decls();
        g_alpha.channels = {
            {"q", CREDITS | OVERFLOW_MODE, 2, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {{"plugin.alpha.q", CREDITS | OVERFLOW_MODE, 2,
                            DFTU_OVERFLOW_FAIL, 0}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        CHECK(send_bytes(scan.a(), ch, "1", 1) == DFTU_PORT_OK);
        CHECK(send_bytes(scan.a(), ch, "2", 1) == DFTU_PORT_OK);
        CHECK(send_bytes(scan.a(), ch, "3", 1) == DFTU_PORT_FULL);
        CHECK(try_send_bytes(scan.a(), ch, "3", 1) == DFTU_PORT_FULL);
        CHECK(recv_bytes(scan.b(), ch, 4).text == "1");
        CHECK(try_send_bytes(scan.a(), ch, "3", 1) == DFTU_PORT_OK);
        CHECK(recv_bytes(scan.b(), ch, 4).text == "2");
        CHECK(recv_bytes(scan.b(), ch, 4).text == "3");
    }

    TEST_CASE("DropOldest: a full channel drops its oldest message") {
        reset_decls();
        g_alpha.channels = {
            {"q", CREDITS | OVERFLOW_MODE, 2, DFTU_OVERFLOW_DROP_OLDEST, 0}};
        g_beta.channels = {{"plugin.alpha.q", CREDITS | OVERFLOW_MODE, 2,
                            DFTU_OVERFLOW_DROP_OLDEST, 0}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        for (const char* m : {"1", "2", "3", "4"}) {
            CHECK(send_bytes(scan.a(), ch, m, 1) == DFTU_PORT_OK);
        }
        CHECK(recv_bytes(scan.b(), ch, 4).text == "3");
        CHECK(recv_bytes(scan.b(), ch, 4).text == "4");
    }

    TEST_CASE("Block: a send waits for a credit and a recv frees it") {
        reset_decls();
        g_alpha.channels = {{"q", CREDITS | OVERFLOW_MODE | TASK_END | SENDS, 1,
                             DFTU_OVERFLOW_BLOCK, 0}};
        g_beta.channels = {{"plugin.alpha.q",
                            CREDITS | OVERFLOW_MODE | TASK_END | RECEIVES, 1,
                            DFTU_OVERFLOW_BLOCK, 0}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        CHECK(send_bytes(scan.a(), ch, "A", 1) == DFTU_PORT_OK);
        CHECK(try_send_bytes(scan.a(), ch, "B", 1) == DFTU_PORT_FULL);

        Ctx c;
        dftu_port_status st = DFTU_PORT_FULL;
        dftu_task* t = nullptr;
        REQUIRE(dftu_test_ports_send(c.get(), scan.a(), ch, "B", 1, &st, &t) ==
                DFTU_OK);
        const auto* svc = static_cast<const dftu_svc_task*>(
            scan.a()->get_service(scan.a()->h, DFTU_SVC_TASK));
        std::atomic<bool> ended{false};
        std::atomic<dftu_status> sent{DFTU_E_UNKNOWN};
        dstd::Thread sender([&] {
            sent.store(run_task(t));
            ended.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(ended.load());
        CHECK(svc->test(scan.a()->h, t) == 0);

        CHECK(recv_bytes(scan.b(), ch, 4).text == "A");
        sender.join();
        CHECK(sent.load() == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        CHECK(recv_bytes(scan.b(), ch, 4).text == "B");
    }

    TEST_CASE("close: a recv drains the queue, then reports Closed") {
        reset_decls();
        g_alpha.channels = {{"q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {{"plugin.alpha.q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        CHECK(send_bytes(scan.a(), ch, "z", 1) == DFTU_PORT_OK);
        Ctx c;
        REQUIRE(dftu_test_ports_close(c.get(), scan.a(), ch) == DFTU_OK);
        CHECK(send_bytes(scan.a(), ch, "y", 1) == DFTU_PORT_CLOSED);
        CHECK(recv_bytes(scan.b(), ch, 4).text == "z");
        CHECK(recv_bytes(scan.b(), ch, 4).status == DFTU_PORT_CLOSED);
    }

    TEST_CASE("a close wakes a receiver that waits on an empty channel") {
        reset_decls();
        g_alpha.channels = {{"q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {{"plugin.alpha.q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        Scan scan;
        const std::uint64_t ch = open_channel(scan.a(), "q");
        Got got;
        std::atomic<dftu_status> ended{DFTU_E_UNKNOWN};
        dstd::Thread receiver(
            [&] { ended.store(recv_raw(scan.b(), ch, 4, &got)); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Ctx c;
        REQUIRE(dftu_test_ports_close(c.get(), scan.a(), ch) == DFTU_OK);
        receiver.join();
        CHECK(ended.load() == DFTU_OK);
        CHECK(got.status == DFTU_PORT_CLOSED);
    }

    TEST_CASE("a channel that no plan declared has no handle") {
        reset_decls();
        Scan scan;
        Ctx c;
        std::uint64_t id = 0;
        CHECK(dftu_test_ports_open(c.get(), scan.a(), "nope", 0, 0,
                                   DFTU_OVERFLOW_FAIL, 0,
                                   &id) == DFTU_E_NOT_FOUND);
        dftu_port_status st = DFTU_PORT_OK;
        CHECK(dftu_test_ports_try_send(c.get(), scan.a(), 12345, "x", 1, &st) ==
              DFTU_E_NOT_FOUND);
    }
}

TEST_SUITE("plugin ports: latest and batch ports") {
    TEST_CASE("latest is NotReady before the first set, then the last set") {
        reset_decls();
        Scan scan;
        Ctx c;
        char buf[16];
        std::uint32_t len = 99;
        std::uint64_t gen = 99;
        dftu_port_status st = DFTU_PORT_OK;
        REQUIRE(dftu_test_ports_latest_get(c.get(), scan.b(), "plugin.alpha.t",
                                           buf, sizeof buf, &len, &gen,
                                           &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_NOT_READY);
        CHECK(gen == 0);

        std::uint64_t first = 0, second = 0;
        REQUIRE(dftu_test_ports_latest_set(c.get(), scan.a(), "t", "one", 3,
                                           &first) == DFTU_OK);
        REQUIRE(dftu_test_ports_latest_set(c.get(), scan.a(), "t", "three", 5,
                                           &second) == DFTU_OK);
        CHECK(first == 1);
        CHECK(second == 2);
        REQUIRE(dftu_test_ports_latest_get(c.get(), scan.b(), "plugin.alpha.t",
                                           buf, sizeof buf, &len, &gen,
                                           &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        CHECK(gen == 2);
        CHECK(dstd::String(buf, len) == "three");

        REQUIRE(dftu_test_ports_latest_get(c.get(), scan.b(), "plugin.alpha.t",
                                           buf, 2, &len, &gen, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_TOO_BIG);
        CHECK(len == 5);
    }

    TEST_CASE("try_publish and try_consume report their status") {
        reset_decls();
        Scan scan;
        Ctx c;
        char buf[8];
        std::uint32_t len = 0;
        dftu_port_status st = DFTU_PORT_OK;
        const std::uint64_t key = dftu_test_ports_key(scan.a(), "k");

        // No bus bound: outside a scan.
        REQUIRE(dftu_test_ports_try_publish(c.get(), scan.a(), key, "v", 1,
                                            &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_CLOSED);

        trace::views::detail::FoldPortBus bus;
        scan.alpha->bind_port_bus(&bus);
        REQUIRE(dftu_test_ports_try_consume(c.get(), scan.a(), key, buf,
                                            sizeof buf, &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_NOT_READY);
        REQUIRE(dftu_test_ports_try_publish(c.get(), scan.a(), key, "value", 5,
                                            &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        REQUIRE(dftu_test_ports_try_consume(c.get(), scan.a(), key, buf, 2,
                                            &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_TOO_BIG);
        CHECK(len == 5);
        REQUIRE(dftu_test_ports_try_consume(c.get(), scan.a(), key, buf,
                                            sizeof buf, &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        CHECK(dstd::String(buf, len) == "value");
        const dstd::String big(common::constants::plugins::PORT_BATCH_BYTES,
                               'x');
        REQUIRE(dftu_test_ports_try_publish(
                    c.get(), scan.a(), key + 1, big.data(),
                    static_cast<std::uint32_t>(big.size()), &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_FULL);
        scan.alpha->bind_port_bus(nullptr);
    }
}

namespace {

struct PhaseCase {
    dftu_merge_op op;
    dftu_type type;
    dstd::Vector<dstd::Vector<std::uint64_t>> parts;
    dstd::Vector<std::uint64_t> expected;
};

}  // namespace

TEST_SUITE("plugin ports: phase results") {
    TEST_CASE("phase_result merges with each op after the seal") {
        const auto f64 = [](double v) {
            std::uint64_t bits;
            std::memcpy(&bits, &v, 8);
            return bits;
        };
        const auto i64 = [](std::int64_t v) {
            return static_cast<std::uint64_t>(v);
        };
        const PhaseCase cases[] = {
            {DFTU_MERGE_SUM,
             DFTU_T_I64,
             {{i64(5)}, {i64(-2)}, {i64(10)}},
             {i64(13)}},
            {DFTU_MERGE_MIN,
             DFTU_T_I64,
             {{i64(5)}, {i64(-2)}, {i64(10)}},
             {i64(-2)}},
            {DFTU_MERGE_MAX, DFTU_T_U64, {{7}, {3}, {9}}, {9}},
            {DFTU_MERGE_SUM,
             DFTU_T_F64,
             {{f64(1.5)}, {f64(2.25)}},
             {f64(3.75)}},
            {DFTU_MERGE_MAX,
             DFTU_T_F64,
             {{f64(1.5)}, {f64(2.25)}},
             {f64(2.25)}},
            {DFTU_MERGE_UNION, DFTU_T_U64, {{3, 1}, {2, 3}, {9}}, {1, 2, 3, 9}},
        };
        for (const PhaseCase& pc : cases) {
            reset_decls();
            g_alpha.phases = {{"p", pc.op, pc.type, 1}};
            Scan scan;
            Ctx c;
            dftu_port_status st = DFTU_PORT_FULL;
            for (const auto& part : pc.parts) {
                REQUIRE(dftu_test_ports_phase_contribute(
                            c.get(), scan.a(), "p", 1, pc.op, pc.type,
                            part.data(),
                            static_cast<std::uint32_t>(part.size() * 8),
                            &st) == DFTU_OK);
                CHECK(st == DFTU_PORT_OK);
            }
            std::uint64_t out[8] = {};
            std::uint32_t len = 0;
            REQUIRE(dftu_test_ports_phase_result(
                        c.get(), scan.b(), "plugin.alpha.p", 1, out, sizeof out,
                        &len, &st) == DFTU_OK);
            CHECK(st == DFTU_PORT_NOT_READY);
            REQUIRE(dftu_test_ports_phase_seal(c.get(), scan.a(), "p", 1) ==
                    DFTU_OK);
            REQUIRE(dftu_test_ports_phase_result(
                        c.get(), scan.b(), "plugin.alpha.p", 1, out, sizeof out,
                        &len, &st) == DFTU_OK);
            CHECK(st == DFTU_PORT_OK);
            REQUIRE(len == pc.expected.size() * 8);
            for (std::size_t i = 0; i < pc.expected.size(); ++i) {
                CHECK(out[i] == pc.expected[i]);
            }
        }
    }

    TEST_CASE("Concat appends and a sealed generation takes no more") {
        reset_decls();
        g_alpha.phases = {{"p", DFTU_MERGE_CONCAT, DFTU_T_BYTES, 0}};
        Scan scan;
        Ctx c;
        dftu_port_status st = DFTU_PORT_FULL;
        for (const char* s : {"ab", "cd"}) {
            REQUIRE(dftu_test_ports_phase_contribute(
                        c.get(), scan.a(), "p", 1, DFTU_MERGE_CONCAT,
                        DFTU_T_BYTES, s, 2, &st) == DFTU_OK);
            CHECK(st == DFTU_PORT_OK);
        }
        REQUIRE(dftu_test_ports_phase_seal(c.get(), scan.a(), "p", 1) ==
                DFTU_OK);
        REQUIRE(dftu_test_ports_phase_contribute(
                    c.get(), scan.a(), "p", 1, DFTU_MERGE_CONCAT, DFTU_T_BYTES,
                    "ef", 2, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_CLOSED);
        char out[8];
        std::uint32_t len = 0;
        REQUIRE(dftu_test_ports_phase_result(c.get(), scan.a(), "p", 1, out, 2,
                                             &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_TOO_BIG);
        CHECK(len == 4);
        REQUIRE(dftu_test_ports_phase_result(c.get(), scan.a(), "p", 1, out,
                                             sizeof out, &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        CHECK(dstd::String(out, len) == "abcd");
    }

    TEST_CASE("generations seal in order and a wrong merge is refused") {
        reset_decls();
        g_alpha.phases = {{"p", DFTU_MERGE_SUM, DFTU_T_I64, 0},
                          {"p", DFTU_MERGE_SUM, DFTU_T_I64, 0}};
        Scan scan;
        Ctx c;
        CHECK(dftu_test_ports_phase_seal(c.get(), scan.a(), "p", 2) ==
              DFTU_E_INVALID);
        CHECK(dftu_test_ports_phase_seal(c.get(), scan.a(), "p", 1) == DFTU_OK);
        CHECK(dftu_test_ports_phase_seal(c.get(), scan.a(), "p", 2) == DFTU_OK);
        dftu_port_status st = DFTU_PORT_OK;
        const std::int64_t one = 1;
        CHECK(dftu_test_ports_phase_contribute(c.get(), scan.a(), "p", 1,
                                               DFTU_MERGE_MAX, DFTU_T_I64, &one,
                                               8, &st) == DFTU_E_INVALID);
        CHECK(dftu_test_ports_phase_contribute(c.get(), scan.a(), "p", 3,
                                               DFTU_MERGE_SUM, DFTU_T_I64, &one,
                                               8, &st) == DFTU_E_NOT_FOUND);
    }
}

TEST_SUITE("plugin ports: plan-time rules") {
    TEST_CASE("a plan that skips a generation fails") {
        reset_decls();
        g_alpha.phases = {{"p", DFTU_MERGE_SUM, DFTU_T_I64, 3}};
        const dstd::String message = plan_error();
        CHECK(message.find("skip") != dstd::String::npos);
        CHECK(message.find("plugin.alpha.p") != dstd::String::npos);
    }

    TEST_CASE("generations the host assigns, or asks in order, are accepted") {
        reset_decls();
        g_alpha.phases = {{"p", DFTU_MERGE_SUM, DFTU_T_I64, 0},
                          {"p", DFTU_MERGE_SUM, DFTU_T_I64, 2},
                          {"p", DFTU_MERGE_SUM, DFTU_T_I64, 0}};
        CHECK(plan_error().empty());
    }

    TEST_CASE("a duplicate namespace is refused") {
        reset_decls();
        g_alpha.channels = {{"q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {{"r", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        dftu_plugin pa = make_bare_plugin();
        dftu_plugin pb = make_bare_plugin();
        auto built = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&pa, "same", declare_alpha},
             InjectedPlugin{&pb, "same", declare_beta}});
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error().message().find("plugin.same.") !=
              dstd::String::npos);
        CHECK(built.error().message().find("both declare") !=
              dstd::String::npos);
    }

    TEST_CASE("a channel declared twice by one plugin is refused") {
        reset_decls();
        g_alpha.channels = {{"q", 0, 0, DFTU_OVERFLOW_FAIL, 0},
                            {"q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().find("declared twice") != dstd::String::npos);
    }

    TEST_CASE("a name in the host namespace or with a bad letter is refused") {
        reset_decls();
        g_alpha.channels = {{"dftu.q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().find("belongs to the host") != dstd::String::npos);
        reset_decls();
        g_alpha.channels = {{"Q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().find("outside [a-z0-9._/-]") != dstd::String::npos);
    }

    TEST_CASE("Block inside the fused scan stage is refused") {
        reset_decls();
        g_alpha.channels = {
            {"q", OVERFLOW_MODE | TASK_END | SENDS, 0, DFTU_OVERFLOW_BLOCK, 0}};
        g_beta.channels = {{"plugin.alpha.q", OVERFLOW_MODE | RECEIVES, 0,
                            DFTU_OVERFLOW_BLOCK, 0}};
        const dstd::String message = plan_error();
        CHECK(message.find("plugin.alpha.q") != dstd::String::npos);
        CHECK(message.find("Block") != dstd::String::npos);
        CHECK(message.find("fused scan stage") != dstd::String::npos);

        reset_decls();
        g_alpha.channels = {{"q", OVERFLOW_MODE, 0, DFTU_OVERFLOW_BLOCK, 0}};
        CHECK(plan_error().find("fused scan stage") != dstd::String::npos);

        reset_decls();
        g_alpha.channels = {
            {"q", OVERFLOW_MODE | TASK_END | SENDS, 0, DFTU_OVERFLOW_BLOCK, 0}};
        g_beta.channels = {{"plugin.alpha.q",
                            OVERFLOW_MODE | TASK_END | RECEIVES, 0,
                            DFTU_OVERFLOW_BLOCK, 0}};
        CHECK(plan_error().empty());
    }

    TEST_CASE("a ping-pong pair of Fail channels is not a cycle") {
        reset_decls();
        g_alpha.channels = {{"req", 0, 0, DFTU_OVERFLOW_FAIL, 0},
                            {"plugin.beta.resp", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {{"resp", 0, 0, DFTU_OVERFLOW_FAIL, 0},
                           {"plugin.alpha.req", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().empty());
    }

    TEST_CASE("a cycle of Block channels is refused with a message naming it") {
        const std::uint32_t own = OVERFLOW_MODE | TASK_END;
        reset_decls();
        g_alpha.channels = {
            {"x", own | SENDS, 0, DFTU_OVERFLOW_BLOCK, 0},
            {"plugin.beta.y", own | RECEIVES, 0, DFTU_OVERFLOW_BLOCK, 0}};
        g_beta.channels = {
            {"y", own | SENDS, 0, DFTU_OVERFLOW_BLOCK, 0},
            {"plugin.alpha.x", own | RECEIVES, 0, DFTU_OVERFLOW_BLOCK, 0}};
        const dstd::String message = plan_error();
        CHECK(message.find("cycle") != dstd::String::npos);
        CHECK(message.find("'alpha' -[plugin.alpha.x]-> 'beta'") !=
              dstd::String::npos);
        CHECK(message.find("-[plugin.beta.y]-> 'alpha'") != dstd::String::npos);
    }

    TEST_CASE("a Block channel needs the direction of every end") {
        reset_decls();
        const std::uint32_t own = OVERFLOW_MODE | TASK_END;
        g_alpha.channels = {{"q", own | SENDS, 0, DFTU_OVERFLOW_BLOCK, 0}};
        g_beta.channels = {{"plugin.alpha.q", own, 0, DFTU_OVERFLOW_BLOCK, 0}};
        const dstd::String message = plan_error();
        CHECK(message.find("plugin.alpha.q") != dstd::String::npos);
        CHECK(message.find("DFTU_CHANNEL_F_RECEIVES") != dstd::String::npos);
    }

    TEST_CASE(
        "a channel no plugin declares, or declared otherwise, is refused") {
        reset_decls();
        g_beta.channels = {{"plugin.alpha.q", 0, 0, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().find("no loaded plugin declares") !=
              dstd::String::npos);

        reset_decls();
        g_alpha.channels = {{"q", CREDITS, 4, DFTU_OVERFLOW_FAIL, 0}};
        g_beta.channels = {
            {"plugin.alpha.q", CREDITS, 8, DFTU_OVERFLOW_FAIL, 0}};
        CHECK(plan_error().find("its owner") != dstd::String::npos);
    }
}

TEST_SUITE("plugin ports: scan end and batch callbacks") {
    TEST_CASE("an attached scan closes its channels when it ends") {
        dftu_utils_test::TestEnvironment env(40);
        trace::views::ViewFile file =
            test_plugin_common::index_trace(make_trace(env, 40));

        dftu_plugin receiver = make_bare_plugin();
        receiver.on_batch = quiet_on_batch;
        receiver.on_finalize = bg_receiver_on_finalize;
        dftu_plugin sender = make_bare_plugin();
        sender.on_batch = quiet_on_batch;
        auto set = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&receiver, "receiver", declare_receiver},
             InjectedPlugin{&sender, "sender", declare_sender}});
        REQUIRE(set.has_value());

        g_bg_done.store(false);
        g_bg_status = DFTU_PORT_FULL;
        dstd::Vector<trace::views::ViewFile> files;
        files.push_back(file);
        trace::views::View base = trace::views::View::from_files(files);
        const plugins::host::Plugins* attached = &*set;
        auto branch = [attached](trace::views::ViewSession& session) {
            return attach_and_wait(*attached, session);
        };
        dataframe::LazyFrame plan = base.branch(branch);
        Runtime rt(2);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            dftu_utils_test::ok(co_await plan.collect());
            co_return;
        });
        dftu_utils_test::ok(rt.submit(std::move(task), "ports-attach").wait());
        rt.shutdown();

        for (int i = 0; i < 500 && !g_bg_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool woke = g_bg_done.load();
        if (!woke) plugins::cancel_abi_task(g_bg_task.load());
        if (g_bg) g_bg->join();
        g_bg.reset();
        CHECK(woke);
        CHECK(g_bg_status == DFTU_PORT_CLOSED);
    }

    TEST_CASE(
        "a Block send from on_batch fails at once and names the channel") {
        dftu_utils_test::TestEnvironment env(40);
        trace::views::ViewFile file =
            test_plugin_common::index_trace(make_trace(env, 40));

        dftu_plugin receiver = make_bare_plugin();
        receiver.on_batch = quiet_on_batch;
        dftu_plugin sender = make_bare_plugin();
        sender.on_batch = block_sender_on_batch;
        auto set = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&sender, "sender", block_declare_sender},
             InjectedPlugin{&receiver, "receiver", block_declare_receiver}});
        REQUIRE(set.has_value());

        dstd::String message;
        dstd::Vector<trace::views::ViewFile> files;
        files.push_back(file);
        const trace::views::View view = trace::views::View::from_files(files);
        const plugins::host::Plugins* planned = &*set;
        Runtime rt(2);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto body = run_for_message(planned, &view, &message);
            co_await body;
            co_return;
        });
        dftu_utils_test::ok(rt.submit(std::move(task), "ports-block").wait());
        rt.shutdown();
        CHECK(message.find("plugin.sender.q") != dstd::String::npos);
        CHECK(message.find("on_batch") != dstd::String::npos);
    }

    TEST_CASE("a Block send from a state update fails and names the channel") {
        dftu_utils_test::TestEnvironment env(40);
        trace::views::ViewFile file =
            test_plugin_common::index_trace(make_trace(env, 40));

        dftu_plugin sender = make_bare_plugin();
        sender.on_batch = keep_host_on_batch;
        dftu_plugin receiver = make_bare_plugin();
        receiver.on_batch = quiet_on_batch;
        auto set = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&sender, "sender", block_declare_sender},
             InjectedPlugin{&receiver, "receiver", state_declare_receiver}});
        REQUIRE(set.has_value());

        g_kept_host = nullptr;
        dstd::String message;
        dstd::Vector<trace::views::ViewFile> files;
        files.push_back(file);
        const trace::views::View view = trace::views::View::from_files(files);
        const plugins::host::Plugins* planned = &*set;
        Runtime rt(2);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto body = run_for_message(planned, &view, &message);
            co_await body;
            co_return;
        });
        dftu_utils_test::ok(rt.submit(std::move(task), "ports-state").wait());
        rt.shutdown();
        g_kept_host = nullptr;
        CHECK(message.find("plugin.sender.q") != dstd::String::npos);
    }

    TEST_CASE("a Block sender listed after its receiver runs first") {
        dftu_plugin receiver = make_bare_plugin();
        dftu_plugin sender = make_bare_plugin();
        auto set = plugins::host::build_injected_plugins_declaring(
            {InjectedPlugin{&receiver, "receiver", block_declare_receiver},
             InjectedPlugin{&sender, "sender", block_declare_sender}});
        REQUIRE(set.has_value());
        const dstd::Vector<std::size_t> order = plugins::host::fold_order(*set);
        REQUIRE(order.size() == 2);
        CHECK(order[0] == 1);
        CHECK(order[1] == 0);
    }
}

TEST_SUITE("plugin ports: latest and phase under threads") {
    TEST_CASE("sets from many threads get distinct generations and merge") {
        reset_decls();
        g_alpha.phases = {{"total", DFTU_MERGE_SUM, DFTU_T_I64, 0}};
        Scan scan;
        constexpr int THREADS = 4;
        constexpr int EACH = 500;
        static std::uint64_t generations[THREADS][EACH];
        {
            dstd::Vector<dstd::Thread> workers;
            for (int t = 0; t < THREADS; ++t) {
                workers.emplace_back([&scan, t] {
                    Ctx c;
                    for (int i = 0; i < EACH; ++i) {
                        const std::int64_t one = 1;
                        dftu_port_status st = DFTU_PORT_OK;
                        dftu_test_ports_latest_set(c.get(), scan.a(), "lv",
                                                   &one, 8, &generations[t][i]);
                        dftu_test_ports_phase_contribute(
                            c.get(), scan.a(), "total", 1, DFTU_MERGE_SUM,
                            DFTU_T_I64, &one, 8, &st);
                    }
                });
            }
        }
        Fixed<bool, THREADS * EACH + 1> seen;
        for (int t = 0; t < THREADS; ++t) {
            for (int i = 0; i < EACH; ++i) {
                const std::uint64_t g = generations[t][i];
                REQUIRE(g >= 1);
                REQUIRE(g <= THREADS * EACH);
                CHECK_FALSE(seen.items[g]);
                seen.items[g] = true;
            }
        }
        Ctx c;
        REQUIRE(dftu_test_ports_phase_seal(c.get(), scan.a(), "total", 1) ==
                DFTU_OK);
        std::int64_t total = 0;
        std::uint32_t len = 0;
        dftu_port_status st = DFTU_PORT_OK;
        REQUIRE(dftu_test_ports_phase_result(c.get(), scan.a(), "total", 1,
                                             &total, 8, &len, &st) == DFTU_OK);
        CHECK(st == DFTU_PORT_OK);
        CHECK(total == THREADS * EACH);
    }
}
