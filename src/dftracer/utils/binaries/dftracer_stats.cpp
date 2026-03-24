#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/chunk_detail_scanner_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::filesystem;
using dftracer::utils::utilities::fileio::lines::sources::
    async_streaming_gz_lines;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::IndexDatabase;

// Files below this compressed size are scanned directly without building
// sidecar index files (.idx).  At 8 MB compressed (~160 MB
// uncompressed with typical 20x JSON compression), a file has only a
// handful of 32 MB checkpoints — the indexing overhead exceeds the
// benefit of bloom-filter skip.
static constexpr std::size_t INDEX_SIZE_THRESHOLD =
    constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;

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

using CountPair = std::pair<std::string, std::uint64_t>;

static std::vector<CountPair> sorted_by_count_desc(
    const std::unordered_map<std::string, std::uint64_t>& counts) {
    std::vector<CountPair> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const CountPair& a, const CountPair& b) {
                  return a.second > b.second;
              });
    return sorted;
}

// Format a byte value for human-readable display
static std::string format_bytes(double bytes) {
    char buf[64];
    if (bytes < 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.0f B", bytes);
    } else if (bytes < 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      bytes / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

// Format a bandwidth value (bytes/sec) for human-readable display
static std::string format_bandwidth(double bps) {
    char buf[64];
    if (bps < 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f B/s", bps);
    } else if (bps < 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
    } else if (bps < 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB/s",
                      bps / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
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

static void print_text_detailed(
    const std::string& file_path, const DetailedStatistics& detailed,
    std::uint64_t total_chunks, std::uint64_t top_n,
    const std::unordered_map<std::string, std::string>& hash_resolutions,
    const TraceStatistics* summary = nullptr,
    std::uint64_t top_n_pid_tid = 10) {
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

    // Global duration distribution
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

    // Per-group duration table, split by category
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

    // Per-group I/O metrics table
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

    std::printf("\n");
}

// Direct-scan a small .pfw.gz file without any sidecar index.
// Streams lines via async_streaming_gz_lines, parses each with yyjson,
// and accumulates stats via ChunkStatistics::update_from_event().
static coro::CoroTask<TraceStatistics> direct_scan_trace_statistics(
    std::string file_path) {
    TraceStatistics result;
    result.file_path = file_path;

    try {
        auto gen = async_streaming_gz_lines(file_path);
        ChunkStatistics stats;

        while (auto line = co_await gen.next()) {
            if (line->content.empty()) continue;

            yyjson_doc* doc = yyjson_read_opts(
                const_cast<char*>(line->content.data()), line->content.size(),
                YYJSON_READ_NOFLAG, nullptr, nullptr);
            if (!doc) continue;

            yyjson_val* root = yyjson_doc_get_root(doc);
            if (root && yyjson_is_obj(root)) {
                using dftracer::utils::utilities::common::json::JsonValue;
                JsonValue json(root);
                std::string_view ph = json["ph"].get<std::string_view>();
                if (ph != "M") {
                    stats.update_from_event(
                        json["name"].get<std::string_view>(),
                        json["cat"].get<std::string_view>(),
                        json["pid"].get<std::uint64_t>(),
                        json["tid"].get<std::uint64_t>(),
                        json["ts"].get<std::uint64_t>(),
                        json["dur"].get<std::uint64_t>());
                }
            }
            yyjson_doc_free(doc);
        }

        result.merged = stats;
        result.num_chunks = 1;
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = "Failed to scan: " + file_path + ": " + e.what();
    }

    co_return result;
}

// Direct-scan a small .pfw.gz for the detailed query path.
// Applies name/category filters and group-by dimensions.
static coro::CoroTask<DetailedStatistics> direct_scan_detailed_statistics(
    std::string file_path, const std::vector<std::string>* filter_names_ptr,
    const std::vector<std::string>* filter_cats_ptr,
    const std::vector<std::string>* group_by_ptr) {
    DetailedStatistics result;

    // Build filter sets from pointer args (pointers are safe: caller's scope
    // outlives this coroutine).
    std::unordered_set<std::string_view> name_filter;
    std::unordered_set<std::string_view> cat_filter;
    for (const auto& n : *filter_names_ptr) name_filter.insert(n);
    for (const auto& c : *filter_cats_ptr) cat_filter.insert(c);
    bool has_name_filter = !name_filter.empty();
    bool has_cat_filter = !cat_filter.empty();
    bool has_grouping = !group_by_ptr->empty();

    // I/O event names (same list as chunk_detail_scanner_utility.cpp)
    static constexpr auto IO_EVENTS = std::to_array<std::string_view>(
        {"read", "write", "pread", "pwrite", "pread64", "pwrite64", "readv",
         "writev"});
    auto is_io = [](std::string_view name) {
        return std::find(IO_EVENTS.begin(), IO_EVENTS.end(), name) !=
               IO_EVENTS.end();
    };

    try {
        auto gen = async_streaming_gz_lines(file_path);

        while (auto line = co_await gen.next()) {
            if (line->content.empty()) continue;

            yyjson_doc* doc = yyjson_read_opts(
                const_cast<char*>(line->content.data()), line->content.size(),
                YYJSON_READ_NOFLAG, nullptr, nullptr);
            if (!doc) continue;

            yyjson_val* root = yyjson_doc_get_root(doc);
            if (root && yyjson_is_obj(root)) {
                using dftracer::utils::utilities::common::json::JsonValue;
                JsonValue json(root);
                std::string_view ph = json["ph"].get<std::string_view>();

                if (ph != "M") {
                    std::string_view name_sv =
                        json["name"].get<std::string_view>();
                    std::string_view cat_sv =
                        json["cat"].get<std::string_view>();

                    bool passes = true;
                    if (has_name_filter &&
                        name_filter.find(name_sv) == name_filter.end()) {
                        passes = false;
                    }
                    if (passes && has_cat_filter &&
                        cat_filter.find(cat_sv) == cat_filter.end()) {
                        passes = false;
                    }

                    if (passes) {
                        double dur = static_cast<double>(
                            json["dur"].get<std::uint64_t>());
                        result.duration.update(dur);

                        JsonValue args = json["args"];
                        std::string io_key;

                        if (has_grouping) {
                            // Build group key inline (same logic as
                            // chunk_detail_scanner_utility.cpp)
                            std::string key;
                            key.reserve(128);
                            for (std::size_t i = 0; i < group_by_ptr->size();
                                 ++i) {
                                if (i > 0) key.push_back('|');
                                const auto& dim = (*group_by_ptr)[i];
                                if (dim == "name") {
                                    key += json["name"].get<std::string>();
                                } else if (dim == "cat") {
                                    key += json["cat"].get<std::string>();
                                } else if (dim == "pid" || dim == "tid") {
                                    key += std::to_string(
                                        json[dim].get<std::uint64_t>());
                                } else if (dim == "pid_tid") {
                                    key += std::to_string(
                                        json["pid"].get<std::uint64_t>());
                                    key.push_back(':');
                                    key += std::to_string(
                                        json["tid"].get<std::uint64_t>());
                                } else if (dim == "fhash") {
                                    if (args.exists())
                                        key += args["fhash"].get<std::string>();
                                } else if (dim == "hhash") {
                                    if (args.exists())
                                        key += args["hhash"].get<std::string>();
                                }
                            }
                            result.grouped_duration[key].update(dur);
                            result.group_key_category.emplace(
                                key, std::string(cat_sv));
                            io_key = std::move(key);
                        } else {
                            io_key = "__global__";
                        }

                        if (is_io(name_sv) && args.exists()) {
                            auto ret_opt =
                                args["ret"].get_optional<std::int64_t>();
                            if (ret_opt.has_value() && ret_opt.value() > 0) {
                                double ret =
                                    static_cast<double>(ret_opt.value());
                                auto& io = result.grouped_io[io_key];
                                io.duration.update(dur);
                                io.size.update(ret);
                                if (dur > 0) {
                                    io.bandwidth.update(ret * 1e6 / dur);
                                }
                                auto offset_opt =
                                    args["offset"]
                                        .get_optional<std::uint64_t>();
                                if (offset_opt.has_value()) {
                                    io.offset.update(static_cast<double>(
                                        offset_opt.value()));
                                }
                            }
                        }

                        result.events_scanned++;
                    }
                }
            }
            yyjson_doc_free(doc);
        }

        result.chunks_scanned = 1;
    } catch (const std::exception&) {
        // Return empty result on open/read failure (matches original behaviour)
    }

    co_return result;
}

// Per-chunk scanning coroutine for parallel detailed stats.
// Scans a single chunk and merges results into shared file_detailed.
static coro::CoroTask<void> scan_chunk_detailed(
    std::string file_path, std::string idx_path, std::size_t checkpoint_size,
    std::size_t file_size, std::size_t num_ckpts, std::uint64_t ckpt_idx,
    const std::vector<std::string>* filter_names_ptr,
    const std::vector<std::string>* filter_cats_ptr,
    const std::vector<std::string>* group_by_ptr,
    std::shared_ptr<DetailedStatistics> file_detailed,
    std::shared_ptr<std::mutex> chunk_mutex) {
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
    scan_input.idx_path = idx_path;
    scan_input.checkpoint_size = checkpoint_size;
    scan_input.start_byte = start_byte;
    scan_input.end_byte = end_byte;
    scan_input.checkpoint_idx = ckpt_idx;
    scan_input.filter_names = *filter_names_ptr;
    scan_input.filter_categories = *filter_cats_ptr;
    scan_input.group_by = *group_by_ptr;

    ChunkDetailScannerUtility scanner;
    auto scan_output = co_await scanner.process(scan_input);

    if (scan_output.success) {
        std::lock_guard<std::mutex> lock(*chunk_mutex);
        file_detailed->merge(scan_output.stats);
    }

    co_return;
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
    std::string idx_path = internal::determine_index_path(file_path, index_dir);

    auto meta_input = MetadataCollectorUtilityInput::from_file(file_path)
                          .with_checkpoint_size(checkpoint_size)
                          .with_force_rebuild(false)
                          .with_index(idx_path);
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

    if (query_ptr && fs::exists(idx_path)) {
        try {
            ChunkPrunerInput pruner_input{idx_path, file_path, *query_ptr,
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

    // Scan candidate chunks in parallel
    auto file_detailed = std::make_shared<DetailedStatistics>();
    file_detailed->chunks_skipped =
        total_checkpoints - candidate_checkpoints.size();
    auto chunk_mutex = std::make_shared<std::mutex>();

    co_await fctx.scope([file_path, idx_path, checkpoint_size, file_size,
                         num_ckpts, filter_names_ptr, filter_cats_ptr,
                         group_by_ptr, file_detailed, chunk_mutex,
                         candidates = std::move(candidate_checkpoints)](
                            CoroScope& chunk_scope) -> coro::CoroTask<void> {
        for (auto ckpt_idx : candidates) {
            chunk_scope.spawn(
                [file_path, idx_path, checkpoint_size, file_size, num_ckpts,
                 ckpt_idx, filter_names_ptr, filter_cats_ptr, group_by_ptr,
                 file_detailed,
                 chunk_mutex](CoroScope& /*cctx*/) -> coro::CoroTask<void> {
                    co_return co_await scan_chunk_detailed(
                        file_path, idx_path, checkpoint_size, file_size,
                        num_ckpts, ckpt_idx, filter_names_ptr, filter_cats_ptr,
                        group_by_ptr, file_detailed, chunk_mutex);
                });
        }
        co_return;
    });

    // Hash resolution (sequential, all chunks done)
    std::unordered_map<std::string, std::string> hash_resolutions;
    if (needs_hash_resolution && fs::exists(idx_path)) {
        try {
            IndexDatabase idx_db(idx_path);
            auto logical =
                utilities::indexer::internal::get_logical_path(file_path);
            int file_info_id = idx_db.get_file_info_id(logical);
            if (file_info_id >= 0) {
                auto resolve_hashes = [&](const std::string& dim) {
                    for (const auto& [key, _] :
                         file_detailed->grouped_duration) {
                        if (hash_resolutions.count(key) == 0) {
                            auto resolved =
                                idx_db.query_resolved_by_hash(dim, key);
                            if (resolved.has_value()) {
                                hash_resolutions[key] = resolved.value();
                            }
                        }
                    }
                    for (const auto& [key, _] : file_detailed->grouped_io) {
                        if (hash_resolutions.count(key) == 0) {
                            auto resolved =
                                idx_db.query_resolved_by_hash(dim, key);
                            if (resolved.has_value()) {
                                hash_resolutions[key] = resolved.value();
                            }
                        }
                    }
                };

                for (const auto& dim : *group_by_ptr) {
                    if (dim == "fhash" || dim == "hhash") {
                        resolve_hashes(dim);
                    }
                }
            }
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("Hash resolution failed for %s: %s",
                                    file_path.c_str(), e.what());
        }
    }

    // Output per-file results
    if (json_output) {
        std::string detail_json = file_detailed->to_json();
        std::string json_obj = std::string("{\"file_path\": \"") + file_path +
                               "\", \"detailed\": " + detail_json + "}";
        std::lock_guard<std::mutex> lock(*output_mutex_ptr);
        json_results_ptr->emplace_back(fi, std::move(json_obj));
    } else {
        std::lock_guard<std::mutex> lock(*output_mutex_ptr);
        print_text_detailed(
            file_path, *file_detailed,
            file_detailed->chunks_scanned + file_detailed->chunks_skipped,
            top_n, hash_resolutions);
    }

    {
        std::lock_guard<std::mutex> lock(*aggregate_mutex_ptr);
        aggregate_detailed_ptr->merge(*file_detailed);
    }

    co_return;
}

static void run_detailed_query_workers(
    CoroScope& scope, const std::vector<std::string>* files_ptr,
    const std::vector<std::string>* small_files_ptr,
    std::size_t executor_threads, std::string index_dir,
    std::size_t checkpoint_size, bool needs_hash_resolution, bool json_output,
    std::size_t top_n, const common::query::Query* qp,
    const std::vector<std::string>* fn, const std::vector<std::string>* fc,
    const std::vector<std::string>* gb, DetailedStatistics* ad, std::mutex* am,
    std::mutex* om, std::vector<std::pair<std::size_t, std::string>>* jr) {
    auto small_set = std::make_shared<std::unordered_set<std::string>>(
        small_files_ptr->begin(), small_files_ptr->end());

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
        scope.spawn([file_chan, files_ptr, index_dir, checkpoint_size,
                     needs_hash_resolution, json_output, top_n, small_set, qp,
                     fn, fc, gb, ad, am, om,
                     jr](CoroScope& fctx) -> coro::CoroTask<void> {
            while (auto fi_opt = co_await file_chan->receive()) {
                std::size_t fi = *fi_opt;
                const auto& file_path = (*files_ptr)[fi];
                bool is_small = small_set->count(file_path) > 0;

                if (is_small) {
                    auto stats = co_await direct_scan_detailed_statistics(
                        file_path, fn, fc, gb);
                    {
                        std::lock_guard<std::mutex> lock(*am);
                        ad->merge(stats);
                    }
                    continue;
                }
                co_await process_file_detailed(
                    fctx, file_path, fi, index_dir, checkpoint_size,
                    needs_hash_resolution, json_output, top_n, qp, fn, fc, gb,
                    ad, am, om, jr);
            }
            co_return;
        });
    }
}

static coro::CoroTask<int> run_stats(argparse::ArgumentParser& program) {
    std::string directory = program.get<std::string>("--directory");
    std::string index_dir = program.get<std::string>("--index-dir");
    bool json_output = program.get<bool>("--json");
    std::string report_str = program.get<std::string>("--report");
    std::uint64_t top_n = program.get<std::uint64_t>("--top-n");
    std::uint64_t top_n_pid_tid = program.get<std::uint64_t>("--top-n-pid-tid");
    bool no_auto_index = program.get<bool>("--no-auto-index");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    auto query_str = program.get<std::string>("--query");
    auto group_by = program.get<std::vector<std::string>>("--group-by");

    using common::query::Query;
    std::optional<Query> query;
    std::vector<std::string> filter_names;
    std::vector<std::string> filter_cats;

    if (!query_str.empty()) {
        auto result = Query::from_string(query_str);
        if (!result) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --query: %s",
                                     result.error().format().c_str());
            co_return 1;
        }
        query = std::move(*result);
    }

    auto report_type = parse_report_type_str(report_str);

    // Default --group-by to "name" for detailed query so users always see
    // per-event breakdowns (not just global I/O aggregates)
    if (report_type == StatisticsQueryType::DETAILED && group_by.empty()) {
        group_by.push_back("name");
    }

    // Validate group-by dimensions
    const std::vector<std::string> valid_dims = {
        "name", "cat", "pid", "tid", "fhash", "hhash", "pid_tid"};
    for (const auto& dim : group_by) {
        if (std::find(valid_dims.begin(), valid_dims.end(), dim) ==
            valid_dims.end()) {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid --group-by dimension: %s. Valid: name, cat, pid, "
                "tid, fhash, hhash, pid_tid",
                dim.c_str());
            co_return 1;
        }
    }

    // Collect files
    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            co_return 1;
        }

        PatternDirectoryScannerUtility scanner;
        PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = co_await scanner.process(scan_input);

        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                     directory.c_str());
            co_return 1;
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            std::cerr << program;
            co_return 1;
        }
    }

    // Partition files: large files get indexed, small files are scanned
    // directly to avoid creating sidecar files on metadata-sensitive
    // filesystems (e.g. Lustre).
    std::vector<std::string> files_needing_index;
    std::vector<std::string> small_files;
    for (const auto& file_path : files) {
        std::string idx_path =
            internal::determine_index_path(file_path, index_dir);
        if (fs::exists(idx_path)) {
            try {
                IndexDatabase db(idx_path);
                auto logical =
                    utilities::indexer::internal::get_logical_path(file_path);
                int fid = db.get_file_info_id(logical);
                if (fid >= 0 && db.has_bloom_data(fid)) continue;
            } catch (...) {
            }
        }
        std::error_code ec;
        auto fsize = fs::file_size(file_path, ec);
        if (ec || fsize == 0) {
            continue;  // skip unreadable or empty files
        }
        if (fsize < INDEX_SIZE_THRESHOLD) {
            small_files.push_back(file_path);
        } else {
            files_needing_index.push_back(file_path);
        }
    }

    if (!small_files.empty()) {
        std::printf(
            "Skipping index for %zu small file(s) (< %zu bytes "
            "compressed); will scan directly.\n",
            small_files.size(), INDEX_SIZE_THRESHOLD);
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing index for %zu file(s) and --no-auto-index is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            co_return 1;
        }

        std::printf("Auto-building index for %zu file(s)...\n",
                    files_needing_index.size());

        auto pipeline_config = PipelineConfig()
                                   .with_name("DFTracer Stats Auto-Indexer")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        std::atomic<std::size_t> indexed_count{0};
        std::atomic<std::size_t> failed_count{0};

        auto index_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                auto file_chan =
                    coro::make_channel<std::string>(executor_threads * 2);

                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    auto* files_ptr = &files_needing_index;
                    scope.spawn(
                        [ch = file_chan->producer(), files_ptr](
                            CoroScope&) mutable -> coro::CoroTask<void> {
                            auto guard = ch.guard();
                            for (const auto& f : *files_ptr) {
                                if (!co_await ch.send(f)) {
                                    co_return;
                                }
                            }
                            co_return;
                        });

                    auto* indexed_count_ptr = &indexed_count;
                    auto* failed_count_ptr = &failed_count;
                    std::string index_dir_copy = index_dir;
                    std::size_t ckpt_size = checkpoint_size;
                    for (std::size_t w = 0; w < executor_threads; ++w) {
                        scope.spawn([file_chan, index_dir_copy, ckpt_size,
                                     indexed_count_ptr, failed_count_ptr](
                                        CoroScope&) -> coro::CoroTask<void> {
                            while (auto file_path =
                                       co_await file_chan->receive()) {
                                try {
                                    IndexBuilderUtility builder;
                                    auto config =
                                        IndexBuildConfig::for_file(*file_path)
                                            .with_index_dir(index_dir_copy)
                                            .with_checkpoint_size(ckpt_size)
                                            .with_bloom(true)
                                            .with_index_threshold(0);
                                    auto result =
                                        co_await builder.process(config);

                                    if (result.success) {
                                        (*indexed_count_ptr)++;
                                    } else {
                                        (*failed_count_ptr)++;
                                        DFTRACER_UTILS_LOG_ERROR(
                                            "Auto-indexing failed "
                                            "for %s: %s",
                                            file_path->c_str(),
                                            result.error_message.c_str());
                                    }
                                } catch (const std::exception& e) {
                                    (*failed_count_ptr)++;
                                    DFTRACER_UTILS_LOG_ERROR(
                                        "Auto-indexing exception "
                                        "for %s: %s",
                                        file_path->c_str(), e.what());
                                }
                            }
                            co_return;
                        });
                    }
                    co_return;
                });

                co_return;
            },
            "AutoIndex");

        pipeline.set_source(index_task);
        pipeline.set_destination(index_task);
        pipeline.execute();

        std::printf("Auto-indexing complete: %zu indexed, %zu failed\n",
                    indexed_count.load(), failed_count.load());
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // Detailed query path: scan chunks on-demand with bloom pre-filtering
    if (report_type == StatisticsQueryType::DETAILED) {
        bool needs_hash_resolution = false;
        for (const auto& dim : group_by) {
            if (dim == "fhash" || dim == "hhash") {
                needs_hash_resolution = true;
                break;
            }
        }

        DetailedStatistics aggregate_detailed;
        std::mutex aggregate_mutex;
        std::mutex output_mutex;
        std::vector<std::pair<std::size_t, std::string>> json_results;

        {
            auto pipeline_config = PipelineConfig()
                                       .with_name("DFTracer Stats Detailed")
                                       .with_compute_threads(executor_threads)
                                       .with_watchdog(false);

            Pipeline pipeline(pipeline_config);

            auto stats_task = make_task(
                [&](CoroScope& ctx) -> coro::CoroTask<void> {
                    co_await ctx.scope(
                        [&](CoroScope& scope) -> coro::CoroTask<void> {
                            run_detailed_query_workers(
                                scope, &files, &small_files, executor_threads,
                                index_dir, checkpoint_size,
                                needs_hash_resolution, json_output, top_n,
                                query ? &*query : nullptr, &filter_names,
                                &filter_cats, &group_by, &aggregate_detailed,
                                &aggregate_mutex, &output_mutex, &json_results);
                            co_return;
                        });
                    co_return;
                },
                "StatsDetailed");

            pipeline.set_source(stats_task);
            pipeline.set_destination(stats_task);
            pipeline.execute();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration =
            end_time - start_time;

        if (json_output) {
            std::printf("[\n");
            std::sort(
                json_results.begin(), json_results.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < json_results.size(); ++i) {
                std::printf("%s%s", json_results[i].second.c_str(),
                            i + 1 < json_results.size() ? ",\n" : "\n");
            }
            std::printf("]\n");
        } else {
            std::printf("==========================================\n");
            std::printf("Consolidated Detailed (%zu files)\n", files.size());
            std::printf("==========================================\n");
            std::unordered_map<std::string, std::string> no_resolutions;
            print_text_detailed(directory, aggregate_detailed,
                                aggregate_detailed.chunks_scanned +
                                    aggregate_detailed.chunks_skipped,
                                top_n, no_resolutions);
            std::printf("  Processing Time: %.2f ms\n", duration.count());
            std::printf("==========================================\n");
        }

        co_return 0;
    }

    // Non-detailed path: aggregate statistics per file in parallel
    std::vector<std::pair<std::size_t, TraceStatistics>> indexed_stats;
    std::mutex stats_mutex;

    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("DFTracer Stats")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        auto stats_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    auto* indexed_stats_ptr = &indexed_stats;
                    auto* stats_mutex_ptr = &stats_mutex;
                    auto* files_ptr = &files;

                    // Build set of small files for O(1) lookup.
                    // shared_ptr so workers keep it alive after this
                    // scope lambda's coroutine frame is destroyed.
                    auto small_set =
                        std::make_shared<std::unordered_set<std::string>>(
                            small_files.begin(), small_files.end());

                    auto file_chan =
                        coro::make_channel<std::size_t>(executor_threads * 2);

                    // Producer: push file indices
                    scope.spawn(
                        [ch = file_chan->producer(), files_ptr](
                            CoroScope&) mutable -> coro::CoroTask<void> {
                            auto guard = ch.guard();
                            for (std::size_t fi = 0; fi < files_ptr->size();
                                 ++fi) {
                                if (!co_await ch.send(fi)) {
                                    co_return;
                                }
                            }
                            co_return;
                        });

                    // Workers: N coroutines, each processing one file at a time
                    for (std::size_t w = 0; w < executor_threads; ++w) {
                        scope.spawn([file_chan, files_ptr, index_dir, small_set,
                                     indexed_stats_ptr, stats_mutex_ptr](
                                        CoroScope&) -> coro::CoroTask<void> {
                            while (auto fi_opt =
                                       co_await file_chan->receive()) {
                                std::size_t fi = *fi_opt;
                                const auto& file_path = (*files_ptr)[fi];
                                bool is_small = small_set->count(file_path) > 0;

                                TraceStatistics result;
                                if (is_small) {
                                    result =
                                        co_await direct_scan_trace_statistics(
                                            file_path);
                                } else {
                                    StatisticsAggregatorInput agg_input;
                                    agg_input.file_path = file_path;
                                    agg_input.index_dir = index_dir;

                                    StatisticsAggregatorUtility aggregator;
                                    result =
                                        co_await aggregator.process(agg_input);
                                }

                                std::lock_guard<std::mutex> lock(
                                    *stats_mutex_ptr);
                                indexed_stats_ptr->emplace_back(
                                    fi, std::move(result));
                            }
                            co_return;
                        });
                    }
                    co_return;
                });

                co_return;
            },
            "StatsProcess");

        pipeline.set_source(stats_task);
        pipeline.set_destination(stats_task);
        pipeline.execute();
    }

    // Restore original file order
    std::sort(indexed_stats.begin(), indexed_stats.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<TraceStatistics> all_stats;
    all_stats.reserve(indexed_stats.size());
    for (auto& [_, stats] : indexed_stats) {
        all_stats.push_back(std::move(stats));
    }

    // Merge all per-file stats into a single consolidated result
    TraceStatistics total;
    total.success = true;
    total.file_path = directory;
    std::size_t successful = 0;
    std::size_t failed = 0;

    for (const auto& stats : all_stats) {
        if (stats.success) {
            total.merged.merge_from(stats.merged);
            total.num_chunks += stats.num_chunks;
            successful++;
        } else {
            failed++;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (json_output) {
        // For JSON, output per-file results
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
            qi.query_type = report_type;
            qi.top_n = top_n;
            auto output = co_await query_util.process(qi);
            std::printf("%s%s", output.to_json().c_str(),
                        i + 1 < all_stats.size() ? ",\n" : "\n");
        }
        std::printf("]\n");
    } else {
        // Text output: print consolidated summary
        std::printf("==========================================\n");
        std::printf("Consolidated (%zu files, %zu successful, %zu failed)\n",
                    files.size(), successful, failed);
        std::printf("==========================================\n");
        auto detailed = to_detailed(total);
        std::unordered_map<std::string, std::string> no_resolutions;
        print_text_detailed(total.file_path, detailed, total.num_chunks, top_n,
                            no_resolutions, &total, top_n_pid_tid);
        std::printf("  Processing Time: %.2f ms\n", duration.count());
        std::printf("==========================================\n");
    }

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_stats",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Display statistics for DFTracer trace files from pre-built "
        "index (.idx) databases. Auto-builds indices if missing. "
        "Zero-cost reads: only SQLite metadata, no decompression.");

    program.add_argument("--files")
        .help("Trace files to inspect (.pfw, .pfw.gz)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .default_value<std::string>("");

    program.add_argument("--index-dir")
        .help("Directory where .idx index files are stored")
        .default_value<std::string>("");

    program.add_argument("--json").help("Output in JSON format").flag();

    program.add_argument("--report")
        .help(
            "Report type: summary, categories, names, pid_tids, time_range, "
            "duration, top-names, top-categories, detailed")
        .default_value<std::string>("summary");

    program.add_argument("--top-n")
        .help(
            "Number of results for top-N queries (0 = show all, "
            "default: 0)")
        .scan<'d', std::uint64_t>()
        .default_value(static_cast<std::uint64_t>(0));

    program.add_argument("--top-n-pid-tid")
        .help("Max PID:TID pairs to display (0 = show all, default: 10)")
        .scan<'d', std::uint64_t>()
        .default_value(static_cast<std::uint64_t>(10));

    program.add_argument("--no-auto-index")
        .help("Disable automatic index building for files missing .idx")
        .flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for auto-indexing in bytes (default: " +
              std::to_string(
                  indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE) +
              ")")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help("Number of worker threads for auto-indexing")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--query")
        .help("Query DSL filter (e.g., 'cat == \"POSIX\" and dur > 1000')")
        .default_value<std::string>("");

    program.add_argument("--group-by")
        .help(
            "Group detailed statistics by dimension(s): name, cat, pid, "
            "tid, fhash, hhash, pid_tid. Multiple values create composite "
            "keys.")
        .nargs(argparse::nargs_pattern::at_least_one)
        .default_value<std::vector<std::string>>({});

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    return run_stats(program).get();
}
