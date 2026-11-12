// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/plan/rowsets.h>
#include <dftracer/utils/trace/index/plan/view_resolver.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/trace/reader/trace_reader.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dftracer::utils::domains::server {

namespace {

// The file's time bounds for the timeline, in microseconds. A path schema's
// span from its time role's zonemap to the latest time plus the longest
// duration. Events without a clock sit at ts 0, so a dftracer file starts at
// its first start above 0 when it has one.
dstd::Result<dftracer::utils::trace::index::gzip::TimeBounds> timeline_bounds(
    const dftracer::utils::trace::index::Snapshot& snapshot,
    const dftracer::utils::trace::index::FileRef& file,
    const dftracer::utils::trace::index::RecordSchema& schema) {
    if (schema.decoder == trace::index::Decoder::Path) {
        trace::index::gzip::TimeBounds bounds;
        const auto& roles = schema.roles;
        if (roles.time.empty()) return bounds;
        const auto time_range =
            trace::index::extensions::kinds::file_value_range(snapshot, file,
                                                              roles.time);
        if (!time_range) return time_range.error();
        const auto& time = *time_range;
        if (!time) return bounds;
        const double time_us = trace::index::micros_per(roles.time_unit);
        double end = time->second * time_us;
        if (!roles.duration.empty()) {
            const auto dur_range =
                trace::index::extensions::kinds::file_value_range(
                    snapshot, file, roles.duration);
            if (!dur_range) return dur_range.error();
            if (const auto& dur = *dur_range; dur && dur->second > 0)
                end +=
                    dur->second * trace::index::micros_per(roles.duration_unit);
        }
        bounds.valid = true;
        bounds.min_timestamp_us = static_cast<std::uint64_t>(
            std::llround(std::max(0.0, time->first * time_us)));
        bounds.max_timestamp_us =
            static_cast<std::uint64_t>(std::llround(std::max(0.0, end)));
        return bounds;
    }
    auto found =
        dftracer::utils::trace::index::extensions::kinds::file_time_bounds(
            snapshot, file);
    if (!found) return found.error();
    auto bounds = *found;
    if (!bounds.valid || bounds.min_timestamp_us != 0) return bounds;
    const auto scalar = trace::index::scalar_stats(snapshot, file);
    if (!scalar) return scalar.error();
    const auto& stats = *scalar;
    if (stats &&
        stats->stats.min_nonzero_timestamp_us <= bounds.max_timestamp_us)
        bounds.min_timestamp_us = stats->stats.min_nonzero_timestamp_us;
    return bounds;
}

// True when the index at `root` no longer matches `paths`: a path is not
// indexed, its stat changed, or the index holds a path that is not listed.
dstd::Result<bool> index_is_stale(dstd::StringView root,
                                  const dstd::Vector<dstd::String>& paths) {
    auto opened = trace::index::plan::open_snapshot(root);
    if (!opened) return opened.error();
    const auto& snapshot = *opened;
    dstd::HashSet<dstd::StringView> listed;
    dstd::Vector<dstd::String> logical_paths;
    logical_paths.reserve(paths.size());
    for (const auto& path : paths) {
        logical_paths.push_back(fileio::get_logical_path(path));
        listed.insert(logical_paths.back());
        common::InputFileStat st;
        trace::index::SourceStat now;
        if (common::stat_input(path, st)) {
            now.mtime = static_cast<std::uint64_t>(st.mtime);
            now.size = st.size;
        }
        if (snapshot.freshness(logical_paths.back(), now) !=
            trace::index::Freshness::Current)
            return true;
    }
    const auto files = snapshot.files();
    if (!files) return files.error();
    for (const auto& file : *files)
        if (listed.find(file.path()) == listed.end()) return true;
    return false;
}

dstd::Result<dstd::SharedPtr<const TraceIndex::FileChunkMeta>> read_chunk_meta(
    const TraceIndex::FileInfo& file) {
    auto opened = trace::index::plan::open_snapshot(file.index_path);
    if (!opened) return opened.error();
    const auto& snapshot = *opened;
    const auto ref = snapshot.file(fileio::get_logical_path(file.path));
    if (!ref) return dstd::SharedPtr<const TraceIndex::FileChunkMeta>{};
    auto m = dstd::make_shared<TraceIndex::FileChunkMeta>();
    auto spans = trace::index::spans(snapshot, *ref);
    if (!spans) return spans.error();
    m->spans = std::move(*spans);
    auto stats = trace::index::chunk_stats(snapshot, *ref);
    if (!stats) return stats.error();
    m->stats = std::move(*stats);
    return dstd::SharedPtr<const TraceIndex::FileChunkMeta>(std::move(m));
}

// Read one file's record schema and time bounds from its index into `info`
// and `schema`. A failure leaves them as they are.
dstd::Result<void> read_file_bounds(
    TraceIndex::FileInfo& info,
    const dftracer::utils::trace::index::RecordSchema*& schema_out) {
    auto opened = trace::index::plan::open_snapshot(info.index_path);
    if (!opened) return opened.error();
    const auto& snapshot = *opened;
    const auto file = snapshot.file(fileio::get_logical_path(info.path));
    if (!file) return {};
    if (auto loaded = trace::index::load_index_schemas(info.index_path);
        !loaded)
        return loaded.error();
    auto recorded = trace::index::plan::recorded_schema(snapshot, info.path);
    if (!recorded) return recorded.error();
    const auto* schema = *recorded;
    if (!schema) {
        auto detected = trace::index::detect_file_schema(info.path);
        if (!detected) return detected.error();
        schema = *detected;
    }
    schema_out = schema;
    auto bounds = timeline_bounds(snapshot, *file, *schema);
    if (!bounds) return bounds.error();
    if (bounds->valid) {
        info.min_timestamp_us = bounds->min_timestamp_us;
        info.max_timestamp_us = bounds->max_timestamp_us;
    }
    return {};
}

}  // namespace

