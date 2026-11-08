// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <ctime>

namespace dftracer::utils::index::store::internal {

dstd::String get_logical_path(dstd::StringView path);
time_t get_file_modification_time(dstd::CStringView file_path);
std::uint64_t calculate_file_hash(dstd::CStringView file_path);
/// The same hash of an open file of `file_size` bytes.
std::uint64_t calculate_file_hash(const dstd::File& file,
                                  std::uint64_t file_size);
std::uint64_t file_size_bytes(dstd::CStringView path);
}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_HELPERS_H
