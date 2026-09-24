#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace dftracer::utils::index::store::internal {

std::string get_logical_path(std::string_view path);
std::string normalize_index_root(std::string_view path);
time_t get_file_modification_time(const std::string &file_path);
std::uint64_t calculate_file_hash(const std::string &file_path);
std::uint64_t file_size_bytes(const std::string &path);
}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
