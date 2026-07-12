#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/chunk_detail_scanner_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
#include <dftracer/utils/utilities/composites/dft/statistics/shared_index_statistics_reader.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/index_batch_writer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::filesystem;
using common::query::Query;
using dftracer::utils::utilities::indexer::ChunkStatistics;
using dftracer::utils::utilities::indexer::has_capability;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::IndexFileEntryCapability;
namespace cli = dftracer::utils::cli;

struct StatsConfig {
    std::string directory;
    std::string index_dir;
    bool json_output = false;
    std::uint64_t top_n = 0;
    std::uint64_t top_n_pid_tid = 10;
    bool no_auto_index = false;
    std::size_t checkpoint_size = 0;
    std::size_t executor_threads = 0;
    std::optional<Query> query;
    std::vector<std::string> filter_names;
    std::vector<std::string> filter_cats;
    std::vector<std::string> group_by;
    StatisticsQueryType report_type = StatisticsQueryType::SUMMARY;
};

class StatsArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args{"Trace files to inspect (.pfw, .pfw.gz)"};
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args;

    bool json_output = false;
    std::string report_str = "summary";
    std::uint64_t top_n = 0;
    std::uint64_t top_n_pid_tid = 10;
    bool no_auto_index = false;
    std::vector<std::string> group_by;
    std::vector<std::string> filter_names;
    std::vector<std::string> filter_cats;

    explicit StatsArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        schema(directory, files_args, pipeline, indexing, query_args);
    }

    bool to_config(StatsConfig& config) const {
        config.directory = directory.value;
        config.index_dir = indexing.index_dir;
        config.json_output = json_output;
        config.top_n = top_n;
        config.top_n_pid_tid = top_n_pid_tid;
        config.no_auto_index = no_auto_index;
        config.checkpoint_size = indexing.checkpoint_size;
        config.executor_threads = pipeline.executor_threads;
        config.filter_names = filter_names;
        config.filter_cats = filter_cats;
        config.group_by = group_by;

        const auto& query_str_val = query_args.query;
        if (!query_str_val.empty()) {
            auto result = Query::from_string(query_str_val);
            if (!result) {
                DFTRACER_UTILS_LOG_ERROR("Invalid --query: %s",
                                         result.error().format().c_str());
                return false;
            }
            config.query = std::move(*result);
        }

        config.report_type = parse_report_type_str(report_str);

        if (config.report_type == StatisticsQueryType::DETAILED &&
            config.group_by.empty()) {
            config.group_by.push_back("name");
        }

        return true;
    }

   protected:
    void register_args() override {
        parser().add_argument("--json").help("Output in JSON format").flag();

        parser()
            .add_argument("--report")
            .help(
                "Report type: summary, categories, names, pid_tids, "
                "time_range, "
                "duration, top-names, top-categories, detailed")
            .default_value<std::string>("summary");

        parser()
            .add_argument("--top-n")
            .help(
                "Number of results for top-N queries (0 = show all, "
                "default: 0)")
            .scan<'d', std::uint64_t>()
            .default_value(static_cast<std::uint64_t>(0));

        parser()
            .add_argument("--top-n-pid-tid")
            .help("Max PID:TID pairs to display (0 = show all, default: 10)")
            .scan<'d', std::uint64_t>()
            .default_value(static_cast<std::uint64_t>(10));

        parser()
            .add_argument("--no-auto-index")
            .help(
                "Disable automatic index building for files missing .dftindex")
            .flag();

        parser()
            .add_argument("--group-by")
            .help(
                "Group detailed statistics by dimension(s): name, cat, pid, "
                "tid, fhash, hhash, pid_tid. Multiple values create composite "
                "keys.")
            .nargs(argparse::nargs_pattern::at_least_one)
            .default_value<std::vector<std::string>>({});

        parser()
            .add_argument("--filter-names")
            .help("Filter by event names")
            .nargs(argparse::nargs_pattern::any)
            .default_value<std::vector<std::string>>({});

        parser()
            .add_argument("--filter-cats")
            .help("Filter by event categories")
            .nargs(argparse::nargs_pattern::any)
            .default_value<std::vector<std::string>>({});
    }

    void post_parse() override {
        json_output = parser().get<bool>("--json");
        report_str = parser().get<std::string>("--report");
        top_n = parser().get<std::uint64_t>("--top-n");
        top_n_pid_tid = parser().get<std::uint64_t>("--top-n-pid-tid");
        no_auto_index = parser().get<bool>("--no-auto-index");
        group_by = parser().get<std::vector<std::string>>("--group-by");
        filter_names = parser().get<std::vector<std::string>>("--filter-names");
        filter_cats = parser().get<std::vector<std::string>>("--filter-cats");
    }

    bool validate() override {
        const std::vector<std::string> valid_reports = {
            "summary",   "categories",     "names",
            "pid_tids",  "time_range",     "duration",
            "top-names", "top-categories", "detailed"};
        if (std::find(valid_reports.begin(), valid_reports.end(), report_str) ==
            valid_reports.end()) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --report value: %s",
                                     report_str.c_str());
            return false;
        }

        const std::vector<std::string> valid_dims = {
            "name", "cat", "pid", "tid", "fhash", "hhash", "pid_tid"};
        for (const auto& dim : group_by) {
            if (std::find(valid_dims.begin(), valid_dims.end(), dim) ==
                valid_dims.end()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Invalid --group-by dimension: %s. Valid: name, cat, pid, "
                    "tid, fhash, hhash, pid_tid",
                    dim.c_str());
                return false;
            }
        }

        return true;
    }

   private:
    static StatisticsQueryType parse_report_type_str(const std::string& s) {
        if (s == "summary") return StatisticsQueryType::SUMMARY;
        if (s == "categories") return StatisticsQueryType::CATEGORIES;
        if (s == "names") return StatisticsQueryType::NAMES;
        if (s == "pid_tids") return StatisticsQueryType::PID_TIDS;
        if (s == "time_range") return StatisticsQueryType::TIME_RANGE;
        if (s == "duration") return StatisticsQueryType::DURATION_STATS;
        if (s == "top-names") return StatisticsQueryType::TOP_N_NAMES;
        if (s == "top-categories") return StatisticsQueryType::TOP_N_CATEGORIES;
        if (s == "detailed") return StatisticsQueryType::DETAILED;
        return StatisticsQueryType::SUMMARY;
    }
};

using indexing::FileWorkItem;
using indexing::IndexResolverUtility;
using indexing::ResolvedFile;
using indexing::ResolverInput;
using indexing::ResolverResult;

struct IndexPartition {
    std::vector<FileWorkItem> files_needing_index;
    std::vector<ResolvedFile> indexed_entries;
    ResolverResult resolver_result;
    std::vector<std::pair<std::size_t, TraceStatistics>> precomputed_failures;
    std::vector<std::pair<std::size_t, TraceStatistics>> precomputed_successes;
};

struct AggregateStatsResult {
    std::vector<std::pair<std::size_t, TraceStatistics>> indexed_stats;
    TraceStatistics total;
    std::size_t successful_count = 0;
    std::size_t failed_count = 0;
    std::int64_t read_elapsed_ns = 0;
    std::unordered_map<std::string, std::uint64_t> read_counters;
};

struct IndexedRootSnapshot {
    std::vector<std::string> logical_files;
    IndexPartition partition;
};

static void append_failed_stats_result(
    std::vector<std::pair<std::size_t, TraceStatistics>>& results,
    std::size_t file_index, const std::string& file_path,
    const std::string& error_message) {
    TraceStatistics failed;
    failed.file_path = file_path;
    failed.success = false;
    failed.error_message = error_message;
    results.emplace_back(file_index, std::move(failed));
}

static void append_empty_indexed_stats_result(
    std::vector<std::pair<std::size_t, TraceStatistics>>& results,
    std::size_t file_index, const std::string& file_path,
    const std::string& index_path) {
    TraceStatistics stats;
    stats.file_path = file_path;
    stats.index_path = index_path;
    stats.success = true;
    stats.num_chunks = 0;
    results.emplace_back(file_index, std::move(stats));
}