using namespace dftracer::utils::trace;

namespace {

// Bump VERSION when the on-disk layout below changes; the loader then ignores
// the stale cache.
constexpr std::uint32_t VIZ_SUMMARY_MAGIC = 0x315A5644;  // "DVZ1"
constexpr std::uint32_t VIZ_SUMMARY_FORMAT_VERSION = 0;

struct BufWriter {
    dstd::String b;
    void u32(std::uint32_t v) { b.append(reinterpret_cast<char*>(&v), 4); }
    void u64(std::uint64_t v) { b.append(reinterpret_cast<char*>(&v), 8); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void dbl(double v) { b.append(reinterpret_cast<char*>(&v), 8); }
    void str(const dstd::String& s) {
        u64(s.size());
        b.append(s);
    }
};

struct BufReader {
    const char* p;
    const char* end;
    bool ok = true;
    std::uint32_t u32() {
        std::uint32_t v = 0;
        if (!ok || p + 4 > end) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        if (!ok || p + 8 > end) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double dbl() {
        double v = 0;
        if (!ok || p + 8 > end) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    dstd::String str() {
        std::uint64_t n = u64();
        if (!ok || n > static_cast<std::uint64_t>(end - p)) {
            ok = false;
            return {};
        }
        dstd::String s(p, p + n);
        p += n;
        return s;
    }
    // Guard a count before resize: each element consumes >= elem_min bytes, so
    // a corrupt count can't request an absurd allocation.
    bool fits(std::uint64_t n, std::size_t elem_min) const {
        if (!ok) return false;
        std::size_t rem = static_cast<std::size_t>(end - p);
        return elem_min == 0 || n <= rem / elem_min;
    }
};

void write_group_rows(BufWriter& w,
                      const dstd::Vector<VizSummary::GroupRow>& rows) {
    w.u64(rows.size());
    for (const auto& r : rows) {
        w.str(r.key);
        w.u64(r.count);
        w.dbl(r.total);
        w.dbl(r.min);
        w.dbl(r.max);
    }
}

bool read_group_rows(BufReader& r, dstd::Vector<VizSummary::GroupRow>& rows) {
    std::uint64_t n = r.u64();
    if (!r.fits(n, 8 + 8 + 8 * 3)) return false;
    rows.resize(n);
    for (auto& row : rows) {
        row.key = r.str();
        row.count = r.u64();
        row.total = r.dbl();
        row.min = r.dbl();
        row.max = r.dbl();
    }
    return r.ok;
}

void write_spans(BufWriter& w, const dstd::Vector<VizSummary::AppSpan>& spans) {
    w.u64(spans.size());
    for (const auto& s : spans) {
        w.u64(s.begin);
        w.u64(s.end);
        w.i64(s.pid);
        w.i64(s.tid);
        w.u32(s.name_id);
        w.str(s.json);
    }
}

bool read_spans(BufReader& r, dstd::Vector<VizSummary::AppSpan>& spans) {
    std::uint64_t n = r.u64();
    if (!r.fits(n, 8 * 4 + 4 + 8)) return false;
    spans.resize(n);
    for (auto& s : spans) {
        s.begin = r.u64();
        s.end = r.u64();
        s.pid = r.i64();
        s.tid = r.i64();
        s.name_id = r.u32();
        s.json = r.str();
    }
    return r.ok;
}

void serialize_summary(BufWriter& w, const VizSummary& s) {
    w.u64(s.t_begin);
    w.u64(s.t_end);
    w.u64(s.nbuckets);
    w.dbl(s.bucket_us);
    w.u64(s.max_dur);
    w.u64(s.total_files);
    w.u64(s.io_files);

    w.dbl(s.long_threshold_us);

    w.u64(s.names.size());
    for (const auto& n : s.names) w.str(n);

    w.u64(s.levels.size());
    for (const auto& lv : s.levels) {
        w.u64(lv.nbuckets);
        w.dbl(lv.bucket_us);
        auto wvec_lvl = [&](const dstd::Vector<double>& v) {
            std::uint64_t nonzero = 0;
            for (double x : v) nonzero += x != 0.0;
            w.u64(v.size());
            w.u64(nonzero);
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] == 0.0) continue;
                w.u32(static_cast<std::uint32_t>(i));
                w.dbl(v[i]);
            }
        };
        wvec_lvl(lv.read_bytes);
        wvec_lvl(lv.write_bytes);
        wvec_lvl(lv.ops);
        w.u64(lv.lanes.size());
        for (const auto& l : lv.lanes) {
            w.i64(l.pid);
            w.i64(l.tid);
            w.u64(l.cells.size());
            for (std::uint32_t bk : l.buckets) w.u32(bk);
            for (const auto& c : l.cells) {
                w.u32(c.count);
                w.u64(c.total);
                w.u32(c.max_dur);
                w.u32(c.name_id);
            }
        }
    }

