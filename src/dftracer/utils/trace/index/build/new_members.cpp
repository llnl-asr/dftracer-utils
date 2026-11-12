// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/fileio/compress/zng_stream.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/trace/index/build/new_members.h>
#include <zlib-ng.h>

#include <algorithm>

namespace dftracer::utils::trace::index::build {

namespace {

constexpr std::size_t CHUNK = std::size_t{1} << 20;
// A writer may pad between members with a short run of zeros.
constexpr std::size_t MAX_PADDING = 32;

/// The end of the whole member at `offset`, its CRC checked. Nullopt when it
/// is short or bad. An error on a read failure.
coro::CoroTask<dstd::Result<dstd::Optional<std::uint64_t>>> whole_member_end(
    int fd, std::uint64_t offset, std::uint64_t file_size) {
    zng_stream zs{};
    // 31: a gzip wrapper, whose trailer inflate checks.
    if (fileio::compress::open_inflate_stream(zs, 31) != Z_OK) {
        co_return common::make_error(common::ErrorCode::Compression,
                                     "failed to allocate inflate state");
    }
    struct End {
        zng_stream* zs;
        ~End() { zng_inflateEnd(zs); }
    } end{&zs};
    const auto in = dstd::make_unique_for_overwrite<unsigned char[]>(CHUNK);
    const auto out = dstd::make_unique_for_overwrite<unsigned char[]>(CHUNK);
    std::uint64_t pos = offset;
    while (pos < file_size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(CHUNK, file_size - pos));
        const ssize_t got =
            co_await runtime::io::pread(fd, in.get(), want, pos);
        if (got < 0) {
            co_return common::make_error(common::ErrorCode::Io,
                                         "cannot read a trace member");
        }
        if (got == 0) co_return dstd::Optional<std::uint64_t>{};
        pos += static_cast<std::uint64_t>(got);
        zs.next_in = in.get();
        zs.avail_in = static_cast<std::uint32_t>(got);
        do {
            zs.next_out = out.get();
            zs.avail_out = static_cast<std::uint32_t>(CHUNK);
            const int rc = zng_inflate(&zs, Z_NO_FLUSH);
            if (rc == Z_STREAM_END)
                co_return dstd::Optional<std::uint64_t>(pos - zs.avail_in);
            if (rc != Z_OK && rc != Z_BUF_ERROR)
                co_return dstd::Optional<std::uint64_t>{};
        } while (zs.avail_in > 0 || zs.avail_out == 0);
    }
    co_return dstd::Optional<std::uint64_t>{};
}

coro::CoroTask<dstd::Result<std::uint32_t>> read_isize(
    int fd, std::uint64_t member_end) {
    unsigned char tail[4] = {0, 0, 0, 0};
    const ssize_t got =
        co_await runtime::io::pread(fd, tail, sizeof tail, member_end - 4);
    if (got != 4) {
        co_return common::make_error(common::ErrorCode::Io,
                                     "cannot read a member trailer");
    }
    co_return dstd::read_u32_le(tail);
}

/// The first offset at or after `offset` that is not padding.
coro::CoroTask<std::uint64_t> skip_padding(int fd, std::uint64_t offset,
                                           std::uint64_t file_size) {
    unsigned char buf[MAX_PADDING];
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(MAX_PADDING, file_size - offset));
    const ssize_t got = co_await runtime::io::pread(fd, buf, want, offset);
    if (got <= 0) co_return offset;
    std::size_t zeros = 0;
    while (zeros < static_cast<std::size_t>(got) && buf[zeros] == 0) ++zeros;
    // A longer run is a hole, which the next member check reports.
    co_return zeros == static_cast<std::size_t>(got) &&
            static_cast<std::size_t>(got) == MAX_PADDING
        ? offset
        : offset + zeros;
}

coro::CoroTask<dstd::Result<NewMembers>> walk_members(int fd,
                                                      std::uint64_t from,
                                                      std::uint64_t file_size) {
    NewMembers found;
    found.c_end = from;
    std::uint64_t at = from;
    while (at < file_size) {
        const auto walked = co_await whole_member_end(fd, at, file_size);
        if (!walked) co_return walked.error();
        const dstd::Optional<std::uint64_t> end = *walked;
        if (!end) break;
        const auto size = co_await read_isize(fd, *end);
        if (!size) co_return size.error();
        const std::uint32_t isize = *size;
        found.members.push_back({at, *end - at, isize});
        found.c_end = *end;
        at = co_await skip_padding(fd, *end, file_size);
    }
    co_return found;
}

}  // namespace

coro::CoroTask<dstd::Result<NewMembers>> find_new_members(
    int fd, std::uint64_t from, std::uint64_t file_size, bool walk) {
    if (walk) co_return co_await walk_members(fd, from, file_size);

    NewMembers found;
    found.c_end = from;
    const bool scanned = co_await gzip::enumerate_gzip_member_candidates(
        fd, file_size, found.members, from);
    if (!scanned || found.members.front().c_offset != from) {
        co_return co_await walk_members(fd, from, file_size);
    }
    while (!found.members.empty()) {
        gzip::GzipMember& last = found.members.back();
        const auto proven =
            co_await whole_member_end(fd, last.c_offset, file_size);
        if (!proven) co_return proven.error();
        const dstd::Optional<std::uint64_t> end = *proven;
        if (end) {
            last.c_size = *end - last.c_offset;
            const auto size = co_await read_isize(fd, *end);
            if (!size) co_return size.error();
            last.isize = *size;
            found.c_end = *end;
            co_return found;
        }
        // Short, or a false header inside the member before it.
        const std::uint64_t cut = last.c_offset;
        found.members.pop_back();
        found.c_end = cut;
        file_size = cut;
    }
    co_return found;
}

coro::CoroTask<dstd::Result<dstd::Optional<std::uint64_t>>>
find_member_after_hole(int fd, std::uint64_t from, std::uint64_t file_size) {
    dstd::Vector<gzip::GzipMember> candidates;
    const bool scanned = co_await gzip::enumerate_gzip_member_candidates(
        fd, file_size, candidates, from);
    if (!scanned || candidates.empty() || candidates.front().c_offset == from)
        co_return dstd::Optional<std::uint64_t>{};
    for (const gzip::GzipMember& candidate : candidates) {
        const auto proven =
            co_await whole_member_end(fd, candidate.c_offset, file_size);
        if (!proven) co_return proven.error();
        if (*proven)
            co_return dstd::Optional<std::uint64_t>(candidate.c_offset);
    }
    co_return dstd::Optional<std::uint64_t>{};
}

}  // namespace dftracer::utils::trace::index::build
