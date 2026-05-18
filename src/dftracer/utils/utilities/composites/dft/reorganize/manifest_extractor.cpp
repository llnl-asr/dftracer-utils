#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/common/query/ast.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/manifest_extractor.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <unordered_map>
#include <variant>

namespace dftracer::utils::utilities::composites::dft::reorganize {

namespace {

struct LineGroupMapping {
    std::unordered_map<std::size_t, std::size_t> line_to_group;
};

bool query_matches_cat_name(const common::query::QueryNode& root,
                            const std::string& cat, const std::string& name) {
    if (std::holds_alternative<common::query::CompareNode>(root.data)) {
        const auto& comp = std::get<common::query::CompareNode>(root.data);
        if (comp.op != common::query::CompareOp::EQ) return false;

        if (comp.field.path == "cat") {
            if (std::holds_alternative<std::string>(comp.value.value)) {
                return std::get<std::string>(comp.value.value) == cat;
            }
        } else if (comp.field.path == "name") {
            if (std::holds_alternative<std::string>(comp.value.value)) {
                return std::get<std::string>(comp.value.value) == name;
            }
        }
    }
    return false;
}

LineGroupMapping build_line_group_mapping(
    const std::vector<indexer::EventRangeResult>& event_ranges,
    const std::vector<PredicateGroup>& groups,
    const std::vector<std::optional<common::query::Query>>& parsed_queries) {
    LineGroupMapping mapping;

    for (const auto& range : event_ranges) {
        std::size_t target_group = SIZE_MAX;

        for (std::size_t g = 0; g < groups.size(); ++g) {
            const auto& query_opt = parsed_queries[g];
            if (!query_opt) {
                target_group = g;
                break;
            }

            if (query_matches_cat_name(query_opt->root(), range.cat,
                                       range.name)) {
                target_group = g;
                break;
            }
        }

        if (target_group != SIZE_MAX) {
            for (std::uint32_t line_num : range.line_numbers) {
                mapping.line_to_group[static_cast<std::size_t>(line_num)] =
                    target_group;
            }
        }
    }

    return mapping;
}

}  // namespace

coro::CoroTask<ManifestExtractorResult> extract_from_manifest(
    ManifestExtractorConfig config) {
    ManifestExtractorResult result;

    try {
        indexer::IndexDatabase db(
            config.index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

        int file_id = db.get_file_info_id(config.file_path);
        if (file_id < 0) {
            result.error_message =
                "File not found in index: " + config.file_path;
            co_return result;
        }

        if (!db.has_manifest_data(file_id)) {
            result.error_message =
                "No manifest data for file: " + config.file_path;
            co_return result;
        }

        auto event_ranges = db.query_event_ranges(file_id);

        std::vector<std::optional<common::query::Query>> parsed_queries;
        parsed_queries.reserve(config.groups.size());
        for (const auto& group : config.groups) {
            if (group.query.empty()) {
                parsed_queries.push_back(std::nullopt);
            } else {
                auto q = common::query::Query::from_string(group.query);
                if (q) {
                    parsed_queries.push_back(std::move(*q));
                } else {
                    parsed_queries.push_back(std::nullopt);
                }
            }
        }

        auto mapping = build_line_group_mapping(event_ranges, config.groups,
                                                parsed_queries);

        std::vector<LineBatch> pending_batches(config.groups.size());
        for (auto& batch : pending_batches) {
            batch.reserve(config.batch_size);
        }

        using fileio::lines::sources::async_streaming_gz_lines;
        std::size_t line_num = 0;
        auto gen = async_streaming_gz_lines(config.file_path);

        while (auto line_opt = co_await gen.next()) {
            const auto& line = *line_opt;

            auto it = mapping.line_to_group.find(line_num);
            if (it != mapping.line_to_group.end()) {
                std::size_t group_idx = it->second;
                auto& batch = pending_batches[group_idx];

                batch.append_line(line.content, config.source_file_idx,
                                  /*checkpoint_idx=*/0, line_num);

                result.events_extracted++;

                if (batch.size() >= config.batch_size) {
                    auto& channel = config.group_channels[group_idx];
                    if (channel) {
                        co_await channel->send(
                            std::make_shared<LineBatch>(std::move(batch)));
                    }
                    batch.clear();
                    batch.reserve(config.batch_size);
                }
            } else {
                result.events_unmatched++;
            }

            line_num++;
        }

        for (std::size_t i = 0; i < pending_batches.size(); ++i) {
            auto& batch = pending_batches[i];
            if (!batch.empty()) {
                auto& channel = config.group_channels[i];
                if (channel) {
                    co_await channel->send(
                        std::make_shared<LineBatch>(std::move(batch)));
                }
            }
        }

        result.success = true;

    } catch (const std::exception& e) {
        result.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR("ManifestExtractor failed for %s: %s",
                                 config.file_path.c_str(), e.what());
    }

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