static double ns_to_ms(std::uint64_t ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

static coro::CoroTask<std::optional<TraceStatistics>>
process_index_group_root_summary(std::string index_path,
                                 std::size_t expected_indexed_files,
                                 StatisticsQueryType report_type) {
    IndexDatabase idx_db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    auto scalar_stats = idx_db.query_root_scalar_stats();

    if (!scalar_stats || scalar_stats->num_files != expected_indexed_files) {
        co_return std::nullopt;
    }

    TraceStatistics result;
    result.file_path = index_path;
    result.index_path = index_path;
    result.num_chunks = scalar_stats->num_chunks;
    result.merged = scalar_stats->stats;
    result.success = true;

    const bool needs_categories =
        report_type == StatisticsQueryType::SUMMARY ||
        report_type == StatisticsQueryType::CATEGORIES ||
        report_type == StatisticsQueryType::TOP_N_CATEGORIES;
    const bool needs_names = report_type == StatisticsQueryType::NAMES ||
                             report_type == StatisticsQueryType::TOP_N_NAMES;
    const bool needs_pid_tids = report_type == StatisticsQueryType::SUMMARY ||
                                report_type == StatisticsQueryType::PID_TIDS;

    if (needs_categories) {
        idx_db.merge_root_category_counts_into(result.merged);
    }
    if (needs_names) {
        idx_db.merge_root_name_counts_into(result.merged);
    }
    if (needs_pid_tids) {
        idx_db.merge_root_pid_tid_counts_into(result.merged);
    }

    co_return result;
}

using CountPair = std::pair<std::string, std::uint64_t>;

template <typename Map>
static std::vector<CountPair> sorted_by_count_desc(const Map& counts) {
    std::vector<CountPair> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const CountPair& a, const CountPair& b) {
                  return a.second > b.second;
              });
    return sorted;
}

// Format a byte value for human-readable display
static std::string format_bytes(double bytes) {
    return cli::human_bytes(bytes);
}

// Format a bandwidth value (bytes/sec) for human-readable display
static std::string format_bandwidth(double bps) {
    return cli::human_bytes(bps, "/s");
}

// Build a DetailedStatistics from TraceStatistics (summary path).
// Per-op distributions come from ChunkStatistics in-memory sketches
// (populated during live scanning, empty when loaded from index).
static DetailedStatistics to_detailed(const TraceStatistics& stats) {
    DetailedStatistics d;
    const auto& m = stats.merged;

    // Global duration
    d.duration.sketch = m.duration_sketch;
    d.duration.histogram = m.duration_histogram;
    d.duration.sum = static_cast<double>(m.duration_sum_us);

    // Per-operation distributions
    for (const auto& [name, sketch] : m.name_duration_sketches) {
        auto& dist = d.grouped_duration[name];
        dist.sketch = sketch;
        auto hist_it = m.name_duration_histograms.find(name);
        if (hist_it != m.name_duration_histograms.end()) {
            dist.histogram = hist_it->second;
        }
        auto sum_it = m.name_duration_sums.find(name);
        if (sum_it != m.name_duration_sums.end()) {
            dist.sum = sum_it->second;
        }
        auto sq_it = m.name_duration_sum_sqs.find(name);
        if (sq_it != m.name_duration_sum_sqs.end()) {
            dist.sum_sq = sq_it->second;
        }
    }

    d.group_key_category = m.name_category;
    d.events_scanned = m.total_events;
    d.chunks_scanned = stats.num_chunks;
    d.chunks_skipped = 0;

    return d;
}

// Resolve a group key for display, looking up hash values if needed
static std::string resolve_display_key(
    const std::string& key,
    const std::unordered_map<std::string, std::string>& hash_resolutions) {
    if (!hash_resolutions.empty()) {
        auto it = hash_resolutions.find(key);
        if (it != hash_resolutions.end()) {
            return it->second;
        }
    }
    return key;
}

// Header plus the optional summary sections (time span, categories, PID:TID).
static void print_detailed_header(const std::string& file_path,
                                  const DetailedStatistics& detailed,
                                  std::uint64_t total_chunks,
                                  const TraceStatistics* summary,
                                  std::uint64_t top_n_pid_tid) {
    std::printf("========================================\n");
    std::printf("File: %s\n", file_path.c_str());
    std::printf("========================================\n");
    std::printf("  Chunks: %llu (scanned: %llu, skipped: %llu)\n",
                (unsigned long long)total_chunks,
                (unsigned long long)detailed.chunks_scanned,
                (unsigned long long)detailed.chunks_skipped);
    std::printf("  Events Scanned: %llu\n",
                (unsigned long long)detailed.events_scanned);

    // Summary sections (categories, PID:TID, time span)
    if (summary && summary->success) {
        if (summary->time_span_seconds() > 0.0) {
            std::printf("  Time Span: %.6f seconds\n",
                        summary->time_span_seconds());
        }

        // Category breakdown
        const auto& cat_counts = summary->merged.category_counts;
        auto sorted_cats_summary = sorted_by_count_desc(cat_counts);
        std::printf("\n  Categories (%zu):\n", cat_counts.size());
        for (const auto& [name, count] : sorted_cats_summary) {
            std::printf("    %-40s %llu\n", name.c_str(),
                        (unsigned long long)count);
        }

        // PID:TID breakdown
        const auto& pid_tid_counts = summary->merged.pid_tid_counts;
        auto sorted_pid_tids = sorted_by_count_desc(pid_tid_counts);
        std::size_t pid_tids_to_show =
            (top_n_pid_tid == 0)
                ? sorted_pid_tids.size()
                : std::min(static_cast<std::size_t>(top_n_pid_tid),
                           sorted_pid_tids.size());
        if (pid_tids_to_show < sorted_pid_tids.size()) {
            std::printf("\n  Process/Thread Pairs (%zu of %zu):\n",
                        pid_tids_to_show, sorted_pid_tids.size());
        } else {
            std::printf("\n  Process/Thread Pairs (%zu):\n",
                        sorted_pid_tids.size());
        }
        for (std::size_t i = 0; i < pid_tids_to_show; ++i) {
            std::printf("    %-40s %llu\n", sorted_pid_tids[i].first.c_str(),
                        (unsigned long long)sorted_pid_tids[i].second);
        }
    }
}

// Global (ungrouped) duration distribution and histogram.
static void print_detailed_global_duration(const DetailedStatistics& detailed) {
    if (detailed.duration.count() > 0) {
        const auto& d = detailed.duration;
        std::printf("\n  Duration (all events):\n");
        std::printf(
            "    Count: %llu   Sum: %.1f us   Mean: %.1f us"
            "   Stddev: %.1f us\n",
            (unsigned long long)d.count(), d.sum, d.mean(), d.stddev());

        if (!d.sketch.empty()) {
            std::printf("    Min: %.1f us   Max: %.1f us\n", d.sketch.min(),
                        d.sketch.max());
            std::printf(
                "    p10: %.1f   p25: %.1f   p50: %.1f"
                "   p75: %.1f   p90: %.1f   p95: %.1f"
                "   p99: %.1f us\n",
                d.sketch.quantile(0.1), d.sketch.quantile(0.25),
                d.sketch.quantile(0.5), d.sketch.quantile(0.75),
                d.sketch.quantile(0.9), d.sketch.quantile(0.95),
                d.sketch.quantile(0.99));
        }

        std::printf("\n  Duration Histogram:\n");
        std::printf("%s", d.histogram.render_blocks(20, "us").c_str());
    }
}

