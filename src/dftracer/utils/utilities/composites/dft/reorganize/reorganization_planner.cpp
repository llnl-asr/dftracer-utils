#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <simdjson.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

using common::query::Query;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;
using dftracer::utils::utilities::indexer::IndexDatabase;
using fileio::lines::sources::async_streaming_gz_lines;
using indexing::IndexResolverUtility;
using indexing::ResolverInput;

}  // namespace

std::vector<PredicateGroup> parse_group_specs(
    const std::vector<std::string>& specs) {
    std::vector<PredicateGroup> groups;
    for (const auto& spec : specs) {
        PredicateGroup g;
        auto colon = spec.find(':');
        if (colon == std::string::npos) {
            g.name = spec;
        } else {
            g.name = spec.substr(0, colon);
            g.query = spec.substr(colon + 1);
        }
        groups.push_back(std::move(g));
    }
    return groups;
}

coro::CoroTask<ExtractionPlan> ReorganizationPlannerUtility::process(
    const ReorganizationPlannerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("plan reorganization");
    CoroScope& scope = context();

    ExtractionPlan plan;
    plan.groups = input.groups;

    std::vector<std::optional<Query>> parsed_queries;
    for (const auto& group : input.groups) {
        if (group.query.empty()) {
            parsed_queries.emplace_back(std::nullopt);
        } else {
            auto result = Query::from_string(group.query);
            if (!result) {
                throw DFTUtilsException(
                    ErrorCode::QUERY, "Invalid query for group '" + group.name +
                                          "': " + result.error().format());
            }
            parsed_queries.push_back(std::move(*result));
        }
    }

    bool has_remainder = false;
    for (const auto& g : input.groups) {
        if (g.query.empty()) {
            has_remainder = true;
            break;
        }
    }
    std::string remainder_name;
    if (!has_remainder) {
        remainder_name = "remainder";
        plan.groups.push_back(PredicateGroup{remainder_name, ""});
        parsed_queries.emplace_back(std::nullopt);
    } else {
        for (const auto& g : input.groups) {
            if (g.query.empty()) {
                remainder_name = g.name;
                break;
            }
        }
    }

    if (input.source_files.empty()) {
        co_return plan;
    }

    // Use IndexResolverUtility to scan files and check capabilities
    IndexResolverUtility resolver;
    ResolverInput resolver_input;
    resolver_input.files = input.source_files;
    resolver_input.index_dir = input.index_dir;
    resolver_input.require_checkpoints = true;
    resolver_input.require_manifest = true;
    auto scan_result = co_await scope.spawn(resolver, resolver_input);

    DFTRACER_UTILS_LOG_INFO(
        "ReorganizationPlanner: %zu files, %zu cached, %zu need checkpoint, "
        "%zu need manifest",
        scan_result.all_files.size(), scan_result.cached.size(),
        scan_result.needs_checkpoint.size(), scan_result.needs_manifest.size());

    // Build indices in parallel for files needing work
    std::vector<std::string> files_needing_index;
    for (const auto& item : scan_result.needs_checkpoint) {
        files_needing_index.push_back(item.file_path);
    }
    for (const auto& item : scan_result.needs_manifest) {
        files_needing_index.push_back(item.file_path);
    }

    if (!files_needing_index.empty()) {
        auto batch_config = std::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths = std::move(files_needing_index);
        batch_config->index_dir = input.index_dir;
        if (input.checkpoint_size > 0) {
            batch_config->checkpoint_size = input.checkpoint_size;
        }
        batch_config->build_manifest = true;
        batch_config->use_batch_write = true;

        auto batch_result =
            co_await IndexBatchBuilderUtility::process(&scope, batch_config);

        for (const auto& result : batch_result.results) {
            if (!result.success) {
                throw DFTUtilsException(
                    ErrorCode::INDEXER,
                    "Failed to build index for: " + result.file_path + ": " +
                        result.error_message);
            }
        }
    }

    // Collect metadata in parallel using when_all
    // Store inputs in vector to ensure lifetime across co_await
    std::vector<MetadataCollectorUtilityInput> meta_inputs;
    meta_inputs.reserve(input.source_files.size());
    for (const auto& file_path : input.source_files) {
        auto index_path =
            internal::determine_index_path(file_path, input.index_dir);
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(file_path).with_index(
                index_path);
        if (input.checkpoint_size > 0) {
            meta_input.with_checkpoint_size(input.checkpoint_size);
        }
        meta_inputs.push_back(std::move(meta_input));
    }

    std::vector<coro::CoroTask<MetadataCollectorUtilityOutput>> metadata_tasks;
    metadata_tasks.reserve(meta_inputs.size());
    for (const auto& meta_input : meta_inputs) {
        MetadataCollectorUtility collector;
        metadata_tasks.push_back(collector.process(meta_input));
    }

    auto metadata_results = co_await coro::when_all(std::move(metadata_tasks));

    // Build source file info from metadata results (same order as input)
    plan.source_files.reserve(input.source_files.size());
    for (std::size_t fi = 0; fi < input.source_files.size(); ++fi) {
        const auto& file_path = input.source_files[fi];
        const auto& meta = metadata_results[fi];

        if (!meta.success) {
            throw DFTUtilsException(
                ErrorCode::IO, "Failed to collect metadata for: " + file_path);
        }

        std::string index_path =
            internal::determine_index_path(file_path, input.index_dir);
        std::size_t eff_ckpts =
            meta.num_checkpoints > 0 ? meta.num_checkpoints : 1;

        SourceFileInfo sfi;
        sfi.file_path = file_path;
        sfi.index_path = index_path;
        sfi.num_checkpoints = eff_ckpts;
        sfi.uncompressed_size = meta.uncompressed_size;
        sfi.checkpoint_size = meta.checkpoint_size;
        plan.source_files.push_back(std::move(sfi));
    }

    // Plan extraction tasks for each file
    for (std::size_t fi = 0; fi < input.source_files.size(); ++fi) {
        const auto& file_path = input.source_files[fi];
        const auto& meta = metadata_results[fi];
        const auto& sfi = plan.source_files[fi];

        IndexDatabase idx_db(
            sfi.index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        int file_info_id = idx_db.get_file_info_id(
            indexer::internal::get_logical_path(file_path));
        if (file_info_id < 0) {
            throw DFTUtilsException(
                ErrorCode::NOT_FOUND,
                "File not found in .dftindex: " + file_path);
        }

        const bool has_manifest = idx_db.has_manifest_data(file_info_id);
        const std::size_t eff_ckpts = sfi.num_checkpoints;

        if (has_manifest) {
            // Manifest-based planning: per-checkpoint extraction tasks.
            std::uint64_t bytes_per_ckpt =
                meta.uncompressed_size > 0 ? meta.uncompressed_size / eff_ckpts
                                           : 0;

            for (std::size_t ckpt = 0; ckpt < eff_ckpts; ++ckpt) {
                std::uint64_t start_byte = ckpt * bytes_per_ckpt;
                std::uint64_t end_byte = (ckpt + 1 == eff_ckpts)
                                             ? meta.uncompressed_size
                                             : (ckpt + 1) * bytes_per_ckpt;

                auto events = idx_db.query_event_ranges_for_checkpoint(
                    file_info_id, ckpt);
                auto metadata = idx_db.query_metadata_lines_for_checkpoint(
                    file_info_id, ckpt);

                std::set<std::uint32_t> meta_lines;
                for (const auto& m : metadata) {
                    meta_lines.insert(m.line_numbers.begin(),
                                      m.line_numbers.end());
                }

                std::map<std::string, std::set<std::uint32_t>> group_lines;

                for (const auto& ev : events) {
                    bool matched = false;
                    for (std::size_t gi = 0; gi < parsed_queries.size(); ++gi) {
                        const auto& q = parsed_queries[gi];
                        if (!q) continue;
                        common::query::ValueMap fields = {{"cat", ev.cat},
                                                          {"name", ev.name}};
                        if (q->evaluate(fields)) {
                            group_lines[plan.groups[gi].name].insert(
                                ev.line_numbers.begin(), ev.line_numbers.end());
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) {
                        group_lines[remainder_name].insert(
                            ev.line_numbers.begin(), ev.line_numbers.end());
                    }
                    plan.total_events += ev.event_count;
                }

                for (auto& [gname, lines] : group_lines) {
                    lines.insert(meta_lines.begin(), meta_lines.end());
                }

                for (auto& [gname, lines] : group_lines) {
                    if (lines.empty()) continue;
                    ExtractionTask task;
                    task.source_file_idx = fi;
                    task.checkpoint_idx = ckpt;
                    task.target_group = gname;
                    task.line_numbers.assign(lines.begin(), lines.end());
                    task.start_byte = start_byte;
                    task.end_byte = end_byte;
                    plan.tasks.push_back(std::move(task));
                }
            }
        } else {
            // Whole-file fallback: stream line-by-line, route each
            // event to a group, emit one task per group covering
            // the entire file.
            std::map<std::string, std::vector<std::uint32_t>> group_lines;
            std::vector<std::uint32_t> meta_line_numbers;

            simdjson::dom::parser parser;

            auto gen = async_streaming_gz_lines(file_path);
            while (auto line_opt = co_await gen.next()) {
                const auto& line = *line_opt;
                if (line.content.empty()) continue;

                const char* begin = line.content.data();
                const char* end = begin + line.content.size();
                while (begin < end &&
                       std::isspace(static_cast<unsigned char>(*begin))) {
                    ++begin;
                }
                while (end > begin &&
                       std::isspace(static_cast<unsigned char>(*(end - 1)))) {
                    --end;
                }
                if (begin == end || *begin != '{' || *(end - 1) != '}') {
                    continue;
                }

                auto result =
                    parser.parse(begin, static_cast<size_t>(end - begin));
                if (result.error()) continue;

                auto root = result.value_unsafe();
                if (!root.is_object()) continue;

                auto line_num = static_cast<std::uint32_t>(line.line_number);
                auto ph_result = root["ph"].get_string();
                const bool is_metadata =
                    !ph_result.error() && ph_result.value_unsafe() == "M";

                if (is_metadata) {
                    meta_line_numbers.push_back(line_num);
                    continue;
                }

                std::string cat_str;
                auto cat_result = root["cat"].get_string();
                if (!cat_result.error()) {
                    cat_str = std::string(cat_result.value_unsafe());
                }

                std::string name_str;
                auto name_result = root["name"].get_string();
                if (!name_result.error()) {
                    name_str = std::string(name_result.value_unsafe());
                }

                bool matched = false;
                for (std::size_t gi = 0; gi < parsed_queries.size(); ++gi) {
                    const auto& q = parsed_queries[gi];
                    if (!q) continue;
                    common::query::ValueMap fields = {{"cat", cat_str},
                                                      {"name", name_str}};
                    if (q->evaluate(fields)) {
                        group_lines[plan.groups[gi].name].push_back(line_num);
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    group_lines[remainder_name].push_back(line_num);
                }
                plan.total_events++;
            }

            for (auto& [gname, lines] : group_lines) {
                lines.insert(lines.end(), meta_line_numbers.begin(),
                             meta_line_numbers.end());
                std::sort(lines.begin(), lines.end());

                ExtractionTask task;
                task.source_file_idx = fi;
                task.checkpoint_idx = 0;
                task.target_group = gname;
                task.line_numbers = std::move(lines);
                task.start_byte = 0;
                task.end_byte = meta.uncompressed_size;
                plan.tasks.push_back(std::move(task));
            }
        }
    }

    co_return plan;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::reorganize
