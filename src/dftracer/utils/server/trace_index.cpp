#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/constants.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::indexing;
using namespace dftracer::utils::utilities::filesystem;
namespace indexer = dftracer::utils::utilities::indexer;

namespace {

// Bump VERSION when the on-disk layout below changes; the loader then ignores
// the stale cache.
constexpr std::uint32_t VIZ_SUMMARY_MAGIC = 0x315A5644;  // "DVZ1"
constexpr std::uint32_t VIZ_SUMMARY_FORMAT_VERSION = 9;

struct BufWriter {
    std::string b;
    void u32(std::uint32_t v) { b.append(reinterpret_cast<char*>(&v), 4); }
    void u64(std::uint64_t v) { b.append(reinterpret_cast<char*>(&v), 8); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void dbl(double v) { b.append(reinterpret_cast<char*>(&v), 8); }
    void str(const std::string& s) {
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
    std::string str() {
        std::uint64_t n = u64();
        if (!ok || n > static_cast<std::uint64_t>(end - p)) {
            ok = false;
            return {};
        }
        std::string s(p, p + n);
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
                      const std::vector<VizSummary::GroupRow>& rows) {
    w.u64(rows.size());
    for (const auto& r : rows) {
        w.str(r.key);
        w.u64(r.count);
        w.dbl(r.total);
        w.dbl(r.min);
        w.dbl(r.max);
    }
}

bool read_group_rows(BufReader& r, std::vector<VizSummary::GroupRow>& rows) {
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

void write_spans(BufWriter& w, const std::vector<VizSummary::AppSpan>& spans) {
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

bool read_spans(BufReader& r, std::vector<VizSummary::AppSpan>& spans) {
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
        auto wvec_lvl = [&](const std::vector<double>& v) {
            w.u64(v.size());
            for (double x : v) w.dbl(x);
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
        auto rvec_lvl = [&](std::vector<double>& v) -> bool {
            std::uint64_t n = r.u64();
            if (!r.fits(n, 8)) return false;
            v.resize(n);
            for (auto& x : v) x = r.dbl();
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
    if (!r.fits(nprocs, 8 * 5 + 8 * 2)) return false;
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

TraceIndex::TraceIndex(const std::string& directory,
                       const std::string& index_dir, std::size_t max_concurrent,
                       std::size_t checkpoint_size)
    : directory_(directory),
      index_dir_(index_dir),
      max_concurrent_(max_concurrent == 0 ? 8 : max_concurrent),
      checkpoint_size_(checkpoint_size == 0
                           ? constants::indexer::DEFAULT_CHECKPOINT_SIZE
                           : checkpoint_size) {}

coro::CoroTask<void> TraceIndex::initialize() {
    PatternDirectoryScannerUtility scanner;
    PatternDirectoryScannerUtilityInput scan_input{
        directory_, {".pfw", ".pfw.gz"}, false};
    auto entries = co_await scanner(scan_input);

    files_.clear();
    path_to_index_.clear();
    files_.reserve(entries.size());

    global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    global_max_ts_ = 0;

    std::vector<std::size_t> needs_build;
    std::vector<std::size_t> large_files;

    // The reuse decision below is existence-only, so a changed .pfw would be
    // served from a stale index. Drop any (shared) index root whose sources
    // changed since indexing; the loop then rebuilds it as if absent.
    {
        std::unordered_map<std::string, std::vector<std::string>> by_root;
        for (const auto& entry : entries)
            by_root[internal::determine_index_path(entry.path.string(),
                                                   index_dir_)]
                .push_back(entry.path.string());
        for (auto& [root, paths] : by_root) {
            if (!fs::exists(root)) continue;
            bool stale = false;
            try {
                indexer::IndexDatabase db(root);
                stale = db.find_stale_files(paths).stale();
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN(
                    "TraceIndex: stale check failed for %s: %s; rebuilding",
                    root.c_str(), e.what());
                stale = true;
            }
            if (stale) {
                DFTRACER_UTILS_LOG_WARN(
                    "TraceIndex: source changed since indexing; rebuilding %s",
                    root.c_str());
                std::error_code ec;
                fs::remove_all(root, ec);
            }
        }
    }

    for (const auto& entry : entries) {
        FileInfo info;
        info.path = entry.path.string();
        info.index_path = internal::determine_index_path(info.path, index_dir_);

        std::error_code ec;
        auto fsize = fs::file_size(info.path, ec);
        info.compressed_size = (!ec && fsize > 0) ? fsize : 0;

        std::size_t idx = files_.size();
        path_to_index_[info.path] = idx;

        info.has_bloom_data = fs::exists(info.index_path);
        info.has_checkpoint_index = fs::exists(info.index_path);
        if (!info.has_bloom_data) {
            needs_build.push_back(idx);
        } else {
            large_files.push_back(idx);
        }

        files_.push_back(std::move(info));
    }

    if (!needs_build.empty() || !large_files.empty()) {
        auto pipeline_config =
            PipelineConfig()
                .with_name("TraceIndex Init")
                .with_compute_threads(max_concurrent_)
                .with_watchdog(false)
                .with_global_timeout(std::chrono::seconds(0))
                .with_task_timeout(std::chrono::seconds(0))
                .with_io_backend(io::IoBackendType::THREADPOOL)
                .with_io_batch_size(1);

        Pipeline pipeline(pipeline_config);

        auto* files_ptr = &files_;
        auto* needs_build_ptr = &needs_build;
        auto* large_files_ptr = &large_files;
        auto* global_min_ts_ptr = &global_min_ts_;
        auto* global_max_ts_ptr = &global_max_ts_;
        std::string index_dir = index_dir_;
        std::size_t max_concurrent = max_concurrent_;
        std::size_t checkpoint_size = checkpoint_size_;

        auto init_task = make_task(
            [files_ptr, needs_build_ptr, large_files_ptr, global_min_ts_ptr,
             global_max_ts_ptr, index_dir, max_concurrent,
             checkpoint_size](CoroScope& ctx) -> coro::CoroTask<void> {
                if (!needs_build_ptr->empty()) {
                    DFTRACER_UTILS_LOG_INFO(
                        "TraceIndex: building index for %zu file(s) ...",
                        needs_build_ptr->size());

                    // One batch build per index root; the batch builder assigns
                    // file ids itself, so no manual pre-assignment is needed.
                    std::unordered_map<std::string, std::vector<std::size_t>>
                        by_index;
                    for (auto idx : *needs_build_ptr)
                        by_index[(*files_ptr)[idx].index_path].push_back(idx);

                    for (auto& [ipath, idxs] : by_index) {
                        auto batch_config =
                            std::make_shared<indexer::IndexBuildBatchConfig>();
                        batch_config->file_paths.reserve(idxs.size());
                        for (auto idx : idxs)
                            batch_config->file_paths.push_back(
                                (*files_ptr)[idx].path);
                        batch_config->index_dir = index_dir;
                        batch_config->checkpoint_size = checkpoint_size;
                        batch_config->parallelism = max_concurrent;
                        co_await indexer::IndexBatchBuilderUtility::process(
                            &ctx, std::move(batch_config));

                        for (auto idx : idxs) {
                            auto* info = &(*files_ptr)[idx];
                            bool bloom_ok = false;
                            try {
                                indexer::IndexDatabase db(
                                    ipath, indexer::IndexOpenMode::ReadOnly);
                                int fid = db.get_file_info_id(
                                    indexer::internal::get_logical_path(
                                        info->path));
                                bloom_ok = fid >= 0 && db.has_bloom_data(fid);
                            } catch (const std::exception&) {
                                bloom_ok = false;
                            }
                            if (bloom_ok) {
                                info->index_path = ipath;
                                info->has_bloom_data = true;
                                info->has_checkpoint_index = fs::exists(ipath);
                                large_files_ptr->push_back(idx);
                            } else {
                                DFTRACER_UTILS_LOG_WARN(
                                    "TraceIndex: failed to index %s",
                                    info->path.c_str());
                            }
                        }
                    }
                }

                if (!large_files_ptr->empty()) {
                    auto meta_chan =
                        coro::make_channel<std::size_t>(max_concurrent * 2);

                    co_await ctx.scope([&meta_chan, files_ptr, large_files_ptr,
                                        max_concurrent](CoroScope& scope)
                                           -> coro::CoroTask<void> {
                        scope.spawn(
                            [ch = meta_chan->producer(), large_files_ptr](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                                auto guard = ch.guard();
                                for (auto idx : *large_files_ptr) {
                                    if (!co_await ch.send(idx)) co_return;
                                }
                                co_return;
                            });

                        for (std::size_t w = 0; w < max_concurrent; ++w) {
                            scope.spawn([ch = meta_chan->consumer(),
                                         files_ptr](CoroScope&)
                                            -> coro::CoroTask<void> {
                                while (auto fi_opt = co_await ch.receive()) {
                                    std::size_t fi = *fi_opt;
                                    auto* info = &(*files_ptr)[fi];

                                    if (info->has_bloom_data) {
                                        try {
                                            indexer::IndexDatabase idx_db(
                                                info->index_path);
                                            auto logical = indexer::internal::
                                                get_logical_path(info->path);
                                            int fid = idx_db.get_file_info_id(
                                                logical);
                                            if (fid >= 0) {
                                                auto bounds =
                                                    idx_db.query_time_bounds(
                                                        fid);
                                                if (bounds.valid) {
                                                    info->min_timestamp_us =
                                                        bounds.min_timestamp_us;
                                                    info->max_timestamp_us =
                                                        bounds.max_timestamp_us;
                                                }
                                            }
                                        } catch (const std::exception& e) {
                                            DFTRACER_UTILS_LOG_WARN(
                                                "TraceIndex: failed to "
                                                "read time bounds from "
                                                "%s: %s",
                                                info->index_path.c_str(),
                                                e.what());
                                        }
                                    }

                                    auto meta_input =
                                        MetadataCollectorUtilityInput::
                                            from_file(info->path)
                                                .with_index(info->index_path);
                                    auto metadata =
                                        co_await MetadataCollectorUtility{}(
                                            meta_input);
                                    if (metadata.success) {
                                        info->uncompressed_size =
                                            metadata.uncompressed_size;
                                        info->num_checkpoints =
                                            metadata.num_checkpoints;
                                        info->checkpoint_size =
                                            metadata.checkpoint_size;
                                        info->compressed_size =
                                            metadata.compressed_size;
                                        info->num_lines = metadata.num_lines;
                                        info->size_mb = metadata.size_mb;
                                    }
                                }
                                co_return;
                            });
                        }
                        co_return;
                    });

                    for (auto fi : *large_files_ptr) {
                        const auto& info = (*files_ptr)[fi];
                        if (info.min_timestamp_us > 0 &&
                            info.min_timestamp_us < *global_min_ts_ptr)
                            *global_min_ts_ptr = info.min_timestamp_us;
                        if (info.max_timestamp_us > *global_max_ts_ptr)
                            *global_max_ts_ptr = info.max_timestamp_us;
                    }
                }

                co_return;
            },
            "TraceIndexInit");

        pipeline.set_source(init_task);
        pipeline.set_destination(init_task);
        pipeline.execute();
    }

    // One time unit per trace: resolve it once from the first file's CM.
    if (!files_.empty()) {
        try {
            utilities::reader::TraceReaderConfig cfg;
            cfg.file_path = files_.front().path;
            cfg.index_dir = index_dir_;
            utilities::reader::TraceReader reader(cfg);
            time_metric_ = co_await reader.read_time_metric();
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: failed to resolve time_metric: %s", e.what());
        }
        if (time_metric_ != TimeMetric::US) {
            DFTRACER_UTILS_LOG_INFO(
                "TraceIndex: trace time unit is %s (scaling viz to us)",
                std::string(time_metric_to_string(time_metric_)).c_str());
        }
    }

    DFTRACER_UTILS_LOG_INFO("TraceIndex: found %zu trace files in %s",
                            files_.size(), directory_.c_str());
    if (global_max_ts_ > 0) {
        DFTRACER_UTILS_LOG_INFO("TraceIndex: global time range [%" PRIu64
                                ", %" PRIu64 "] us",
                                global_min_ts_, global_max_ts_);
    }

    // Adopt a valid cached summary now rather than on the first request: the
    // viewer opens with several summary-backed requests at once, and all of
    // them would otherwise queue behind the load.
    load_persisted_viz_summary();
}

// Looks up single hashes and keeps each root's database open, rather than
// caching whole hash tables per request.
std::string TraceIndex::resolve_hash(HashType type, const std::string& hash) {
    if (hash.empty()) return {};
    // Memoized across requests, misses included: the viewer re-asks for the
    // same hashes on every zoom.
    std::string memo_key;
    memo_key.reserve(hash.size() + 1);
    memo_key.push_back(static_cast<char>(static_cast<int>(type)));
    memo_key.append(hash);
    {
        std::lock_guard<std::mutex> lk(hash_db_mutex_);
        auto it = hash_names_.find(memo_key);
        if (it != hash_names_.end()) return it->second;
    }
    std::string resolved;
    for (const auto& f : files_) {
        if (f.index_path.empty()) continue;
        std::shared_ptr<indexer::IndexDatabase> db;
        {
            std::lock_guard<std::mutex> lk(hash_db_mutex_);
            auto it = hash_dbs_.find(f.index_path);
            if (it != hash_dbs_.end()) {
                db = it->second;
            } else {
                try {
                    db = std::make_shared<indexer::IndexDatabase>(
                        f.index_path, dftracer::utils::utilities::indexer::
                                          IndexOpenMode::ReadOnly);
                } catch (const std::exception&) {
                    db = nullptr;
                }
                hash_dbs_.emplace(f.index_path, db);
            }
        }
        if (!db) continue;
        try {
            auto name = db->lookup_hash(type, hash);
            if (name && !name->empty()) {
                resolved = std::move(*name);
                break;
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    {
        std::lock_guard<std::mutex> lk(hash_db_mutex_);
        hash_names_.emplace(std::move(memo_key), resolved);
    }
    return resolved;
}

std::string TraceIndex::viz_summary_cache_path() const {
    return (fs::path(index_dir_) / ".dftviz_summary").string();
}

// A changed source is re-indexed in initialize(), shifting these per-file
// values, so the cache invalidates itself on mismatch.
std::string TraceIndex::viz_summary_fingerprint() const {
    std::uint64_t h = dftracer::utils::hash::FNV1A_OFFSET_BASIS_LEGACY;
    auto mix = [&](const void* data, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= dftracer::utils::hash::FNV1A_PRIME;
        }
    };
    std::uint32_t ver = VIZ_SUMMARY_FORMAT_VERSION;
    mix(&ver, sizeof ver);
    int tm = static_cast<int>(time_metric_);
    mix(&tm, sizeof tm);
    mix(&global_min_ts_, sizeof global_min_ts_);
    mix(&global_max_ts_, sizeof global_max_ts_);
    for (const auto& f : files_) {
        mix(f.path.data(), f.path.size());
        mix(&f.uncompressed_size, sizeof f.uncompressed_size);
        mix(&f.num_checkpoints, sizeof f.num_checkpoints);
        mix(&f.min_timestamp_us, sizeof f.min_timestamp_us);
        mix(&f.max_timestamp_us, sizeof f.max_timestamp_us);
    }
    char out[17];
    std::snprintf(out, sizeof out, "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(out);
}

bool TraceIndex::load_persisted_viz_summary() {
    try {
        auto path = viz_summary_cache_path();
        std::ifstream is(path, std::ios::binary | std::ios::ate);
        if (!is) return false;
        auto size = is.tellg();
        if (size <= 0) return false;
        std::string buf(static_cast<std::size_t>(size), '\0');
        is.seekg(0);
        if (!is.read(buf.data(), size)) return false;

        BufReader r{buf.data(), buf.data() + buf.size()};
        if (r.u32() != VIZ_SUMMARY_MAGIC ||
            r.u32() != VIZ_SUMMARY_FORMAT_VERSION)
            return false;
        std::string fp = r.str();
        if (!r.ok || fp != viz_summary_fingerprint()) return false;

        auto summary = std::make_unique<VizSummary>();
        if (!deserialize_summary(r, *summary)) return false;

        set_viz_summary(std::move(summary));
        DFTRACER_UTILS_LOG_INFO("TraceIndex: loaded cached viz summary from %s",
                                path.c_str());
        return true;
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("TraceIndex: viz summary cache load failed: %s",
                                e.what());
        return false;
    }
}

void TraceIndex::persist_viz_summary() const {
    if (viz_summary_state_.load(std::memory_order_acquire) != 2 ||
        !viz_summary_)
        return;
    try {
        BufWriter w;
        w.u32(VIZ_SUMMARY_MAGIC);
        w.u32(VIZ_SUMMARY_FORMAT_VERSION);
        w.str(viz_summary_fingerprint());
        serialize_summary(w, *viz_summary_);

        auto path = viz_summary_cache_path();
        auto tmp = path + ".tmp";
        {
            std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
            if (!os.write(w.b.data(),
                          static_cast<std::streamsize>(w.b.size()))) {
                std::error_code rec;
                fs::remove(tmp, rec);
                return;
            }
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) fs::remove(tmp, ec);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN(
            "TraceIndex: viz summary cache write failed: %s", e.what());
    }
}

const TraceIndex::FileInfo* TraceIndex::find_file(
    const std::string& path) const {
    auto it = path_to_index_.find(path);
    if (it == path_to_index_.end()) return nullptr;
    return &files_[it->second];
}

const TraceIndex::FileInfo* TraceIndex::file_at(std::size_t index) const {
    if (index >= files_.size()) return nullptr;
    return &files_[index];
}

std::shared_ptr<const TraceIndex::FileChunkMeta> TraceIndex::chunk_meta(
    const FileInfo& file) {
    {
        std::lock_guard<std::mutex> lock(chunk_meta_mutex_);
        auto it = chunk_meta_.find(file.path);
        if (it != chunk_meta_.end()) return it->second;
    }

    // Build outside the lock: the index read is slow and the result is
    // immutable, so a rare concurrent double-read just discards one copy.
    std::shared_ptr<const FileChunkMeta> meta;  // null == known-unavailable
    if (!file.index_path.empty()) {
        try {
            indexer::IndexDatabase db(
                file.index_path,
                dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
            int fid = db.get_file_info_id(
                indexer::internal::get_logical_path(file.path));
            if (fid >= 0) {
                auto m = std::make_shared<FileChunkMeta>();
                m->spans = db.query_chunk_spans(fid);
                m->stats = db.query_chunk_statistics(fid);
                meta = std::move(m);
            }
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN(
                "TraceIndex: chunk metadata read failed for %s: %s",
                file.path.c_str(), e.what());
        }
    }

    std::lock_guard<std::mutex> lock(chunk_meta_mutex_);
    return chunk_meta_.try_emplace(file.path, std::move(meta)).first->second;
}

std::vector<const TraceIndex::FileInfo*> collect_candidate_files(
    TraceIndex& index, const QueryParams& params) {
    std::vector<const TraceIndex::FileInfo*> files;
    auto file_param = params.get("file");
    if (!file_param.empty()) {
        auto* f = index.find_file(std::string(file_param));
        if (f) files.push_back(f);
    } else {
        for (const auto& f : index.files()) {
            files.push_back(&f);
        }
    }
    return files;
}

}  // namespace dftracer::utils::server
