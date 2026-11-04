// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H
#define DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::index::store {

/// One immutable index shard of an index root. `path` names the shard's own
/// index root, relative to the root holding the manifest. `file_count` counts
/// the trace files the shard indexes and `event_count` its event count.
struct IndexShardEntry {
    dstd::String path;
    std::uint64_t file_count = 0;
    std::uint64_t event_count = 0;

    bool empty() const noexcept { return file_count == 0; }
};

/// Catalog of the immutable shards one index root collects before its merge.
/// Persisted as `shards.json` in the root and published atomically, so a reader
/// sees either the whole previous manifest or the whole new one, never a torn
/// file. `format_version` is the index FORMAT_VERSION the shards were built
/// with; a reader rejects a manifest whose version it cannot read.
struct IndexShardManifest {
    dstd::Optional<std::uint32_t> format_version;
    dstd::Vector<IndexShardEntry> shards;
};

/// Serialize to the on-disk JSON representation.
dstd::String to_json(const IndexShardManifest& manifest);

/// Parse the on-disk JSON representation. Throws DFTUtilsException on malformed
/// input.
IndexShardManifest parse_manifest(dstd::StringView json);

/// Write `manifest` to the root's `shards.json`, creating `root` if needed.
/// Writes a temporary file beside it and renames it into place so a concurrent
/// reader never observes a partial manifest. Throws DFTUtilsException on I/O
/// failure.
void write_shard_manifest(dstd::StringView root,
                          const IndexShardManifest& manifest);

/// Read the root's `shards.json`. Returns dstd::nullopt when the file is
/// absent; throws DFTUtilsException when it exists but cannot be read or
/// parsed.
dstd::Optional<IndexShardManifest> read_shard_manifest(dstd::StringView root);

/// True when the root holds a `shards.json`: the shards of a build are waiting
/// to be merged.
bool has_shard_manifest(dstd::StringView root);

/// Resolve the index root under `target` that holds a manifest, else empty.
/// Accepts the root itself or the directory its `.dftindex` sits in, so a tool
/// autodetects a shard set by pointing at the trace directory.
dstd::String resolve_shard_set_root(dstd::StringView target);

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H