// Per-group duration table, split by category.
static void print_detailed_grouped_duration(
    const DetailedStatistics& detailed, std::uint64_t top_n,
    const std::unordered_map<std::string, std::string>& hash_resolutions) {
    if (!detailed.grouped_duration.empty()) {
        using DurPair = std::pair<std::string, const DistributionStats*>;

        // Group entries by category
        std::unordered_map<std::string, std::vector<DurPair>> by_category;
        for (const auto& [key, dist] : detailed.grouped_duration) {
            auto cat_it = detailed.group_key_category.find(key);
            std::string cat = (cat_it != detailed.group_key_category.end())
                                  ? cat_it->second
                                  : "other";
            by_category[cat].emplace_back(key, &dist);
        }

        // Sort categories by total event count descending
        using CatPair = std::pair<std::string, std::vector<DurPair>*>;
        std::vector<CatPair> sorted_cats;
        sorted_cats.reserve(by_category.size());
        for (auto& [cat, entries] : by_category) {
            // Sort entries within category by count descending
            std::sort(entries.begin(), entries.end(),
                      [](const DurPair& a, const DurPair& b) {
                          return a.second->count() > b.second->count();
                      });
            sorted_cats.emplace_back(cat, &entries);
        }
        std::sort(sorted_cats.begin(), sorted_cats.end(),
                  [](const CatPair& a, const CatPair& b) {
                      std::uint64_t sum_a = 0, sum_b = 0;
                      for (const auto& e : *a.second)
                          sum_a += e.second->count();
                      for (const auto& e : *b.second)
                          sum_b += e.second->count();
                      return sum_a > sum_b;
                  });

        for (const auto& [cat, entries_ptr] : sorted_cats) {
            const auto& entries = *entries_ptr;

            std::size_t show =
                (top_n == 0)
                    ? entries.size()
                    : std::min(static_cast<std::size_t>(top_n), entries.size());

            if (show < entries.size()) {
                std::printf("\n  Duration [%s] (top %zu of %zu):\n",
                            cat.c_str(), show, entries.size());
            } else {
                std::printf("\n  Duration [%s] (%zu):\n", cat.c_str(),
                            entries.size());
            }
            std::printf(
                "    %-30s %10s %14s %10s %10s %10s"
                " %10s %10s %10s %10s %10s %10s %10s %10s\n",
                "Name", "Count", "Sum us", "Mean us", "Stddev us", "Min us",
                "p10 us", "p25 us", "p50 us", "p75 us", "p90 us", "p95 us",
                "p99 us", "Max us");

            for (std::size_t i = 0; i < show; ++i) {
                const auto& [key, dist] = entries[i];
                std::string display_key =
                    resolve_display_key(key, hash_resolutions);
                if (display_key.size() > 30) {
                    display_key = display_key.substr(0, 27) + "...";
                }

                bool has_sketch = !dist->sketch.empty();
                double sk_min = has_sketch ? dist->sketch.min() : 0.0;
                double sk_max = has_sketch ? dist->sketch.max() : 0.0;
                double p10 = has_sketch ? dist->sketch.quantile(0.1) : 0.0;
                double p25 = has_sketch ? dist->sketch.quantile(0.25) : 0.0;
                double p50 = has_sketch ? dist->sketch.quantile(0.5) : 0.0;
                double p75 = has_sketch ? dist->sketch.quantile(0.75) : 0.0;
                double p90 = has_sketch ? dist->sketch.quantile(0.9) : 0.0;
                double p95 = has_sketch ? dist->sketch.quantile(0.95) : 0.0;
                double p99 = has_sketch ? dist->sketch.quantile(0.99) : 0.0;

                std::printf(
                    "    %-30s %10llu %14.1f %10.1f %10.1f %10.1f"
                    " %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f"
                    " %10.1f %10.1f\n",
                    display_key.c_str(), (unsigned long long)dist->count(),
                    dist->sum, dist->mean(), dist->stddev(), sk_min, p10, p25,
                    p50, p75, p90, p95, p99, sk_max);

                // Inline histogram after each operation
                if (dist->histogram.total_count() > 0) {
                    std::printf(
                        "%s", dist->histogram.render_blocks(20, "us", "      ")
                                  .c_str());
                }
            }
        }
    }
}

// Per-group I/O metrics table (or simple global I/O stats when ungrouped).
static void print_detailed_grouped_io(
    const DetailedStatistics& detailed, std::uint64_t top_n,
    const std::unordered_map<std::string, std::string>& hash_resolutions) {
    if (!detailed.grouped_io.empty()) {
        // Check if this is the global (no grouping) case
        bool is_global = (detailed.grouped_io.size() == 1 &&
                          detailed.grouped_io.count("__global__") == 1);

        using IOPair = std::pair<std::string, const IOEventMetrics*>;
        std::vector<IOPair> sorted_io;
        sorted_io.reserve(detailed.grouped_io.size());
        for (const auto& [key, io] : detailed.grouped_io) {
            sorted_io.emplace_back(key, &io);
        }
        std::sort(sorted_io.begin(), sorted_io.end(),
                  [](const IOPair& a, const IOPair& b) {
                      return a.second->size.count() > b.second->size.count();
                  });

        std::size_t show =
            (top_n == 0)
                ? sorted_io.size()
                : std::min(static_cast<std::size_t>(top_n), sorted_io.size());

        if (is_global) {
            std::printf("\n  I/O Statistics:\n");
        } else if (show < sorted_io.size()) {
            std::printf("\n  I/O Events by group (top %zu of %zu):\n", show,
                        sorted_io.size());
        } else {
            std::printf("\n  I/O Events by group (%zu):\n", sorted_io.size());
        }

        if (is_global) {
            // Simple global I/O stats
            const auto& io = *sorted_io[0].second;
            std::printf("    Count: %llu   Size Mean: %s",
                        (unsigned long long)io.size.count(),
                        format_bytes(io.size.mean()).c_str());
            if (!io.size.sketch.empty()) {
                std::printf("   Size p50: %s",
                            format_bytes(io.size.sketch.quantile(0.5)).c_str());
            }
            if (io.bandwidth.count() > 0 && !io.bandwidth.sketch.empty()) {
                std::printf("   BW p50: %s",
                            format_bandwidth(io.bandwidth.sketch.quantile(0.5))
                                .c_str());
            }
            std::printf("\n");

            // Size histogram
            if (io.size.count() > 0) {
                std::printf("\n  I/O Size Distribution:\n");
                std::printf("%s",
                            io.size.histogram.render_ascii(40, "B").c_str());
                if (!io.size.sketch.empty()) {
                    std::printf("  I/O Size Percentiles:\n");
                    std::printf(
                        "    p50: %s  p90: %s  p99: %s\n",
                        format_bytes(io.size.sketch.quantile(0.5)).c_str(),
                        format_bytes(io.size.sketch.quantile(0.9)).c_str(),
                        format_bytes(io.size.sketch.quantile(0.99)).c_str());
                }
            }
        } else {
            // Per-group I/O table
            std::printf("    %-30s %12s %12s %12s %12s %12s\n", "Key", "Count",
                        "Size Mean", "Size p50", "BW p50", "Offset p50");

            for (std::size_t i = 0; i < show; ++i) {
                const auto& [key, io] = sorted_io[i];
                std::string display_key =
                    resolve_display_key(key, hash_resolutions);
                if (display_key.size() > 30) {
                    display_key = display_key.substr(0, 27) + "...";
                }

                std::string size_mean = format_bytes(io->size.mean());
                std::string size_p50 =
                    io->size.sketch.empty()
                        ? "-"
                        : format_bytes(io->size.sketch.quantile(0.5));
                std::string bw_p50 =
                    (io->bandwidth.count() == 0 || io->bandwidth.sketch.empty())
                        ? "-"
                        : format_bandwidth(io->bandwidth.sketch.quantile(0.5));
                std::string offset_p50 =
                    (io->offset.count() == 0 || io->offset.sketch.empty())
                        ? "-"
                        : format_bytes(io->offset.sketch.quantile(0.5));

                std::printf("    %-30s %12llu %12s %12s %12s %12s\n",
                            display_key.c_str(),
                            (unsigned long long)io->size.count(),
                            size_mean.c_str(), size_p50.c_str(), bw_p50.c_str(),
                            offset_p50.c_str());
            }

            // Print I/O size histogram for top event
            if (!sorted_io.empty() && sorted_io[0].second->size.count() > 0) {
                const auto& [top_key, top_io] = sorted_io[0];
                std::string display_key =
                    resolve_display_key(top_key, hash_resolutions);
                std::printf("\n  I/O Size Histogram (top: %s):\n",
                            display_key.c_str());
                std::printf(
                    "%s", top_io->size.histogram.render_ascii(40, "B").c_str());
            }
        }
    }
}

