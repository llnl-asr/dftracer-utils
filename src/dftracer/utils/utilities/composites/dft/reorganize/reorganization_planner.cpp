#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/predicate_parser_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

using indexing::determine_manifest_index_path;
using indexing::ManifestIndexDatabase;
using indexing::PredicateMap;
using indexing::PredicateParserInput;
using indexing::PredicateParserUtility;
using indexing::queries::EventRangeResult;
using indexing::queries::MetadataLinesResult;
using indexing::queries::query_event_ranges_for_checkpoint;
using indexing::queries::query_metadata_lines_for_checkpoint;

// Check if an event (cat, name) matches a parsed predicate
// map. A predicate map has dimension -> values. An event
// matches if for every dimension in the map, the event's
// value for that dimension is in the values list. "cat"
// matches against cat, "name" matches against name.
bool matches_predicate(const std::string& cat, const std::string& name,
                       const PredicateMap& pred) {
    for (const auto& [dim, values] : pred) {
        const std::string* event_val = nullptr;
        if (dim == "cat") {
            event_val = &cat;
        } else if (dim == "name") {
            event_val = &name;
        } else {
            // Unknown dimension -- no match
            return false;
        }
        bool found = false;
        for (const auto& v : values) {
            if (*event_val == v) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

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
            g.predicate = spec.substr(colon + 1);
        }
        groups.push_back(std::move(g));
    }
    return groups;
}

