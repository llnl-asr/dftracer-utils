// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
#define DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H

#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Helpers for tests that write synthetic index data by hand or read the index
// back through a snapshot.
namespace dftu_utils_test {

using dftracer::utils::index::store::IndexExtension;

template <class T>
T index_take(
    dftracer::utils::dstd::Expected<T, dftracer::utils::dstd::Error> result) {
    if (!result) throw std::runtime_error("index test: an index call failed");
    return std::move(*result);
}

inline dftracer::utils::index::Index open_test_index(
    const std::string& index_path) {
    return index_take(dftracer::utils::index::Index::open(
        dftracer::utils::index::store::internal::normalize_index_root(
            index_path)));
}

inline dftracer::utils::index::Snapshot index_snapshot(
    const std::string& index_path) {
    return index_take(open_test_index(index_path).snapshot());
}

inline std::string index_key(const std::string& trace_path) {
    return dftracer::utils::index::store::internal::get_logical_path(
        trace_path);
}

/// The file of `trace_path`, found under the key the indexer files it by.
inline std::optional<dftracer::utils::index::FileRef> find_indexed_file(
    const dftracer::utils::index::Snapshot& snapshot,
    const std::string& trace_path) {
    const std::string key = index_key(trace_path);
    auto found = snapshot.file(dftracer::utils::dstd::StringView(key));
    if (!found) return std::nullopt;
    return std::move(*found);
}

inline dftracer::utils::index::FileRef require_indexed_file(
    const dftracer::utils::index::Snapshot& snapshot,
    const std::string& trace_path) {
    auto found = find_indexed_file(snapshot, trace_path);
    if (!found) throw std::runtime_error("index test: the file is not indexed");
    return std::move(*found);
}

/// One record of the store under an index root.
struct IndexRecord {
    std::string name;
    std::string scope;
    std::string key;
    std::string value;
    bool operator==(const IndexRecord&) const = default;
    auto operator<=>(const IndexRecord&) const = default;
};

/// Every live record of every namespace of the index at `index_path`, in
/// namespace, scope and key order. `scope` joins the scope paths with NUL.
inline std::vector<IndexRecord> dump_index(const std::string& index_path) {
    namespace kv = dftracer::utils::store;
    auto store = index_take(kv::Store::open(
        dftracer::utils::index::store::internal::normalize_index_root(
            index_path) +
        "/store"));
    auto snapshot = index_take(store.snapshot());
    auto declared = index_take(snapshot.namespaces());
    std::vector<IndexRecord> out;
    for (const auto& decl : declared) {
        kv::Namespace ns;
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
    dftracer::utils::index::gzip::GzipMemberRecord>
indexed_members(const std::string& index_path, const std::string& trace_path) {
    const auto snapshot = index_snapshot(index_path);
    return index_take(dftracer::utils::index::members(
        snapshot, require_indexed_file(snapshot, trace_path)));
}

inline void commit_test_build(dftracer::utils::index::FileBuild& build) {
    auto done = dftracer::utils::default_runtime().submit(build.commit()).get();
    if (!done) throw std::runtime_error("index test: a commit failed");
}

/// Starts the data of `logical_path`: the file metadata and the Members copy
/// that holds the file's stat. Commit it with commit_test_build.
inline dftracer::utils::index::FileBuild register_test_file(
    const dftracer::utils::index::Index& index, std::string_view logical_path,
    std::uint64_t hash = 0, std::uint64_t mtime = 0, std::uint64_t size = 0) {
    dftracer::utils::index::SourceStat stat;
    stat.mtime = mtime;
    stat.size = size;
    stat.hash = hash;
    auto build =
        index_take(index.build(dftracer::utils::dstd::StringView(
                                   logical_path.data(), logical_path.size()),
                               stat));
    dftracer::utils::index::put_file_metadata(build, 0, 0, 0, false);
    build.finish(IndexExtension::Members, 0);
    return build;
}

/// Starts a rebuild of an indexed file, with the stat it was indexed with.
inline dftracer::utils::index::FileBuild rebuild_test_file(
    const dftracer::utils::index::Index& index, const std::string& trace_path) {
    const std::string key = index_key(trace_path);
    return index_take(dftracer::utils::index::rebuild(
        index, dftracer::utils::dstd::StringView(key)));
}

/// Finishes the pruning extensions the build did not finish, so readers use
/// the data a test wrote. Members is finished too unless the build did.
inline void mark_built(dftracer::utils::index::FileBuild& build) {
    for (auto ext : {IndexExtension::Members, IndexExtension::Zonemap,
                     IndexExtension::Bloom, IndexExtension::Counts,
                     IndexExtension::Postings, IndexExtension::Stats})
        build.finish(ext, 0);
}

/// One dimension's statistics as the dftracer schema writes them: a zonemap
/// and, when the values fit, counts. `observed` is the chunk's data events.
inline void put_dimension_stats(
    dftracer::utils::index::FileBuild& build, std::uint64_t chunk,
    const dftracer::utils::index::extensions::ChunkDimensionStats& ds,
    std::uint64_t observed) {
    namespace kinds = dftracer::utils::index::extensions::kinds;
    namespace idx = dftracer::utils::index;
    const auto compressed = ds.compress_value_counts(4096);
    std::optional<std::uint64_t> present;
    if (ds.dimension == "ts" || ds.dimension == "dur") {
        present = observed;
    } else if (compressed && ds.value_counts) {
        std::uint64_t carried = 0;
        for (const auto& [value, count] : *ds.value_counts) carried += count;
        present = carried;
    }
    kinds::Zone zone;
    zone.value_type = ds.value_type;
    zone.min = ds.min_value;
    zone.max = ds.max_value;
    zone.observed = observed;
    zone.present = present;
    const std::string dimension(ds.dimension.data(), ds.dimension.size());
    const std::string zone_payload = kinds::encode_zone(zone);
    idx::put_path_granule(build, IndexExtension::Zonemap, dimension, chunk,
                          zone_payload);
    if (compressed) {
        const std::string counts_payload =
            kinds::encode_counts({observed, *compressed});
        idx::put_path_granule(build, IndexExtension::Counts, dimension, chunk,
                              counts_payload);
    }
    idx::put_path(build, IndexExtension::Zonemap, dimension);
}

inline void put_chunk_bloom(dftracer::utils::index::FileBuild& build,
                            std::uint64_t chunk, std::string_view path,
                            std::span<const unsigned char> bloom,
                            std::uint64_t num_entries) {
    const std::string payload =
        dftracer::utils::index::extensions::kinds::encode_bloom(bloom,
                                                                num_entries);
    dftracer::utils::index::put_path_granule(
        build, IndexExtension::Bloom,
        dftracer::utils::dstd::StringView(path.data(), path.size()), chunk,
        payload);
}

inline void put_file_bloom(dftracer::utils::index::FileBuild& build,
                           std::string_view path,
                           std::span<const unsigned char> bloom,
                           std::uint64_t num_entries) {
    const std::string payload =
        dftracer::utils::index::extensions::kinds::encode_bloom(bloom,
                                                                num_entries);
    dftracer::utils::index::put_path_file(
        build, IndexExtension::Bloom,
        dftracer::utils::dstd::StringView(path.data(), path.size()), payload);
}

/// The pruning statistics of the file are built at the format this code reads.
inline bool pruning_tier_current(
    const dftracer::utils::index::Snapshot& snapshot,
    const dftracer::utils::index::FileRef& file) {
    return dftracer::utils::index::extension_current(snapshot, file,
                                                     IndexExtension::Stats);
}

/// Chunks that hold a bloom of `path`.
inline std::size_t bloom_chunks(
    const dftracer::utils::index::Snapshot& snapshot,
    const dftracer::utils::index::FileRef& file, std::string_view path) {
    return index_take(
               dftracer::utils::index::granules(
                   snapshot, file, IndexExtension::Bloom,
                   dftracer::utils::dstd::StringView(path.data(), path.size())))
        .size();
}

inline bool has_file_bloom(const dftracer::utils::index::Snapshot& snapshot,
                           const dftracer::utils::index::FileRef& file,
                           std::string_view path) {
    return index_take(
               dftracer::utils::index::path_file(
                   snapshot, file, IndexExtension::Bloom,
                   dftracer::utils::dstd::StringView(path.data(), path.size())))
        .has_value();
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_INDEX_TEST_HELPERS_H
