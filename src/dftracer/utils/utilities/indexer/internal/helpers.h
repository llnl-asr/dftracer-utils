#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_HELPERS_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_HELPERS_H

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer::internal {

std::string get_logical_path(std::string_view path);
time_t get_file_modification_time(const std::string &file_path);
std::uint64_t calculate_file_hash(const std::string &file_path);
std::uint64_t file_size_bytes(const std::string &path);
bool index_exists_and_valid(const std::string &idx_path);

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_HELPERS_H