    write_group_rows(w, s.by_name);
    write_group_rows(w, s.by_cat);
    write_group_rows(w, s.by_pid);
    write_group_rows(w, s.by_fhash);

    w.u64(s.name_cats.size());
    for (const auto& p : s.name_cats) {
        w.str(p.first);
        w.str(p.second);
    }

    w.u64(s.columns.size());
    for (const auto& c : s.columns) w.str(c);

    write_spans(w, s.app_spans);
    write_spans(w, s.long_events);

    w.u64(s.idle_gaps.size());
    for (const auto& g : s.idle_gaps) {
        w.u64(g.first);
        w.u64(g.second);
    }

    w.u64(s.procs.size());
    for (const auto& p : s.procs) {
        w.i64(p.pid);
        w.i64(p.ppid);
        w.u64(p.first_ts);
        w.u64(p.bytes);
        w.u64(p.io_ops);
        w.dbl(p.io_busy);
        w.str(p.hhash);
        w.str(p.rank);
        w.str(p.label);
    }

    w.u64(s.forks.size());
    for (const auto& f : s.forks) {
        w.u64(f.ts);
        w.i64(f.pid);
        w.i64(f.child);
    }

    w.u64(s.hosts.size());
    for (const auto& h : s.hosts) {
        w.str(h.first);
        w.str(h.second);
    }

    w.dbl(s.fine_bucket_us);
    w.u64(s.counter_series.size());
    for (const auto& c : s.counter_series) {
        w.str(c.name);
        w.str(c.key);
        w.str(c.cat);
        w.i64(c.pid);
        w.i64(c.tid);
        w.u64(c.buckets.size());
        for (auto bk : c.buckets) w.u32(bk);
        for (double x : c.sum) w.dbl(x);
        for (auto n : c.cnt) w.u32(n);
    }

