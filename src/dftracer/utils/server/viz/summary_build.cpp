#include <dftracer/utils/core/common/hash/constants.h>
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
#include <dftracer/utils/server/viz/internal.h>
#include <dftracer/utils/server/viz/summary_build.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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

// --- Activity summary ("mipmap") build -------------------------------------
// Cells are shared and updated with relaxed atomics with no per-event lock:
// workers scan disjoint checkpoint (time) ranges, so they touch mostly-disjoint
// buckets. Only first-time lane and name creation take a brief lock.

namespace {

struct PidTid {
    std::int64_t pid;
    std::int64_t tid;
    bool operator==(const PidTid& o) const {
        return pid == o.pid && tid == o.tid;
    }
};

struct PidTidHash {
    std::uint64_t operator()(const PidTid& k) const noexcept {
        std::uint64_t h = dftracer::utils::hash::FNV1A_OFFSET_BASIS_LEGACY;
        h = (h ^ static_cast<std::uint64_t>(k.pid)) *
            dftracer::utils::hash::FNV1A_PRIME;
        h = (h ^ static_cast<std::uint64_t>(k.tid)) *
            dftracer::utils::hash::FNV1A_PRIME;
        return h;
    }
};

// One worker's sparse cells at the finest level, keyed by lane and bucket.
// `demote` coarsens the whole accumulator by 4x when it outgrows its budget;
// since every level is an exact 4:1 fold of the one below, workers can sit at
// different depths and still merge.
struct FineAcc {
    ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> slot;
    std::vector<std::uint64_t> keys;
    std::vector<VizSummary::Cell> cells;
    unsigned demote = 0;

    static std::uint64_t key_of(std::uint32_t lane, std::uint64_t bucket) {
        return (static_cast<std::uint64_t>(lane) << 32) | bucket;
    }

    void add(std::uint32_t lane, std::uint64_t bucket, double dur,
             std::uint32_t name_id) {
        std::uint64_t k = key_of(lane, bucket >> (2 * demote));
        auto it = slot.find(k);
        std::uint32_t i;
        if (it != slot.end()) {
            i = it->second;
        } else {
            i = static_cast<std::uint32_t>(cells.size());
            slot.emplace(k, i);
            keys.push_back(k);
            cells.emplace_back();
        }
        auto& c = cells[i];
        c.count += 1;
        c.total += static_cast<std::uint64_t>(dur < 0 ? 0 : dur);
        std::uint32_t d = dur >= 4294967295.0
                              ? 4294967295u
                              : static_cast<std::uint32_t>(dur < 0 ? 0 : dur);
        if (d > c.max_dur || c.count == 1) {
            c.max_dur = d;
            c.name_id = name_id;
        }
    }

    void coarsen() {
        ++demote;
        ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> ns;
        std::vector<std::uint64_t> nk;
        std::vector<VizSummary::Cell> nc;
        ns.reserve(keys.size() / 2);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            std::uint64_t k = key_of(static_cast<std::uint32_t>(keys[i] >> 32),
                                     (keys[i] & 0xFFFFFFFFULL) >> 2);
            auto it = ns.find(k);
            if (it == ns.end()) {
                ns.emplace(k, static_cast<std::uint32_t>(nc.size()));
                nk.push_back(k);
                nc.push_back(cells[i]);
            } else {
                auto& dst = nc[it->second];
                const auto& src = cells[i];
                dst.count += src.count;
                dst.total += src.total;
                if (src.max_dur > dst.max_dur) {
                    dst.max_dur = src.max_dur;
                    dst.name_id = src.name_id;
                }
            }
        }
        slot = std::move(ns);
        keys = std::move(nk);
        cells = std::move(nc);
    }
};

struct SumBuild {
    std::size_t nb = 0;       // level 0 (counter grid)
    double bucket_us = 1;
    std::size_t nb_fine = 0;  // finest level, before any coarsening
    double fine_bucket_us = 1;
    // Only events at least this wide are kept whole in `long_evs`; narrower
    // ones fold into cells and a request finds them by scanning back at most
    // this far. A fraction of the span keeps the list bounded without a
    // per-bucket quota.
    double enc_threshold_us = 1;
    std::size_t max_lanes = 0;
    std::size_t fine_cell_cap = 0;  // per worker
    std::size_t long_cap = 0;       // per worker
    std::uint64_t t0 = 0;

    std::mutex lane_mtx;
    ankerl::unordered_dense::map<PidTid, std::uint32_t, PidTidHash> lane_of;
    std::vector<PidTid> lane_keys;

    std::vector<std::atomic<double>> cread;
    std::vector<std::atomic<double>> cwrite;
    std::vector<std::atomic<double>> cops;
    std::atomic<std::uint64_t> gmax_dur{0};

    std::atomic<bool> has_aggregated{false};
    std::atomic<double> agg_interval_us{
        0};  // cfg.trace_interval_ms, if declared

    std::mutex name_mtx;
    dftracer::utils::StringViewMap<std::uint32_t> name_of;
    std::vector<std::string> names;

    // Per-worker caches so the hot path never locks.
    std::vector<ankerl::unordered_dense::map<PidTid, std::uint32_t, PidTidHash>>
        lane_cache;
    std::vector<dftracer::utils::StringViewMap<std::uint32_t>> name_cache;

    // Per-worker simdjson parser + reusable buffer. Frame-local (owned here,
    // constructed on the caller's thread) rather than thread_local, which a
    // coroutine running on a pool thread leaves zero-initialised.
    std::vector<simdjson::dom::parser> parsers;
    std::vector<std::string> parse_bufs;

    std::vector<FineAcc> fine;

    // Per-worker Analyze aggregates (no lock); merged single-threaded after the
    // scan. One map per GroupBy dimension.
    std::vector<NameMap> g_name, g_cat, g_pid, g_fhash;

    // Per-worker FH resolution (file-hash -> path) from metadata records, so
    // the "By file" grouping can show real paths without depending on the
    // client.
    std::vector<dftracer::utils::StringViewMap<std::string>> fh_parts;

    std::vector<dftracer::utils::StringViewMap<std::string>> sh_parts;
    std::vector<ankerl::unordered_dense::map<std::int64_t, std::string>>
        app_start, app_end;

    // Per-pid application name from the CM "app" config record. Traces that put
    // the app name in CM (not the start event's exec_hash) rely on this for the
    // synthetic app-span label.
    std::vector<ankerl::unordered_dense::map<std::int64_t, std::string>> cm_app;

    // Per-worker events at least one finest-level bucket wide, kept whole (see
    // VizSummary::long_events).
    std::vector<std::vector<VizSummary::AppSpan>> long_evs;

