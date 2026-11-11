// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_NEW_MEMBERS_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_NEW_MEMBERS_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/gzip/gzip_member_scanner.h>

#include <cstdint>

namespace dftracer::utils::trace::index::build {

/// The gzip members of one file from a known boundary on.
struct NewMembers {
    dstd::Vector<gzip::GzipMember> members;
    /// The end of the last member. A build reads nothing past it.
    std::uint64_t c_end = 0;
};

/// Finds the members of `fd` from `from`, which is a member boundary, to
/// `file_size`. The fast form scans for headers and proves only the last
/// member whole, since the inflate pass checks the others. With `walk` it
/// inflates each member in turn and stops at the first one that is short or
/// bad, so `c_end` is proven, at the price of a second inflate. An Io error on
/// a read failure.
coro::CoroTask<dstd::Result<NewMembers>> find_new_members(
    int fd, std::uint64_t from, std::uint64_t file_size, bool walk);

/// When the bytes at `from` do not open a gzip member, the offset of the first
/// whole member after them. Nullopt when `from` opens a member, which is then
/// short or bad, or when no whole member follows. An Io error on a read
/// failure.
coro::CoroTask<dstd::Result<dstd::Optional<std::uint64_t>>>
find_member_after_hole(int fd, std::uint64_t from, std::uint64_t file_size);

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_NEW_MEMBERS_H
