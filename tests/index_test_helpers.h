// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
#define DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

// Helpers for tests that write synthetic index data by hand or read the index
// back through a snapshot.
namespace dftu_utils_test {

inline dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> dstd_paths(
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& paths) {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> out;
    out.reserve(paths.size());
    for (const auto& path : paths) out.emplace_back(path);
    return out;
}

template <class T>
T index_take(
    dftracer::utils::dstd::Expected<T, dftracer::utils::dstd::Error> result) {
    if (!result) throw std::runtime_error("index test: an index call failed");
    return std::move(*result);
}

inline dftracer::utils::trace::index::Index open_test_index(
    dftracer::utils::dstd::StringView index_path) {
    return index_take(dftracer::utils::trace::index::Index::open(
        dftracer::utils::trace::index::normalize_index_root(index_path)));
}

inline dftracer::utils::trace::index::Snapshot index_snapshot(
    dftracer::utils::dstd::StringView index_path) {
    return index_take(open_test_index(index_path).snapshot());
}

inline dftracer::utils::dstd::String index_key(
    dftracer::utils::dstd::StringView trace_path) {
    return dftracer::utils::fileio::get_logical_path(trace_path);
}

/// The file of `trace_path`, found under the key the indexer files it by.
inline dftracer::utils::dstd::Optional<dftracer::utils::trace::index::FileRef>
find_indexed_file(const dftracer::utils::trace::index::Snapshot& snapshot,
                  dftracer::utils::dstd::StringView trace_path) {
    const dftracer::utils::dstd::String key = index_key(trace_path);
    auto found = snapshot.file(dftracer::utils::dstd::StringView(key));
    if (!found) return dftracer::utils::dstd::nullopt;
    return std::move(*found);
}

inline dftracer::utils::trace::index::FileRef require_indexed_file(
    const dftracer::utils::trace::index::Snapshot& snapshot,
    dftracer::utils::dstd::StringView trace_path) {
    auto found = find_indexed_file(snapshot, trace_path);
    if (!found) throw std::runtime_error("index test: the file is not indexed");
    return std::move(*found);
}

/// One record of the store under an index root.
struct IndexRecord {
    dftracer::utils::dstd::String name;
    dftracer::utils::dstd::String scope;
    dftracer::utils::dstd::String key;
    dftracer::utils::dstd::String value;
    bool operator==(const IndexRecord&) const = default;
    auto operator<=>(const IndexRecord&) const = default;
};

/// Every live record of every namespace of the index at `index_path`, in
/// namespace, scope and key order. `scope` joins the scope paths with NUL.
inline dftracer::utils::dstd::Vector<IndexRecord> dump_index(
    dftracer::utils::dstd::StringView index_path) {
    auto store = index_take(dftracer::utils::store::Store::open(
        dftracer::utils::trace::index::normalize_index_root(index_path) +
        "/store"));
    auto snapshot = index_take(store.snapshot());
    auto declared = index_take(snapshot.namespaces());
    dftracer::utils::dstd::Vector<IndexRecord> out;
    for (const auto& decl : declared) {
        dftracer::utils::store::Namespace ns;
        ns.spec.name = decl.name;
        ns.spec.format_version = decl.format_version;
        auto cursor = snapshot.scan_all(ns, {}, {});
        for (; !cursor.at_end(); cursor.next()) {
            IndexRecord record;
            record.name.assign(decl.name.data(), decl.name.size());
            for (const auto& path : cursor.scope().paths) {
                record.scope.append(path.data(), path.size());
                record.scope.push_back('\0');
            }
            const auto key = cursor.key();
            record.key.assign(key.as<char>(), key.size());
            const auto value = index_take(cursor.value());
            record.value.assign(value.as<char>(), value.size());
            out.push_back(std::move(record));
        }
        if (cursor.error())
            throw std::runtime_error("index test: a scan failed");
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The members the index at `index_path` holds for `trace_path`.
inline dftracer::utils::dstd::Vector<
    dftracer::utils::trace::index::gzip::GzipMemberRecord>
indexed_members(dftracer::utils::dstd::StringView index_path,
                dftracer::utils::dstd::StringView trace_path) {
    const auto snapshot = index_snapshot(index_path);
    return index_take(dftracer::utils::trace::index::members(
        snapshot, require_indexed_file(snapshot, trace_path)));
}

inline void commit_test_build(dftracer::utils::trace::index::FileBuild& build) {
    auto done = dftracer::utils::default_runtime().submit(build.commit()).get();
    if (!done) throw std::runtime_error("index test: a commit failed");
}

/// Starts the data of `logical_path`: the file metadata and the Members copy
/// that holds the file's stat. Commit it with commit_test_build.
inline dftracer::utils::trace::index::FileBuild register_test_file(
    const dftracer::utils::trace::index::Index& index,
    dftracer::utils::dstd::StringView logical_path, std::uint64_t hash = 0,
    std::uint64_t mtime = 0, std::uint64_t size = 0) {
    dftracer::utils::trace::index::SourceStat stat;
    stat.mtime = mtime;
    stat.size = size;
    stat.hash = hash;
    auto build =
        index_take(index.build(dftracer::utils::dstd::StringView(
                                   logical_path.data(), logical_path.size()),
                               stat));
    dftracer::utils::trace::index::put_file_metadata(build, 0, 0, 0, false);
    build.finish(dftracer::utils::trace::index::store::IndexExtension::Members,
                 0);
    return build;
}

/// Starts a rebuild of an indexed file, with the stat it was indexed with.
inline dftracer::utils::trace::index::FileBuild rebuild_test_file(
    const dftracer::utils::trace::index::Index& index,
    dftracer::utils::dstd::StringView trace_path) {
    const dftracer::utils::dstd::String key = index_key(trace_path);
    return index_take(dftracer::utils::trace::index::rebuild(
        index, dftracer::utils::dstd::StringView(key)));
}

/// Finishes the pruning extensions the build did not finish, so readers use
/// the data a test wrote. Members is finished too unless the build did.
inline void mark_built(dftracer::utils::trace::index::FileBuild& build) {
    for (auto ext :
         {dftracer::utils::trace::index::store::IndexExtension::Members,
          dftracer::utils::trace::index::store::IndexExtension::Zonemap,
          dftracer::utils::trace::index::store::IndexExtension::Bloom,
          dftracer::utils::trace::index::store::IndexExtension::Counts,
          dftracer::utils::trace::index::store::IndexExtension::Postings,
          dftracer::utils::trace::index::store::IndexExtension::Stats})
        build.finish(ext, 0);
}

/// One dimension's statistics as the dftracer schema writes them: a zonemap
/// and, when the values fit, counts. `observed` is the chunk's data events.
inline void put_dimension_stats(
    dftracer::utils::trace::index::FileBuild& build, std::uint64_t chunk,
    const dftracer::utils::trace::index::extensions::ChunkDimensionStats& ds,
    std::uint64_t observed) {
    const auto compressed = ds.compress_value_counts(4096);
    dftracer::utils::dstd::Optional<std::uint64_t> present;
    if (ds.dimension == "ts" || ds.dimension == "dur") {
        present = observed;
    } else if (compressed && ds.value_counts) {
        std::uint64_t carried = 0;
        for (const auto& [value, count] : *ds.value_counts) carried += count;
        present = carried;
    }
    dftracer::utils::trace::index::extensions::kinds::Zone zone;
    zone.value_type = ds.value_type;
    zone.min = ds.min_value;
    zone.max = ds.max_value;
    zone.observed = observed;
    zone.present = present;
    const dftracer::utils::dstd::String dimension(ds.dimension.data(),
                                                  ds.dimension.size());
    const dftracer::utils::dstd::String zone_payload =
        dftracer::utils::trace::index::extensions::kinds::encode_zone(zone);
    dftracer::utils::trace::index::put_path_granule(
        build, dftracer::utils::trace::index::store::IndexExtension::Zonemap,
        dimension, chunk, zone_payload);
    if (compressed) {
        const dftracer::utils::dstd::String counts_payload =
            dftracer::utils::trace::index::extensions::kinds::encode_counts(
                {observed, *compressed});
        dftracer::utils::trace::index::put_path_granule(
            build, dftracer::utils::trace::index::store::IndexExtension::Counts,
            dimension, chunk, counts_payload);
    }
    dftracer::utils::trace::index::put_path(
        build, dftracer::utils::trace::index::store::IndexExtension::Zonemap,
        dimension);
}

inline void put_chunk_bloom(
    dftracer::utils::trace::index::FileBuild& build, std::uint64_t chunk,
    dftracer::utils::dstd::StringView path,
    dftracer::utils::dstd::Span<const unsigned char> bloom,
    std::uint64_t entry_count) {
    const dftracer::utils::dstd::String payload =
        dftracer::utils::trace::index::extensions::kinds::encode_bloom(
            bloom, entry_count);
    dftracer::utils::trace::index::put_path_granule(
        build, dftracer::utils::trace::index::store::IndexExtension::Bloom,
        dftracer::utils::dstd::StringView(path.data(), path.size()), chunk,
        payload);
}

inline void put_file_bloom(
    dftracer::utils::trace::index::FileBuild& build,
    dftracer::utils::dstd::StringView path,
    dftracer::utils::dstd::Span<const unsigned char> bloom,
    std::uint64_t entry_count) {
    const dftracer::utils::dstd::String payload =
        dftracer::utils::trace::index::extensions::kinds::encode_bloom(
            bloom, entry_count);
    dftracer::utils::trace::index::put_path_file(
        build, dftracer::utils::trace::index::store::IndexExtension::Bloom,
        dftracer::utils::dstd::StringView(path.data(), path.size()), payload);
}

/// The pruning statistics of the file are built at the format this code reads.
inline bool pruning_tier_current(
    const dftracer::utils::trace::index::Snapshot& snapshot,
    const dftracer::utils::trace::index::FileRef& file) {
    return dftracer::utils::trace::index::extension_current(
        snapshot, file,
        dftracer::utils::trace::index::store::IndexExtension::Stats);
}

/// Chunks that hold a bloom of `path`.
inline std::size_t bloom_chunks(
    const dftracer::utils::trace::index::Snapshot& snapshot,
    const dftracer::utils::trace::index::FileRef& file,
    dftracer::utils::dstd::StringView path) {
    return index_take(
               dftracer::utils::trace::index::granules(
                   snapshot, file,
                   dftracer::utils::trace::index::store::IndexExtension::Bloom,
                   dftracer::utils::dstd::StringView(path.data(), path.size())))
        .size();
}

inline bool has_file_bloom(
    const dftracer::utils::trace::index::Snapshot& snapshot,
    const dftracer::utils::trace::index::FileRef& file,
    dftracer::utils::dstd::StringView path) {
    return index_take(
               dftracer::utils::trace::index::path_file(
                   snapshot, file,
                   dftracer::utils::trace::index::store::IndexExtension::Bloom,
                   dftracer::utils::dstd::StringView(path.data(), path.size())))
        .has_value();
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