    // Per-worker column discovery: distinct column names, and the event names
    // already harvested (schema is per event name).
    std::vector<dftracer::utils::StringViewSet> cols, col_seen;

    // Per-worker operation-name -> category (first seen); merged after the scan
    // into VizSummary::name_cats for real layer labels.
    std::vector<dftracer::utils::StringViewMap<std::string>> name_cat;

    // Per-worker file-hashes seen on a read/write op (files with real data
    // I/O).
    std::vector<dftracer::utils::StringViewSet> io_fh;

    // Per-worker process-hierarchy state (see VizSummary::procs), keyed by pid
    // so it stays small however many events a process emits.
    std::vector<ankerl::unordered_dense::map<std::int64_t, VizSummary::ProcRow>>
        procs;
    std::vector<std::vector<VizSummary::ForkEdge>> forks;
    std::vector<dftracer::utils::StringViewMap<std::string>> hh_parts;

    // Per-worker ph="C" counter samples: series key ("name\x1fkey") -> its
    // family cat + a sparse map of fine-bucket -> {sum, count}. ph="C" is rare,
    // so this stays small; merged after the scan into
    // VizSummary::counter_series.
    struct CtrAccum {
        std::string cat;
        std::int64_t pid = 0;
        std::int64_t tid = 0;
        ankerl::unordered_dense::map<std::uint32_t,
                                     std::pair<double, std::uint32_t>>
            byb;
    };
    std::vector<ankerl::unordered_dense::map<std::string, CtrAccum>> c_series;

    // Series identity includes pid/tid so a per-process counter keeps one
    // series per emitter (node-level counters emit under pid 0).
    void fold_counter_sample(std::size_t w, std::string_view name,
                             std::string_view key, std::string_view cat,
                             std::int64_t pid, std::int64_t tid,
                             std::uint32_t bucket, double val) {
        std::string sk;
        std::string pids = std::to_string(pid);
        std::string tids = std::to_string(tid);
        sk.reserve(name.size() + key.size() + pids.size() + tids.size() + 3);
        sk.append(name).push_back('\x1f');
        sk.append(key).push_back('\x1f');
        sk.append(pids).push_back('\x1f');
        sk.append(tids);
        auto& acc = c_series[w][sk];
        if (acc.cat.empty()) acc.cat.assign(cat);
        acc.pid = pid;
        acc.tid = tid;
        auto& bc = acc.byb[bucket];
        bc.first += val;
        bc.second += 1;
    }

    VizSummary::ProcRow& proc_of(std::size_t w, std::int64_t pid) {
        auto& m = procs[w];
        auto it = m.find(pid);
        if (it == m.end()) {
            it = m.emplace(pid, VizSummary::ProcRow{}).first;
            it->second.pid = pid;
        }
        return it->second;
    }

    static constexpr std::uint32_t NO_LANE =
        std::numeric_limits<std::uint32_t>::max();

    std::uint32_t get_lane(std::size_t w, std::int64_t pid, std::int64_t tid) {
        PidTid key{pid, tid};
        auto& cache = lane_cache[w];
        auto ci = cache.find(key);
        if (ci != cache.end()) return ci->second;
        std::lock_guard<std::mutex> lk(lane_mtx);
        auto it = lane_of.find(key);
        std::uint32_t lane;
        if (it != lane_of.end()) {
            lane = it->second;
        } else {
            if (lane_keys.size() >= max_lanes)
                return NO_LANE;  // budget reached; drop further lanes
            lane = static_cast<std::uint32_t>(lane_keys.size());
            lane_of.emplace(key, lane);
            lane_keys.push_back(key);
        }
        cache.emplace(key, lane);
        return lane;
    }

    // Fold an event into the worker's cells, coarsening when over budget.
    void fold_cell(std::size_t w, std::uint32_t lane, double ts, double dur,
                   std::uint32_t name_id) {
        auto& acc = fine[w];
        double rel = (ts - static_cast<double>(t0)) / fine_bucket_us;
        auto bucket = rel <= 0 ? 0 : static_cast<std::uint64_t>(rel);
        if (bucket >= nb_fine) bucket = nb_fine - 1;
        acc.add(lane, bucket, dur, name_id);
        while (acc.cells.size() > fine_cell_cap &&
               acc.demote < VizSummary::EXTRA_LEVELS)
            acc.coarsen();
    }

    std::uint64_t coarse_bucket(std::uint64_t ts) const {
        double rel =
            (static_cast<double>(ts) - static_cast<double>(t0)) / bucket_us;
        if (rel <= 0) return 0;
        auto bk = static_cast<std::uint64_t>(rel);
        return bk >= nb ? nb - 1 : bk;
    }

    // long_evs holds only enclosers >= enc_threshold_us (>= 1/K of the span),
    // so it is naturally small (~K per lane per depth) and needs no per-bucket
    // quota. This is only a backstop for a pathological trace with too many:
    // keep the widest, fold the rest into cells.
    void compact_long(std::size_t w) {
        auto& lv = long_evs[w];
        if (lv.size() <= long_cap) return;
        std::nth_element(
            lv.begin(), lv.begin() + static_cast<std::ptrdiff_t>(long_cap),
            lv.end(),
            [](const VizSummary::AppSpan& a, const VizSummary::AppSpan& x) {
                return a.end - a.begin > x.end - x.begin;
            });
        for (std::size_t i = long_cap; i < lv.size(); ++i) {
            auto lane = get_lane(w, lv[i].pid, lv[i].tid);
            if (lane != NO_LANE)
                fold_cell(w, lane, static_cast<double>(lv[i].begin),
                          static_cast<double>(lv[i].end - lv[i].begin),
                          lv[i].name_id);
        }
        lv.resize(long_cap);
    }

    void push_long(std::size_t w, VizSummary::AppSpan&& span) {
        auto& lv = long_evs[w];
        lv.push_back(std::move(span));
        if (lv.size() > long_cap * 2) compact_long(w);
    }

    std::uint32_t intern(std::size_t w, std::string_view name) {
        auto& cache = name_cache[w];
        auto ci = cache.find(name);
        if (ci != cache.end()) return ci->second;
        std::lock_guard<std::mutex> lk(name_mtx);
        auto it = name_of.find(name);
        std::uint32_t id;
        if (it != name_of.end()) {
            id = it->second;
        } else {
            id = static_cast<std::uint32_t>(names.size());
            names.emplace_back(name);
            name_of.emplace(std::string(name), id);
        }
        cache.emplace(std::string(name), id);
        return id;
    }
};