ExtractionPlan ReorganizationPlannerUtility::process(
    const ReorganizationPlannerInput& input) {
    ExtractionPlan plan;
    plan.groups = input.groups;

    // Parse predicates for each group
    PredicateParserUtility parser;
    std::vector<PredicateMap> parsed_predicates;
    for (const auto& group : input.groups) {
        if (group.predicate.empty()) {
            parsed_predicates.emplace_back();
        } else {
            auto result = parser.process(
                PredicateParserInput().with_predicate_string(group.predicate));
            parsed_predicates.push_back(std::move(result.predicates));
        }
    }

    // Ensure "remainder" group exists if not already
    // specified
    bool has_remainder = false;
    for (const auto& g : input.groups) {
        if (g.predicate.empty()) {
            has_remainder = true;
            break;
        }
    }
    std::string remainder_name;
    if (!has_remainder) {
        remainder_name = "remainder";
        plan.groups.push_back(PredicateGroup{remainder_name, ""});
        parsed_predicates.emplace_back();
    } else {
        // Find the name of the group with empty predicate
        for (const auto& g : input.groups) {
            if (g.predicate.empty()) {
                remainder_name = g.name;
                break;
            }
        }
    }

    // Process each source file
    for (std::size_t fi = 0; fi < input.source_files.size(); ++fi) {
        const auto& file_path = input.source_files[fi];

        // Build .idx if needed
        IndexBuilderUtility idx_builder;
        auto idx_input = IndexBuildUtilityInput::from_file(file_path);
        if (!input.index_dir.empty()) {
            idx_input.with_index(
                internal::determine_index_path(file_path, input.index_dir));
        }
        if (input.checkpoint_size > 0) {
            idx_input.with_checkpoint_size(input.checkpoint_size);
        }
        auto idx_result = idx_builder.process(idx_input);
        if (!idx_result.success) {
            throw std::runtime_error("Failed to build index for: " + file_path);
        }

        // Collect metadata
        MetadataCollectorUtility metadata_collector;
        auto meta_input =
            MetadataCollectorUtilityInput::from_file(file_path).with_index(
                idx_result.idx_path);
        if (input.checkpoint_size > 0) {
            meta_input.with_checkpoint_size(input.checkpoint_size);
        }
        auto meta = metadata_collector.process(meta_input);
        if (!meta.success) {
            throw std::runtime_error("Failed to collect metadata for: " +
                                     file_path);
        }

        // Determine midx path
        std::string midx_path =
            determine_manifest_index_path(file_path, input.index_dir);

        // Effective checkpoint count: treat 0 as 1
        std::size_t eff_ckpts =
            meta.num_checkpoints > 0 ? meta.num_checkpoints : 1;

        SourceFileInfo sfi;
        sfi.file_path = file_path;
        sfi.idx_path = idx_result.idx_path;
        sfi.midx_path = midx_path;
        sfi.num_checkpoints = eff_ckpts;
        sfi.uncompressed_size = meta.uncompressed_size;
        sfi.checkpoint_size = meta.checkpoint_size;
        plan.source_files.push_back(std::move(sfi));

        // Open .midx
        ManifestIndexDatabase midx_db(midx_path);
        int file_info_id = midx_db.get_file_info_id(file_path);
        if (file_info_id < 0) {
            throw std::runtime_error("File not found in .midx: " + file_path);
        }

        // Compute the bytes-per-chunk using the same formula as
        // manifest_index_builder: integer division of uncompressed
        // size by checkpoint count. The MIDX line_numbers are
        // 0-based within each such chunk, so the extraction byte
        // ranges must match exactly.
        //
        // Using meta.checkpoint_size (the nominal 32 MB target)
        // instead would produce different boundaries because the
        // gzip indexer places checkpoints at deflate block
        // boundaries, making the actual chunk count differ from
        // ceil(file_size / checkpoint_size).
        std::uint64_t bytes_per_ckpt =
            meta.uncompressed_size > 0 ? meta.uncompressed_size / eff_ckpts : 0;

        // For each checkpoint, build extraction tasks.
        // When num_checkpoints is 0 (file smaller than
        // checkpoint size), treat as a single checkpoint
        // at index 0 -- matching manifest_index_builder
        // behavior.
        for (std::size_t ckpt = 0; ckpt < eff_ckpts; ++ckpt) {
            // Compute byte range for this checkpoint using the
            // same formula as manifest_index_builder (line 100-104):
            //   start = i * bytes_per
            //   end   = (last chunk) ? file_size : (i+1) * bytes_per
            std::uint64_t start_byte = ckpt * bytes_per_ckpt;
            std::uint64_t end_byte = (ckpt + 1 == eff_ckpts)
                                         ? meta.uncompressed_size
                                         : (ckpt + 1) * bytes_per_ckpt;

            // Query event ranges for this checkpoint
            auto events = query_event_ranges_for_checkpoint(midx_db.db(),
                                                            file_info_id, ckpt);

            // Query metadata lines for this checkpoint
            auto metadata = query_metadata_lines_for_checkpoint(
                midx_db.db(), file_info_id, ckpt);

            // Collect metadata line numbers (go to ALL
            // groups)
            std::set<std::uint32_t> meta_lines;
            for (const auto& m : metadata) {
                meta_lines.insert(m.line_numbers.begin(), m.line_numbers.end());
            }

            // Route events to groups
            // Map: group_name -> set of line numbers
            std::map<std::string, std::set<std::uint32_t>> group_lines;

            for (const auto& ev : events) {
                bool matched = false;
                for (std::size_t gi = 0; gi < parsed_predicates.size(); ++gi) {
                    const auto& pred = parsed_predicates[gi];
                    // Skip remainder group
                    if (pred.empty()) {
                        continue;
                    }
                    if (matches_predicate(ev.cat, ev.name, pred)) {
                        group_lines[plan.groups[gi].name].insert(
                            ev.line_numbers.begin(), ev.line_numbers.end());
                        matched = true;
                        // First match wins
                        break;
                    }
                }
                if (!matched) {
                    group_lines[remainder_name].insert(ev.line_numbers.begin(),
                                                       ev.line_numbers.end());
                }
                plan.total_events += ev.event_count;
            }

            // Add metadata lines to ALL groups that have
            // any event lines for this checkpoint
            for (auto& [gname, lines] : group_lines) {
                lines.insert(meta_lines.begin(), meta_lines.end());
            }

            // Create ExtractionTasks
            for (auto& [gname, lines] : group_lines) {
                if (lines.empty()) {
                    continue;
                }
                ExtractionTask task;
                task.source_file_idx = fi;
                task.checkpoint_idx = ckpt;
                task.target_group = gname;
                task.line_numbers.assign(lines.begin(), lines.end());
                // Already sorted since std::set
                task.start_byte = start_byte;
                task.end_byte = end_byte;
                plan.tasks.push_back(std::move(task));
            }
        }
    }

    return plan;
}

}  // namespace
   // dftracer::utils::utilities::composites::dft::reorganize