    w.u32(s.has_aggregated ? 1 : 0);
    w.dbl(s.agg_interval_us);
}

bool deserialize_summary(BufReader& r, VizSummary& s) {
    s.t_begin = r.u64();
    s.t_end = r.u64();
    s.nbuckets = static_cast<std::size_t>(r.u64());
    s.bucket_us = r.dbl();
    s.max_dur = r.u64();
    s.total_files = static_cast<std::size_t>(r.u64());
    s.io_files = static_cast<std::size_t>(r.u64());

    s.long_threshold_us = r.dbl();

    std::uint64_t nnames = r.u64();
    if (!r.fits(nnames, 8)) return false;
    s.names.resize(nnames);
    for (auto& n : s.names) n = r.str();

    std::uint64_t nlevels = r.u64();
    if (!r.fits(nlevels, 8 + 8 + 8)) return false;
    s.levels.resize(nlevels);
    for (auto& lv : s.levels) {
        lv.nbuckets = static_cast<std::size_t>(r.u64());
        lv.bucket_us = r.dbl();
        auto rvec_lvl = [&](dstd::Vector<double>& v) -> bool {
            std::uint64_t n = r.u64();
            std::uint64_t nonzero = r.u64();
            constexpr std::uint64_t MAX_BUCKETS =
                VizSummary::MAX_BUCKETS_PER_LANE
                << (2 * VizSummary::EXTRA_LEVELS);
            if (n != lv.nbuckets || n > MAX_BUCKETS || !r.fits(nonzero, 4 + 8))
                return false;
            v.assign(n, 0.0);
            for (std::uint64_t i = 0; i < nonzero; ++i) {
                std::uint32_t at = r.u32();
                double x = r.dbl();
                if (at >= n) return false;
                v[at] = x;
            }
            return r.ok;
        };
        if (!rvec_lvl(lv.read_bytes) || !rvec_lvl(lv.write_bytes) ||
            !rvec_lvl(lv.ops))
            return false;
        std::uint64_t nlanes = r.u64();
        if (!r.fits(nlanes, 8 * 2 + 8)) return false;
        lv.lanes.resize(nlanes);
        for (auto& l : lv.lanes) {
            l.pid = r.i64();
            l.tid = r.i64();
            std::uint64_t ncells = r.u64();
            if (!r.fits(ncells, 4 + 4 + 8 + 4 + 4)) return false;
            l.buckets.resize(ncells);
            for (auto& bk : l.buckets) bk = r.u32();
            l.cells.resize(ncells);
            for (auto& c : l.cells) {
                c.count = r.u32();
                c.total = r.u64();
                c.max_dur = r.u32();
                c.name_id = r.u32();
            }
        }
    }

    if (!read_group_rows(r, s.by_name) || !read_group_rows(r, s.by_cat) ||
        !read_group_rows(r, s.by_pid) || !read_group_rows(r, s.by_fhash))
        return false;

    std::uint64_t ncats = r.u64();
    if (!r.fits(ncats, 8 * 2)) return false;
    s.name_cats.resize(ncats);
    for (auto& p : s.name_cats) {
        p.first = r.str();
        p.second = r.str();
    }

    std::uint64_t ncols = r.u64();
    if (!r.fits(ncols, 8)) return false;
    s.columns.resize(ncols);
    for (auto& c : s.columns) c = r.str();

    if (!read_spans(r, s.app_spans) || !read_spans(r, s.long_events))
        return false;

    std::uint64_t ngaps = r.u64();
    if (!r.fits(ngaps, 8 * 2)) return false;
    s.idle_gaps.resize(ngaps);
    for (auto& g : s.idle_gaps) {
        g.first = r.u64();
        g.second = r.u64();
    }

    std::uint64_t nprocs = r.u64();
    if (!r.fits(nprocs, 8 * 5 + 8 * 3)) return false;
    s.procs.resize(nprocs);
    for (auto& p : s.procs) {
        p.pid = r.i64();
        p.ppid = r.i64();
        p.first_ts = r.u64();
        p.bytes = r.u64();
        p.io_ops = r.u64();
        p.io_busy = r.dbl();
        p.hhash = r.str();
        p.rank = r.str();
        p.label = r.str();
    }

    std::uint64_t nforks = r.u64();
    if (!r.fits(nforks, 8 * 3)) return false;
    s.forks.resize(nforks);
    for (auto& f : s.forks) {
        f.ts = r.u64();
        f.pid = r.i64();
        f.child = r.i64();
    }

    std::uint64_t nhosts = r.u64();
    if (!r.fits(nhosts, 8 * 2)) return false;
    s.hosts.resize(nhosts);
    for (auto& h : s.hosts) {
        h.first = r.str();
        h.second = r.str();
    }

    s.fine_bucket_us = r.dbl();
    std::uint64_t ncs = r.u64();
    if (!r.fits(ncs, 8)) return false;
    s.counter_series.resize(ncs);
    for (auto& c : s.counter_series) {
        c.name = r.str();
        c.key = r.str();
        c.cat = r.str();
        c.pid = r.i64();
        c.tid = r.i64();
        std::uint64_t nb = r.u64();
        if (!r.fits(nb, 4)) return false;
        c.buckets.resize(nb);
        for (auto& bk : c.buckets) bk = r.u32();
        c.sum.resize(nb);
        for (auto& x : c.sum) x = r.dbl();
        c.cnt.resize(nb);
        for (auto& n : c.cnt) n = r.u32();
    }

    s.has_aggregated = r.u32() != 0;
    s.agg_interval_us = r.dbl();

    return r.ok;
}

}  // namespace

TraceIndex::TraceIndex(const dstd::String& directory,
                       const dstd::String& index_dir,
                       std::size_t max_concurrent, std::size_t checkpoint_size)
    : directory_(directory),
      index_dir_(index_dir),
      max_concurrent_(max_concurrent == 0 ? 8 : max_concurrent),
      checkpoint_size_(checkpoint_size == 0
                           ? common::constants::indexer::DEFAULT_CHECKPOINT_SIZE
                           : checkpoint_size) {}

