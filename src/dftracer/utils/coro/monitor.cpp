// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/sharded_mutex.h>
#include <dftracer/utils/common/symbolize.h>
#include <dftracer/utils/coro/monitor.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/ptr_hash.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dftracer::utils::coro {

namespace detail {
bool g_monitor_deep = false;
}

namespace {

enum class Mode { Off, Summary, Tree, Trace, Deep };

using Clock = std::chrono::steady_clock;

// A coroutine still running, tracked by frame address. Zero strings on the hot
// path: only the resume-function pointer is kept, symbolized later at render.
struct Live {
    long long id;
    long long parent;
    const void* resume_fn;
    Clock::time_point start;
    CoroKind kind;
    int spawn_tid;  // worker that first registered it
};

// A finished coroutine, kept for the end-of-run report.
struct Done {
    long long id;
    long long parent;
    const void* resume_fn;
    long long wall_us;
    CoroKind kind;
    int spawn_tid;   // worker that registered it
    int finish_tid;  // worker of the final resume (-1 if never finished)
};

// Per-shard state: the live coroutine set plus the finished records collected
// for the end-of-run report. Both live under one shard lock, so a completion
// (erase live + append done) takes a single lock, not two.
struct ShardData {
    ankerl::unordered_dense::map<const void*, Live, dstd::PtrHash> live;
    dstd::Vector<Done> done;
};

struct Registry {
    common::ShardedMutex<ShardData, 64> shards;
    std::atomic<long long> next_id{0};
    Mode mode = Mode::Off;
    dstd::ScopedFile csv;
    long long min_us = 0;  // hide coroutines shorter than this in the report
};

Registry& registry() { return dstd::Singleton<Registry>::get(); }

std::size_t shard_key(const void* handle) { return dstd::PtrHash{}(handle); }

// The coroutine currently resuming on this worker thread (its monitor id), so
// work it enqueues attributes as a child. -1 = none/untracked.
thread_local long long t_current = -1;

// This thread's executor worker id (set once per worker), -1 on other threads.
thread_local int t_worker = -1;

Mode parse_mode() {
    const auto v = common::Env::get<dstd::StringView>("DFTRACER_UTILS_MONITOR");
    const auto file =
        common::Env::get<dstd::StringView>("DFTRACER_UTILS_MONITOR_FILE");
    if (!v.has_value() && !file.has_value()) return Mode::Off;
    if (v.has_value()) {
        if (*v == "tree") return Mode::Tree;
        if (*v == "deep") return Mode::Deep;
        if (*v == "trace") return Mode::Trace;
        if ((*v == "0" || *v == "false") && !file.has_value()) return Mode::Off;
    }
    return Mode::Summary;
}

// Resolve each resume_fn once; many coroutine instances share one function.
dstd::String label(
    const void* fn,
    dstd::HashMap<const void*, dstd::String, dstd::PtrHash>& cache) {
    auto it = cache.find(fn);
    if (it != cache.end()) return it->second;
    return cache.emplace(fn, common::symbolize_function(fn)).first->second;
}

const char* kind_str(CoroKind k) {
    switch (k) {
        case CoroKind::Task:
            return "task";
        case CoroKind::Spawn:
            return "spawn";
        case CoroKind::Io:
            return "io";
        case CoroKind::Sync:
            return "sync";
    }
    return "?";
}

// "@t3" if the coroutine stayed on one worker, "@t3>t7" if it was registered on
// 3 but finished on 7 (work stealing / migration). Empty if no worker known.
dstd::String thread_str(int spawn_tid, int finish_tid) {
    if (spawn_tid < 0 && finish_tid < 0) return "";
    if (spawn_tid >= 0 && finish_tid >= 0 && spawn_tid != finish_tid) {
        return " @t" + dstd::to_string(spawn_tid) + ">t" +
               dstd::to_string(finish_tid);
    }
    return " @t" + dstd::to_string(finish_tid >= 0 ? finish_tid : spawn_tid);
}

void render_table(const dstd::Vector<Done>& recs) {
    dstd::HashMap<const void*, dstd::String, dstd::PtrHash> cache;
    struct Agg {
        long long count = 0, total = 0, min = 0, max = 0;
    };
    dstd::HashMap<dstd::String, Agg> by;
    for (const Done& d : recs) {
        Agg& a = by[dstd::String("[") + kind_str(d.kind) + "] " +
                    label(d.resume_fn, cache)];
        if (a.count == 0) {
            a.min = d.wall_us;
            a.max = d.wall_us;
        }
        ++a.count;
        a.total += d.wall_us;
        a.min = std::min(a.min, d.wall_us);
        a.max = std::max(a.max, d.wall_us);
    }
    dstd::Vector<dstd::Pair<dstd::String, Agg>> rows(by.begin(), by.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.total > b.second.total;
    });
    std::fprintf(stderr, "\n=== dftracer-utils monitor summary ===\n");
    std::fprintf(stderr, "%8s %12s %10s %10s %10s  %s\n", "count", "total(ms)",
                 "avg(us)", "min(us)", "max(us)", "coroutine");
    for (const auto& [name, a] : rows) {
        const double avg = a.count ? static_cast<double>(a.total) /
                                         static_cast<double>(a.count)
                                   : 0.0;
        std::fprintf(stderr, "%8lld %12.3f %10.1f %10lld %10lld  %s\n", a.count,
                     static_cast<double>(a.total) / 1000.0, avg, a.min, a.max,
                     name.c_str());
    }
}