static void print_text_detailed(
    const std::string& file_path, const DetailedStatistics& detailed,
    std::uint64_t total_chunks, std::uint64_t top_n,
    const std::unordered_map<std::string, std::string>& hash_resolutions,
    const TraceStatistics* summary = nullptr,
    std::uint64_t top_n_pid_tid = 10) {
    print_detailed_header(file_path, detailed, total_chunks, summary,
                          top_n_pid_tid);
    print_detailed_global_duration(detailed);
    print_detailed_grouped_duration(detailed, top_n, hash_resolutions);
    print_detailed_grouped_io(detailed, top_n, hash_resolutions);
    std::printf("\n");
}

// Per-chunk scanning coroutine for parallel detailed stats.
// Scans a single chunk and merges results into shared file_detailed.
static coro::CoroTask<std::optional<DetailedStatistics>> scan_chunk_detailed(
    std::string file_path, std::string index_path, std::size_t checkpoint_size,
    std::size_t file_size, std::size_t num_ckpts, std::uint64_t ckpt_idx,
    const std::vector<std::string>* filter_names_ptr,
    const std::vector<std::string>* filter_cats_ptr,
    const std::vector<std::string>* group_by_ptr) {
    std::size_t start_byte = 0;
    std::size_t end_byte = file_size;

    if (num_ckpts > 0) {
        std::size_t bytes_per = file_size / num_ckpts;
        start_byte = ckpt_idx * bytes_per;
        end_byte = (ckpt_idx + 1 == num_ckpts) ? file_size
                                               : (ckpt_idx + 1) * bytes_per;
    }

    ChunkDetailScanInput scan_input;
    scan_input.file_path = file_path;
    scan_input.index_path = index_path;
    scan_input.checkpoint_size = checkpoint_size;
    scan_input.start_byte = start_byte;
    scan_input.end_byte = end_byte;
    scan_input.checkpoint_idx = ckpt_idx;
    scan_input.filter_names = filter_names_ptr;
    scan_input.filter_categories = filter_cats_ptr;
    scan_input.group_by = group_by_ptr;

    ChunkDetailScannerUtility scanner;
    auto scan_output = co_await scanner.process(scan_input);

    if (scan_output) {
        co_return scan_output->stats;
    }

    co_return std::nullopt;
}

// Per-file detailed stats coroutine. Spawns parallel chunk scans,
// then resolves hashes and produces output.
static coro::CoroTask<void> process_file_detailed(
    CoroScope& fctx, std::string file_path, std::size_t fi,
    std::string index_dir, std::size_t checkpoint_size,
    bool needs_hash_resolution, bool json_output, std::uint64_t top_n,
    const common::query::Query* query_ptr,
    const std::vector<std::string>* filter_names_ptr,
    const std::vector<std::string>* filter_cats_ptr,
    const std::vector<std::string>* group_by_ptr,
    DetailedStatistics* aggregate_detailed_ptr, std::mutex* aggregate_mutex_ptr,
    std::mutex* output_mutex_ptr,
    std::vector<std::pair<std::size_t, std::string>>* json_results_ptr) {
    std::string index_path =
        internal::determine_index_path(file_path, index_dir);

    auto meta_input = MetadataCollectorUtilityInput::from_file(file_path)
                          .with_checkpoint_size(checkpoint_size)
                          .with_force_rebuild(false)
                          .with_index(index_path);
    auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);

    if (!metadata.success) {
        DFTRACER_UTILS_LOG_ERROR("Failed to collect metadata for %s: %s",
                                 file_path.c_str(),
                                 metadata.error_message.c_str());
        co_return;
    }

    std::size_t file_size = metadata.uncompressed_size;
    std::size_t num_ckpts = metadata.num_checkpoints;

    // Determine candidate checkpoints via bloom pre-filtering
    std::vector<std::uint64_t> candidate_checkpoints;
    std::uint64_t total_checkpoints = (num_ckpts == 0) ? 1 : num_ckpts;

    if (query_ptr && fs::exists(index_path)) {
        try {
            ChunkPrunerInput pruner_input{index_path, file_path, *query_ptr,
                                          nullptr};
            ChunkPrunerUtility pruner;
            auto pruner_output = co_await pruner.process(pruner_input);

            if (pruner_output.success) {
                candidate_checkpoints = pruner_output.candidate_checkpoints;
                total_checkpoints = pruner_output.total_checkpoints;
            } else {
                for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                    candidate_checkpoints.push_back(i);
                }
            }
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN(
                "Chunk pruner failed for %s: %s, scanning all chunks",
                file_path.c_str(), e.what());
            for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                candidate_checkpoints.push_back(i);
            }
        }
    } else {
        for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
            candidate_checkpoints.push_back(i);
        }
    }

    // Scan candidate chunks in parallel, then merge sequentially per file.
    DetailedStatistics file_detailed;
    file_detailed.chunks_skipped =
        total_checkpoints - candidate_checkpoints.size();
    std::vector<std::uint64_t> candidates = std::move(candidate_checkpoints);
    std::vector<std::optional<DetailedStatistics>> chunk_results(
        candidates.size());

    const auto* file_path_ptr = &file_path;
    const auto* index_path_ptr = &index_path;
    auto* candidates_ptr = &candidates;
    auto* chunk_results_ptr = &chunk_results;
    co_await fctx.scope(
        [file_path_ptr, index_path_ptr, checkpoint_size, file_size, num_ckpts,
         filter_names_ptr, filter_cats_ptr, group_by_ptr, candidates_ptr,
         chunk_results_ptr](CoroScope& chunk_scope) -> coro::CoroTask<void> {
            for (std::size_t result_idx = 0;
                 result_idx < candidates_ptr->size(); ++result_idx) {
                std::uint64_t ckpt_idx = (*candidates_ptr)[result_idx];
                chunk_scope.spawn(
                    [file_path_ptr, index_path_ptr, checkpoint_size, file_size,
                     num_ckpts, ckpt_idx, filter_names_ptr, filter_cats_ptr,
                     group_by_ptr, chunk_results_ptr,
                     result_idx](CoroScope& /*cctx*/) -> coro::CoroTask<void> {
                        (*chunk_results_ptr)[result_idx] =
                            co_await scan_chunk_detailed(
                                *file_path_ptr, *index_path_ptr,
                                checkpoint_size, file_size, num_ckpts, ckpt_idx,
                                filter_names_ptr, filter_cats_ptr,
                                group_by_ptr);
                        co_return;
                    });
            }
            co_return;
        });

    for (const auto& chunk_result : chunk_results) {
        if (chunk_result.has_value()) {
            file_detailed.merge(*chunk_result);
        }
    }

    // Hash resolution (sequential, all chunks done)
    std::unordered_map<std::string, std::string> hash_resolutions;
    if (needs_hash_resolution && fs::exists(index_path)) {
        try {
            IndexDatabase idx_db(index_path);
            auto resolve_hashes = [&](IndexDatabase::HashType hash_type) {
                for (const auto& [key, _] : file_detailed.grouped_duration) {
                    if (hash_resolutions.count(key) == 0) {
                        auto resolved = idx_db.resolve_hash(hash_type, key);
                        if (resolved.has_value()) {
                            hash_resolutions[key] = resolved.value();
                        }
                    }
                }
                for (const auto& [key, _] : file_detailed.grouped_io) {
                    if (hash_resolutions.count(key) == 0) {
                        auto resolved = idx_db.resolve_hash(hash_type, key);
                        if (resolved.has_value()) {
                            hash_resolutions[key] = resolved.value();
                        }
                    }
                }
            };

            for (const auto& dim : *group_by_ptr) {
                if (dim == "fhash") {
                    resolve_hashes(IndexDatabase::HashType::FILE);
                } else if (dim == "hhash") {
                    resolve_hashes(IndexDatabase::HashType::HOST);
                }
            }
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("Hash resolution failed for %s: %s",
                                    file_path.c_str(), e.what());
        }
    }

    // Output per-file results
    if (json_output) {
        std::string detail_json = file_detailed.to_json();
        std::string json_obj = std::string("{\"file_path\": \"") + file_path +
                               "\", \"detailed\": " + detail_json + "}";
        std::lock_guard<std::mutex> lock(*output_mutex_ptr);
        json_results_ptr->emplace_back(fi, std::move(json_obj));
    } else {
        std::lock_guard<std::mutex> lock(*output_mutex_ptr);
        print_text_detailed(
            file_path, file_detailed,
            file_detailed.chunks_scanned + file_detailed.chunks_skipped, top_n,
            hash_resolutions);
    }

    {
        std::lock_guard<std::mutex> lock(*aggregate_mutex_ptr);
        aggregate_detailed_ptr->merge(file_detailed);
    }

    co_return;
}

