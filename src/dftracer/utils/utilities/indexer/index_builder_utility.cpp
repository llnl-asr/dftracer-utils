#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/indexer/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/indexer/visitors/manifest_visitor.h>

#include <chrono>
#include <optional>

namespace dftracer::utils::utilities::indexer {

using composites::dft::internal::determine_index_path;
using internal::IndexerFactory;

// ---------------------------------------------------------------------------
// IndexBuildConfig builder methods
// ---------------------------------------------------------------------------

IndexBuildConfig IndexBuildConfig::for_file(const std::string& path) {
    IndexBuildConfig cfg;
    cfg.file_path = path;
    return cfg;
}

IndexBuildConfig& IndexBuildConfig::with_index_dir(const std::string& dir) {
    index_dir = dir;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_checkpoint_size(std::size_t size) {
    checkpoint_size = size;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_index_threshold(
    std::size_t threshold) {
    index_threshold = threshold;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_force_rebuild(bool force) {
    force_rebuild = force;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_bloom(bool enable) {
    build_bloom = enable;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_manifest(bool enable) {
    build_manifest = enable;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_bloom_config(
    const composites::dft::indexing::ChunkIndexerConfig& config) {
    bloom_config = config;
    return *this;
}

IndexBuildConfig& IndexBuildConfig::with_bloom_dimensions(
    std::vector<std::string> dims) {
    bloom_dimensions = std::move(dims);
    return *this;
}

coro::CoroTask<IndexBuildResult> IndexBuilderUtility::process(
    const IndexBuildConfig& config) {
    IndexBuildResult result;
    result.file_path = config.file_path;

    try {
        std::string idx_path =
            determine_index_path(config.file_path, config.index_dir);
        result.idx_path = idx_path;

        // Check compressed file size against threshold (0 = always index).
        std::uintmax_t file_sz = 0;
        if (fs::exists(config.file_path)) {
            file_sz = fs::file_size(config.file_path);
        }
        const bool below_threshold =
            config.index_threshold != 0 && file_sz < config.index_threshold;

        auto build_start = std::chrono::steady_clock::now();

        auto indexer = IndexerFactory::create(
            config.file_path, idx_path,
            static_cast<std::uint64_t>(config.checkpoint_size),
            config.force_rebuild);

        if (!indexer) {
            result.error_message =
                "Unsupported archive format: " + config.file_path;
            co_return result;
        }

        // NOTE(perf): compute need_rebuild once: used for both skip decision
        // and checkpoints_valid below. Avoids duplicate fingerprint check.
        bool idx_exists = !below_threshold && indexer->exists();
        bool needs_rebuild = idx_exists ? indexer->need_rebuild() : true;

        // Skip if index exists, is current, and all requested features present.
        if (idx_exists && !config.force_rebuild && !needs_rebuild) {
            auto logical = internal::get_logical_path(config.file_path);
            bool bloom_ok = !config.build_bloom || [&] {
                try {
                    IndexDatabase db(idx_path);
                    int fid = db.get_file_info_id(logical);
                    return fid >= 0 && db.has_bloom_data(fid);
                } catch (...) {
                    return false;
                }
            }();
            bool manifest_ok = !config.build_manifest || [&] {
                try {
                    IndexDatabase db(idx_path);
                    int fid = db.get_file_info_id(logical);
                    return fid >= 0 && db.has_manifest_data(fid);
                } catch (...) {
                    return false;
                }
            }();

            if (bloom_ok && manifest_ok) {
                DFTRACER_UTILS_LOG_INFO("Skipping already-indexed file: %s",
                                        config.file_path.c_str());
                result.success = true;
                result.was_skipped = true;
                result.index_created = true;
                co_return result;
            }
        }

        // Resolve effective bloom dimensions.
        std::vector<std::string> dims =
            (config.build_bloom && config.bloom_dimensions.empty())
                ? default_bloom_dimensions()
                : config.bloom_dimensions;

        // Construct visitors.
        std::optional<BloomVisitor> bloom_visitor;
        std::optional<ManifestVisitor> manifest_visitor;

        internal::Indexer::VisitorList visitor_list;
        if (config.build_bloom) {
            bloom_visitor.emplace(config.bloom_config, dims);
            visitor_list.emplace_back(*bloom_visitor);
        }
        if (config.build_manifest) {
            manifest_visitor.emplace();
            visitor_list.emplace_back(*manifest_visitor);
        }

        // Decide whether checkpoints need rebuilding.
        // Reuses the need_rebuild result computed above.
        bool checkpoints_valid =
            !config.force_rebuild && idx_exists && !needs_rebuild;

        if (checkpoints_valid && !visitor_list.empty()) {
            // Checkpoints exist — only need a streaming pass for visitors.
            using fileio::lines::sources::async_streaming_gz_lines;
            for (auto& v : visitor_list) {
                v.get().begin(0);
            }
            std::size_t ckpt_idx = 0;
            std::size_t cumulative_bytes = 0;
            std::size_t bytes_per_ckpt =
                config.checkpoint_size > 0 ? config.checkpoint_size : 1;
            auto gen = async_streaming_gz_lines(config.file_path);
            while (auto line_opt = co_await gen.next()) {
                const auto& line = *line_opt;
                cumulative_bytes += line.content.length() + 1;
                std::size_t new_ckpt = cumulative_bytes / bytes_per_ckpt;
                if (new_ckpt != ckpt_idx) {
                    for (auto& v : visitor_list) {
                        v.get().on_checkpoint(new_ckpt);
                    }
                    ckpt_idx = new_ckpt;
                }
                for (auto& v : visitor_list) {
                    v.get().on_line(line.content, ckpt_idx);
                }
            }
        } else {
            // Need full checkpoint build — visitors run inline.
            if (!visitor_list.empty()) {
                indexer->set_visitors(std::move(visitor_list));
            }
            co_await indexer->build_async();
        }

        result.total_lines = indexer->get_num_lines();
        result.chunks_processed =
            static_cast<std::size_t>(indexer->get_checkpoints().size());

        // Persist visitor data into the .idx database only when the file meets
        // the size threshold (or threshold is disabled).
        if (!below_threshold && (config.build_bloom || config.build_manifest)) {
            const std::string& built_idx = indexer->get_idx_path();

            IndexDatabase db(built_idx);

            auto logical = internal::get_logical_path(config.file_path);
            int fid = db.get_file_info_id(logical);
            if (fid < 0) {
                result.error_message =
                    "File not found in index after build: " + logical;
                co_return result;
            }

            db.begin_transaction();
            try {
                if (config.build_bloom && bloom_visitor) {
                    db.init_bloom_schema();
                    bloom_visitor->finalize(db, fid);
                }
                if (config.build_manifest && manifest_visitor) {
                    db.init_manifest_schema();
                    manifest_visitor->finalize(db, fid);
                }
                db.commit_transaction();
            } catch (const std::exception& e) {
                result.error_message =
                    std::string("Failed to persist index data: ") + e.what();
                DFTRACER_UTILS_LOG_ERROR(
                    "IndexBuilder finalize failed for %s: %s",
                    config.file_path.c_str(), e.what());
                co_return result;
            }
        }

        result.index_created = !below_threshold;
        result.success = true;

        auto build_end = std::chrono::steady_clock::now();
        double elapsed_s =
            std::chrono::duration<double>(build_end - build_start).count();
        DFTRACER_UTILS_LOG_INFO(
            "Built index for %s (%zu chunks, %zu lines, %.2fs)",
            config.file_path.c_str(), result.chunks_processed,
            result.total_lines, elapsed_s);
    } catch (const std::exception& e) {
        result.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR("IndexBuilder failed for %s: %s",
                                 config.file_path.c_str(), e.what());
    }

    co_return result;
}

}  // namespace dftracer::utils::utilities::indexer
