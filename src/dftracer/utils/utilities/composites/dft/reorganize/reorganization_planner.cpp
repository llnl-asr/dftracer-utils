#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

using common::query::Query;
using dftracer::utils::utilities::indexer::IndexBuildConfig;
using dftracer::utils::utilities::indexer::IndexBuilderUtility;
using dftracer::utils::utilities::indexer::IndexDatabase;
using fileio::lines::sources::async_streaming_gz_lines;

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
    ExtractionPlan plan;
    plan.groups = input.groups;

    std::vector<std::optional<Query>> parsed_queries;
    for (const auto& group : input.groups) {
        if (group.query.empty()) {
            parsed_queries.emplace_back(std::nullopt);
        } else {
            auto result = Query::from_string(group.query);
            if (!result) {
                throw std::runtime_error("Invalid query for group '" +
                                         group.name +
                                         "': " + result.error().format());
            }
            parsed_queries.push_back(std::move(*result));
        }
    }

    // Ensure "remainder" group exists if not already
    // specified
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

    // Process each source file
    for (std::size_t fi = 0; fi < input.source_files.size(); ++fi) {
        const auto& file_path = input.source_files[fi];

        // Build the shared `.dftindex` store if needed.
        IndexBuilderUtility idx_builder;
        auto idx_input = IndexBuildConfig::for_file(file_path).with_index_dir(
            input.index_dir);
        if (input.checkpoint_size > 0) {
            idx_input.with_checkpoint_size(input.checkpoint_size);
        }
        auto idx_result = co_await idx_builder.process(idx_input);
        if (!idx_result.success) {
            throw std::runtime_error("Failed to build index for: " + file_path);
        }

        // Collect metadata
        MetadataCollectorUtility metadata_collector;
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(file_path).with_index(
                idx_result.index_path);
        if (input.checkpoint_size > 0) {
            meta_input.with_checkpoint_size(input.checkpoint_size);
        }
        auto meta = co_await metadata_collector.process(meta_input);
        if (!meta.success) {
            throw std::runtime_error("Failed to collect metadata for: " +
                                     file_path);
        }

        // Determine the root-local `.dftindex` store path.
        std::string index_path =
            internal::determine_index_path(file_path, input.index_dir);

        // Effective checkpoint count: treat 0 as 1
        std::size_t eff_ckpts =
            meta.num_checkpoints > 0 ? meta.num_checkpoints : 1;

        SourceFileInfo sfi;
        sfi.file_path = file_path;
        sfi.index_path = index_path;
        sfi.num_checkpoints = eff_ckpts;
        sfi.uncompressed_size = meta.uncompressed_size;
        sfi.checkpoint_size = meta.checkpoint_size;
        plan.source_files.push_back(std::move(sfi));

        // Open the shared index store and try manifest-based planning. Fall
        // back to whole-file streaming when manifest tables are absent (file
        // was below index_threshold).
        IndexDatabase idx_db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        int file_info_id = idx_db.get_file_info_id(
            indexer::internal::get_logical_path(file_path));
        if (file_info_id < 0) {
            throw std::runtime_error("File not found in .dftindex: " +
                                     file_path);
        }

        const bool has_manifest = idx_db.has_manifest_data(file_info_id);

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
            // the entire file.  Only enabled for files at or below
            // the default index threshold to avoid pathological
            // memory usage on large traces with corrupt/partial
            // indexes.
            auto file_size = fs::file_size(file_path);
            if (file_size > constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD) {
                throw std::runtime_error(
                    "Manifest tables missing for large file (>" +
                    std::to_string(
                        constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD) +
                    " bytes): " + file_path +
                    ". Re-index with index_threshold=0.");
            }

            std::map<std::string, std::vector<std::uint32_t>> group_lines;
            std::vector<std::uint32_t> meta_line_numbers;

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

                yyjson_doc* doc =
                    yyjson_read(begin, static_cast<size_t>(end - begin),
                                YYJSON_READ_NOFLAG);
                if (!doc) continue;

                yyjson_val* root = yyjson_doc_get_root(doc);
                if (!root || !yyjson_is_obj(root)) {
                    yyjson_doc_free(doc);
                    continue;
                }

                auto line_num = static_cast<std::uint32_t>(line.line_number);
                yyjson_val* ph_val = yyjson_obj_get(root, "ph");
                const bool is_metadata =
                    ph_val && yyjson_is_str(ph_val) &&
                    std::string_view(yyjson_get_str(ph_val),
                                     yyjson_get_len(ph_val)) == "M";

                if (is_metadata) {
                    meta_line_numbers.push_back(line_num);
                    yyjson_doc_free(doc);
                    continue;
                }

                std::string cat_str;
                if (yyjson_val* cat_val = yyjson_obj_get(root, "cat");
                    cat_val && yyjson_is_str(cat_val)) {
                    cat_str.assign(yyjson_get_str(cat_val),
                                   yyjson_get_len(cat_val));
                }

                std::string name_str;
                if (yyjson_val* name_val = yyjson_obj_get(root, "name");
                    name_val && yyjson_is_str(name_val)) {
                    name_str.assign(yyjson_get_str(name_val),
                                    yyjson_get_len(name_val));
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

                yyjson_doc_free(doc);
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