static void run_detailed_query_workers(
    CoroScope& scope, const std::vector<std::string>* files_ptr,
    std::size_t executor_threads, const std::string* index_dir_ptr,
    std::size_t checkpoint_size, bool needs_hash_resolution, bool json_output,
    std::size_t top_n, const common::query::Query* qp,
    const std::vector<std::string>* fn, const std::vector<std::string>* fc,
    const std::vector<std::string>* gb, DetailedStatistics* ad, std::mutex* am,
    std::mutex* om, std::vector<std::pair<std::size_t, std::string>>* jr) {
    auto file_chan = coro::make_channel<std::size_t>(executor_threads * 2);

    scope.spawn([ch = file_chan->producer(),
                 files_ptr](CoroScope&) mutable -> coro::CoroTask<void> {
        auto guard = ch.guard();
        for (std::size_t fi = 0; fi < files_ptr->size(); ++fi) {
            if (!co_await ch.send(fi)) {
                co_return;
            }
        }
        co_return;
    });

    for (std::size_t w = 0; w < executor_threads; ++w) {
        scope.spawn([ch = file_chan->consumer(), files_ptr, index_dir_ptr,
                     checkpoint_size, needs_hash_resolution, json_output, top_n,
                     qp, fn, fc, gb, ad, am, om,
                     jr](CoroScope& fctx) -> coro::CoroTask<void> {
            while (auto fi_opt = co_await ch.receive()) {
                std::size_t fi = *fi_opt;
                std::string file_path = (*files_ptr)[fi];
                co_await process_file_detailed(
                    fctx, std::move(file_path), fi, *index_dir_ptr,
                    checkpoint_size, needs_hash_resolution, json_output, top_n,
                    qp, fn, fc, gb, ad, am, om, jr);
            }
            co_return;
        });
    }
}

static void print_text_query_output(const TraceStatistics& stats,
                                    const StatisticsQueryOutput& output) {
    std::printf("========================================\n");
    std::printf("File: %s\n", stats.file_path.c_str());
    std::printf("========================================\n");
    std::printf("  Chunks: %llu\n", (unsigned long long)stats.num_chunks);
    std::printf("  Events Scanned: %llu\n",
                (unsigned long long)output.total_events);

    switch (
        output.query_type_name == "categories" ? StatisticsQueryType::CATEGORIES
        : output.query_type_name == "names"    ? StatisticsQueryType::NAMES
        : output.query_type_name == "pid_tids" ? StatisticsQueryType::PID_TIDS
        : output.query_type_name == "time_range"
            ? StatisticsQueryType::TIME_RANGE
        : output.query_type_name == "duration_stats"
            ? StatisticsQueryType::DURATION_STATS
        : output.query_type_name == "top_n_names"
            ? StatisticsQueryType::TOP_N_NAMES
        : output.query_type_name == "top_n_categories"
            ? StatisticsQueryType::TOP_N_CATEGORIES
            : StatisticsQueryType::SUMMARY) {
        case StatisticsQueryType::CATEGORIES:
        case StatisticsQueryType::TOP_N_CATEGORIES:
            std::printf("\n  Categories (%zu):\n", output.results.size());
            for (const auto& [name, count] : output.results) {
                std::printf("    %-40s %llu\n", name.c_str(),
                            (unsigned long long)count);
            }
            break;

        case StatisticsQueryType::NAMES:
        case StatisticsQueryType::TOP_N_NAMES:
            std::printf("\n  Names (%zu):\n", output.results.size());
            for (const auto& [name, count] : output.results) {
                std::printf("    %-40s %llu\n", name.c_str(),
                            (unsigned long long)count);
            }
            break;

        case StatisticsQueryType::PID_TIDS:
            std::printf("\n  Process/Thread Pairs (%zu):\n",
                        output.results.size());
            for (const auto& [name, count] : output.results) {
                std::printf("    %-40s %llu\n", name.c_str(),
                            (unsigned long long)count);
            }
            break;

        case StatisticsQueryType::TIME_RANGE:
            std::printf("\n  Time Span: %.6f seconds\n",
                        output.time_span_seconds);
            std::printf("  Min Timestamp: %llu us\n",
                        (unsigned long long)output.min_timestamp_us);
            std::printf("  Max Timestamp: %llu us\n",
                        (unsigned long long)output.max_timestamp_us);
            break;

        case StatisticsQueryType::DURATION_STATS:
            std::printf("\n  Duration (all events):\n");
            std::printf("    Count: %llu   Mean: %.1f us   Stddev: %.1f us\n",
                        (unsigned long long)output.duration_count,
                        output.duration_mean_us, output.duration_stddev_us);
            std::printf("    Min: %llu us   Max: %llu us\n",
                        (unsigned long long)output.duration_min_us,
                        (unsigned long long)output.duration_max_us);
            break;

        case StatisticsQueryType::SUMMARY:
        case StatisticsQueryType::DETAILED:
            break;
    }
}

static coro::CoroTask<std::vector<std::string>> collect_files(
    CoroScope& ctx, const std::vector<std::string>& cli_files,
    std::string directory) {
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            co_return std::vector<std::string>{};
        }

        auto files = co_await cli::scan_directory_trace_files(
            ctx, directory, /*recursive=*/false);

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                     directory.c_str());
        }
        co_return files;
    }

    if (cli_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No files or directory specified. Use --help for usage.");
    }
    co_return cli_files;
}

static std::unique_ptr<IndexedRootSnapshot> load_index_root_snapshot_impl(
    const std::string& index_path) {
    auto snapshot = std::make_unique<IndexedRootSnapshot>();
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

    auto registry = db.query_all_file_registry();

    snapshot->logical_files.reserve(registry.size());
    snapshot->partition.indexed_entries.reserve(registry.size());

    std::size_t file_index = 0;
    for (auto& [logical_path, reg] : registry) {
        snapshot->logical_files.push_back(logical_path);
        const bool has_summary = has_capability(
            reg.capabilities, IndexFileEntryCapability::FILE_SUMMARY);
        if (!has_summary) {
            append_failed_stats_result(
                snapshot->partition.precomputed_failures, file_index,
                logical_path,
                "File registry entry exists but no file summary data was "
                "found in the shared index");
        } else {
            snapshot->partition.indexed_entries.push_back(ResolvedFile{
                file_index, logical_path, reg.file_id, reg.capabilities});
        }
        ++file_index;
    }
    snapshot->partition.resolver_result.index_path = index_path;
    return snapshot;
}

static coro::CoroTask<std::unique_ptr<IndexedRootSnapshot>>
load_index_root_snapshot(std::string index_path) {
    co_return load_index_root_snapshot_impl(index_path);
}