coro::CoroTask<dstd::Result<void>> TraceIndex::initialize() {
    runtime::fs::FindOptions find_options;
    find_options.patterns =
        runtime::fs::names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    find_options.recursive = false;
    auto scoped = coro::with_scope([&](coro::Scope& scope) {
        return runtime::fs::find_all(scope, dstd::PathView(directory_),
                                     find_options);
    });
    auto found = co_await std::move(scoped);
    if (!found) co_return found.error();
    const auto& entries = *found;

    files_.clear();
    path_to_index_.clear();
    files_.reserve(entries.size());

    global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    global_max_ts_ = 0;

    dstd::Vector<std::size_t> needs_build;
    dstd::Vector<std::size_t> large_files;
    dstd::Vector<const dftracer::utils::trace::index::RecordSchema*>
        file_schemas(entries.size(), nullptr);
    auto default_schema = trace::index::get_schema("dftracer");
    if (!default_schema) co_return default_schema.error();
    record_schema_ = *default_schema;

    // The reuse decision below is existence-only, so a changed .pfw would be
    // served from a stale index. Drop any (shared) index root whose sources
    // changed since indexing; the loop then rebuilds it as if absent.
    {
        dstd::Map<dstd::String, dstd::Vector<dstd::String>> by_root;
        for (const auto& entry : entries)
            by_root[internal::determine_index_path(entry.path.c_str(),
                                                   index_dir_)]
                .emplace_back(entry.path.str());
        for (auto& [root, paths] : by_root) {
            const dstd::String store = trace::index::store_of(root);
            const auto store_exists = dstd::fs::exists(dstd::PathView(store));
            if (!store_exists.has_value()) co_return store_exists.error();
            if (!*store_exists) {
                continue;
            }
            bool stale = false;
            const auto checked = index_is_stale(root, paths);
            if (checked.has_value()) {
                stale = *checked;
            } else {
                DFTRACER_UTILS_LOG_WARN(
                    "TraceIndex: stale check failed for {}: {}; rebuilding",
                    root, checked.error().message());
                stale = true;
            }
            if (stale) {
                DFTRACER_UTILS_LOG_WARN(
                    "TraceIndex: source changed since indexing; rebuilding {}",
                    root);
                // Drop the cached handle first, as the builder does: a live
                // handle on a removed directory would serve the old data.
                trace::index::release_root(root);
                dstd::fs::remove_all(dstd::PathView(store)).discard();
                common::forget_input(store);
            }
        }
    }

    for (const auto& entry : entries) {
        FileInfo info;
        info.path = dstd::String(entry.path.str());
        info.index_path = internal::determine_index_path(info.path, index_dir_);

        const auto fsize = dstd::fs::file_size(dstd::PathView(info.path));
        info.compressed_size = (fsize && *fsize > 0) ? *fsize : 0;

        std::size_t idx = files_.size();
        path_to_index_[info.path] = idx;

        const auto has_store = dstd::fs::exists(
            dstd::PathView(trace::index::store_of(info.index_path)));
        if (!has_store.has_value()) co_return has_store.error();
        info.has_bloom_data = *has_store;
        info.has_checkpoint_index = info.has_bloom_data;
        if (!info.has_bloom_data) {
            needs_build.push_back(idx);
        } else {
            large_files.push_back(idx);
        }

        files_.push_back(std::move(info));
    }

    if (!needs_build.empty() || !large_files.empty()) {
        RuntimeSettings settings;
        settings.threads = max_concurrent_;
        settings.io_backend = runtime::io::IoBackendType::Threadpool;
        settings.io_batch_threshold = 1;
        settings.deadlock_timeout =
            std::chrono::seconds(common::constants::executor_settings::
                                     TOOL_DEADLOCK_TIMEOUT_SECONDS);

        auto* files_ptr = &files_;
        auto* needs_build_ptr = &needs_build;
        auto* large_files_ptr = &large_files;
        auto* global_min_ts_ptr = &global_min_ts_;
        auto* global_max_ts_ptr = &global_max_ts_;
        auto* schemas_ptr = &file_schemas;
        dstd::String index_dir = index_dir_;
        std::size_t max_concurrent = max_concurrent_;
        std::size_t checkpoint_size = checkpoint_size_;

        auto init_task =
            [files_ptr, needs_build_ptr, large_files_ptr, global_min_ts_ptr,
             global_max_ts_ptr, schemas_ptr, index_dir, max_concurrent,
             checkpoint_size](
                coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            if (!needs_build_ptr->empty()) {
                DFTRACER_UTILS_LOG_INFO(
                    "TraceIndex: building index for {} file(s) ...",
                    needs_build_ptr->size());

                // One batch build per index root; the batch builder assigns
                // file ids itself, so no manual pre-assignment is needed.
                dstd::HashMap<dstd::String, dstd::Vector<std::size_t>> by_index;
                for (auto idx : *needs_build_ptr)
                    by_index[(*files_ptr)[idx].index_path].push_back(idx);

                for (auto& [ipath, idxs] : by_index) {
                    dstd::Vector<dstd::String> paths;
                    paths.reserve(idxs.size());
                    for (auto idx : idxs)
                        paths.emplace_back(
                            dstd::StringView((*files_ptr)[idx].path));
                    dftracer::utils::trace::index::IndexerOptions opts;
                    opts.index_dir = index_dir;
                    opts.checkpoint_size = checkpoint_size;
                    opts.parallelism = max_concurrent;
                    auto ix = dftracer::utils::trace::index::Indexer::open(
                        std::move(paths), std::move(opts));
                    if (!ix) {
                        DFTRACER_UTILS_LOG_WARN("TraceIndex: {}",
                                                ix.error().message());
                    } else {
                        auto built =
                            co_await coro::with_scope([&](coro::Scope& scope) {
                                return ix->build(scope);
                            });
                        if (!built)
                            DFTRACER_UTILS_LOG_WARN("TraceIndex: {}",
                                                    built.error().message());
                    }

                    dstd::Optional<trace::index::Snapshot> snapshot;
                    if (auto opened = trace::index::plan::open_snapshot(ipath);
                        opened.has_value())
                        snapshot.emplace(std::move(*opened));

                    for (auto idx : idxs) {
                        auto* info = &(*files_ptr)[idx];
                        bool bloom_ok = false;
                        if (snapshot) {
                            const auto file = snapshot->file(
                                fileio::get_logical_path(info->path));
                            bloom_ok =
                                file &&
                                trace::index::extension_current(
                                    *snapshot, *file,
                                    trace::index::store::IndexExtension::Stats);
                        }
                        if (bloom_ok) {
                            info->index_path = ipath;
                            info->has_bloom_data = true;
                            const auto has_index =
                                dstd::fs::exists(dstd::PathView(ipath));
                            if (!has_index.has_value())
                                co_return has_index.error();
                            info->has_checkpoint_index = *has_index;
                            large_files_ptr->push_back(idx);
                        } else {
                            DFTRACER_UTILS_LOG_WARN(
                                "TraceIndex: failed to index {}", info->path);
                        }
                    }
                }
            }

            if (!large_files_ptr->empty()) {
                auto meta_chan =
                    coro::make_channel<std::size_t>(max_concurrent * 2);

                auto meta_scope =
                    [&meta_chan, files_ptr, large_files_ptr, schemas_ptr,
                     max_concurrent](
                        coro::Scope& scope) -> coro::CoroTask<void> {
                    scope.spawn(
                        [ch = meta_chan->producer(),
                         large_files_ptr]() mutable -> coro::CoroTask<void> {
                            auto guard = ch.guard();
                            for (auto idx : *large_files_ptr) {
                                const auto send_ok = co_await ch.send(idx);
                                if (!send_ok) co_return;
                            }
                            co_return;
                        });

                    for (std::size_t w = 0; w < max_concurrent; ++w) {
                        scope.spawn([ch = meta_chan->consumer(), files_ptr,
                                     schemas_ptr]() -> coro::CoroTask<void> {
                            for (;;) {
                                auto fi_opt = co_await ch.receive();
                                if (!fi_opt) break;
                                std::size_t fi = *fi_opt;
                                auto* info = &(*files_ptr)[fi];

                                if (info->has_bloom_data) {
                                    if (auto read = read_file_bounds(
                                            *info, (*schemas_ptr)[fi]);
                                        !read)
                                        DFTRACER_UTILS_LOG_WARN(
                                            "TraceIndex: failed to read time "
                                            "bounds from {}: {}",
                                            info->index_path,
                                            read.error().message());
                                }

                                auto meta_input =
                                    MetadataCollectorUtilityInput::from_file(
                                        dstd::String(info->path))
                                        .with_index(
                                            dstd::String(info->index_path));
                                MetadataCollectorUtility collector;
                                auto metadata = co_await collector(meta_input);
                                if (metadata.success) {
                                    info->uncompressed_size =
                                        metadata.uncompressed_size;
                                    info->checkpoint_count =
                                        metadata.checkpoint_count;
                                    info->checkpoint_size =
                                        metadata.checkpoint_size;
                                    info->compressed_size =
                                        metadata.compressed_size;
                                    info->line_count = metadata.line_count;
                                    info->size_mb = metadata.size_mb;
                                }
                            }
                            co_return;
                        });
                    }
                    co_return;
                };
                co_await coro::with_scope(meta_scope);

                for (auto fi : *large_files_ptr) {
                    const auto& info = (*files_ptr)[fi];
                    if (info.min_timestamp_us > 0 &&
                        info.min_timestamp_us < *global_min_ts_ptr)
                        *global_min_ts_ptr = info.min_timestamp_us;
                    if (info.max_timestamp_us > *global_max_ts_ptr)
                        *global_max_ts_ptr = info.max_timestamp_us;
                }
            }

            co_return dstd::Result<void>{};
        };

        const auto ran = configure_default_runtime(settings).block_on(
            coro::named("TraceIndexInit", coro::with_scope(init_task)));
        if (!ran) {
            DFTRACER_UTILS_LOG_ERROR("TraceIndex: init failed: {}",
                                     ran.error().format());
            co_return ran.error();
        }
    }

    // One time unit per trace: resolve it once from the first file's CM.
    if (!files_.empty()) {
        trace::reader::TraceReaderConfig cfg;
        cfg.file_path = files_.front().path;
        cfg.index_dir = index_dir_;
        trace::reader::TraceReader reader(cfg);
        auto metric = co_await reader.read_time_metric();
        if (metric)
            time_metric_ = *metric;
        else
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: failed to resolve time_metric: {}",
                metric.error().message());
        if (time_metric_ != TimeMetric::Us) {
            DFTRACER_UTILS_LOG_INFO(
                "TraceIndex: trace time unit is {} (scaling viz to us)",
                dstd::String(time_metric_to_string(time_metric_)));
        }
    }

    // A viewer reads one record schema, voted by the files that hold
    // records; files of another are reported, and a request that reads them
    // fails with the View's error.
    if (!files_.empty()) {
        dstd::Vector<dftracer::utils::trace::index::plan::RowSetFile> sources;
        sources.reserve(files_.size());
        for (const FileInfo& f : files_)
            sources.push_back({dstd::String(dstd::StringView(f.path)),
                               dstd::String(dstd::StringView(f.index_path))});
        auto voted = dftracer::utils::trace::index::plan::files_schema(sources);
        if (voted) {
            record_schema_ = *voted;
            for (std::size_t i = 0; i < files_.size(); ++i) {
                const auto* schema = file_schemas[i];
                if (schema && schema->id != record_schema_->id)
                    DFTRACER_UTILS_LOG_WARN(
                        "TraceIndex: {} has record schema {}, not {}; a viewer "
                        "serves files of one record schema",
                        files_[i].path, schema->id, record_schema_->id);
            }
        } else {
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: failed to resolve the record schema: {}",
                voted.error().message());
        }
    }

    DFTRACER_UTILS_LOG_INFO("TraceIndex: found {} trace files in {}",
                            files_.size(), directory_);
    if (global_max_ts_ > 0) {
        DFTRACER_UTILS_LOG_INFO("TraceIndex: global time range [{}, {}] us",
                                global_min_ts_, global_max_ts_);
    }

    // Adopt a valid cached summary now rather than on the first request: the
    // viewer opens with several summary-backed requests at once, and all of
    // them would otherwise queue behind the load.
    load_persisted_viz_summary();
    co_return dstd::Result<void>{};
}

