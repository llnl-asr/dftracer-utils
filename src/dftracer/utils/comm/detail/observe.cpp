// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/detail/observe.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::comm::detail {

namespace {

constexpr std::memory_order RELAXED = std::memory_order_relaxed;

dstd::StringView end_name(EndKind end) noexcept {
    switch (end) {
        case EndKind::Ok:
            return "ok";
        case EndKind::Failed:
            return "failed";
        case EndKind::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

std::atomic<std::uint64_t> g_next_tracer{0};

/// A counter that one thread at a time writes: no read-modify-write.
void bump_locked(std::atomic<std::uint64_t>& counter,
                 std::uint64_t by) noexcept {
    counter.store(counter.load(RELAXED) + by, RELAXED);
}

void add_rows(const KindTable& table, std::uint64_t group,
              dstd::Vector<CallStats>& rows) {
    for (std::size_t at = 0; at < CALL_KIND_COUNT; ++at) {
        CallStats row;
        row.group = group;
        row.kind = call_name(static_cast<CallKind>(at));
        const EndCounters& ended = table.ended[at];
        row.started = table.started[at].load(RELAXED);
        row.ok = ended.ok.load(RELAXED);
        row.failed = ended.failed.load(RELAXED);
        row.cancelled = ended.cancelled.load(RELAXED);
        row.bytes_sent = ended.bytes_sent.load(RELAXED);
        row.bytes_received = ended.bytes_received.load(RELAXED);
        if (row.started == 0 && row.ok == 0 && row.failed == 0 &&
            row.cancelled == 0)
            continue;
        rows.push_back(row);
    }
}

}  // namespace

dstd::SharedPtr<Tracer> Tracer::make(dstd::StringView directory) {
    if (directory.empty()) return {};
    const auto made = dstd::fs::create_directories(dstd::PathView(directory));
    if (!made.has_value()) {
        DFTRACER_UTILS_LOG_WARN("comm trace: cannot make the directory {}: {}",
                                directory, made.error().message());
        return {};
    }
    return dstd::make_shared<Tracer>(dstd::String(directory),
                                     g_next_tracer.fetch_add(1, RELAXED));
}

Tracer::Tracer(dstd::String directory, std::uint64_t number)
    : directory_(std::move(directory)),
      pid_(static_cast<std::uint64_t>(::getpid())),
      number_(number) {}

Tracer::~Tracer() { flush(); }

void Tracer::event(std::uint32_t world_rank, CallKind kind, std::uint64_t group,
                   std::uint64_t seq, std::uint64_t dur_ns, std::uint64_t bytes,
                   EndKind end) {
    const std::uint64_t now_ns = dstd::system_now_ns();
    const std::uint64_t begin_ns = now_ns > dur_ns ? now_ns - dur_ns : 0;
    const std::uint64_t tid = dstd::Thread::current_id().hash();
    const std::uint64_t dur_us =
        std::max<std::uint64_t>(1, (dur_ns + 999) / 1000);
    std::lock_guard<std::mutex> lock(mutex_);
    Sink& sink = sinks_[world_rank];
    if (sink.path.empty()) {
        sink.path = dstd::fmt<"{}/comm-{}-{}-{}.pfw">(directory_, world_rank,
                                                      pid_, number_);
    }
    dstd::fmt_append<
        R"({"id":{},"name":"{}","cat":"comm","pid":{},"tid":{},"ts":{},"dur":{},"ph":"X",)">(
        sink.buffer, next_id_++, call_name(kind), pid_, tid, begin_ns / 1000,
        dur_us);
    dstd::fmt_append<
        R"("args":{"group":{},"seq":{},"bytes":{},"status":"{}"}})">(
        sink.buffer, group, seq, bytes, end_name(end));
    sink.buffer.push_back('\n');
    if (sink.buffer.size() >=
        common::constants::comm_settings::COMM_TRACE_FLUSH_BYTES)
        write_locked(sink);
}

void Tracer::warn_once(dstd::StringView path, const dstd::Error& why) {
    if (warned_) return;
    warned_ = true;
    DFTRACER_UTILS_LOG_WARN("comm trace: cannot write {}: {}", path,
                            why.message());
}

void Tracer::write_locked(Sink& sink) {
    if (sink.buffer.empty() && sink.started) return;
    if (!sink.started) {
        const auto first =
            dstd::fs::write_file(dstd::PathView(sink.path), "[\n");
        if (!first.has_value()) {
            warn_once(sink.path, first.error());
            sink.buffer.clear();
            return;
        }
        sink.started = true;
    }
    if (!sink.buffer.empty()) {
        const auto more =
            dstd::fs::append_file(dstd::PathView(sink.path), sink.buffer);
        if (!more.has_value()) warn_once(sink.path, more.error());
        sink.buffer.clear();
    }
}

void Tracer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : sinks_) write_locked(entry.second);
}