void render_tree_node(
    const dstd::Vector<Done>& recs,
    const dstd::HashMap<long long, dstd::Vector<std::size_t>>& children,
    dstd::HashMap<const void*, dstd::String, dstd::PtrHash>& cache,
    std::size_t idx, int depth) {
    const Done& d = recs[idx];
    std::fprintf(stderr, "%*s[%s] %s%s [%.3f ms]\n", depth * 2, "",
                 kind_str(d.kind), label(d.resume_fn, cache).c_str(),
                 thread_str(d.spawn_tid, d.finish_tid).c_str(),
                 static_cast<double>(d.wall_us) / 1000.0);
    auto it = children.find(d.id);
    if (it != children.end()) {
        for (std::size_t c : it->second) {
            render_tree_node(recs, children, cache, c, depth + 1);
        }
    }
}

void render_tree(const dstd::Vector<Done>& recs) {
    std::fprintf(stderr, "\n=== dftracer-utils monitor coroutine tree ===\n");
    dstd::HashMap<const void*, dstd::String, dstd::PtrHash> cache;
    dstd::HashMap<long long, dstd::Vector<std::size_t>> children;
    dstd::HashMap<long long, std::size_t> id_to_idx;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        children[recs[i].parent].push_back(i);
        id_to_idx[recs[i].id] = i;
    }
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const bool is_root = recs[i].parent < 0 ||
                             id_to_idx.find(recs[i].parent) == id_to_idx.end();
        if (is_root) render_tree_node(recs, children, cache, i, 0);
    }
}

void at_exit_render() {
    Registry& r = registry();
    const auto now = Clock::now();
    dstd::Vector<Done> all;
    // Drain finished records plus any coroutines still live at exit (e.g. the
    // top-level task, completed outside the worker loop) so parents appear and
    // children nest instead of orphaning into roots.
    r.shards.for_each_shard([&](ShardData& s) {
        all.insert(all.end(), s.done.begin(), s.done.end());
        for (const auto& entry : s.live) {
            const auto& l = entry.second;
            const long long us =
                std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                      l.start)
                    .count();
            all.push_back(
                Done{l.id, l.parent, l.resume_fn, us, l.kind, l.spawn_tid, -1});
        }
    });
    r.csv.reset();
    // Hide tiny coroutines. A parent's wall time always covers its children's,
    // so the kept set is upward-closed and the tree stays connected.
    if (r.min_us > 0) {
        all.erase(
            std::remove_if(all.begin(), all.end(),
                           [&](const Done& d) { return d.wall_us < r.min_us; }),
            all.end());
    }
    if (all.empty()) return;
    // Stable ordering by id so the tree/table are deterministic across shards.
    std::sort(all.begin(), all.end(),
              [](const Done& a, const Done& b) { return a.id < b.id; });
    if (r.mode == Mode::Tree || r.mode == Mode::Deep) {
        render_tree(all);
    } else {
        render_table(all);
    }
}

}  // namespace