template <class T>
static void atomic_max(std::atomic<T>& a, T v) {
    T cur = a.load(std::memory_order_relaxed);
    while (v > cur &&
           !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

static void atomic_add_double(std::atomic<double>& a, double v) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

// Fold one (key, dur) sample into a group aggregate, mirroring fold_event.
static void fold_group(NameMap& m, std::string_view key, double dur) {
    auto it = m.find(key);
    if (it == m.end()) {
        it = m.emplace(std::string(key), NameStat{}).first;
        it->second.min = dur;
        it->second.max = dur;
    } else {
        it->second.min = std::min(it->second.min, dur);
        it->second.max = std::max(it->second.max, dur);
    }
    it->second.count += 1;
    it->second.total += dur;
}

static void fold_summary(std::size_t w, std::string_view event, SumBuild& b) {
    simdjson::dom::parser& parser = b.parsers[w];
    std::string& buf = b.parse_bufs[w];
    // Give simdjson zero-filled trailing padding it can over-read into. Parsing
    // a bare std::string pads it in place, leaving that padding uninitialised.
    buf.assign(event);
    buf.append(simdjson::SIMDJSON_PADDING, '\0');
    auto res =
        parser.parse(buf.data(), event.size(), /*realloc_if_needed=*/false);
    if (res.error()) return;
    auto root = res.value_unsafe();
    if (!root.is_object()) return;

    std::string_view name0;
    {
        auto nr = root["name"];
        if (!nr.error() && nr.is_string())
            name0 = nr.get_string().value_unsafe();
    }

    // ph="C" counter events carry no dur, so handle them before the dur gate:
    // fold each numeric arg into its (name, key) series at this ts's fine
    // bucket. They are neither cells nor I/O counters.
    {
        auto phr = root["ph"];
        if (!phr.error() &&
            read_phase(phr.value_unsafe()) == RecordPhase::COUNTER) {
            auto tr = root["ts"];
            if (tr.error()) return;
            double ts = json_number(tr.value_unsafe());
            std::int64_t bucket = static_cast<std::int64_t>(
                (ts - static_cast<double>(b.t0)) / b.fine_bucket_us);
            if (bucket < 0) return;
            if (bucket >= static_cast<std::int64_t>(b.nb_fine))
                bucket = static_cast<std::int64_t>(b.nb_fine) - 1;
            std::string_view ccat;
            auto ccr = root["cat"];
            if (!ccr.error() && ccr.is_string())
                ccat = ccr.get_string().value_unsafe();
            std::int64_t cpid = 0;
            std::int64_t ctid = 0;
            {
                auto pr = root["pid"];
                if (!pr.error())
                    cpid = static_cast<std::int64_t>(
                        json_number(pr.value_unsafe()));
                auto tr2 = root["tid"];
                if (!tr2.error())
                    ctid = static_cast<std::int64_t>(
                        json_number(tr2.value_unsafe()));
            }
            auto args = root["args"];
            if (!args.error() && args.is_object()) {
                for (auto field : args.get_object()) {
                    auto vv = field.value;
                    double val;
                    if (vv.is_int64())
                        val =
                            static_cast<double>(vv.get_int64().value_unsafe());
                    else if (vv.is_uint64())
                        val =
                            static_cast<double>(vv.get_uint64().value_unsafe());
                    else if (vv.is_double())
                        val = vv.get_double().value_unsafe();
                    else
                        continue;
                    b.fold_counter_sample(w, name0, field.key, ccat, cpid, ctid,
                                          static_cast<std::uint32_t>(bucket),
                                          val);
                }
            }
            return;
        }
    }

    // ph=3 SELECTIVE-aggregation records: the individual events are gone, so
    // they cannot fold into duration cells. Flag the trace (density requests
    // then live-scan) and skip the gates below.
    {
        auto phr = root["ph"];
        if (!phr.error() &&
            read_phase(phr.value_unsafe()) == RecordPhase::AGGREGATED) {
            b.has_aggregated.store(true, std::memory_order_relaxed);
            return;
        }
    }

    // The trailing "end" record declares the aggregation window under
    // args.cfg.trace_interval_ms; keep it so a density request can cross-check
    // its inferred interval.
    if (name0 == "end") {
        auto args = root["args"];
        if (!args.error() && args.is_object()) {
            auto cfg = args["cfg"];
            if (!cfg.error() && cfg.is_object()) {
                auto ti = cfg["trace_interval_ms"];
                if (!ti.error()) {
                    double ms = json_number(ti.value_unsafe());
                    if (ms > 0)
                        b.agg_interval_us.store(ms * 1000.0,
                                                std::memory_order_relaxed);
                }
            }
        }
    }

    // Hash and rank metadata carry no ts/dur, so they are harvested before the
    // gates below drop such records.
    if (name0 == "FH" || name0 == "SH" || name0 == "HH") {
        auto args = root["args"];
        if (!args.error() && args.is_object()) {
            // Data events on some traces carry no args, so metadata records are
            // the only source of a process's host hash; capture it here.
            auto pp = root["pid"];
            auto hr = args["hhash"];
            if (!pp.error() && !hr.error() && hr.is_string()) {
                auto pid =
                    static_cast<std::int64_t>(json_number(pp.value_unsafe()));
                auto& row = b.proc_of(w, pid);
                if (row.hhash.empty())
                    row.hhash = std::string(hr.get_string().value_unsafe());
            }
            auto v = args["value"];
            auto n = args["name"];
            if (!v.error() && v.is_string() && !n.error() && n.is_string()) {
                auto& tbl = name0 == "FH"   ? b.fh_parts[w]
                            : name0 == "SH" ? b.sh_parts[w]
                                            : b.hh_parts[w];
                tbl.emplace(std::string(v.get_string().value_unsafe()),
                            std::string(n.get_string().value_unsafe()));
            }
        }
        return;
    }
    if (name0 == "PR") {
        auto args = root["args"];
        auto pp = root["pid"];
        if (!args.error() && args.is_object() && !pp.error()) {
            auto an = args["name"];
            auto av = args["value"];
            if (!an.error() && an.is_string() &&
                an.get_string().value_unsafe() == "rank" && !av.error() &&
                av.is_string()) {
                auto pid =
                    static_cast<std::int64_t>(json_number(pp.value_unsafe()));
                auto& row = b.proc_of(w, pid);
                if (row.rank.empty())
                    row.rank = std::string(av.get_string().value_unsafe());
            }
        }
        return;
    }
    if (name0 == "CM") {
        auto args = root["args"];
        auto pp = root["pid"];
        if (!args.error() && args.is_object() && !pp.error()) {
            auto an = args["name"];
            auto av = args["value"];
            if (!an.error() && an.is_string() &&
                an.get_string().value_unsafe() == "app" && !av.error() &&
                av.is_string()) {
                auto pid =
                    static_cast<std::int64_t>(json_number(pp.value_unsafe()));
                b.cm_app[w].emplace(
                    pid, std::string(av.get_string().value_unsafe()));
            }
        }
        return;
    }

    auto dr = root["dur"];
    bool has_dur = !dr.error();
    double dur = has_dur ? json_number(dr.value_unsafe()) : 0;

    // Process hierarchy (see VizSummary::procs). Runs before the dur gate so it
    // sees exactly what a live proctree scan sees.
    {
        auto pr = root["pid"];
        auto tr = root["ts"];
        if (!pr.error() && !tr.error()) {
            auto pid =
                static_cast<std::int64_t>(json_number(pr.value_unsafe()));
            auto ts =
                static_cast<std::uint64_t>(json_number(tr.value_unsafe()));
            auto& row = b.proc_of(w, pid);
            if (row.first_ts == 0 || ts < row.first_ts) row.first_ts = ts;

            double ret = 0;
            auto args = root["args"];
            if (!args.error() && args.is_object()) {
                auto ppr = args["ppid"];
                if (!ppr.error()) {
                    auto pp = static_cast<std::int64_t>(
                        json_number(ppr.value_unsafe()));
                    if (pp > 0 && pp != pid) row.ppid = pp;
                }
                auto rr = args["ret"];
                if (!rr.error()) ret = json_number(rr.value_unsafe());
                auto hr = args["hhash"];
                if (!hr.error() && hr.is_string() && row.hhash.empty())
                    row.hhash = std::string(hr.get_string().value_unsafe());
            }
            if (ret > 0 && (name0.find("read") != std::string_view::npos ||
                            name0.find("write") != std::string_view::npos))
                row.bytes += static_cast<std::uint64_t>(ret);

            auto cr = root["cat"];
            if (!cr.error() && cr.is_string()) {
                auto cat = cr.get_string().value_unsafe();
                if (cat == "POSIX" || cat == "STDIO" || cat == "IO") {
                    row.io_ops += 1;
                    if (has_dur) row.io_busy += dur;
                }
            }
            if (!name0.empty() && is_fork_syscall(name0))
                b.forks[w].push_back(VizSummary::ForkEdge{
                    ts, pid, ret > 0 ? static_cast<std::int64_t>(ret) : -1});
        }
    }

    if (!has_dur) return;

    // Column discovery: harvest the schema once per distinct event name (same
    // name => same keys), so this is O(distinct names), not O(events).
    if (b.col_seen[w].find(name0) == b.col_seen[w].end()) {
        b.col_seen[w].emplace(name0);
        static const ankerl::unordered_dense::set<std::string_view> SKIP = {
            "pid", "tid", "ts", "dur", "ph", "id", "args"};
        auto obj = root.get_object();
        if (!obj.error())
            for (auto kv : obj.value_unsafe())
                if (SKIP.find(kv.key) == SKIP.end()) b.cols[w].emplace(kv.key);
        auto ar = root["args"];
        if (!ar.error() && ar.is_object()) {
            simdjson::dom::object args_obj;
            if (ar.get_object().get(args_obj) == simdjson::SUCCESS)
                for (auto kv : args_obj) b.cols[w].emplace(kv.key);
        }
    }

    if (name0 == "start" || name0 == "end") {
        auto cr = root["cat"];
        auto pp = root["pid"];
        if (!cr.error() && cr.is_string() &&
            cr.get_string().value_unsafe() == "dftracer" && !pp.error()) {
            auto p = static_cast<std::int64_t>(json_number(pp.value_unsafe()));
            (name0 == "start" ? b.app_start[w] : b.app_end[w])
                .emplace(p, std::string(event));
        }
    }

    std::int64_t pid = 0, tid = 0;
    auto pr = root["pid"];
    if (!pr.error())
        pid = static_cast<std::int64_t>(json_number(pr.value_unsafe()));

    // Analyze aggregates: whole-trace, ts-independent so they match a live
    // scan.
    {
        auto nr = root["name"];
        auto cr = root["cat"];
        if (!nr.error() && nr.is_string()) {
            std::string_view nm = nr.get_string().value_unsafe();
            fold_group(b.g_name[w], nm, dur);
            if (!cr.error() && cr.is_string() &&
                b.name_cat[w].find(nm) == b.name_cat[w].end())
                b.name_cat[w].emplace(
                    std::string(nm),
                    std::string(cr.get_string().value_unsafe()));
        }
        if (!cr.error() && cr.is_string())
            fold_group(b.g_cat[w], cr.get_string().value_unsafe(), dur);
        thread_local std::string pidkey;
        pidkey.assign(std::to_string(pid));
        fold_group(b.g_pid[w], pidkey, dur);
        auto ar = root["args"];
        if (!ar.error() && ar.is_object()) {
            auto fr = ar["fhash"];
            if (!fr.error() && fr.is_string())
                fold_group(b.g_fhash[w], fr.get_string().value_unsafe(), dur);
        }
    }

    auto tr = root["ts"];
    if (tr.error()) return;  // bucketing/counters below need a timestamp
    double ts = json_number(tr.value_unsafe());
    std::int64_t bucket = static_cast<std::int64_t>(
        (ts - static_cast<double>(b.t0)) / b.fine_bucket_us);
    if (bucket < 0) return;
    if (bucket >= static_cast<std::int64_t>(b.nb_fine)) bucket = b.nb_fine - 1;
    // Counters live at the finest resolution and are folded down to the coarser
    // levels afterwards, so a zoomed-in counter track needs no live scan.
    auto bi = static_cast<std::size_t>(bucket);

    auto tir = root["tid"];
    if (!tir.error())
        tid = static_cast<std::int64_t>(json_number(tir.value_unsafe()));

    std::uint32_t name_id;
    {
        std::string_view name;
        auto nr = root["name"];
        if (!nr.error() && nr.is_string())
            name = nr.get_string().value_unsafe();
        name_id = b.intern(w, name);
    }

    if (dur >= b.enc_threshold_us && dur > 0) {
        // Wide enough to enclose many windows: keep it whole so a deep zoom can
        // find it without an unbounded scan back. Narrower events fold into
        // cells (and a request finds them with a short scan back).
        b.push_long(
            w, VizSummary::AppSpan{static_cast<std::uint64_t>(ts),
                                   static_cast<std::uint64_t>(ts + dur), pid,
                                   tid, name_id, std::string(event)});
    } else {
        auto lane = b.get_lane(w, pid, tid);
        if (lane != SumBuild::NO_LANE) b.fold_cell(w, lane, ts, dur, name_id);
    }
    atomic_max(b.gmax_dur, static_cast<std::uint64_t>(dur < 0 ? 0 : dur));

    // Counters: read/write bytes and op counts for POSIX/STDIO/IO events.
    auto cat_r = root["cat"];
    if (cat_r.error() || !cat_r.is_string()) return;
    std::string_view cat = cat_r.get_string().value_unsafe();
    if (cat != "POSIX" && cat != "STDIO" && cat != "IO") return;
    atomic_add_double(b.cops[bi], 1.0);
    double bytes = 0;
    auto args = root["args"];
    if (!args.error() && args.is_object()) {
        auto ret = args["ret"];
        if (!ret.error()) bytes = json_number(ret.value_unsafe());
    }
    if (bytes <= 0) return;
    std::string_view name;
    auto nr = root["name"];
    if (!nr.error() && nr.is_string()) name = nr.get_string().value_unsafe();
    bool is_write = name.find("write") != std::string_view::npos;
    bool is_read = name.find("read") != std::string_view::npos;
    if (is_write)
        atomic_add_double(b.cwrite[bi], bytes);
    else if (is_read)
        atomic_add_double(b.cread[bi], bytes);
    if ((is_read || is_write) && !args.error() && args.is_object()) {
        auto fr = args["fhash"];
        if (!fr.error() && fr.is_string())
            b.io_fh[w].emplace(std::string(fr.get_string().value_unsafe()));
    }
}

}  // namespace

// Build the activity summary by scanning every event once. Blocks the caller
// (the first overview request) for the scan; cached for the server's lifetime.
static coro::CoroTask<void> build_viz_summary(TraceIndex& index) {
    auto summary = std::make_unique<VizSummary>();
    std::uint64_t gmin = index.global_min_timestamp_us();
    std::uint64_t gmax = index.global_max_timestamp_us();
    if (gmin == std::numeric_limits<std::uint64_t>::max() || gmax <= gmin) {
        index.set_viz_summary(std::move(summary));
        co_return;
    }

    std::size_t est_lanes = std::max<std::size_t>(1, index.file_count()) * 8;
    std::size_t nb = VizSummary::MAX_CELLS / est_lanes;
    nb = std::clamp(nb, VizSummary::MIN_BUCKETS_PER_LANE,
                    VizSummary::MAX_BUCKETS_PER_LANE);

    SumBuild b;
    b.nb = nb;
    b.t0 = gmin;
    b.bucket_us = static_cast<double>(gmax - gmin) / static_cast<double>(nb);
    b.max_lanes = VizSummary::MAX_LANES;
    b.nb_fine = nb << (2 * VizSummary::EXTRA_LEVELS);
    b.fine_bucket_us =
        static_cast<double>(gmax - gmin) / static_cast<double>(b.nb_fine);
    // Whole-run enclosers span >= 1/K of the trace: there are at most ~K of
    // them per lane per depth, so the list stays bounded with no quota.
    constexpr double ENC_THRESHOLD_FRACTION = 32.0;
    b.enc_threshold_us =
        std::max(b.fine_bucket_us,
                 static_cast<double>(gmax - gmin) / ENC_THRESHOLD_FRACTION);
    b.cread = std::vector<std::atomic<double>>(b.nb_fine);
    b.cwrite = std::vector<std::atomic<double>>(b.nb_fine);
    b.cops = std::vector<std::atomic<double>>(b.nb_fine);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    b.fine_cell_cap =
        std::max<std::size_t>(1, VizSummary::MAX_FINE_CELLS / slots);
    b.long_cap = std::max<std::size_t>(2, VizSummary::MAX_LONG_EVENTS / slots);
    b.fine.resize(slots);
    b.parsers.resize(slots);
    b.parse_bufs.resize(slots);
    b.lane_cache.resize(slots);
    b.name_cache.resize(slots);
    b.g_name.resize(slots);
    b.g_cat.resize(slots);
    b.g_pid.resize(slots);
    b.g_fhash.resize(slots);
    b.fh_parts.resize(slots);
    b.sh_parts.resize(slots);
    b.app_start.resize(slots);
    b.app_end.resize(slots);
    b.cm_app.resize(slots);
    b.long_evs.resize(slots);
    b.cols.resize(slots);
    b.col_seen.resize(slots);
    b.name_cat.resize(slots);
    b.io_fh.resize(slots);
    b.procs.resize(slots);
    b.forks.resize(slots);
    b.hh_parts.resize(slots);
    b.c_series.resize(slots);

    // The summary aggregates all hash metadata (hosts, file paths); traces
    // whose data events carry no hash args would otherwise never surface it.
    std::vector<const TraceIndex::FileInfo*> files;
    files.reserve(index.files().size());
    for (const auto& f : index.files()) files.push_back(&f);

    co_await views::View::from_files(to_view_files(files), &index.bloom_cache())
        .phase(views::Phase::Any)  // ph="X" cells + ph="C" counter series
        .emit_all_metadata(true)
        .for_each_batch(
            [&b](std::size_t w, const std::vector<std::string_view>& events) {
                for (auto ev : events) fold_summary(w, ev, b);
            },
            slots);

    summary->t_begin = gmin;
    summary->t_end = gmax;
    summary->nbuckets = nb;
    summary->bucket_us = b.bucket_us;
    summary->max_dur = b.gmax_dur.load(std::memory_order_relaxed);
    summary->has_aggregated = b.has_aggregated.load(std::memory_order_relaxed);
    summary->agg_interval_us =
        b.agg_interval_us.load(std::memory_order_relaxed);
    summary->names = std::move(b.names);
    summary->long_threshold_us = b.enc_threshold_us;
    summary->fine_bucket_us = b.fine_bucket_us;

    // Merge per-worker ph="C" samples by series key into sparse, ascending
    // fine-bucket series.
    {
        ankerl::unordered_dense::map<std::string, SumBuild::CtrAccum> merged;
        for (auto& wm : b.c_series)
            for (auto& [sk, acc] : wm) {
                auto& m = merged[sk];
                if (m.cat.empty()) m.cat = acc.cat;
                m.pid = acc.pid;
                m.tid = acc.tid;
                for (auto& [bk, sc] : acc.byb) {
                    auto& d = m.byb[bk];
                    d.first += sc.first;
                    d.second += sc.second;
                }
            }
        summary->counter_series.reserve(merged.size());
        for (auto& [sk, acc] : merged) {
            VizSummary::CounterSeriesData cd;
            // sk = name \x1f key \x1f pid \x1f tid; take only name and key
            // here, pid/tid come from the accumulator.
            auto sep1 = sk.find('\x1f');
            cd.name = sk.substr(0, sep1);
            auto sep2 = sk.find('\x1f', sep1 + 1);
            cd.key = sk.substr(sep1 + 1, sep2 - (sep1 + 1));
            cd.cat = acc.cat;
            cd.pid = acc.pid;
            cd.tid = acc.tid;
            std::vector<std::uint32_t> bks;
            bks.reserve(acc.byb.size());
            for (auto& [bk, sc] : acc.byb) {
                (void)sc;
                bks.push_back(bk);
            }
            std::sort(bks.begin(), bks.end());
            cd.buckets = bks;
            cd.sum.reserve(bks.size());
            cd.cnt.reserve(bks.size());
            for (auto bk : bks) {
                auto& sc = acc.byb[bk];
                cd.sum.push_back(sc.first);
                cd.cnt.push_back(sc.second);
            }
            summary->counter_series.push_back(std::move(cd));
        }
    }
    // Workers each hold up to their own per-bucket quota, so the union can
    // exceed the budget; compact once more across all of them.
    for (std::size_t w = 1; w < b.long_evs.size(); ++w) {
        auto& part = b.long_evs[w];
        for (auto& ev : part) b.long_evs[0].push_back(std::move(ev));
        part.clear();
        part.shrink_to_fit();
    }
    // Backstop only: keep the widest if a pathological trace somehow exceeds
    // the budget (the enc_threshold keep-rule already bounds this in practice).
    if (b.long_evs[0].size() > VizSummary::MAX_LONG_EVENTS) {
        auto& lv = b.long_evs[0];
        std::nth_element(
            lv.begin(),
            lv.begin() +
                static_cast<std::ptrdiff_t>(VizSummary::MAX_LONG_EVENTS),
            lv.end(),
            [](const VizSummary::AppSpan& a, const VizSummary::AppSpan& x) {
                return a.end - a.begin > x.end - x.begin;
            });
        for (std::size_t i = VizSummary::MAX_LONG_EVENTS; i < lv.size(); ++i) {
            auto lane = b.get_lane(0, lv[i].pid, lv[i].tid);
            if (lane != SumBuild::NO_LANE)
                b.fold_cell(0, lane, static_cast<double>(lv[i].begin),
                            static_cast<double>(lv[i].end - lv[i].begin),
                            lv[i].name_id);
        }
        lv.resize(VizSummary::MAX_LONG_EVENTS);
    }
    summary->long_events = std::move(b.long_evs[0]);

    // Workers that outgrew their cell budget coarsened independently; the
    // merged pyramid can only be as fine as the coarsest of them.
    unsigned demote = 0;
    for (auto& acc : b.fine) demote = std::max(demote, acc.demote);
    for (auto& acc : b.fine)
        while (acc.demote < demote) acc.coarsen();
    std::sort(summary->long_events.begin(), summary->long_events.end(),
              [](const VizSummary::AppSpan& a, const VizSummary::AppSpan& x) {
                  return a.begin < x.begin;
              });

    {
        std::size_t total = 0;
        for (auto& acc : b.fine) total += acc.cells.size();
        std::vector<std::pair<std::uint64_t, VizSummary::Cell>> merged;
        merged.reserve(total);
        for (auto& acc : b.fine) {
            for (std::size_t i = 0; i < acc.keys.size(); ++i)
                merged.emplace_back(acc.keys[i], acc.cells[i]);
            acc = FineAcc{};
        }
        std::sort(
            merged.begin(), merged.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });

        std::size_t nb_fine = b.nb_fine >> (2 * demote);
        // Counters were folded at the pre-coarsening resolution; bring them to
        // the merged one, then they ride the same 4:1 folds as the cells.
        std::vector<double> cread(nb_fine, 0.0), cwrite(nb_fine, 0.0),
            cops(nb_fine, 0.0);
        for (std::size_t i = 0; i < b.nb_fine; ++i) {
            std::size_t j = i >> (2 * demote);
            if (j >= nb_fine) j = nb_fine - 1;
            cread[j] += b.cread[i].load(std::memory_order_relaxed);
            cwrite[j] += b.cwrite[i].load(std::memory_order_relaxed);
            cops[j] += b.cops[i].load(std::memory_order_relaxed);
        }

        for (std::size_t l = 0; l <= VizSummary::EXTRA_LEVELS - demote; ++l) {
            if (l > 0) {
                // Fold 4:1 into the next coarser level, in place: keys stay
                // sorted because the lane stays in the high bits.
                std::size_t out = 0;
                for (std::size_t i = 0; i < merged.size(); ++i) {
                    std::uint64_t k = (merged[i].first & ~0xFFFFFFFFULL) |
                                      ((merged[i].first & 0xFFFFFFFFULL) >> 2);
                    if (out > 0 && merged[out - 1].first == k) {
                        auto& dst = merged[out - 1].second;
                        const auto& src = merged[i].second;
                        dst.count += src.count;
                        dst.total += src.total;
                        if (src.max_dur > dst.max_dur) {
                            dst.max_dur = src.max_dur;
                            dst.name_id = src.name_id;
                        }
                    } else {
                        merged[out] = merged[i];
                        merged[out].first = k;
                        ++out;
                    }
                }
                merged.resize(out);
                nb_fine >>= 2;
                auto fold = [](const std::vector<double>& src, std::size_t n) {
                    std::vector<double> dst(n, 0.0);
                    for (std::size_t i = 0; i < src.size(); ++i) {
                        std::size_t j = i >> 2;
                        dst[j < n ? j : n - 1] += src[i];
                    }
                    return dst;
                };
                cread = fold(cread, nb_fine);
                cwrite = fold(cwrite, nb_fine);
                cops = fold(cops, nb_fine);
            } else {
                std::size_t out = 0;
                for (std::size_t i = 0; i < merged.size(); ++i) {
                    if (out > 0 && merged[out - 1].first == merged[i].first) {
                        auto& dst = merged[out - 1].second;
                        const auto& src = merged[i].second;
                        dst.count += src.count;
                        dst.total += src.total;
                        if (src.max_dur > dst.max_dur) {
                            dst.max_dur = src.max_dur;
                            dst.name_id = src.name_id;
                        }
                    } else {
                        merged[out++] = merged[i];
                    }
                }
                merged.resize(out);
            }

            VizSummary::Level level;
            level.nbuckets = nb_fine;
            level.bucket_us =
                static_cast<double>(gmax - gmin) / static_cast<double>(nb_fine);
            level.read_bytes = cread;
            level.write_bytes = cwrite;
            level.ops = cops;
            level.lanes.resize(b.lane_keys.size());
            for (std::size_t i = 0; i < b.lane_keys.size(); ++i) {
                level.lanes[i].pid = b.lane_keys[i].pid;
                level.lanes[i].tid = b.lane_keys[i].tid;
            }
            for (const auto& [k, cell] : merged) {
                auto li = static_cast<std::size_t>(k >> 32);
                if (li >= level.lanes.size()) continue;
                level.lanes[li].buckets.push_back(
                    static_cast<std::uint32_t>(k & 0xFFFFFFFFULL));
                level.lanes[li].cells.push_back(cell);
            }
            summary->levels.push_back(std::move(level));
        }
        std::reverse(summary->levels.begin(), summary->levels.end());
    }

    {
        ankerl::unordered_dense::set<std::string> all_cols;
        for (auto& c : b.cols)
            for (auto& k : c) all_cols.emplace(k);
        summary->columns.assign(all_cols.begin(), all_cols.end());
        std::sort(summary->columns.begin(), summary->columns.end());
    }

    // Merge the per-worker Analyze aggregates and sort each by total desc.
    auto finalize_group = [](std::vector<NameMap>& parts) {
        NameMap merged;
        for (auto& p : parts) {
            for (auto& kv : p) {
                auto it = merged.find(kv.first);
                if (it == merged.end())
                    merged.emplace(kv.first, kv.second);
                else
                    it->second.merge_from(kv.second);
            }
        }
        std::vector<VizSummary::GroupRow> rows;
        rows.reserve(merged.size());
        for (auto& kv : merged)
            rows.push_back({kv.first, kv.second.count, kv.second.total,
                            kv.second.min, kv.second.max});
        std::sort(rows.begin(), rows.end(),
                  [](const VizSummary::GroupRow& lhs,
                     const VizSummary::GroupRow& rhs) {
                      return lhs.total > rhs.total;
                  });
        return rows;
    };
    summary->by_name = finalize_group(b.g_name);
    summary->by_cat = finalize_group(b.g_cat);
    summary->by_pid = finalize_group(b.g_pid);
    summary->by_fhash = finalize_group(b.g_fhash);

    // Resolve file-hash keys to real paths where a metadata record was seen.
    dftracer::utils::StringViewMap<std::string> fh;
    for (auto& part : b.fh_parts)
        for (auto& kv : part) fh.emplace(kv.first, kv.second);
    for (auto& r : summary->by_fhash) {
        auto it = fh.find(r.key);
        if (it != fh.end()) r.key = it->second;
    }
    summary->total_files = fh.size();

    {
        dftracer::utils::StringViewMap<std::string> sh;
        for (auto& part : b.sh_parts)
            for (auto& kv : part) sh.emplace(kv.first, kv.second);
        ankerl::unordered_dense::map<std::int64_t, std::string> starts, ends;
        for (auto& part : b.app_start)
            for (auto& kv : part) starts.emplace(kv.first, kv.second);
        for (auto& part : b.app_end)
            for (auto& kv : part) ends.emplace(kv.first, kv.second);
        ankerl::unordered_dense::map<std::int64_t, std::string> cm_apps;
        for (auto& part : b.cm_app)
            for (auto& kv : part) cm_apps.emplace(kv.first, kv.second);

        auto resolve = [](dftracer::utils::StringViewMap<std::string>& tbl,
                          const std::string& h) -> std::string {
            auto it = tbl.find(h);
            return it != tbl.end() ? it->second : h;
        };
        simdjson::dom::parser sp, ep;
        for (auto& [pid, sjson] : starts) {
            auto eit = ends.find(pid);
            if (eit == ends.end()) continue;
            std::string sbuf(sjson), ebuf(eit->second);
            auto sr = sp.parse(sbuf);
            auto er = ep.parse(ebuf);
            if (sr.error() || er.error()) continue;
            auto se = sr.value_unsafe();
            auto ee = er.value_unsafe();

            auto num = [](simdjson::dom::element r, const char* k) -> double {
                auto v = r[k];
                return v.error() ? 0.0 : json_number(v.value_unsafe());
            };
            auto sarg = [](simdjson::dom::element r,
                           const char* k) -> std::string {
                auto a = r["args"];
                if (a.error()) return "";
                auto v = a[k];
                return (!v.error() && v.is_string())
                           ? std::string(v.get_string().value_unsafe())
                           : "";
            };
            auto narg = [](simdjson::dom::element r, const char* k) -> double {
                auto a = r["args"];
                if (a.error()) return 0.0;
                auto v = a[k];
                return v.error() ? 0.0 : json_number(v.value_unsafe());
            };

            auto start_ts = static_cast<std::uint64_t>(num(se, "ts"));
            auto end_ts = static_cast<std::uint64_t>(num(ee, "ts"));
            if (end_ts <= start_ts) continue;
            auto tid = static_cast<std::int64_t>(num(se, "tid"));
            std::string app = resolve(sh, sarg(se, "exec_hash"));
            std::string cmd = resolve(sh, sarg(se, "cmd_hash"));
            std::string cwd = resolve(fh, sarg(se, "cwd"));
            std::string version = sarg(se, "version");
            std::string date = sarg(se, "date");
            auto ppid = static_cast<std::int64_t>(narg(se, "ppid"));
            auto num_events = static_cast<std::int64_t>(narg(ee, "num_events"));
            if (app.empty()) {
                auto ci = cm_apps.find(pid);
                if (ci != cm_apps.end()) app = ci->second;
            }
            if (app.empty()) app = "app " + std::to_string(pid);

            auto& jb = scratch_json_builder();
            jb.start_object();
            jb.append_key_value("name", app);
            jb.append_comma();
            jb.append_key_value("cat", "dftracer");
            jb.append_comma();
            jb.append_key_value("pid", pid);
            jb.append_comma();
            jb.append_key_value("tid", tid);
            jb.append_comma();
            jb.append_key_value("ts", start_ts);
            jb.append_comma();
            jb.append_key_value("dur", end_ts - start_ts);
            jb.append_comma();
            jb.append_key_value("ph", phase_to_int(RecordPhase::COMPLETE));
            jb.append_comma();
            jb.append_key_value("type", event_type_to_int(EventType::DFTRACER));
            jb.append_comma();
            jb.escape_and_append_with_quotes("args");
            jb.append_colon();
            jb.start_object();
            jb.append_key_value("app", app);
            jb.append_comma();
            jb.append_key_value("cmd", cmd);
            jb.append_comma();
            jb.append_key_value("cwd", cwd);
            jb.append_comma();
            jb.append_key_value("ppid", ppid);
            jb.append_comma();
            jb.append_key_value("version", version);
            jb.append_comma();
            jb.append_key_value("date", date);
            jb.append_comma();
            jb.append_key_value("num_events", num_events);
            jb.end_object();
            jb.end_object();
            summary->app_spans.push_back(
                {start_ts, end_ts, pid, tid,
                 std::numeric_limits<std::uint32_t>::max(), std::string(jb)});
        }
    }

    dftracer::utils::StringViewSet io_fh;
    for (auto& part : b.io_fh)
        for (auto& k : part) io_fh.emplace(k);
    summary->io_files = io_fh.size();

    {
        ankerl::unordered_dense::map<std::int64_t, VizSummary::ProcRow> procs;
        for (auto& part : b.procs) {
            for (auto& [pid, src] : part) {
                auto it = procs.find(pid);
                if (it == procs.end()) {
                    procs.emplace(pid, src);
                    continue;
                }
                auto& dst = it->second;
                if (dst.first_ts == 0 ||
                    (src.first_ts != 0 && src.first_ts < dst.first_ts))
                    dst.first_ts = src.first_ts;
                if (dst.ppid < 0) dst.ppid = src.ppid;
                dst.bytes += src.bytes;
                dst.io_ops += src.io_ops;
                dst.io_busy += src.io_busy;
                if (dst.hhash.empty()) dst.hhash = src.hhash;
                if (dst.rank.empty()) dst.rank = src.rank;
            }
            part.clear();
        }
        summary->procs.reserve(procs.size());
        for (auto& [pid, row] : procs) summary->procs.push_back(row);
        std::sort(summary->procs.begin(), summary->procs.end(),
                  [](const VizSummary::ProcRow& a,
                     const VizSummary::ProcRow& x) { return a.pid < x.pid; });

        for (auto& part : b.forks) {
            for (auto& f : part) summary->forks.push_back(f);
            part.clear();
        }
        std::sort(summary->forks.begin(), summary->forks.end(),
                  [](const VizSummary::ForkEdge& a,
                     const VizSummary::ForkEdge& x) { return a.ts < x.ts; });

        dftracer::utils::StringViewMap<std::string> hh;
        for (auto& part : b.hh_parts)
            for (auto& kv : part) hh.emplace(kv.first, kv.second);
        summary->hosts.reserve(hh.size());
        for (auto& kv : hh) summary->hosts.emplace_back(kv.first, kv.second);
    }

    // Merge the per-worker name -> category maps (first writer wins).
    dftracer::utils::StringViewMap<std::string> nc;
    for (auto& part : b.name_cat)
        for (auto& kv : part) nc.emplace(kv.first, kv.second);
    summary->name_cats.reserve(nc.size());
    for (auto& kv : nc) summary->name_cats.emplace_back(kv.first, kv.second);

    if (!summary->app_spans.empty()) {
        // Break = dead time between runs: merge app-spans into run clusters and
        // emit every gap between them, no size threshold.
        std::vector<const VizSummary::AppSpan*> spans;
        spans.reserve(summary->app_spans.size());
        for (const auto& sp : summary->app_spans) spans.push_back(&sp);
        std::sort(spans.begin(), spans.end(),
                  [](const auto* lhs, const auto* rhs) {
                      return lhs->begin < rhs->begin;
                  });
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        for (const auto* sp : spans) {
            if (!runs.empty() && sp->begin <= runs.back().second)
                runs.back().second = std::max(runs.back().second, sp->end);
            else
                runs.emplace_back(sp->begin, sp->end);
        }
        for (std::size_t i = 1; i < runs.size(); ++i)
            if (runs[i].first > runs[i - 1].second)
                summary->idle_gaps.emplace_back(runs[i - 1].second,
                                                runs[i].first);
    } else if (nb > 0 && summary->bucket_us > 0) {
        // No app-spans (malformed trace): fall back to a lane-activity scan,
        // requiring a gap to be a fraction of active (not total) time.
        std::vector<bool> active(nb, false);
        if (!summary->levels.empty())
            for (const auto& lane : summary->levels.front().lanes)
                for (std::uint32_t bk : lane.buckets)
                    if (bk < nb) active[bk] = true;
        for (const auto& ev : summary->long_events) {
            auto b0 = summary->bucket_of(static_cast<double>(ev.begin));
            auto b1 = summary->bucket_of(static_cast<double>(ev.end));
            if (b0 < 0) continue;
            if (b1 < b0) b1 = b0;
            for (std::int64_t i = b0; i <= b1; ++i)
                active[static_cast<std::size_t>(i)] = true;
        }
        std::size_t first = 0, last = 0, active_count = 0;
        bool any = false;
        for (std::size_t i = 0; i < nb; ++i)
            if (active[i]) {
                if (!any) {
                    first = i;
                    any = true;
                }
                last = i;
                ++active_count;
            }
        if (any) {
            const double active_us =
                static_cast<double>(active_count) * summary->bucket_us;
            const double min_gap_us =
                std::max(3.0 * summary->bucket_us, 0.05 * active_us);
            for (std::size_t i = first; i <= last;) {
                if (active[i]) {
                    ++i;
                    continue;
                }
                std::size_t j = i;
                while (j <= last && !active[j]) ++j;
                if (static_cast<double>(j - i) * summary->bucket_us >=
                    min_gap_us) {
                    auto g0 = summary->t_begin +
                              static_cast<std::uint64_t>(
                                  static_cast<double>(i) * summary->bucket_us);
                    auto g1 = summary->t_begin +
                              static_cast<std::uint64_t>(
                                  static_cast<double>(j) * summary->bucket_us);
                    summary->idle_gaps.emplace_back(g0, g1);
                }
                i = j;
            }
        }
    }

    if (g_shutdown_requested.load(std::memory_order_acquire)) {
        DFTRACER_UTILS_LOG_INFO("viz: summary build abandoned (shutting down)");
        co_return;
    }

    std::size_t cells = 0;
    for (const auto& lane : summary->levels.back().lanes)
        cells += lane.cells.size();
    DFTRACER_UTILS_LOG_INFO(
        "viz: built activity summary (%zu lanes, %zu levels, finest %.0f us "
        "with %zu cells, %zu long events, %zu idle gaps)",
        b.lane_keys.size(), summary->levels.size(),
        summary->levels.back().bucket_us, cells, summary->long_events.size(),
        summary->idle_gaps.size());
    index.set_viz_summary(std::move(summary));
}

// The summary, building it on first use. Requests that arrive during the build
// wait for it: a whole-trace live scan each (the old fallback) costs more than
// the build they are waiting on, and the client opens the timeline with half a
// dozen summary-backed requests at once.
coro::CoroTask<const VizSummary*> ensure_viz_summary(TraceIndex& index) {
    const VizSummary* s = index.viz_summary();
    if (s) co_return s;
    co_await index.viz_summary_mutex().lock();
    coro::AsyncMutexGuard guard(index.viz_summary_mutex());
    s = index.viz_summary();
    if (!s) {
        if (!index.load_persisted_viz_summary()) {
            co_await build_viz_summary(index);
            index.persist_viz_summary();
        }
        s = index.viz_summary();
    }
    co_return s;
}

coro::CoroTask<void> prewarm_viz_summary(TraceIndex& index) {
    co_await ensure_viz_summary(index);
}

}  // namespace dftracer::utils::server
