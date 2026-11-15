// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
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
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/hash/hex64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
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
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
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

// True when the index at `root` no longer matches `paths` in `directory`: a
// path is not indexed, its stat changed, or the index holds another path from
// the same directory that is not listed. `unlisted` reports those entries, the
// ones a stale rebuild drops from the root: no other directory names them, so
// keeping them makes this directory stale on every start. A path from another
// directory that shares the root makes neither stale. A file belongs to the
// directory only when its parent directory is the directory, because the scan
// is not recursive and a subfolder is a trace folder of its own.
dstd::Result<bool> index_is_stale(dstd::StringView root,
                                  const dstd::Vector<dstd::String>& paths,
                                  dstd::StringView directory,
                                  dstd::Vector<dstd::String>& unlisted) {
    auto opened = trace::index::plan::open_snapshot(root);
    if (!opened) return opened.error();
    const auto& snapshot = *opened;
    dstd::HashSet<dstd::StringView> listed;
    dstd::Vector<dstd::String> logical_paths;
    logical_paths.reserve(paths.size());
    bool changed = false;
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
            changed = true;
    }
    const auto files = snapshot.files();
    if (!files) return files.error();
    // The index names a file by its logical path, which is absolute and
    // normalized, so the folder is built the same way; dstd::normalize alone
    // keeps a relative folder relative and matches nothing.
    const dstd::String folder = fileio::get_logical_path(directory);
    const dstd::StringView folder_view(folder);
    for (const auto& file : *files) {
        if (listed.find(file.path()) != listed.end()) continue;
        if (dstd::PathView(file.path()).parent().str() != folder_view) continue;
        unlisted.emplace_back(file.path());
    }
    return changed || !unlisted.empty();
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

// Bump VERSION when the summary value layout below changes; the store namespace
// version then keeps older copies from loading. It also starts the folder
// fingerprint, so a bump recomputes the stored viz results as well.
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

constexpr dstd::StringView VIZ_SUMMARY_NAMESPACE = "server.viz_summary";
constexpr dstd::StringView VIZ_SUMMARY_KEY = "summary";
// Bump VERSION when the stored result layout below changes; the store namespace
// version then keeps older copies from loading.
constexpr dstd::StringView VIZ_RESULT_NAMESPACE = "server.viz_result";
constexpr std::uint32_t VIZ_RESULT_FORMAT_VERSION = 0;

dftracer::utils::store::NamespaceSpec viz_summary_namespace() {
    dftracer::utils::store::NamespaceSpec spec;
    spec.name.assign(VIZ_SUMMARY_NAMESPACE.data(),
                     VIZ_SUMMARY_NAMESPACE.size());
    spec.format_version = VIZ_SUMMARY_FORMAT_VERSION;
    return spec;
}

// Shared so a result written by an earlier run is still there: a scope holds
// every writer's keys, and the fingerprint in a key keeps an old copy from
// being served.
dftracer::utils::store::NamespaceSpec viz_result_namespace() {
    dftracer::utils::store::NamespaceSpec spec;
    spec.name.assign(VIZ_RESULT_NAMESPACE.data(), VIZ_RESULT_NAMESPACE.size());
    spec.format_version = VIZ_RESULT_FORMAT_VERSION;
    spec.shared = true;
    return spec;
}

dstd::String viz_store_path(const TraceIndex& index) {
    return dftracer::utils::trace::index::store_of(
        dftracer::utils::trace::internal::determine_index_path(
            index.directory(), index.index_dir()));
}

// One scope per folder, in both namespaces, so a file added to or removed from
// the folder keeps the folder's scope: the fingerprint in a result key and in
// the stored summary is what tells a changed file set from an unchanged one.
// The folder is named the way the index names a file, absolute and normalized,
// so a relative folder name and an absolute one land on the same scope.
dftracer::utils::store::ScopeKey viz_folder_scope(const TraceIndex& index) {
    const dstd::String folder = fileio::get_logical_path(index.directory());
    return dftracer::utils::store::ScopeKey::file(dstd::StringView(folder));
}