Tally::~Tally() {
    if (!observer) return;
    KindTable& to = observer->retired;
    for (std::size_t at = 0; at < CALL_KIND_COUNT; ++at) {
        auto add = [](std::atomic<std::uint64_t>& into,
                      const std::atomic<std::uint64_t>& from) {
            const std::uint64_t value = from.load(RELAXED);
            if (value != 0) into.fetch_add(value, RELAXED);
        };
        add(to.started[at], table.started[at]);
        const EndCounters& was = table.ended[at];
        EndCounters& now = to.ended[at];
        add(now.ok, was.ok);
        add(now.failed, was.failed);
        add(now.cancelled, was.cancelled);
        add(now.bytes_sent, was.bytes_sent);
        add(now.bytes_received, was.bytes_received);
    }
}

CallMeta make_meta(Tally* tally, CallKind kind, std::uint32_t world_rank,
                   bool batched) noexcept {
    CallMeta meta;
    if (tally == nullptr) return meta;
    meta.tally = tally;
    meta.kind = kind;
    meta.world_rank = world_rank;
    meta.batched = batched && !tally->traced;
    if (tally->traced) meta.start_ns = dstd::steady_now_ns();
    return meta;
}

void count_start_locked(Tally& tally, CallKind kind) noexcept {
    bump_locked(tally.table.started[static_cast<std::size_t>(kind)], 1);
}

CallMeta begin_call(Tally* tally, CallKind kind, std::uint32_t world_rank) {
    CallMeta meta = make_meta(tally, kind, world_rank);
    if (tally != nullptr)
        tally->table.started[static_cast<std::size_t>(kind)].fetch_add(1,
                                                                       RELAXED);
    return meta;
}

std::uint64_t call_age_ns(const CallMeta& meta,
                          std::uint64_t slot_start_ns) noexcept {
    if (meta.tally == nullptr) return 0;
    if (meta.start_ns != 0) {
        const std::uint64_t now = dstd::steady_now_ns();
        return now > meta.start_ns ? now - meta.start_ns : 0;
    }
    if (slot_start_ns == 0) return 0;
    const std::uint64_t now = dstd::coarse_now_ns();
    return now > slot_start_ns ? now - slot_start_ns : 0;
}

void end_call(const CallMeta& meta, EndKind end, std::uint64_t bytes_sent,
              std::uint64_t bytes_received) noexcept {
    Tally& tally = *meta.tally;
    if (end == EndKind::Ok) {
        if (meta.batched) return;
        count_batch(tally, meta.kind, 1, bytes_sent, bytes_received);
    } else {
        EndCounters& counters =
            tally.table.ended[static_cast<std::size_t>(meta.kind)];
        (end == EndKind::Failed ? counters.failed : counters.cancelled)
            .fetch_add(1, RELAXED);
    }
    if (tally.traced) {
        tally.observer->tracer->event(meta.world_rank, meta.kind, tally.group,
                                      meta.seq,
                                      dstd::steady_now_ns() - meta.start_ns,
                                      bytes_sent + bytes_received, end);
    }
}

void count_batch_locked(Tally& tally, CallKind kind, std::uint64_t ok,
                        std::uint64_t bytes_sent,
                        std::uint64_t bytes_received) noexcept {
    EndCounters& counters = tally.table.ended[static_cast<std::size_t>(kind)];
    bump_locked(counters.ok, ok);
    if (bytes_sent != 0) bump_locked(counters.bytes_sent, bytes_sent);
    if (bytes_received != 0)
        bump_locked(counters.bytes_received, bytes_received);
}

void count_batch(Tally& tally, CallKind kind, std::uint64_t ok,
                 std::uint64_t bytes_sent,
                 std::uint64_t bytes_received) noexcept {
    EndCounters& counters = tally.table.ended[static_cast<std::size_t>(kind)];
    counters.ok.fetch_add(ok, RELAXED);
    if (bytes_sent != 0) counters.bytes_sent.fetch_add(bytes_sent, RELAXED);
    if (bytes_received != 0)
        counters.bytes_received.fetch_add(bytes_received, RELAXED);
}

void add_stats(const Tally& tally, dstd::Vector<CallStats>& rows) {
    add_rows(tally.table, tally.group, rows);
}

void add_retired(const Observer& observer, dstd::Vector<CallStats>& rows) {
    add_rows(observer.retired, 0, rows);
}

}  // namespace dftracer::utils::comm::detail