static std::unique_ptr<AggregateStatsResult> load_root_aggregate_impl(
    const std::string& index_path, StatisticsQueryType report_type) {
    IndexDatabase idx_db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    auto scalar_stats = idx_db.query_root_scalar_stats();
    if (!scalar_stats) {
        return nullptr;
    }

    auto agg = std::make_unique<AggregateStatsResult>();
    agg->total.success = true;
    agg->total.file_path = index_path;
    agg->total.index_path = index_path;
    agg->total.num_chunks = scalar_stats->num_chunks;
    agg->total.merged = scalar_stats->stats;

    const bool needs_categories =
        report_type == StatisticsQueryType::SUMMARY ||
        report_type == StatisticsQueryType::CATEGORIES ||
        report_type == StatisticsQueryType::TOP_N_CATEGORIES;
    const bool needs_names = report_type == StatisticsQueryType::NAMES ||
                             report_type == StatisticsQueryType::TOP_N_NAMES;
    const bool needs_pid_tids = report_type == StatisticsQueryType::SUMMARY ||
                                report_type == StatisticsQueryType::PID_TIDS;

    if (needs_categories) {
        idx_db.merge_root_category_counts_into(agg->total.merged);
    }
    if (needs_names) {
        idx_db.merge_root_name_counts_into(agg->total.merged);
    }
    if (needs_pid_tids) {
        idx_db.merge_root_pid_tid_counts_into(agg->total.merged);
    }

    agg->successful_count = static_cast<std::size_t>(scalar_stats->num_files);
    return agg;
}

static coro::CoroTask<std::unique_ptr<AggregateStatsResult>>
load_root_aggregate_result(std::string index_path,
                           StatisticsQueryType report_type) {
    co_return load_root_aggregate_impl(index_path, report_type);
}

static IndexPartition build_partition(ResolverResult result) {
    IndexPartition partition;
    partition.files_needing_index = std::move(result.needs_checkpoint);
    partition.indexed_entries = std::move(result.cached);
    partition.resolver_result = std::move(result);

    // Files that have checkpoints but no bloom get empty stats
    for (const auto& entry : partition.resolver_result.needs_bloom) {
        append_empty_indexed_stats_result(partition.precomputed_successes,
                                          entry.file_index, entry.file_path,
                                          partition.resolver_result.index_path);
    }

    return partition;
}

static coro::CoroTask<IndexPartition> resolve_index_state(
    std::vector<std::string> files, std::string index_dir,
    StatisticsQueryType report_type) {
    IndexResolverUtility resolver;
    ResolverInput input;
    input.files = std::move(files);
    input.index_dir = std::move(index_dir);
    input.require_bloom = report_type == StatisticsQueryType::DETAILED;
    auto result = co_await resolver.process(input);
    co_return build_partition(std::move(result));
}

static coro::CoroTask<indexer::IndexBuildBatchResult> run_batch_build(
    CoroScope* ctx, std::shared_ptr<indexer::IndexBuildBatchConfig> config) {
    co_return co_await indexer::IndexBatchBuilderUtility::process(
        ctx, std::move(config));
}

static coro::CoroTask<void> auto_index_files(CoroScope& ctx,
                                             IndexPartition& partition,
                                             const std::string& index_dir,
                                             std::size_t checkpoint_size,
                                             std::size_t executor_threads) {
    auto index_path = internal::determine_index_path(
        partition.files_needing_index.front().file_path, index_dir);
    dftracer::utils::rocksdb::RocksDBManager::instance().reset(index_path);

    std::printf("Auto-building index for %zu file(s)...\n",
                partition.files_needing_index.size());

    const bool all_gzip = std::all_of(
        partition.files_needing_index.begin(),
        partition.files_needing_index.end(), [](const FileWorkItem& item) {
            return item.file_path.ends_with(".gz");
        });

    {
        auto batch_config = std::make_shared<indexer::IndexBuildBatchConfig>();
        batch_config->file_paths.reserve(partition.files_needing_index.size());
        for (const auto& item : partition.files_needing_index) {
            batch_config->file_paths.push_back(item.file_path);
        }
        batch_config->index_dir = index_dir;
        batch_config->checkpoint_size = checkpoint_size;
        batch_config->parallelism = executor_threads;

        batch_config->use_batch_write = all_gzip;
        batch_config->rebuild_root_summaries = all_gzip;

        auto batch_result =
            co_await run_batch_build(&ctx, std::move(batch_config));

        DFTRACER_UTILS_LOG_INFO(
            "Shared root auto-index metrics: root=%s files=%zu "
            "enqueued=%zu parsed=%zu written=%zu "
            "parse=%.2fms writer_db=%.2fms",
            index_path.c_str(), partition.files_needing_index.size(),
            batch_result.metrics.files_enqueued,
            batch_result.metrics.files_parsed,
            batch_result.metrics.files_written,
            ns_to_ms(batch_result.metrics.parse_ns),
            ns_to_ms(batch_result.metrics.write_ns));

        for (const auto& result : batch_result.results) {
            if (!result.success && !result.error_message.empty()) {
                DFTRACER_UTILS_LOG_ERROR("Auto-indexing failed for %s: %s",
                                         result.file_path.c_str(),
                                         result.error_message.c_str());
            }
        }

        std::printf("Auto-indexing complete: %zu indexed, %zu failed\n",
                    batch_result.indexed, batch_result.failed);
    }

    // Re-resolve newly indexed files
    std::vector<std::string> newly_indexed;
    newly_indexed.reserve(partition.files_needing_index.size());
    for (const auto& item : partition.files_needing_index) {
        newly_indexed.push_back(item.file_path);
    }

    IndexResolverUtility resolver;
    ResolverInput refresh_input;
    refresh_input.files = std::move(newly_indexed);
    refresh_input.index_dir = index_dir;
    refresh_input.require_checkpoints = true;

    auto refresh_result = co_await resolver.process(refresh_input);

    // Add successfully indexed files
    for (auto& entry : refresh_result.cached) {
        bool has_bloom = indexer::has_capability(
            entry.capabilities, indexer::IndexFileEntryCapability::BLOOM);
        if (has_bloom) {
            partition.indexed_entries.push_back(std::move(entry));
        } else {
            append_empty_indexed_stats_result(partition.precomputed_successes,
                                              entry.file_index, entry.file_path,
                                              refresh_result.index_path);
        }
    }

    // Handle files that still need checkpoints (failed to index)
    for (const auto& item : refresh_result.needs_checkpoint) {
        append_failed_stats_result(
            partition.precomputed_failures, item.file_index, item.file_path,
            "Auto-index completed but no readable file summary "
            "data was found in the shared index");
    }

    // Update resolver result
    partition.resolver_result.index_path = refresh_result.index_path;

    co_return;
}

static coro::CoroTask<int> run_detailed_stats(
    CoroScope& ctx, const StatsConfig* config_ptr,
    const std::vector<std::string>* files_ptr) {
    auto start_time = std::chrono::high_resolution_clock::now();

    bool needs_hash_resolution = false;
    for (const auto& dim : config_ptr->group_by) {
        if (dim == "fhash" || dim == "hhash") {
            needs_hash_resolution = true;
            break;
        }
    }

    auto aggregate_detailed = std::make_unique<DetailedStatistics>();
    std::mutex aggregate_mutex;
    std::mutex output_mutex;
    auto json_results =
        std::make_unique<std::vector<std::pair<std::size_t, std::string>>>();

    auto* filter_names_ptr = &config_ptr->filter_names;
    auto* filter_cats_ptr = &config_ptr->filter_cats;
    auto* group_by_ptr = &config_ptr->group_by;
    auto* aggregate_detailed_ptr = aggregate_detailed.get();
    auto* aggregate_mutex_ptr = &aggregate_mutex;
    auto* output_mutex_ptr = &output_mutex;
    auto* json_results_ptr = json_results.get();
    auto* query_ptr = config_ptr->query ? &*config_ptr->query : nullptr;
    const auto* index_dir_for_detailed_ptr = &config_ptr->index_dir;
    std::size_t checkpoint_size_for_detailed = config_ptr->checkpoint_size;
    std::size_t executor_threads_for_detailed = config_ptr->executor_threads;
    bool needs_hash_resolution_for_detailed = needs_hash_resolution;
    bool json_output_for_detailed = config_ptr->json_output;
    std::size_t top_n_for_detailed = config_ptr->top_n;

    co_await ctx.scope(
        [files_ptr, executor_threads_for_detailed, index_dir_for_detailed_ptr,
         checkpoint_size_for_detailed, needs_hash_resolution_for_detailed,
         json_output_for_detailed, top_n_for_detailed, query_ptr,
         filter_names_ptr, filter_cats_ptr, group_by_ptr,
         aggregate_detailed_ptr, aggregate_mutex_ptr, output_mutex_ptr,
         json_results_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            run_detailed_query_workers(
                scope, files_ptr, executor_threads_for_detailed,
                index_dir_for_detailed_ptr, checkpoint_size_for_detailed,
                needs_hash_resolution_for_detailed, json_output_for_detailed,
                top_n_for_detailed, query_ptr, filter_names_ptr,
                filter_cats_ptr, group_by_ptr, aggregate_detailed_ptr,
                aggregate_mutex_ptr, output_mutex_ptr, json_results_ptr);
            co_return;
        });

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (config_ptr->json_output) {
        std::printf("[\n");
        std::sort(
            json_results->begin(), json_results->end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < json_results->size(); ++i) {
            std::printf("%s%s", (*json_results)[i].second.c_str(),
                        i + 1 < json_results->size() ? ",\n" : "\n");
        }
        std::printf("]\n");
    } else {
        std::printf("==========================================\n");
        std::printf("Consolidated Detailed (%zu files)\n", files_ptr->size());
        std::printf("==========================================\n");
        std::unordered_map<std::string, std::string> no_resolutions;
        print_text_detailed(config_ptr->directory, *aggregate_detailed,
                            aggregate_detailed->chunks_scanned +
                                aggregate_detailed->chunks_skipped,
                            config_ptr->top_n, no_resolutions);
        std::printf("  Processing Time: %.2f ms\n", duration.count());
        std::printf("==========================================\n");
    }

    co_return 0;
}

