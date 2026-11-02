// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace dftracer::utils::index::store::internal {

std::string get_logical_path(dstd::StringView path);
std::string normalize_index_root(dstd::StringView path);
time_t get_file_modification_time(const std::string &file_path);
std::uint64_t calculate_file_hash(const std::string &file_path);
/// The same hash of an open file of `file_size` bytes.
std::uint64_t calculate_file_hash(int fd, std::uint64_t file_size);
std::uint64_t file_size_bytes(const std::string &path);
}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