// The stored bytes of the folder's responses, under its own key so the store
// keeps the count across a restart. A result key starts with the fingerprint,
// never with a NUL.
constexpr dstd::StringView VIZ_RESULT_BYTES_KEY("\0bytes", 6);

dstd::ByteView viz_result_bytes_key() {
    return dstd::ByteView(VIZ_RESULT_BYTES_KEY.data(),
                          VIZ_RESULT_BYTES_KEY.size());
}

// The most bytes of stored responses one folder keeps.
std::uint64_t viz_result_store_limit() {
    const auto parsed = common::Env::get<std::uint64_t>(
        common::constants::server::VIZ_RESULT_STORE_BYTES_ENV);
    return parsed.has_value()
               ? *parsed
               : common::constants::server::VIZ_RESULT_DEFAULT_STORE_BYTES;
}

// A stored result key carries the folder's fingerprint, so a re-indexed folder
// or a changed file misses even before the stale scope is dropped.
dstd::String viz_result_key(const TraceIndex& index,
                            const dstd::String& request_key) {
    return dstd::fmt<"{}|{}">(index.folder_fingerprint(), request_key);
}

coro::CoroTask<void> flush_viz_store(dftracer::utils::store::Store store,
                                     dstd::String directory) {
    const auto flushed = co_await store.flush();
    if (!flushed) {
        DFTRACER_UTILS_LOG_WARN(
            "TraceIndex: failed to flush the viz store of "
            "{}: {}",
            directory, flushed.error().message());
    }
    co_return;
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

TraceIndex::~TraceIndex() { drain_viz_writes(); }

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
    dstd::HashSet<dstd::String> dropped_paths;
    dstd::Vector<const dftracer::utils::trace::index::RecordSchema*>
        file_schemas(entries.size(), nullptr);
    auto default_schema = trace::index::get_schema("dftracer");
    if (!default_schema) co_return default_schema.error();
    record_schema_ = *default_schema;

    // The reuse decision below is existence-only, so a changed .pfw would be
    // served from a stale index. Drop this directory's entries from the shared
    // index root and summary scope when its sources changed; the loop then
    // rebuilds the dropped files as if absent.
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
            dstd::Vector<dstd::String> unlisted;
            const auto checked =
                index_is_stale(root, paths, directory_, unlisted);
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
                for (const auto& path : paths) dropped_paths.insert(path);
                auto viz_store = dftracer::utils::store::Store::open(store);
                if (viz_store) {
                    const auto scope = viz_folder_scope(*this);
                    const dftracer::utils::store::NamespaceSpec specs[] = {
                        viz_summary_namespace(), viz_result_namespace()};
                    for (const auto& spec : specs) {
                        auto ns = viz_store->ns(spec);
                        if (!ns) {
                            DFTRACER_UTILS_LOG_WARN(
                                "TraceIndex: failed to register {} for {}: "
                                "{}",
                                spec.name, root, ns.error().message());
                            continue;
                        }
                        const auto dropped = viz_store->drop(*ns, scope);
                        if (!dropped) {
                            DFTRACER_UTILS_LOG_WARN(
                                "TraceIndex: failed to drop {} of {}: {}",
                                spec.name, root, dropped.error().message());
                        }
                    }
                    const auto flushed = co_await viz_store->flush();
                    if (!flushed) {
                        DFTRACER_UTILS_LOG_WARN(
                            "TraceIndex: failed to flush the viz cache drops "
                            "for {}: {}",
                            root, flushed.error().message());
                    }
                } else {
                    DFTRACER_UTILS_LOG_WARN(
                        "TraceIndex: failed to open store for {}: {}", root,
                        viz_store.error().message());
                }
                auto index = trace::index::Index::open(root);
                if (index) {
                    // Every entry this directory owns: the listed paths, whose
                    // source changed, and the entries the scan no longer
                    // finds, whose source is gone.
                    dstd::Vector<dstd::String> dropped;
                    dropped.reserve(paths.size() + unlisted.size());
                    for (const auto& path : paths)
                        dropped.push_back(fileio::get_logical_path(path));
                    for (const auto& logical : unlisted)
                        dropped.push_back(logical);
                    for (const auto& logical : dropped) {
                        const auto removed = co_await index->remove(logical);
                        if (!removed) {
                            DFTRACER_UTILS_LOG_WARN(
                                "TraceIndex: failed to drop {} from {}: {}",
                                logical, root, removed.error().message());
                        }
                    }
                } else {
                    DFTRACER_UTILS_LOG_WARN(
                        "TraceIndex: failed to open index for {}: {}", root,
                        index.error().message());
                }
                trace::index::release_root(root);
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
        const bool was_dropped =
            dropped_paths.find(info.path) != dropped_paths.end();
        info.has_bloom_data = *has_store && !was_dropped;
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
    folder_fingerprint_ = compute_folder_fingerprint();
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

// A changed source is re-indexed in initialize(), and each file's current
// mtime and size are folded in here, so a stored value invalidates itself on
// mismatch. The hash is equality only; it is not used as a table key.
dstd::String TraceIndex::compute_folder_fingerprint() const {
    BufWriter w;
    w.u32(VIZ_SUMMARY_FORMAT_VERSION);
    w.u32(static_cast<std::uint32_t>(time_metric_));
    w.u64(record_schema().params_hash());
    w.u64(global_min_ts_);
    w.u64(global_max_ts_);
    for (const auto& f : files_) {
        w.str(f.path);
        common::InputFileStat st;
        std::uint64_t size = f.uncompressed_size;
        std::uint64_t mtime = 0;
        if (common::stat_input(f.path, st)) {
            size = st.size;
            mtime = static_cast<std::uint64_t>(st.mtime_ns);
        }
        w.u64(size);
        w.u64(mtime);
        w.u64(f.checkpoint_count);
        w.u64(f.min_timestamp_us);
        w.u64(f.max_timestamp_us);
    }
    const std::uint64_t h =
        dftracer::utils::dstd::hash::fnv1a_hash(w.b.data(), w.b.size());
    return dftracer::utils::dstd::hash::format_hex64(h);
}

// The folder's store handle, the writes no request waits for and the bytes the
// folder keeps stored. The writes hold a reference, so it outlives a request
// that started one; the index's destructor waits for them all.
struct TraceIndex::VizStore {
    VizStore(dftracer::utils::store::Store opened,
             dftracer::utils::store::Namespace result_namespace,
             dftracer::utils::store::ScopeKey result_scope,
             dstd::SharedPtr<Runtime> write_runtime)
        : store(std::move(opened)),
          ns(std::move(result_namespace)),
          scope(std::move(result_scope)),
          runtime(std::move(write_runtime)) {}

    dftracer::utils::store::Store store;
    dftracer::utils::store::Namespace ns;
    dftracer::utils::store::ScopeKey scope;
    /// The runtime the writes run on, so waiting for one never blocks it.
    dstd::SharedPtr<Runtime> runtime;
    /// One commit at a time, so the count a write stores never goes back.
    coro::AsyncMutex commit;
    /// The bytes of the folder's stored responses, seeded from the store.
    std::uint64_t bytes = 0;  ///< guarded by `commit`
    bool seeded = false;      ///< guarded by `commit`

    std::mutex pending_mutex;
    dstd::Vector<dstd::SharedFuture<dstd::Result<void>>> writes;
};

dstd::SharedPtr<TraceIndex::VizStore> TraceIndex::viz_store() {
    std::lock_guard<std::mutex> lock(viz_store_mutex_);
    if (viz_store_) return viz_store_;
    const dstd::String store_path = viz_store_path(*this);
    auto opened = dftracer::utils::store::Store::open(store_path);
    if (!opened) {
        DFTRACER_UTILS_LOG_WARN(
            "TraceIndex: failed to open the store for {}: {}", directory_,
            opened.error().message());
        return nullptr;
    }
    auto ns = opened->ns(viz_result_namespace());
    if (!ns) {
        DFTRACER_UTILS_LOG_WARN("TraceIndex: failed to open {} for {}: {}",
                                VIZ_RESULT_NAMESPACE, directory_,
                                ns.error().message());
        return nullptr;
    }
    auto viz = dstd::make_shared<VizStore>(std::move(*opened), std::move(*ns),
                                           viz_folder_scope(*this),
                                           default_runtime_shared());
    viz_store_ = std::move(viz);
    return viz_store_;
}

coro::CoroTask<void> TraceIndex::write_viz_result(dstd::SharedPtr<VizStore> viz,
                                                  dstd::String stored_key,
                                                  dstd::String body,
                                                  dstd::String directory) {
    co_await viz->commit.lock();
    coro::AsyncMutexGuard guard(viz->commit);

    if (!viz->seeded) {
        viz->seeded = true;
        auto snap = viz->store.snapshot();
        if (snap) {
            auto found = snap->get(viz->ns, viz->scope, viz_result_bytes_key());
            if (found && *found) {
                const dstd::ByteView bytes = (**found).bytes();
                std::uint64_t stored = 0;
                dstd::from_chars(
                    reinterpret_cast<const char*>(bytes.data()),
                    reinterpret_cast<const char*>(bytes.data()) + bytes.size(),
                    stored);
                viz->bytes = stored;
            }
        } else {
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: failed to read the stored bytes of {}: {}",
                directory, snap.error().message());
        }
    }

    // A folder past its bound starts again: the drop clears every stored
    // response of the folder, and this one is the first of the new set.
    std::uint64_t total = viz->bytes + body.size();
    if (total > viz_result_store_limit()) {
        const auto dropped = viz->store.drop(viz->ns, viz->scope);
        if (!dropped) {
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: failed to drop the stored viz results of {}: {}",
                directory, dropped.error().message());
        }
        total = body.size();
    }
    viz->bytes = total;

    const dstd::String count = dstd::to_string(total);
    auto batch = viz->store.batch(viz->ns, viz->scope);
    batch.put(dstd::ByteView(stored_key),
              dstd::ByteView(body.data(), body.size()));
    batch.put(viz_result_bytes_key(),
              dstd::ByteView(count.data(), count.size()));
    const auto committed = co_await viz->store.commit(std::move(batch));
    if (!committed) {
        DFTRACER_UTILS_LOG_WARN(
            "TraceIndex: failed to store a viz result of {}: {}", directory,
            committed.error().message());
    }
    co_return;
}