static coro::CoroTask<void> process_index_group(
    const std::string* index_path_ptr,
    const std::vector<ResolvedFile>* group_ptr,
    std::vector<std::pair<std::size_t, TraceStatistics>>* indexed_stats_ptr,
    std::mutex* stats_mutex_ptr, std::size_t expected_indexed_files,
    bool needs_per_file_results, TraceStatistics* total_ptr,
    std::mutex* total_mutex_ptr, std::atomic<std::size_t>* successful_ptr,
    std::atomic<std::size_t>* failed_ptr,
    StatisticsQueryType report_type_for_reader, Timer* metrics_timer_ptr) {
    try {
        metrics_timer_ptr->increment("root_summary_attempts");
        if (!needs_per_file_results) {
            auto root_summary = co_await process_index_group_root_summary(
                *index_path_ptr, expected_indexed_files,
                report_type_for_reader);
            if (root_summary && root_summary->success) {
                metrics_timer_ptr->increment("root_summary_hits");
                std::lock_guard<std::mutex> lock(*total_mutex_ptr);
                total_ptr->merged.merge_from(root_summary->merged);
                total_ptr->num_chunks += root_summary->num_chunks;
                successful_ptr->fetch_add(group_ptr->size(),
                                          std::memory_order_relaxed);
                co_return;
            }
        }
        metrics_timer_ptr->increment("root_summary_misses");
        metrics_timer_ptr->increment("fallback_groups");
        metrics_timer_ptr->increment("fallback_files", group_ptr->size());

        SharedIndexStatisticsReader reader;
        auto batch_rows = co_await reader.query(*index_path_ptr, *group_ptr,
                                                report_type_for_reader);
        auto callback = [indexed_stats_ptr, stats_mutex_ptr,
                         needs_per_file_results, total_ptr, total_mutex_ptr,
                         successful_ptr, failed_ptr](std::size_t file_index,
                                                     TraceStatistics&& stats) {
            if (needs_per_file_results) {
                std::lock_guard<std::mutex> lock(*stats_mutex_ptr);
                indexed_stats_ptr->emplace_back(file_index, std::move(stats));
                return;
            }

            if (stats.success) {
                std::lock_guard<std::mutex> lock(*total_mutex_ptr);
                total_ptr->merged.merge_from(stats.merged);
                total_ptr->num_chunks += stats.num_chunks;
                successful_ptr->fetch_add(1, std::memory_order_relaxed);
            } else {
                failed_ptr->fetch_add(1, std::memory_order_relaxed);
            }
        };
        SharedIndexStatisticsReader::process_batch_results(batch_rows,
                                                           callback);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Indexed stats batch failed for %s: %s",
                                 index_path_ptr->c_str(), e.what());
        if (needs_per_file_results) {
            std::lock_guard<std::mutex> lock(*stats_mutex_ptr);
            for (const auto& entry : *group_ptr) {
                TraceStatistics failed_result;
                failed_result.file_path = entry.file_path;
                failed_result.success = false;
                failed_result.error_message = e.what();
                indexed_stats_ptr->emplace_back(entry.file_index,
                                                std::move(failed_result));
            }
        }
        failed_ptr->fetch_add(group_ptr->size(), std::memory_order_relaxed);
    }
    co_return;
}

static coro::CoroTask<AggregateStatsResult> run_aggregate_stats(
    CoroScope& /*ctx*/, const StatsConfig* config_ptr,
    std::unique_ptr<IndexPartition> partition_ptr) {
    auto& partition = *partition_ptr;
    auto agg = std::make_unique<AggregateStatsResult>();
    agg->total.success = true;
    agg->total.file_path = config_ptr->directory;

    const bool needs_per_file_results = config_ptr->json_output;
    std::atomic<std::size_t> successful{0};
    std::atomic<std::size_t> failed{0};

    if (!partition.precomputed_failures.empty()) {
        if (needs_per_file_results) {
            agg->indexed_stats.insert(
                agg->indexed_stats.end(),
                std::make_move_iterator(partition.precomputed_failures.begin()),
                std::make_move_iterator(partition.precomputed_failures.end()));
        }
        failed.fetch_add(partition.precomputed_failures.size(),
                         std::memory_order_relaxed);
    }
    if (!partition.precomputed_successes.empty()) {
        if (needs_per_file_results) {
            agg->indexed_stats.insert(
                agg->indexed_stats.end(),
                std::make_move_iterator(
                    partition.precomputed_successes.begin()),
                std::make_move_iterator(partition.precomputed_successes.end()));
        }
        successful.fetch_add(partition.precomputed_successes.size(),
                             std::memory_order_relaxed);
    }

    std::mutex stats_mutex;
    std::mutex total_mutex;
    Timer read_timer("stats_read_path", true, false);

    auto* indexed_stats_ptr = &agg->indexed_stats;
    auto* stats_mutex_ptr = &stats_mutex;
    auto* indexed_entries_ptr = &partition.indexed_entries;
    const auto* index_path_ptr = &partition.resolver_result.index_path;
    auto* total_ptr = &agg->total;
    auto* total_mutex_ptr = &total_mutex;
    auto* successful_ptr = &successful;
    auto* failed_ptr = &failed;
    auto* read_timer_ptr = &read_timer;
    StatisticsQueryType report_type_for_reader = config_ptr->report_type;

    if (!indexed_entries_ptr->empty()) {
        const auto expected_indexed_files = indexed_entries_ptr->size();
        co_await process_index_group(
            index_path_ptr, indexed_entries_ptr, indexed_stats_ptr,
            stats_mutex_ptr, expected_indexed_files, needs_per_file_results,
            total_ptr, total_mutex_ptr, successful_ptr, failed_ptr,
            report_type_for_reader, read_timer_ptr);
    }
    read_timer.stop();

    agg->successful_count = successful.load(std::memory_order_relaxed);
    agg->failed_count = failed.load(std::memory_order_relaxed);
    agg->read_elapsed_ns = read_timer.elapsed();
    agg->read_counters = read_timer.counters();
    co_return std::move(*agg);
}

