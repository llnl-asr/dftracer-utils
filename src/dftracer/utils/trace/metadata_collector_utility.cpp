// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/trace/internal/index_root.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cinttypes>
#include <mutex>

namespace dftracer::utils::trace {

using namespace utilities::fileio::lines;

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::operator()(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.success = false;

    try {
        const dstd::String& file_path = input.file_path;
        if (file_path.size() <= 3 ||
            file_path.substr(file_path.size() - 3) != ".gz") {
            throw common::DFTUtilsException(
                common::ErrorCode::InvalidArgument,
                "Not a gzip trace: " + file_path +
                    " (dftracer traces must be gzip-compressed)");
        }

        {
            MetadataCollectorUtilityInput modified_input = input;
            if (modified_input.index_path.empty()) {
                modified_input.index_path =
                    internal::determine_index_path(file_path, "");
            } else {
                modified_input.index_path =
                    dftracer::utils::index::normalize_index_root(
                        modified_input.index_path);
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

namespace {

// The index record of `file` when the index holds it unchanged since the stat
// the caller read, or a prefix of it. Only a file that grew is opened again.
dstd::Optional<internal::IndexedFile> fresh_index_record(
    const dstd::String& index_path, const dstd::String& file,
    const dftracer::utils::common::InputFileStat& st, bool& is_prefix) {
    try {
        auto snapshot = dftracer::utils::common::unwrap(
            index::plan::open_snapshot(index_path));
        const auto logical = index::store::internal::get_logical_path(file);
        const bool is_current =
            snapshot.freshness(
                logical, index::SourceStat{static_cast<std::uint64_t>(st.mtime),
                                           st.size}) ==
            index::Freshness::Current;
        const auto ref = snapshot.file(logical);
        if (!ref) return dstd::nullopt;
        // A clean append keeps its index, which stops at the verified end. A
        // full rebuild here would read the member still being written.
        if (!is_current &&
            !index::is_append_of(dstd::StringView(file), st.size, ref->stat()))
            return dstd::nullopt;
        const auto meta = dftracer::utils::common::unwrap(
            index::file_metadata(snapshot, *ref));
        if (!meta || meta->checkpoint_size == 0) return dstd::nullopt;
        if (!is_current && (meta->truncated || meta->has_open_line))
            return dstd::nullopt;
        is_prefix = !is_current;
        return internal::IndexedFile{
            {st.mtime, st.size},
            meta->line_count,
            meta->max_bytes,
            meta->checkpoint_size,
            dftracer::utils::common::unwrap(index::members(snapshot, *ref))
                .size()};
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_DEBUG("Index of %s unreadable (%s)", file.c_str(),
                                 e.what());
        return dstd::nullopt;
    }
}

}  // namespace

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::process_compressed(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.index_path = input.index_path;

    try {
        meta.format = dftracer::utils::index::gzip::CheckpointIndexerFactory::
            detect_format(input.file_path);
        dftracer::utils::common::InputFileStat input_stat;
        if (!dftracer::utils::common::stat_input(input.file_path, input_stat))
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Io, "cannot stat ", input.file_path);
        meta.compressed_size = input_stat.size;

        const dstd::SharedPtr<internal::IndexRoot> root =
            internal::index_root(input.index_path);
        const dstd::Pair<std::int64_t, std::uint64_t> stamp{input_stat.mtime,
                                                            input_stat.size};
        const dstd::String& file_key = input.file_path;

        using dftracer::utils::index::gzip::CheckpointIndexer;
        using dftracer::utils::index::gzip::CheckpointIndexerFactory;
        auto take = [&](const CheckpointIndexer& indexer) {
            meta.has_index = true;
            const internal::IndexedFile seen{
                stamp, indexer.line_count(), indexer.max_bytes(),
                indexer.checkpoint_size(), indexer.members().size()};
            std::lock_guard<std::mutex> lock(root->current_mutex);
            root->current[file_key] = seen;
            return seen;
        };
        auto known = [&]() -> dstd::Optional<internal::IndexedFile> {
            std::lock_guard<std::mutex> lock(root->current_mutex);
            const auto it = root->current.find(file_key);
            if (it == root->current.end() || it->second.stamp != stamp)
                return dstd::nullopt;
            meta.has_index = true;
            return it->second;
        };

        // A file this process built or read is not opened again.
        dstd::Optional<internal::IndexedFile> seen;
        if (!input.force_rebuild) seen = known();
        // A check that meets a writer reads the index as stale and takes the
        // slow path below, which decides again under the writer permits.
        if (!seen && !input.force_rebuild) {
            co_await root->gate.acquire(1);
            const internal::RootPermit reading(*root, 1);
            if (common::input_exists(
                    dftracer::utils::index::store_of(input.index_path))) {
                // A same-size fill of a hole keeps this stat, so a record of a
                // prefix is not kept past this read.
                bool is_prefix = false;
                seen = fresh_index_record(input.index_path, input.file_path,
                                          input_stat, is_prefix);
                if (seen && !is_prefix) {
                    std::lock_guard<std::mutex> lock(root->current_mutex);
                    root->current[file_key] = *seen;
                }
            }
        }
        if (!seen) {
            // Scans of one file that start together would each build or
            // rebuild its index and remove the other's files.
            co_await root->gate.acquire(internal::IndexRoot::ALL_PERMITS);
            const internal::RootPermit writing(
                *root, internal::IndexRoot::ALL_PERMITS);
            if (!input.force_rebuild) seen = known();
            if (!seen) {
                meta.has_index = common::input_exists(
                    dftracer::utils::index::store_of(input.index_path));
                dstd::SharedPtr<CheckpointIndexer> indexer;
                if (!meta.has_index || input.force_rebuild) {
                    if (input.force_rebuild && meta.has_index) {
                        DFTRACER_UTILS_LOG_DEBUG("Removing existing index: %s",
                                                 input.index_path.c_str());
                        dftracer::utils::index::release_root(input.index_path);
                        const dstd::String store =
                            dftracer::utils::index::store_of(input.index_path);
                        common::unwrap(
                            dstd::fs::remove_all(dstd::PathView(store)));
                        common::forget_input(store);
                    }
                    DFTRACER_UTILS_LOG_DEBUG("Building index for: %s",
                                             input.file_path.c_str());
                    indexer = CheckpointIndexerFactory::create(
                        input.file_path, input.index_path,
                        input.checkpoint_size, true);
                } else {
                    // The root can exist without this file in it, as when a
                    // concurrent first-touch bootstrap reserved the file id.
                    // The build returns at once for a fresh record.
                    indexer = CheckpointIndexerFactory::create(
                        input.file_path, input.index_path,
                        input.checkpoint_size, false);
                }
                co_await indexer->build_async();
                seen = take(*indexer);
            }
        }
        const std::size_t total_lines = seen->line_count;
        meta.uncompressed_size = seen->max_bytes;
        meta.checkpoint_size = seen->checkpoint_size;
        meta.checkpoint_count = seen->checkpoint_count;
        meta.index_valid = true;
        meta.line_count = total_lines;

        if (total_lines == 0) {
            DFTRACER_UTILS_LOG_DEBUG("File %s has no lines",
                                     input.file_path.c_str());
            meta.success = true;
            co_return meta;
        }

        std::size_t file_size_bytes = input_stat.size;
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
                for (;;) {
                    auto line_opt = co_await line_gen.next();
                    if (!line_opt) break;
                    const auto& line = *line_opt;
                    const char* trimmed;
                    std::size_t trimmed_size;
                    if (json::trim_and_validate(line.content.data(),
                                                line.content.length(), trimmed,
                                                trimmed_size) &&
                        trimmed_size > 8) {
                        hasher.reset();
                        hasher.update(dstd::StringView(trimmed, trimmed_size));
                        content_hash += hasher.hash().value;
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