void TraceIndex::store_viz_result(dstd::String request_key, dstd::String body) {
    dstd::SharedPtr<VizStore> viz = viz_store();
    if (!viz || !viz->runtime) return;
    const dstd::String stored_key = viz_result_key(*this, request_key);
    auto handle = viz->runtime->submit(
        write_viz_result(viz, stored_key, std::move(body), directory_),
        "TraceIndexVizResult");

    // Keep every write that has not finished: the destructor waits for them.
    std::lock_guard<std::mutex> lock(viz->pending_mutex);
    dstd::Vector<dstd::SharedFuture<dstd::Result<void>>> running;
    running.reserve(viz->writes.size() + 1);
    for (auto& write : viz->writes) {
        if (write.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
            running.push_back(std::move(write));
    }
    running.push_back(std::move(handle.future));
    viz->writes = std::move(running);
}

dstd::Vector<dstd::Optional<dstd::String>> TraceIndex::load_viz_results(
    const dstd::Vector<dstd::String>& request_keys) {
    dstd::Vector<dstd::Optional<dstd::String>> out(request_keys.size());
    if (request_keys.empty()) return out;
    viz_result_store_read_count_.fetch_add(1, std::memory_order_relaxed);
    const dstd::String store_path = viz_store_path(*this);
    const auto exists = dstd::fs::exists(dstd::PathView(store_path));
    if (!exists.has_value() || !*exists) return out;
    dstd::SharedPtr<VizStore> viz = viz_store();
    if (!viz) return out;
    auto snap = viz->store.snapshot();
    if (!snap) return out;

    dstd::Vector<dstd::String> stored_keys;
    stored_keys.reserve(request_keys.size());
    for (const auto& key : request_keys)
        stored_keys.push_back(viz_result_key(*this, key));
    dstd::Vector<dstd::ByteView> views;
    views.reserve(stored_keys.size());
    for (const auto& key : stored_keys)
        views.emplace_back(key.data(), key.size());
    auto found = snap->multi_get(viz->ns, viz->scope,
                                 dstd::Span<const dstd::ByteView>(views));
    if (!found) return out;
    for (std::size_t i = 0; i < found->size(); ++i) {
        if (!(*found)[i]) continue;
        const dstd::ByteView bytes = (*(*found)[i]).bytes();
        out[i] = dstd::String(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size());
    }
    return out;
}

bool TraceIndex::load_persisted_viz_summary() {
    const dstd::String store_path = viz_store_path(*this);
    const auto exists = dstd::fs::exists(dstd::PathView(store_path));
    if (!exists.has_value() || !*exists) return false;

    auto opened = dftracer::utils::store::Store::open(store_path);
    if (!opened) return false;
    auto ns = opened->ns(viz_summary_namespace());
    if (!ns) return false;
    const auto scope = viz_folder_scope(*this);
    auto snap = opened->snapshot();
    if (!snap) return false;
    const auto found = snap->get(*ns, scope, dstd::ByteView(VIZ_SUMMARY_KEY));
    if (!found) return false;
    if (!*found) return false;
    const dstd::ByteView bytes = (**found).bytes();
    const char* data = reinterpret_cast<const char*>(bytes.data());
    BufReader r{data, data + bytes.size()};
    dstd::String fp = r.str();
    if (!r.ok || fp != folder_fingerprint()) return false;

    auto summary = dstd::make_unique<VizSummary>();
    if (!deserialize_summary(r, *summary)) return false;

    set_viz_summary(std::move(summary));
    DFTRACER_UTILS_LOG_INFO("TraceIndex: loaded cached viz summary from {}",
                            store_path);
    return true;
}

coro::CoroTask<dstd::Result<void>> TraceIndex::persist_viz_summary() {
    if (viz_summary_state_.load(std::memory_order_acquire) != 2 ||
        !viz_summary_)
        co_return dstd::Result<void>{};
    BufWriter w;
    w.str(folder_fingerprint());
    serialize_summary(w, *viz_summary_);

    const dstd::String store_path = viz_store_path(*this);
    auto opened = dftracer::utils::store::Store::open(store_path);
    if (!opened) co_return opened.error();
    auto ns = opened->ns(viz_summary_namespace());
    if (!ns) co_return ns.error();
    const auto scope = viz_folder_scope(*this);
    auto batch = opened->batch(*ns, scope);
    batch.put(dstd::ByteView(VIZ_SUMMARY_KEY),
              dstd::ByteView(w.b.data(), w.b.size()));
    const auto committed = co_await opened->commit(std::move(batch));
    if (!committed) co_return committed.error();
    co_return dstd::Result<void>{};
}

void TraceIndex::drain_viz_writes() {
    dstd::SharedPtr<VizStore> viz;
    {
        std::lock_guard<std::mutex> lock(viz_store_mutex_);
        viz = viz_store_;
    }
    if (!viz) return;
    dstd::Vector<dstd::SharedFuture<dstd::Result<void>>> writes;
    {
        std::lock_guard<std::mutex> lock(viz->pending_mutex);
        writes.swap(viz->writes);
    }
    auto rt = viz->runtime;
    if (!rt) return;
    // A worker of this runtime hands its run slot over for the wait, so a
    // write it waits for is not the one its own thread would run.
    const bool reentrant = coro::Executor::current() == rt->executor();
    coro::BlockingRegion block(reentrant ? rt->executor() : nullptr);
    for (auto& write : writes) write.get().discard();
    auto flushed =
        rt->submit(flush_viz_store(viz->store, directory_), "TraceIndexFlush")
            .get();
    if (!flushed) {
        DFTRACER_UTILS_LOG_WARN(
            "TraceIndex: failed to flush the viz store of {}: {}", directory_,
            flushed.error().message());
        return;
    }
    viz_store_flush_count_.fetch_add(1, std::memory_order_relaxed);
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