dstd::String TraceIndex::resolve(dstd::StringView rowset, dstd::StringView key,
                                 dstd::StringView value,
                                 const dstd::String& k) {
    if (k.empty()) return {};
    dstd::String column(rowset);
    column.append(1, '\0').append(key).append(1, '\0').append(value);
    std::lock_guard<std::mutex> lk(rowset_mutex_);
    auto it = rowset_columns_.find(column);
    if (it == rowset_columns_.end()) {
        dstd::Vector<dftracer::utils::trace::index::plan::RowSetFile> files;
        for (const auto& f : files_)
            files.push_back({dstd::String(dstd::StringView(f.path)),
                             dstd::String(dstd::StringView(f.index_path))});
        dftracer::utils::trace::index::plan::GroupResolver r(
            files, {{"v", dstd::String(rowset), dstd::String(key),
                     dstd::String(value)}});
        dstd::HashMap<dstd::String, dstd::String> values;
        if (const auto* c = r.column("v"))
            for (const auto& [kk, vv] : *c)
                values.emplace(kk, dstd::String(vv.data(), vv.size()));
        it =
            rowset_columns_.emplace(std::move(column), std::move(values)).first;
    }
    const auto hit = it->second.find(k);
    return hit == it->second.end() ? dstd::String() : hit->second;
}

