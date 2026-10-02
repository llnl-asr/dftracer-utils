#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/async_mutex.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cinttypes>
#include <memory>
#include <mutex>

namespace dftracer::utils::trace {

namespace hash = dftracer::utils::utilities::hash;
using namespace utilities::fileio::lines;

namespace {

// What this process knows of the index at one root, by canonical path: the lock
// that serializes building it, and the files it built or found current, by
// modification time and size. Held under the lock.
struct IndexedFile {
    std::pair<std::int64_t, std::uint64_t> stamp;
    std::size_t num_lines = 0;
    std::uint64_t max_bytes = 0;
    std::uint64_t checkpoint_size = 0;
    std::size_t num_checkpoints = 0;
};

struct IndexRoot {
    coro::AsyncMutex building;
    StringViewMap<IndexedFile> current;
};

std::shared_ptr<IndexRoot> index_root(const std::string& index_path) {
    static std::mutex guard;
    static StringViewMap<std::shared_ptr<IndexRoot>> roots;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(index_path, ec);
    const std::lock_guard<std::mutex> lock(guard);
    auto& r = roots[ec ? index_path : canonical.string()];
    if (!r) r = std::make_shared<IndexRoot>();
    return r;
}

}  // namespace

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::operator()(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.success = false;

    try {
        const std::string& file_path = input.file_path;
        if (file_path.size() <= 3 ||
            file_path.substr(file_path.size() - 3) != ".gz") {
            throw DFTUtilsException(
                ErrorCode::INVALID_ARGUMENT,
                "Not a gzip trace: " + file_path +
                    " (dftracer traces must be gzip-compressed)");
        }

        {
            MetadataCollectorUtilityInput modified_input = input;
            if (modified_input.index_path.empty()) {
                modified_input.index_path =
                    internal::determine_index_path(file_path, "");
            } else {
                modified_input.index_path = dftracer::utils::index::store::
                    internal::normalize_index_root(modified_input.index_path);
            }
            meta.index_path = modified_input.index_path;
            co_return co_await process_compressed(modified_input);
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to collect metadata for %s: %s",
                                 input.file_path.c_str(), e.what());
        meta.error_message = e.what();
        co_return meta;
    }
}

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::process_compressed(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.index_path = input.index_path;

    try {
        meta.format = dftracer::utils::index::gzip::CheckpointIndexerFactory::
            detect_format(input.file_path);
        meta.compressed_size = fs::file_size(input.file_path);

        // Scans of one file that start together would each build or
        // rebuild its index and remove the other's files.
        const std::shared_ptr<IndexRoot> root = index_root(input.index_path);
        std::size_t total_lines = 0;
        {
            co_await root->building.lock();
            const coro::AsyncMutexGuard held(root->building);
            const std::pair<std::int64_t, std::uint64_t> stamp{
                static_cast<std::int64_t>(
                    index::store::internal::get_file_modification_time(
                        input.file_path)),
                index::store::internal::file_size_bytes(input.file_path)};
            // A scan writing to the root makes a read-only open fail, which
            // need_rebuild() takes for a stale index, so a file this process
            // built or read is not opened again.
            std::error_code ec;
            const fs::path canonical =
                fs::weakly_canonical(input.file_path, ec);
            const std::string file_key =
                ec ? input.file_path : canonical.string();
            if (const auto seen = root->current.find(file_key);
                seen != root->current.end() && seen->second.stamp == stamp) {
                meta.has_index = true;
                total_lines = seen->second.num_lines;
                meta.uncompressed_size = seen->second.max_bytes;
                meta.checkpoint_size = seen->second.checkpoint_size;
                meta.num_checkpoints = seen->second.num_checkpoints;
            } else {
                meta.has_index = fs::exists(input.index_path);
                std::shared_ptr<dftracer::utils::index::gzip::CheckpointIndexer>
                    indexer;
                if (!meta.has_index || input.force_rebuild) {
                    if (input.force_rebuild && meta.has_index) {
                        DFTRACER_UTILS_LOG_DEBUG("Removing existing index: %s",
                                                 input.index_path.c_str());
                        fs::remove_all(input.index_path);
                    }
                    DFTRACER_UTILS_LOG_DEBUG("Building index for: %s",
                                             input.file_path.c_str());
                    indexer =
                        dftracer::utils::index::gzip::CheckpointIndexerFactory::
                            create(input.file_path, input.index_path,
                                   input.checkpoint_size, true);
                    co_await indexer->build_async();
                    meta.has_index = true;
                } else {
                    indexer =
                        dftracer::utils::index::gzip::CheckpointIndexerFactory::
                            create(input.file_path, input.index_path,
                                   input.checkpoint_size, false);
                }
                total_lines = indexer->get_num_lines();
                meta.uncompressed_size = indexer->get_max_bytes();
                meta.checkpoint_size = indexer->get_checkpoint_size();
                meta.num_checkpoints = indexer->get_members().size();
                root->current[file_key] =
                    IndexedFile{stamp, total_lines, meta.uncompressed_size,
                                meta.checkpoint_size, meta.num_checkpoints};
            }
            meta.index_valid = true;
            meta.num_lines = total_lines;
        }

        if (total_lines == 0) {
            DFTRACER_UTILS_LOG_DEBUG("File %s has no lines",
                                     input.file_path.c_str());
            meta.success = true;
            co_return meta;
        }

        std::size_t file_size_bytes = fs::file_size(input.file_path);
        double size_mb =
            static_cast<double>(file_size_bytes) / (1024.0 * 1024.0);

        meta.valid_events = (total_lines > 2) ? (total_lines - 2) : 0;
        meta.event_hash = 0;

        if (input.compute_hash) {
            std::size_t content_hash = 0;
            std::size_t actual_valid_events = 0;
            utilities::hash::HasherUtility hasher;
            {
                auto line_gen = StreamingLineReader::read_async(
                    StreamingLineReaderConfig()
                        .with_file(input.file_path)
                        .with_index(input.index_path)
                        .with_line_range(1, total_lines));
                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json::trim_and_validate(line.content.data(),
                                                line.content.length(), trimmed,
                                                trimmed_length) &&
                        trimmed_length > 8) {
                        hasher.reset();
                        hasher.update(
                            std::string_view(trimmed, trimmed_length));
                        content_hash += hasher.get_hash().value;
                        actual_valid_events++;
                    }
                }
            }
            meta.valid_events = actual_valid_events;
            meta.event_hash = content_hash;
        }

        meta.size_mb = size_mb;
        meta.start_line = 1;
        meta.end_line = total_lines;
        meta.size_per_line =
            (meta.valid_events > 0)
                ? size_mb / static_cast<double>(meta.valid_events)
                : 0;
        meta.success = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "File %s: %.2f MB, %zu valid events from %zu lines, %.8f MB/event",
            input.file_path.c_str(), meta.size_mb, meta.valid_events,
            total_lines, meta.size_per_line);

    } catch (const std::exception& e) {
        meta.error_message = e.what();
        meta.success = false;
    }

    co_return meta;
}

}  // namespace dftracer::utils::trace
