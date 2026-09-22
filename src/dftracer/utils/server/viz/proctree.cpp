#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/json/json_doc_guard.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz/calltree.h>
#include <dftracer/utils/server/viz/density.h>
#include <dftracer/utils/server/viz/handlers.h>
#include <dftracer/utils/server/viz/internal.h>
#include <dftracer/utils/server/viz/scan.h>
#include <dftracer/utils/server/viz/summary_build.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::views;
using dftracer::utils::json::json_number;

// Process-spawning calls: dftracer POSIX (exact "fork"/"clone"/...) and kernel
// syscalls ("__arm64_sys_clone"). Excludes library helpers like ibv_*fork* and
// register_tm_clones, which contain "fork"/"clone" but don't spawn.

// One process in the proctree response. `host` borrows the hostname table;
// `rank` is null when the trace carries no "PR" metadata for the pid, and the
// key is then omitted.
struct ProcNode {
    std::int64_t pid;
    std::int64_t parent;
    std::uint64_t spawn_ts;
    std::uint64_t first_ts;
    std::string_view host;
    std::uint64_t bytes;
    std::uint64_t io_ops;
    double io_busy;
    const std::string* rank;
};

template <typename builder_type>
void tag_invoke(simdjson::serialize_tag, builder_type& b, const ProcNode& n) {
    b.start_object();
    b.append_key_value("pid", n.pid);
    b.append_comma();
    b.append_key_value("parent", n.parent);
    b.append_comma();
    b.append_key_value("spawn_ts", n.spawn_ts);
    b.append_comma();
    b.append_key_value("first_ts", n.first_ts);
    b.append_comma();
    b.append_key_value("host", n.host);
    b.append_comma();
    b.append_key_value("bytes", n.bytes);
    b.append_comma();
    b.append_key_value("io_ops", n.io_ops);
    b.append_comma();
    b.append_key_value("io_busy", n.io_busy);
    if (n.rank) {
        b.append_comma();
        b.append_key_value("rank", *n.rank);
    }
    b.end_object();
}

