#ifndef DFTRACER_UTILS_INDEX_STORE_ROOT_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_ROOT_LAYOUT_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::index {

inline constexpr std::string_view INDEX_ROOT_NAME = ".dftindex";
inline constexpr std::string_view STORE_DIR = "store";
inline constexpr std::string_view ROLLUPS_DIR = "rollups";
inline constexpr std::string_view VIEWS_DIR = "views";
inline constexpr std::string_view VIEW_PARTS_DIR = "parts";
inline constexpr std::string_view SHARDS_DIR = "shards";
inline constexpr std::string_view SHARD_MANIFEST = "shards.json";
inline constexpr std::string_view SPILL_DIR = "spill";

std::string store_dir(std::string_view root);
std::string rollups_dir(std::string_view root);
std::string views_dir(std::string_view root);
std::string view_root(std::string_view root, std::string_view slug);
std::string view_parts_dir(std::string_view view_root);
std::string shards_dir(std::string_view root);
std::string shard_root(std::string_view root, std::size_t n);
std::string shard_manifest_path(std::string_view root);
std::string spill_dir(std::string_view root);

// The nearest ancestor-or-self of `path` that is a view or shard root, that is
// `<x>/.dftindex/{views,shards}/<name>`. Lexical only.
std::optional<std::string> nested_root_of(std::string_view path);

}  // namespace dftracer::utils::index

#endif