bool monitoring_enabled() {
    static const bool enabled = [] {
        Registry& r = registry();
        r.mode = parse_mode();
        if (r.mode == Mode::Off) return false;
        const auto file =
            common::Env::get<dstd::StringView>("DFTRACER_UTILS_MONITOR_FILE");
        if (file.has_value()) {
            r.csv.reset(std::fopen(dstd::String(*file).c_str(), "w"));
            if (r.csv) {
                std::fprintf(
                    r.csv.get(),
                    "id,parent,kind,coroutine,spawn_tid,finish_tid,micros\n");
            }
        }
        detail::g_monitor_deep = (r.mode == Mode::Deep);
        const auto min =
            common::Env::get<dstd::StringView>("DFTRACER_UTILS_MONITOR_MIN_US");
        if (min.has_value()) {
            r.min_us = std::strtoll(dstd::String(*min).c_str(), nullptr, 10);
        }
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches,
                                at_exit_render);
        return true;
    }();
    return enabled;
}

void monitor_set_worker(int worker_id) { t_worker = worker_id; }

long long monitor_enqueue(const void* handle, CoroKind kind) {
    Registry& r = registry();
    long long id = -1;
    r.shards.with_shard(shard_key(handle), [&](ShardData& s) {
        auto it = s.live.find(handle);
        if (it != s.live.end()) {
            id = it->second.id;  // re-enqueue / re-seen
            return;
        }
        // ABI: the first pointer-sized word of a coroutine frame is its resume
        // fn.
        const void* resume_fn = *reinterpret_cast<const void* const*>(handle);
        id = r.next_id.fetch_add(1, std::memory_order_relaxed);
        s.live.emplace(handle, Live{id, t_current, resume_fn, Clock::now(),
                                    kind, t_worker});
    });
    return id;
}

void monitor_resume_begin(long long id) { t_current = id; }

namespace {

// Record a finished coroutine (trace/csv/done) and erase it from live. Returns
// its parent id. Caller holds the shard lock.
template <typename It>
long long record_and_erase(Registry& r, ShardData& s, It it, int finish_tid) {
    const Live& l = it->second;
    const long long parent = l.parent;
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(
                             Clock::now() - l.start)
                             .count();
    if (r.mode == Mode::Trace) {
        std::fprintf(stderr, "[monitor] %s [%.3f ms]\n",
                     common::symbolize_function(l.resume_fn).c_str(),
                     static_cast<double>(us) / 1000.0);
    }
    if (r.csv) {
        std::fprintf(r.csv.get(), "%lld,%lld,%s,%s,%d,%d,%lld\n", l.id,
                     l.parent, kind_str(l.kind),
                     common::symbolize_function(l.resume_fn).c_str(),
                     l.spawn_tid, finish_tid, us);
    }
    s.done.push_back(
        Done{l.id, l.parent, l.resume_fn, us, l.kind, l.spawn_tid, finish_tid});
    s.live.erase(it);
    return parent;
}

}  // namespace

void monitor_resume_end(const void* handle, bool done) {
    t_current = -1;
    if (!done) return;
    Registry& r = registry();
    r.shards.with_shard(shard_key(handle), [&](ShardData& s) {
        auto it = s.live.find(handle);
        if (it == s.live.end()) return;
        record_and_erase(r, s, it, t_worker);
    });
}

void monitor_sync_complete(const void* handle) {
    Registry& r = registry();
    long long parent = -1;
    r.shards.with_shard(shard_key(handle), [&](ShardData& s) {
        auto it = s.live.find(handle);
        if (it == s.live.end()) return;  // already finalized
        parent = record_and_erase(r, s, it, t_worker);
    });
    t_current = parent;  // restore the awaiting coroutine as current
}

}  // namespace dftracer::utils::coro
