#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_query_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/chunk_detail_scanner_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::statistics;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::filesystem;

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

static void print_text_summary(const TraceStatistics& stats,
                               std::uint64_t top_n) {
    std::printf("========================================\n");
    std::printf("File: %s\n", stats.file_path.c_str());
    std::printf("========================================\n");

    if (!stats.success) {
        std::printf("  Status: ERROR - %s\n\n", stats.error_message.c_str());
        return;
    }

    std::printf("  Chunks: %llu\n", (unsigned long long)stats.num_chunks);
    std::printf("  Total Events: %llu\n",
                (unsigned long long)stats.total_events());

    if (stats.time_span_seconds() > 0.0) {
        std::printf("  Time Span: %.6f seconds\n", stats.time_span_seconds());
    }

    // Category breakdown
    const auto& cat_counts = stats.merged.category_counts;
    auto sorted_cats = sorted_by_count_desc(cat_counts);
    std::printf("\n  Categories (%zu):\n", cat_counts.size());
    for (const auto& [name, count] : sorted_cats) {
        std::printf("    %-40s %llu\n", name.c_str(),
                    (unsigned long long)count);
    }

    // Top operation names
    const auto& name_counts = stats.merged.name_counts;
    auto sorted_names = sorted_by_count_desc(name_counts);
    std::size_t names_to_show =
        std::min(static_cast<std::size_t>(top_n), sorted_names.size());
    if (names_to_show < sorted_names.size()) {
        std::printf("\n  Top Operations (%zu of %zu):\n", names_to_show,
                    sorted_names.size());
    } else {
        std::printf("\n  Operations (%zu):\n", sorted_names.size());
    }
    for (std::size_t i = 0; i < names_to_show; ++i) {
        std::printf("    %-40s %llu\n", sorted_names[i].first.c_str(),
                    (unsigned long long)sorted_names[i].second);
    }

    // PID:TID breakdown
    const auto& pid_tid_counts = stats.merged.pid_tid_counts;
    auto sorted_pid_tids = sorted_by_count_desc(pid_tid_counts);
    std::size_t pid_tids_to_show =
        std::min(static_cast<std::size_t>(top_n), sorted_pid_tids.size());
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

    // Duration stats
    if (stats.merged.duration_count > 0) {
        std::printf("\n  Duration:\n");
        std::printf("    Count: %llu\n",
                    (unsigned long long)stats.merged.duration_count);
        std::printf("    Mean: %.2f us\n", stats.duration_mean_us());
        std::printf("    Stddev: %.2f us\n", stats.duration_stddev_us());
        if (stats.merged.duration_min_us !=
            std::numeric_limits<std::uint64_t>::max()) {
            std::printf("    Min: %llu us\n",
                        (unsigned long long)stats.merged.duration_min_us);
        }
        std::printf("    Max: %llu us\n",
                    (unsigned long long)stats.merged.duration_max_us);
    }

    std::printf("\n");
}