dstd::Path TraceIndex::viz_summary_cache_path() const {
    return dstd::PathView(index_dir_) / ".dftviz_summary";
}

// A changed source is re-indexed in initialize(), shifting these per-file
// values, so the cache invalidates itself on mismatch.
dstd::String TraceIndex::viz_summary_fingerprint() const {
    std::uint64_t h = dftracer::utils::dstd::hash::FNV1A_OFFSET_BASIS_LEGACY;
    auto mix = [&](const void* data, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= dftracer::utils::dstd::hash::FNV1A_PRIME;
        }
    };
    std::uint32_t ver = VIZ_SUMMARY_FORMAT_VERSION;
    mix(&ver, sizeof ver);
    int tm = static_cast<int>(time_metric_);
    mix(&tm, sizeof tm);
    const std::uint64_t schema = record_schema().params_hash();
    mix(&schema, sizeof schema);
    mix(&global_min_ts_, sizeof global_min_ts_);
    mix(&global_max_ts_, sizeof global_max_ts_);
    for (const auto& f : files_) {
        mix(f.path.data(), f.path.size());
        mix(&f.uncompressed_size, sizeof f.uncompressed_size);
        mix(&f.checkpoint_count, sizeof f.checkpoint_count);
        mix(&f.min_timestamp_us, sizeof f.min_timestamp_us);
        mix(&f.max_timestamp_us, sizeof f.max_timestamp_us);
    }
    return dstd::fmt<"{:016x}">(h);
}

