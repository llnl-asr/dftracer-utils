#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_dispatcher.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/hash_table_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>

#include <chrono>
#include <optional>

namespace dftracer::utils::utilities::indexer {

using composites::dft::internal::determine_index_path;
using composites::dft::visitors::BloomVisitor;
using composites::dft::visitors::HashTableVisitor;
using composites::dft::visitors::ManifestVisitor;
using internal::IndexerFactory;

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

IndexBuildConfig& IndexBuildConfig::with_force_rebuild(bool force) {
    force_rebuild = force;
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

static coro::CoroTask<IndexBuildResult> run_index_build(
    IndexBuildConfig config) {
    IndexBuildResult result;
    result.file_path = config.file_path;

    try {
        std::string index_path =
            determine_index_path(config.file_path, config.index_dir);
        result.index_path = index_path;

#if DFTRACER_UTILS_LOGGER_LEVEL_DEBUG
        auto build_start = std::chrono::steady_clock::now();
#endif

        auto indexer = IndexerFactory::create(
            config.file_path, index_path,
            static_cast<std::uint64_t>(config.checkpoint_size),
            config.force_rebuild);

        if (!indexer) {
            result.error_message =
                "Unsupported archive format: " + config.file_path;
            co_return result;
        }

        bool idx_exists = indexer->exists();
        bool needs_rebuild = idx_exists ? indexer->need_rebuild() : true;

        // Skip if index exists, is current, all features present,
        // and no extra visitors need to run.
        if (idx_exists && !config.force_rebuild && !needs_rebuild &&
            config.extra_dft_visitors.empty()) {
            auto logical = internal::get_logical_path(config.file_path);
            bool bloom_ok = [&] {
                try {
                    IndexDatabase db(index_path,
                                     dftracer::utils::rocksdb::RocksDatabase::
                                         OpenMode::ReadOnly);
                    int fid = db.get_file_info_id(logical);
                    return fid >= 0 && db.has_bloom_data(fid);
                } catch (...) {
                    return false;
                }
            }();
            bool manifest_ok = !config.build_manifest || [&] {
                try {
                    IndexDatabase db(index_path,
                                     dftracer::utils::rocksdb::RocksDatabase::
                                         OpenMode::ReadOnly);
                    int fid = db.get_file_info_id(logical);
                    return fid >= 0 && db.has_manifest_data(fid);
                } catch (...) {
                    return false;
                }
            }();

            if (bloom_ok && manifest_ok) {
                DFTRACER_UTILS_LOG_DEBUG("Skipping already-indexed file: %s",
                                         config.file_path.c_str());
                result.success = true;
                result.was_skipped = true;
                result.index_created = true;
                co_return result;
            }
        }

        std::vector<std::string> dims =
            config.bloom_dimensions.empty()
                ? std::vector<std::string>(DEFAULT_BLOOM_DIMENSIONS.begin(),
                                           DEFAULT_BLOOM_DIMENSIONS.end())
                : config.bloom_dimensions;

        // Construct DFT event visitors and wrap in single dispatcher.
        BloomVisitor bloom_visitor(config.bloom_config, dims);
        HashTableVisitor hash_table_visitor;
        std::optional<ManifestVisitor> manifest_visitor;

        composites::dft::DftEventDispatcher::VisitorList dft_visitors;
        dft_visitors.emplace_back(bloom_visitor);
        dft_visitors.emplace_back(hash_table_visitor);
        if (config.build_manifest) {
            manifest_visitor.emplace();
            dft_visitors.emplace_back(*manifest_visitor);
        }
        for (auto& extra : config.extra_dft_visitors) {
            dft_visitors.emplace_back(extra);
        }

        composites::dft::DftEventDispatcher dispatcher(std::move(dft_visitors));
        internal::Indexer::VisitorList visitor_list;
        visitor_list.emplace_back(dispatcher);

        // Decide whether checkpoints need rebuilding.
        // Reuses the need_rebuild result computed above.
        bool checkpoints_valid =
            !config.force_rebuild && idx_exists && !needs_rebuild;

        if (checkpoints_valid && !visitor_list.empty()) {
            // Checkpoints exist, only need a streaming pass for visitors.
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
                auto buffer = std::make_shared<std::string>(line.content);
                std::string_view sv(buffer->data(), buffer->size());
                for (auto& v : visitor_list) {
                    v.get().on_line(sv, buffer, ckpt_idx);
                    if (v.get().wants_drain()) {
                        co_await v.get().drain_pending();
                    }
                }
            }
        } else {
            // Need full checkpoint build, visitors run inline.
            if (!visitor_list.empty()) {
                indexer->set_visitors(std::move(visitor_list));
            }
            co_await indexer->build_async();
        }

        result.total_lines = indexer->get_num_lines();
        result.chunks_processed =
            static_cast<std::size_t>(indexer->get_checkpoints().size());
        result.events_processed =
            static_cast<std::size_t>(bloom_visitor.total_events());

        {
            const std::string& built_index_path = indexer->get_index_path();

            try {
                IndexDatabase db(built_index_path);
                auto logical = internal::get_logical_path(config.file_path);
                const auto hash =
                    internal::calculate_file_hash(config.file_path);
                const auto mtime = static_cast<std::uint64_t>(
                    internal::get_file_modification_time(config.file_path));
                const auto bytes = internal::file_size_bytes(config.file_path);

                IndexFileEntryCapability caps =
                    IndexFileEntryCapability::INDEXING_COMPLETE |
                    IndexFileEntryCapability::BLOOM |
                    IndexFileEntryCapability::CHECKPOINTS |
                    IndexFileEntryCapability::FILE_SUMMARY;
                if (config.build_manifest && manifest_visitor) {
                    caps |= IndexFileEntryCapability::MANIFEST;
                }
                auto writer = db.begin_write();
                int fid = writer->get_or_create_file_info(logical, hash, caps,
                                                          mtime, bytes);
                writer->delete_chunk_statistics(fid);
                bloom_visitor.finalize(*writer, fid);
                hash_table_visitor.finalize(*writer, fid);
                if (config.build_manifest && manifest_visitor) {
                    manifest_visitor->finalize(*writer, fid);
                }
                writer->commit();
            } catch (const std::exception& e) {
                result.error_message =
                    std::string("Failed to persist index data: ") + e.what();
                DFTRACER_UTILS_LOG_ERROR(
                    "IndexBuilder finalize failed for %s: %s",
                    config.file_path.c_str(), e.what());
                co_return result;
            }
        }

        result.index_created = true;
        result.success = true;

#if DFTRACER_UTILS_LOGGER_LEVEL_DEBUG
        auto build_end = std::chrono::steady_clock::now();
        double elapsed_s =
            std::chrono::duration<double>(build_end - build_start).count();
        DFTRACER_UTILS_LOG_DEBUG(
            "Built index for %s (%zu chunks, %zu lines, %.2fs)",
            config.file_path.c_str(), result.chunks_processed,
            result.total_lines, elapsed_s);
#endif
    } catch (const std::exception& e) {
        result.error_message = e.what();
        DFTRACER_UTILS_LOG_ERROR("IndexBuilder failed for %s: %s",
                                 config.file_path.c_str(), e.what());
    }

    co_return result;
}

coro::CoroTask<IndexBuildResult> IndexBuilderUtility::process(
    const IndexBuildConfig& config) {
    DFTRACER_UTILS_TRACE_SCOPE("build index");
    return run_index_build(config);
}

}  // namespace dftracer::utils::utilities::indexer