static void print_text_query(const StatisticsQueryOutput& output,
                             const std::string& file_path) {
    std::printf("========================================\n");
    std::printf("Query: %s  File: %s\n", output.query_type_name.c_str(),
                file_path.c_str());
    std::printf("========================================\n");
    std::printf("  Total Events: %llu\n",
                (unsigned long long)output.total_events);

    if (!output.results.empty()) {
        std::printf("  Results:\n");
        for (const auto& [name, count] : output.results) {
            std::printf("    %-40s %llu\n", name.c_str(),
                        (unsigned long long)count);
        }
    }

    if (output.min_timestamp_us > 0 || output.max_timestamp_us > 0) {
        std::printf("  Time Range:\n");
        std::printf("    Min Timestamp: %llu us\n",
                    (unsigned long long)output.min_timestamp_us);
        std::printf("    Max Timestamp: %llu us\n",
                    (unsigned long long)output.max_timestamp_us);
        std::printf("    Span: %.6f seconds\n", output.time_span_seconds);
    }

    if (output.duration_count > 0) {
        std::printf("  Duration:\n");
        std::printf("    Count: %llu\n",
                    (unsigned long long)output.duration_count);
        std::printf("    Mean: %.2f us\n", output.duration_mean_us);
        std::printf("    Stddev: %.2f us\n", output.duration_stddev_us);
        std::printf("    Min: %llu us\n",
                    (unsigned long long)output.duration_min_us);
        std::printf("    Max: %llu us\n",
                    (unsigned long long)output.duration_max_us);
    }

    std::printf("\n");
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
    const std::unordered_map<std::string, std::string>& hash_resolutions) {
    std::printf("========================================\n");
    std::printf("File: %s\n", file_path.c_str());
    std::printf("========================================\n");
    std::printf("  Chunks: %llu (scanned: %llu, skipped: %llu)\n",
                (unsigned long long)total_chunks,
                (unsigned long long)detailed.chunks_scanned,
                (unsigned long long)detailed.chunks_skipped);
    std::printf("  Events Scanned: %llu\n",
                (unsigned long long)detailed.events_scanned);

    // Global duration distribution
    if (detailed.duration.count() > 0) {
        std::printf("\n  Duration Distribution:\n");
        std::printf("%s",
                    detailed.duration.histogram.render_ascii(40, "us").c_str());

        if (!detailed.duration.sketch.empty()) {
            std::printf("  Duration Percentiles:\n");
            std::printf(
                "    p50: %.1f us  p90: %.1f us  p99: %.1f us  p99.9: "
                "%.1f us\n",
                detailed.duration.sketch.quantile(0.5),
                detailed.duration.sketch.quantile(0.9),
                detailed.duration.sketch.quantile(0.99),
                detailed.duration.sketch.quantile(0.999));
        }
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

        // Track overall top event for histogram
        const DistributionStats* overall_top_dist = nullptr;
        std::string overall_top_key;

        for (const auto& [cat, entries_ptr] : sorted_cats) {
            const auto& entries = *entries_ptr;

            std::size_t show =
                std::min(static_cast<std::size_t>(top_n), entries.size());

            if (show < entries.size()) {
                std::printf("\n  Duration [%s] (top %zu of %zu):\n",
                            cat.c_str(), show, entries.size());
            } else {
                std::printf("\n  Duration [%s] (%zu):\n", cat.c_str(),
                            entries.size());
            }
            std::printf("    %-30s %12s %12s %12s %12s %12s\n", "Name", "Count",
                        "Mean us", "p50 us", "p90 us", "p99 us");

            for (std::size_t i = 0; i < show; ++i) {
                const auto& [key, dist] = entries[i];
                std::string display_key =
                    resolve_display_key(key, hash_resolutions);
                if (display_key.size() > 30) {
                    display_key = display_key.substr(0, 27) + "...";
                }

                double p50 =
                    dist->sketch.empty() ? 0.0 : dist->sketch.quantile(0.5);
                double p90 =
                    dist->sketch.empty() ? 0.0 : dist->sketch.quantile(0.9);
                double p99 =
                    dist->sketch.empty() ? 0.0 : dist->sketch.quantile(0.99);

                std::printf("    %-30s %12llu %12.1f %12.1f %12.1f %12.1f\n",
                            display_key.c_str(),
                            (unsigned long long)dist->count(), dist->mean(),
                            p50, p90, p99);

                // Track the overall top event
                if (!overall_top_dist ||
                    dist->count() > overall_top_dist->count()) {
                    overall_top_dist = dist;
                    overall_top_key = key;
                }
            }
        }

        // Print histogram for overall top event
        if (overall_top_dist) {
            std::string display_key =
                resolve_display_key(overall_top_key, hash_resolutions);
            std::printf("\n  Duration Histogram (top: %s):\n",
                        display_key.c_str());
            std::printf(
                "%s",
                overall_top_dist->histogram.render_ascii(40, "us").c_str());
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
            std::min(static_cast<std::size_t>(top_n), sorted_io.size());

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

static coro::CoroTask<int> run_stats(argparse::ArgumentParser& program) {
    std::string directory = program.get<std::string>("--directory");
    std::string index_dir = program.get<std::string>("--index-dir");
    bool json_output = program.get<bool>("--json");
    std::string report_str = program.get<std::string>("--report");
    std::uint64_t top_n = program.get<std::uint64_t>("--top-n");
    bool no_auto_index = program.get<bool>("--no-auto-index");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    auto query_strs = program.get<std::vector<std::string>>("--query");
    auto group_by = program.get<std::vector<std::string>>("--group-by");

    // Parse --query into unified predicates
    PredicateParserInput parser_input;
    parser_input.with_predicate_strings(query_strs);
    auto parsed = PredicateParserUtility{}.process(parser_input);
    PredicateMap merged_predicates =
        parsed.success ? parsed.predicates : PredicateMap{};

    // Extract filter_names and filter_cats from parsed predicates for
    // event-level exact matching
    std::vector<std::string> filter_names;
    std::vector<std::string> filter_cats;
    if (auto it = merged_predicates.find("name");
        it != merged_predicates.end()) {
        filter_names = it->second;
    }
    if (auto it = merged_predicates.find("cat");
        it != merged_predicates.end()) {
        filter_cats = it->second;
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

    // Auto-build bloom indices for files missing .bidx
    std::vector<std::string> files_needing_index;
    for (const auto& file_path : files) {
        std::string bidx_path =
            determine_bloom_index_path(file_path, index_dir);
        if (!fs::exists(bidx_path)) {
            files_needing_index.push_back(file_path);
        }
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing .bidx index for %zu file(s) and --no-auto-index is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            co_return 1;
        }

        std::printf("Auto-building bloom index for %zu file(s)...\n",
                    files_needing_index.size());

        auto pipeline_config = PipelineConfig()
                                   .with_name("DFTracer Stats Auto-Indexer")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        std::atomic<std::size_t> indexed_count{0};
        std::atomic<std::size_t> failed_count{0};

        BloomIndexBuildInput build_template;
        build_template.index_dir = index_dir;
        build_template.checkpoint_size = checkpoint_size;
        build_template.dimensions = default_bloom_dimensions();

        auto index_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    auto* indexed_count_ptr = &indexed_count;
                    auto* failed_count_ptr = &failed_count;
                    for (std::size_t i = 0; i < files_needing_index.size();
                         ++i) {
                        const auto file_path = files_needing_index[i];
                        scope.spawn([build_template, file_path,
                                     indexed_count_ptr,
                                     failed_count_ptr](CoroScope& fctx)
                                        -> coro::CoroTask<void> {
                            BloomIndexBuildInput build_input = build_template;
                            build_input.file_path = file_path;

                            auto utility =
                                std::make_shared<BloomIndexBuilderUtility>();
                            behaviors::BehaviorChain<BloomIndexBuildInput,
                                                     BloomIndexBuildOutput>
                                chain;
                            behaviors::UtilityExecutor<
                                BloomIndexBuildInput, BloomIndexBuildOutput,
                                utilities::tags::NeedsContext>
                                executor(utility, std::move(chain));

                            auto result =
                                co_await executor.execute_with_context(
                                    fctx, build_input);

                            if (result.success) {
                                (*indexed_count_ptr)++;
                            } else {
                                (*failed_count_ptr)++;
                                DFTRACER_UTILS_LOG_ERROR(
                                    "Auto-indexing failed for %s: %s",
                                    file_path.c_str(),
                                    result.error_message.c_str());
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
        // Determine if we need hash resolutions
        bool needs_hash_resolution = false;
        for (const auto& dim : group_by) {
            if (dim == "fhash" || dim == "hhash") {
                needs_hash_resolution = true;
                break;
            }
        }

        if (json_output) {
            std::printf("[\n");
        }

        DetailedStatistics aggregate_detailed;

        for (std::size_t fi = 0; fi < files.size(); ++fi) {
            const auto& file_path = files[fi];

            // Resolve paths
            std::string bidx_path =
                determine_bloom_index_path(file_path, index_dir);
            std::string idx_path =
                internal::determine_index_path(file_path, index_dir);

            // Collect metadata
            auto meta_input =
                MetadataCollectorUtilityInput::from_file(file_path)
                    .with_checkpoint_size(checkpoint_size)
                    .with_force_rebuild(false)
                    .with_index(idx_path);
            auto metadata =
                co_await MetadataCollectorUtility{}.process(meta_input);

            if (!metadata.success) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to collect metadata for %s: %s", file_path.c_str(),
                    metadata.error_message.c_str());
                continue;
            }

            std::size_t file_size = metadata.uncompressed_size;
            std::size_t num_ckpts = metadata.num_checkpoints;

            // Use merged predicates for bloom pre-filtering
            const auto& predicates = merged_predicates;

            // Determine candidate checkpoints via bloom pre-filtering
            std::vector<std::uint64_t> candidate_checkpoints;
            std::uint64_t total_checkpoints = (num_ckpts == 0) ? 1 : num_ckpts;

            if (!predicates.empty() && fs::exists(bidx_path)) {
                try {
                    BloomQueryInput bq_input;
                    bq_input.bidx_path = bidx_path;
                    bq_input.file_path = file_path;
                    bq_input.predicates = predicates;

                    BloomQueryUtility bloom_query;
                    auto bq_output = co_await bloom_query.process(bq_input);

                    if (bq_output.success) {
                        candidate_checkpoints = bq_output.candidate_checkpoints;
                        total_checkpoints = bq_output.total_checkpoints;
                    } else {
                        // Fallback: scan all chunks
                        for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                            candidate_checkpoints.push_back(i);
                        }
                    }
                } catch (const std::exception& e) {
                    DFTRACER_UTILS_LOG_WARN(
                        "Bloom query failed for %s: %s, scanning all chunks",
                        file_path.c_str(), e.what());
                    for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                        candidate_checkpoints.push_back(i);
                    }
                }
            } else {
                // No filters or no bidx: scan all chunks
                for (std::uint64_t i = 0; i < total_checkpoints; ++i) {
                    candidate_checkpoints.push_back(i);
                }
            }

            // Compute byte ranges and scan candidate chunks
            DetailedStatistics file_detailed;
            file_detailed.chunks_skipped =
                total_checkpoints - candidate_checkpoints.size();

            ChunkDetailScannerUtility scanner;

            for (auto ckpt_idx : candidate_checkpoints) {
                std::size_t start_byte = 0;
                std::size_t end_byte = file_size;

                if (num_ckpts > 0) {
                    std::size_t bytes_per = file_size / num_ckpts;
                    start_byte = ckpt_idx * bytes_per;
                    end_byte = (ckpt_idx + 1 == num_ckpts)
                                   ? file_size
                                   : (ckpt_idx + 1) * bytes_per;
                }

                ChunkDetailScanInput scan_input;
                scan_input.file_path = file_path;
                scan_input.idx_path = idx_path;
                scan_input.checkpoint_size = checkpoint_size;
                scan_input.start_byte = start_byte;
                scan_input.end_byte = end_byte;
                scan_input.checkpoint_idx = ckpt_idx;
                scan_input.filter_names = filter_names;
                scan_input.filter_categories = filter_cats;
                scan_input.group_by = group_by;

                auto scan_output = co_await scanner.process(scan_input);
                if (scan_output.success) {
                    file_detailed.merge(scan_output.stats);
                }
            }

            // Load hash resolutions for display if needed
            std::unordered_map<std::string, std::string> hash_resolutions;
            if (needs_hash_resolution && fs::exists(bidx_path)) {
                try {
                    BloomIndexDatabase bidx_db(bidx_path);
                    int file_info_id = bidx_db.get_file_info_id(file_path);
                    if (file_info_id >= 0) {
                        // Collect all unique hash keys that need resolution
                        auto resolve_hashes = [&](const std::string& dim) {
                            // Check grouped_duration keys
                            for (const auto& [key, _] :
                                 file_detailed.grouped_duration) {
                                if (hash_resolutions.count(key) == 0) {
                                    auto resolved =
                                        queries::query_resolved_by_hash(
                                            bidx_db.db(), dim, key);
                                    if (resolved.has_value()) {
                                        hash_resolutions[key] =
                                            resolved.value();
                                    }
                                }
                            }
                            // Check grouped_io keys
                            for (const auto& [key, _] :
                                 file_detailed.grouped_io) {
                                if (hash_resolutions.count(key) == 0) {
                                    auto resolved =
                                        queries::query_resolved_by_hash(
                                            bidx_db.db(), dim, key);
                                    if (resolved.has_value()) {
                                        hash_resolutions[key] =
                                            resolved.value();
                                    }
                                }
                            }
                        };

                        for (const auto& dim : group_by) {
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

            if (json_output) {
                // Wrap per-file detailed stats in a file object
                std::string detail_json = file_detailed.to_json();
                std::printf("{\"file_path\": \"%s\", \"detailed\": %s}%s",
                            file_path.c_str(), detail_json.c_str(),
                            fi + 1 < files.size() ? ",\n" : "\n");
            } else {
                print_text_detailed(file_path, file_detailed, total_checkpoints,
                                    top_n, hash_resolutions);
            }

            aggregate_detailed.merge(file_detailed);
        }

        if (json_output) {
            std::printf("]\n");
        }

        // Aggregate for multiple files
        if (files.size() > 1 && !json_output) {
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> duration =
                end_time - start_time;

            std::printf("==========================================\n");
            std::printf("Aggregate Detailed Summary (%zu files)\n",
                        files.size());
            std::printf("==========================================\n");
            std::printf("  Total Events Scanned: %llu\n",
                        (unsigned long long)aggregate_detailed.events_scanned);
            std::printf("  Total Chunks Scanned: %llu\n",
                        (unsigned long long)aggregate_detailed.chunks_scanned);
            std::printf("  Total Chunks Skipped: %llu\n",
                        (unsigned long long)aggregate_detailed.chunks_skipped);

            if (aggregate_detailed.duration.count() > 0 &&
                !aggregate_detailed.duration.sketch.empty()) {
                std::printf("\n  Duration Percentiles:\n");
                std::printf("    p50: %.1f us  p90: %.1f us  p99: %.1f us\n",
                            aggregate_detailed.duration.sketch.quantile(0.5),
                            aggregate_detailed.duration.sketch.quantile(0.9),
                            aggregate_detailed.duration.sketch.quantile(0.99));
            }

            // Show global I/O summary if present
            auto global_io_it =
                aggregate_detailed.grouped_io.find("__global__");
            if (global_io_it != aggregate_detailed.grouped_io.end() &&
                global_io_it->second.size.count() > 0) {
                std::printf("\n  I/O Size:\n");
                std::printf(
                    "    Count: %llu   Mean: %s\n",
                    (unsigned long long)global_io_it->second.size.count(),
                    format_bytes(global_io_it->second.size.mean()).c_str());
            }

            std::printf("  Processing Time: %.2f ms\n", duration.count());
            std::printf("==========================================\n");
        }

        co_return 0;
    }

    // Aggregate statistics per file
    StatisticsAggregatorUtility aggregator;
    StatisticsQueryUtility query_util;

    std::vector<TraceStatistics> all_stats;
    all_stats.reserve(files.size());

    for (const auto& file_path : files) {
        StatisticsAggregatorInput agg_input;
        agg_input.file_path = file_path;
        agg_input.index_dir = index_dir;

        all_stats.push_back(co_await aggregator.process(agg_input));
    }

    // Query and output per file
    if (json_output) {
        std::printf("[\n");
    }

    for (std::size_t i = 0; i < all_stats.size(); ++i) {
        const auto& stats = all_stats[i];

        if (!stats.success) {
            if (json_output) {
                std::printf("%s%s", stats.to_json().c_str(),
                            i + 1 < all_stats.size() ? ",\n" : "\n");
            } else {
                print_text_summary(stats, top_n);
            }
            continue;
        }

        StatisticsQueryInput qi;
        qi.stats = stats;
        qi.query_type = report_type;
        qi.top_n = top_n;

        auto output = co_await query_util.process(qi);

        if (json_output) {
            std::printf("%s%s", output.to_json().c_str(),
                        i + 1 < all_stats.size() ? ",\n" : "\n");
        } else if (report_type == StatisticsQueryType::SUMMARY) {
            print_text_summary(stats, top_n);
        } else {
            print_text_query(output, stats.file_path);
        }
    }

    if (json_output) {
        std::printf("]\n");
    }

    // Aggregate totals for multiple files
    if (files.size() > 1 && !json_output) {
        TraceStatistics total;
        total.success = true;
        std::size_t successful = 0;

        for (const auto& stats : all_stats) {
            if (stats.success) {
                total.merged.merge_from(stats.merged);
                total.num_chunks += stats.num_chunks;
                successful++;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration =
            end_time - start_time;

        std::printf("==========================================\n");
        std::printf("Aggregate Summary (%zu files)\n", files.size());
        std::printf("==========================================\n");
        std::printf("  Successful: %zu / %zu\n", successful, files.size());
        std::printf("  Total Chunks: %llu\n",
                    (unsigned long long)total.num_chunks);
        std::printf("  Total Events: %llu\n",
                    (unsigned long long)total.merged.total_events);
        std::printf("  Unique Categories: %zu\n",
                    total.merged.category_counts.size());
        std::printf("  Unique Names: %zu\n", total.merged.name_counts.size());
        std::printf("  Unique PID:TIDs: %zu\n",
                    total.merged.pid_tid_counts.size());

        if (total.merged.duration_count > 0) {
            std::printf("  Duration Mean: %.2f us\n",
                        total.merged.duration_mean());
            std::printf("  Duration Min: %llu us\n",
                        (unsigned long long)total.merged.duration_min_us);
            std::printf("  Duration Max: %llu us\n",
                        (unsigned long long)total.merged.duration_max_us);
        }

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
        "Display statistics for DFTracer trace files from pre-built bloom "
        "index (.bidx) databases. Auto-builds indices if missing. "
        "Zero-cost reads: only SQLite metadata, no decompression.");

    program.add_argument("--files")
        .help("Trace files to inspect (.pfw, .pfw.gz)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .default_value<std::string>("");

    program.add_argument("--index-dir")
        .help("Directory where .bidx index files are stored")
        .default_value<std::string>("");

    program.add_argument("--json").help("Output in JSON format").flag();

    program.add_argument("--report")
        .help(
            "Report type: summary, categories, names, pid_tids, time_range, "
            "duration, top-names, top-categories, detailed")
        .default_value<std::string>("summary");

    program.add_argument("--top-n")
        .help("Number of results for top-N queries (default: 10)")
        .scan<'d', std::uint64_t>()
        .default_value(static_cast<std::uint64_t>(10));

    program.add_argument("--no-auto-index")
        .help("Disable automatic bloom index building for files missing .bidx")
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
        .help(
            "Inline query for event filtering (e.g., "
            "cat=POSIX,name=read|write). Uses bloom pre-filtering + exact "
            "match for --report detailed.")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

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