bool TraceIndex::load_persisted_viz_summary() {
    const auto path = viz_summary_cache_path();
    const auto loaded = dstd::fs::read_file(path.view());
    if (!loaded || loaded->empty()) return false;
    const dstd::String& buf = *loaded;

    BufReader r{buf.data(), buf.data() + buf.size()};
    if (r.u32() != VIZ_SUMMARY_MAGIC || r.u32() != VIZ_SUMMARY_FORMAT_VERSION)
        return false;
    dstd::String fp = r.str();
    if (!r.ok || fp != viz_summary_fingerprint()) return false;

    auto summary = dstd::make_unique<VizSummary>();
    if (!deserialize_summary(r, *summary)) return false;

    set_viz_summary(std::move(summary));
    DFTRACER_UTILS_LOG_INFO("TraceIndex: loaded cached viz summary from {}",
                            path);
    return true;
}

void TraceIndex::persist_viz_summary() const {
    if (viz_summary_state_.load(std::memory_order_acquire) != 2 ||
        !viz_summary_)
        return;
    BufWriter w;
    w.u32(VIZ_SUMMARY_MAGIC);
    w.u32(VIZ_SUMMARY_FORMAT_VERSION);
    w.str(viz_summary_fingerprint());
    serialize_summary(w, *viz_summary_);

    const auto path = viz_summary_cache_path();
    dstd::String tmp_text(path.str());
    tmp_text.append(".tmp");
    const dstd::Path tmp{dstd::StringView(tmp_text)};
    if (!dstd::fs::write_file(tmp.view(),
                              dstd::StringView(w.b.data(), w.b.size()))) {
        dstd::fs::remove(tmp.view()).discard();
        return;
    }
    // A cache that cannot be published is dropped, and the next run
    // builds it again.
    if (!dstd::fs::rename(tmp.view(), path.view())) {
        dstd::fs::remove(tmp.view()).discard();
    }
}

const TraceIndex::FileInfo* TraceIndex::find_file(
    const dstd::String& path) const {
    auto it = path_to_index_.find(path);
    if (it == path_to_index_.end()) return nullptr;
    return &files_[it->second];
}

const TraceIndex::FileInfo* TraceIndex::file_at(std::size_t index) const {
    if (index >= files_.size()) return nullptr;
    return &files_[index];
}

dstd::SharedPtr<const TraceIndex::FileChunkMeta> TraceIndex::chunk_meta(
    const FileInfo& file) {
    {
        std::lock_guard<std::mutex> lock(chunk_meta_mutex_);
        auto it = chunk_meta_.find(file.path);
        if (it != chunk_meta_.end()) return it->second;
    }

    // Build outside the lock: the index read is slow and the result is
    // immutable, so a rare concurrent double-read just discards one copy.
    dstd::SharedPtr<const FileChunkMeta> meta;  // null == known-unavailable
    if (!file.index_path.empty()) {
        auto built = read_chunk_meta(file);
        if (built)
            meta = std::move(*built);
        else
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: chunk metadata read failed for {}: {}", file.path,
                built.error().message());
    }

    std::lock_guard<std::mutex> lock(chunk_meta_mutex_);
    return chunk_meta_.try_emplace(file.path, std::move(meta)).first->second;
}

dstd::Vector<const TraceIndex::FileInfo*> collect_candidate_files(
    TraceIndex& index, const QueryParams& params) {
    dstd::Vector<const TraceIndex::FileInfo*> files;
    auto file_param = params.get("file");
    if (!file_param.empty()) {
        auto* f = index.find_file(dstd::String(file_param));
        if (f) files.push_back(f);
    } else {
        for (const auto& f : index.files()) {
            files.push_back(&f);
        }
    }
    return files;
}

}  // namespace dftracer::utils::domains::server