// GET /api/viz/proctree: infer the process fork hierarchy. The traces record
// the fork/clone in the parent but not the child pid, so link each process to
// the nearest preceding clone in another process (child start follows the clone
// by microseconds). Respects ?file= for per-node trees on multi-node traces.
coro::CoroTask<HttpResponse> handle_viz_proctree(const HttpRequest& req,
                                                 const QueryParams& params,
                                                 TraceIndex& index) {
    std::uint64_t gmin = index.global_min_timestamp_us();
    std::uint64_t gmax = index.global_max_timestamp_us();
    if (gmin == std::numeric_limits<std::uint64_t>::max() || gmax <= gmin)
        co_return HttpResponse::ok("{\"nodes\":[]}");

    auto ts_norm_param = params.get("ts_normalize");
    bool normalize = ts_norm_param.empty() || ts_norm_param != "0";
    std::uint64_t base = normalize ? gmin : 0;

    struct Acc {
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> first_ts;
        // Explicit parent from the process metadata's args.ppid.
        ankerl::unordered_dense::map<std::int64_t, std::int64_t> ppid;
        // (ts, parent_pid, child_pid): child_pid is args.ret when the fork
        // event records it (dftracer POSIX), else -1 to fall back to inference.
        std::vector<std::tuple<std::uint64_t, std::int64_t, std::int64_t>>
            forks;
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> bytes;
        // I/O operation count and busy time (us) per process, over I/O-category
        // (POSIX/STDIO/IO) events only.
        ankerl::unordered_dense::map<std::int64_t, std::uint64_t> io_ops;
        ankerl::unordered_dense::map<std::int64_t, double> io_busy;
        ankerl::unordered_dense::map<std::int64_t, std::string> pid_hhash;
        // pid -> rank, from "PR" metadata (args.name == "rank").
        ankerl::unordered_dense::map<std::int64_t, std::string> rank;
        dftracer::utils::StringViewMap<std::string> hh;
    };
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    std::vector<Acc> accs(slots);
    auto on_batch = [&accs](std::size_t w,
                            const std::vector<std::string_view>& events) {
        thread_local simdjson::dom::parser parser;
        thread_local std::string buf;
        Acc& acc = accs[w];
        for (auto ev : events) {
            buf.assign(ev);
            auto res = parser.parse(buf);
            if (res.error()) continue;
            auto root = res.value_unsafe();
            EventScalars s;
            if (!parse_event_scalars(root, s)) continue;

            // HH metadata (hhash -> hostname) carries no ts.
            if (s.name == "HH") {
                auto a = root["args"];
                if (!a.error() && a.is_object()) {
                    auto v = a["value"];
                    auto n = a["name"];
                    if (!v.error() && v.is_string() && !n.error() &&
                        n.is_string())
                        acc.hh.emplace(
                            std::string(v.get_string().value_unsafe()),
                            std::string(n.get_string().value_unsafe()));
                }
                continue;
            }
            // PR metadata (pid -> rank) also carries no ts.
            if (s.name == "PR") {
                auto a = root["args"];
                if (!a.error() && a.is_object() && s.has_pid) {
                    auto an = a["name"];
                    auto av = a["value"];
                    if (!an.error() && an.is_string() &&
                        an.get_string().value_unsafe() == "rank" &&
                        !av.error() && av.is_string())
                        acc.rank.emplace(
                            s.pid, std::string(av.get_string().value_unsafe()));
                }
                continue;
            }
            if (!s.has_pid || !s.has_ts) continue;
            const std::int64_t pid = s.pid;
            const auto ts = static_cast<std::uint64_t>(s.ts);
            auto it = acc.first_ts.find(pid);
            if (it == acc.first_ts.end())
                acc.first_ts.emplace(pid, ts);
            else if (ts < it->second)
                it->second = ts;

            std::int64_t child = -1;
            double ret = 0;
            auto args = root["args"];
            if (!args.error() && args.is_object()) {
                auto ppr = args["ppid"];
                if (!ppr.error()) {
                    auto pp = static_cast<std::int64_t>(
                        json_number(ppr.value_unsafe()));
                    if (pp > 0 && pp != pid) acc.ppid[pid] = pp;
                }
                auto rr = args["ret"];
                if (!rr.error()) {
                    ret = json_number(rr.value_unsafe());
                    child = static_cast<std::int64_t>(ret);
                }
                auto hr = args["hhash"];
                if (!hr.error() && hr.is_string() &&
                    acc.pid_hhash.find(pid) == acc.pid_hhash.end())
                    acc.pid_hhash.emplace(
                        pid, std::string(hr.get_string().value_unsafe()));
            }
            // I/O bytes per process: args.ret on read/write ops.
            if (ret > 0 && (s.name.find("read") != std::string_view::npos ||
                            s.name.find("write") != std::string_view::npos))
                acc.bytes[pid] += static_cast<std::uint64_t>(ret);

            if (s.cat == "POSIX" || s.cat == "STDIO" || s.cat == "IO") {
                acc.io_ops[pid] += 1;
                if (s.has_dur) acc.io_busy[pid] += s.dur;
            }

            if (!s.name.empty() && is_fork_syscall(s.name))
                acc.forks.emplace_back(ts, pid, child > 0 ? child : -1);
        }
    };

    // The per-process totals are exact and whole-trace, so the summary answers
    // this identically to a scan; only a single-file view has to scan.
    bool single_file = !params.get("file").empty();
    const VizSummary* summary =
        single_file ? nullptr : co_await ensure_viz_summary(index);
    if (summary != nullptr) {
        Acc& acc = accs[0];
        for (const auto& p : summary->procs) {
            if (p.first_ts != 0) acc.first_ts.emplace(p.pid, p.first_ts);
            if (p.ppid > 0) acc.ppid.emplace(p.pid, p.ppid);
            if (p.bytes != 0) acc.bytes.emplace(p.pid, p.bytes);
            if (p.io_ops != 0) acc.io_ops.emplace(p.pid, p.io_ops);
            if (p.io_busy != 0) acc.io_busy.emplace(p.pid, p.io_busy);
            if (!p.hhash.empty()) acc.pid_hhash.emplace(p.pid, p.hhash);
            if (!p.rank.empty()) acc.rank.emplace(p.pid, p.rank);
        }
        for (const auto& f : summary->forks)
            acc.forks.emplace_back(f.ts, f.pid, f.child);
        for (const auto& h : summary->hosts) acc.hh.emplace(h.first, h.second);
    } else {
        auto files =
            select_viz_target_files(index, params, static_cast<double>(gmin),
                                    static_cast<double>(gmax));
        co_await views::View::from_files(to_view_files(files),
                                         &index.bloom_cache())
            .phase(views::Phase::Events)
            .cancel_when([&req]() { return req.cancel_token.cancelled(); })
            .for_each_batch(on_batch, slots);
    }

    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> first_ts;
    ankerl::unordered_dense::map<std::int64_t, std::int64_t> parent_of;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> spawn_of;
    std::vector<std::pair<std::uint64_t, std::int64_t>> inf_forks;  // (ts, pid)
    for (auto& a : accs) {
        for (auto& kv : a.first_ts) {
            auto it = first_ts.find(kv.first);
            if (it == first_ts.end())
                first_ts.emplace(kv.first, kv.second);
            else if (kv.second < it->second)
                it->second = kv.second;
        }
        // Explicit edges from the fork event's args.ret (child pid + spawn ts).
        for (auto& [ts, ppid, child] : a.forks) {
            if (child > 0) {
                parent_of[child] = ppid;
                spawn_of[child] = ts;
            } else {
                inf_forks.emplace_back(ts, ppid);
            }
        }
    }
    // args.ppid metadata fills in any process not linked by a fork event.
    for (auto& a : accs)
        for (auto& kv : a.ppid)
            if (parent_of.find(kv.first) == parent_of.end())
                parent_of.emplace(kv.first, kv.second);
    std::sort(inf_forks.begin(), inf_forks.end());

    // Merge host (hhash -> hostname resolved) and I/O bytes per process.
    dftracer::utils::StringViewMap<std::string> hh;
    ankerl::unordered_dense::map<std::int64_t, std::string> pid_hhash;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> bytes;
    ankerl::unordered_dense::map<std::int64_t, std::uint64_t> io_ops;
    ankerl::unordered_dense::map<std::int64_t, double> io_busy;
    ankerl::unordered_dense::map<std::int64_t, std::string> rank;
    for (auto& a : accs) {
        for (auto& kv : a.hh) hh.emplace(kv.first, kv.second);
        for (auto& kv : a.pid_hhash) pid_hhash.emplace(kv.first, kv.second);
        for (auto& kv : a.bytes) bytes[kv.first] += kv.second;
        for (auto& kv : a.io_ops) io_ops[kv.first] += kv.second;
        for (auto& kv : a.io_busy) io_busy[kv.first] += kv.second;
        for (auto& kv : a.rank) rank.emplace(kv.first, kv.second);
    }

    std::vector<std::pair<std::uint64_t, std::int64_t>>
        procs;  // (first_ts, pid)
    procs.reserve(first_ts.size());
    for (auto& kv : first_ts) procs.emplace_back(kv.second, kv.first);
    std::sort(procs.begin(), procs.end());

    // Time-inference fallback (traces without args.ret/ppid): link a process to
    // the nearest preceding clone in another process.
    std::vector<bool> used(inf_forks.size(), false);
    std::vector<ProcNode> nodes;
    nodes.reserve(procs.size());
    const double proc_dur_us =
        dftracer::utils::trace::time_metric_us_scale(index.time_metric());
    for (auto& [fts, pid] : procs) {
        std::int64_t parent = -1;
        std::uint64_t spawn_ts = 0;
        auto pit = parent_of.find(pid);
        auto sit = spawn_of.find(pid);
        // A fork cannot spawn a child that already existed before it: reject
        // such edges (pid reuse across runs) and fall back to time inference.
        bool valid = pit != parent_of.end() &&
                     (sit == spawn_of.end() || sit->second <= fts);
        if (valid) {
            parent = pit->second;
            if (sit != spawn_of.end()) spawn_ts = sit->second - base;
        } else {
            auto hi = std::upper_bound(
                inf_forks.begin(), inf_forks.end(),
                std::make_pair(fts, std::numeric_limits<std::int64_t>::max()));
            for (auto it = hi; it != inf_forks.begin();) {
                --it;
                auto idx = static_cast<std::size_t>(it - inf_forks.begin());
                if (!used[idx] && it->second != pid) {
                    used[idx] = true;
                    parent = it->second;
                    spawn_ts = it->first - base;
                    break;
                }
            }
        }
        std::string_view host;
        auto hp = pid_hhash.find(pid);
        if (hp != pid_hhash.end()) {
            auto hn = hh.find(hp->second);
            if (hn != hh.end()) host = hn->second;
        }
        auto bp = bytes.find(pid);
        auto op = io_ops.find(pid);
        auto ib = io_busy.find(pid);
        auto rk = rank.find(pid);
        nodes.push_back({pid, parent, index.native_to_us(spawn_ts),
                         index.native_to_us(fts > base ? fts - base : 0), host,
                         bp != bytes.end() ? bp->second : 0,
                         op != io_ops.end() ? op->second : 0,
                         (ib != io_busy.end() ? ib->second : 0.0) * proc_dur_us,
                         rk != rank.end() ? &rk->second : nullptr});
    }

    auto& sb = scratch_json_builder();
    sb.start_object();
    sb.append_key_value("nodes", nodes);
    sb.end_object();
    co_return HttpResponse::ok(std::string(sb));
}

}  // namespace dftracer::utils::server
