// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_ROOT_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_ROOT_LAYOUT_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::index {

inline constexpr dstd::StringView INDEX_ROOT_NAME = ".dftindex";
inline constexpr dstd::StringView STORE_DIR = "store";
inline constexpr dstd::StringView ROLLUPS_DIR = "rollups";
inline constexpr dstd::StringView LOOKUPS_DIR = "lookups";
inline constexpr dstd::StringView VIEWS_DIR = "views";
inline constexpr dstd::StringView VIEW_PARTS_DIR = "parts";
inline constexpr dstd::StringView SHARDS_DIR = "shards";
inline constexpr dstd::StringView SHARD_MANIFEST = "shards.json";
inline constexpr dstd::StringView SPILL_DIR = "spill";

// The index root `path` names: the `.dftindex` directory itself, the one beside
// a file or inside a directory, or the enclosing view or shard root.
dstd::String normalize_index_root(dstd::StringView path);
dstd::String store_dir(dstd::StringView root);
// The store directory under the root `path` names, normalized first, so a plain
// directory names its own `.dftindex`. Empty `path` names nothing.
dstd::String store_of(dstd::StringView path);
dstd::String rollups_dir(dstd::StringView root);
dstd::String lookups_dir(dstd::StringView root);
dstd::String views_dir(dstd::StringView root);
dstd::String view_root(dstd::StringView root, dstd::StringView slug);
dstd::String view_parts_dir(dstd::StringView view_root);
dstd::String shards_dir(dstd::StringView root);
dstd::String shard_root(dstd::StringView root, std::size_t n);
dstd::String shard_manifest_path(dstd::StringView root);
dstd::String spill_dir(dstd::StringView root);

// The nearest ancestor-or-self of `path` that is a view or shard root, that is
// `<x>/.dftindex/{views,shards}/<name>`. Lexical only.
dstd::Optional<dstd::String> nested_root_of(dstd::StringView path);

}  // namespace dftracer::utils::index

#endif