static coro::CoroTask<int> output_aggregate_stats(
    const StatsConfig* config_ptr, const std::vector<std::string>* files_ptr,
    std::unique_ptr<AggregateStatsResult> agg, Timer overall) {
    std::vector<TraceStatistics> all_stats;
    if (config_ptr->json_output) {
        std::sort(
            agg->indexed_stats.begin(), agg->indexed_stats.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        all_stats.reserve(agg->indexed_stats.size());
        for (auto& [_, stats] : agg->indexed_stats) {
            all_stats.push_back(std::move(stats));
        }
    }

    double duration_ms = static_cast<double>(overall.elapsed()) / 1e6;

    if (config_ptr->json_output) {
        StatisticsQueryUtility query_util;
        std::printf("[\n");
        for (std::size_t i = 0; i < all_stats.size(); ++i) {
            const auto& stats = all_stats[i];
            if (!stats.success) {
                std::printf("%s%s", stats.to_json().c_str(),
                            i + 1 < all_stats.size() ? ",\n" : "\n");
                continue;
            }
            StatisticsQueryInput qi;
            qi.stats = stats;
            qi.query_type = config_ptr->report_type;
            qi.top_n = config_ptr->top_n;
            auto output = co_await query_util.process(qi);
            std::printf("%s%s", output.to_json().c_str(),
                        i + 1 < all_stats.size() ? ",\n" : "\n");
        }
        std::printf("]\n");
    } else {
        const auto displayed_files =
            files_ptr->empty() ? agg->successful_count + agg->failed_count
                               : files_ptr->size();
        std::printf("==========================================\n");
        std::printf("Consolidated (%zu files, %zu successful, %zu failed)\n",
                    displayed_files, agg->successful_count, agg->failed_count);
        std::printf("==========================================\n");
        if (config_ptr->report_type == StatisticsQueryType::SUMMARY) {
            auto detailed = to_detailed(agg->total);
            std::unordered_map<std::string, std::string> no_resolutions;
            print_text_detailed(agg->total.file_path, detailed,
                                agg->total.num_chunks, config_ptr->top_n,
                                no_resolutions, &agg->total,
                                config_ptr->top_n_pid_tid);
        } else {
            StatisticsQueryUtility query_util;
            StatisticsQueryInput qi;
            qi.stats = agg->total;
            qi.query_type = config_ptr->report_type;
            qi.top_n = config_ptr->top_n;
            auto output = co_await query_util.process(qi);
            print_text_query_output(agg->total, output);
        }
        auto counter = [&agg](const char* key) -> std::uint64_t {
            auto it = agg->read_counters.find(key);
            return it == agg->read_counters.end() ? 0 : it->second;
        };
        DFTRACER_UTILS_LOG_INFO(
            "Stats read metrics: report=%d elapsed=%.2fms "
            "root_attempts=%" PRIu64 " root_hits=%" PRIu64
            " root_misses=%" PRIu64 " fallback_groups=%" PRIu64
            " fallback_files=%" PRIu64,
            static_cast<int>(config_ptr->report_type),
            static_cast<double>(agg->read_elapsed_ns) / 1'000'000.0,
            counter("root_summary_attempts"), counter("root_summary_hits"),
            counter("root_summary_misses"), counter("fallback_groups"),
            counter("fallback_files"));
        std::printf("  Processing Time: %.2f ms\n", duration_ms);
        std::printf("==========================================\n");
    }

    co_return 0;
}

static coro::CoroTask<int> run_stats(CoroScope& ctx,
                                     const StatsArgParse* args) {
    StatsConfig config;
    if (!args->to_config(config)) {
        co_return 1;
    }

    Timer stages_storage("dftracer_stats");
    Timer* stages = args->pipeline.time_profiling ? &stages_storage : nullptr;
    Timer overall(true);

    std::vector<std::string> files;
    IndexPartition partition;
    bool used_index_source_of_truth = false;
    std::unique_ptr<AggregateStatsResult> direct_root_aggregate;

    {
        ScopedTimer _t(stages, "collect_and_classify");
        if (!config.no_auto_index) {
            ScopedTimer _ef(stages, "ensure_index_fresh");
            if (!config.directory.empty())
                co_await ensure_index_fresh(&ctx, config.directory, "",
                                            config.index_dir);
            else
                co_await ensure_indexes_fresh(&ctx, "", args->files_args.value,
                                              config.index_dir);
        }
        if (!config.directory.empty() &&
            config.report_type != StatisticsQueryType::DETAILED) {
            auto trusted_index_path = internal::determine_index_path(
                config.directory, config.index_dir);
            const bool trusted_index_exists = fs::exists(trusted_index_path);
            DFTRACER_UTILS_LOG_DEBUG(
                "Stats direct-index decision: directory=%s index_path=%s "
                "exists=%d json=%d report=%d",
                config.directory.c_str(), trusted_index_path.c_str(),
                trusted_index_exists ? 1 : 0, config.json_output ? 1 : 0,
                static_cast<int>(config.report_type));
            if (trusted_index_exists) {
                {
                    ScopedTimer _ra(stages, "root_aggregate_read");
                    if (!config.json_output) {
                        direct_root_aggregate =
                            co_await load_root_aggregate_result(
                                trusted_index_path, config.report_type);
                        DFTRACER_UTILS_LOG_DEBUG(
                            "Stats direct-index aggregate: index_path=%s "
                            "hit=%d",
                            trusted_index_path.c_str(),
                            direct_root_aggregate ? 1 : 0);
                    }
                }
                if (direct_root_aggregate) {
                    used_index_source_of_truth = true;
                } else {
                    ScopedTimer _ls(stages, "load_index_snapshot");
                    auto snapshot =
                        co_await load_index_root_snapshot(trusted_index_path);
                    files = std::move(snapshot->logical_files);
                    partition = std::move(snapshot->partition);
                    used_index_source_of_truth = true;
                }
            }
        }

        if (!used_index_source_of_truth) {
            {
                ScopedTimer _cf(stages, "collect_files");
                files = co_await collect_files(ctx, args->files_args.value,
                                               config.directory);
            }
            if (files.empty()) {
                co_return 1;
            }
            {
                ScopedTimer _ri(stages, "resolve_index_state");
                partition = co_await resolve_index_state(
                    files, config.index_dir, config.report_type);
            }
        }
    }

    if (!partition.files_needing_index.empty()) {
        if (config.no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing index for %zu file(s) and --no-auto-index is "
                "set. Run dftracer_index first.",
                partition.files_needing_index.size());
            for (const auto& f : partition.files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n",
                             f.file_path.c_str());
            }
            co_return 1;
        }
        ScopedTimer _ai(stages, "auto_index_files");
        co_await auto_index_files(ctx, partition, config.index_dir,
                                  config.checkpoint_size,
                                  config.executor_threads);
    }

    if (config.report_type == StatisticsQueryType::DETAILED) {
        if (stages) stages->print_stages();
        co_return co_await run_detailed_stats(ctx, &config, &files);
    }

    std::unique_ptr<AggregateStatsResult> agg_ptr =
        std::move(direct_root_aggregate);
    if (!agg_ptr) {
        ScopedTimer _ag(stages, "aggregate_stats");
        auto agg_val = co_await run_aggregate_stats(
            ctx, &config,
            std::make_unique<IndexPartition>(std::move(partition)));
        agg_ptr = std::make_unique<AggregateStatsResult>(std::move(agg_val));
    }

    if (stages) stages->print_stages();
    co_return co_await output_aggregate_stats(&config, &files,
                                              std::move(agg_ptr), overall);
}

int main(int argc, char** argv) {
    // Guard stays at main() scope so its destructor runs at true process exit.
    struct RocksDbExitGuard {
        ~RocksDbExitGuard() {
            dftracer::utils::rocksdb::mark_process_exiting_for_rocksdb();
        }
    } rocksdb_exit_guard;

    return cli::cli_main<StatsArgParse>(
        argc, argv, "dftracer_stats",
        "Display statistics for DFTracer trace files from pre-built "
        ".dftindex databases. Auto-builds indexes if missing. "
        "Zero-cost reads from RocksDB metadata, no decompression.",
        [](StatsArgParse& args) {
            return cli::run_single_task(
                "DFTracer Stats Main", args.pipeline,
                [&args](CoroScope& ctx) -> coro::CoroTask<int> {
                    co_return co_await run_stats(ctx, &args);
                });
        });
}
