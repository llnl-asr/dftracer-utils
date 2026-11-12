// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/fileio/hash/hasher_utility.h>

namespace dftracer::utils::fileio {

// Registry key: the canonical absolute path, so same-named files in different
// directories are distinct and the key locates the trace. Applied on both the
// write and read side so lookups stay consistent; relative inputs resolve
// against the cwd, so the index is not portable across an absolute-layout move.
dstd::String get_logical_path(dstd::StringView path) {
    const auto abs = dstd::fs::absolute(dstd::PathView(path));
    if (!abs) return dstd::String(dstd::normalize(dstd::PathView(path)).str());
    return dstd::String(dstd::normalize(abs->view()).str());
}

time_t get_file_modification_time(dstd::CStringView file_path) {
    dftracer::utils::common::InputFileStat st;
    if (!dftracer::utils::common::stat_input(file_path, st)) return 0;
    return static_cast<time_t>(st.mtime);
}

std::uint64_t calculate_file_hash(dstd::CStringView file_path) {
    auto file = dstd::File::open(dstd::PathView(file_path), {});
    if (!file) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open file for hash calculation: {}",
                                 file_path);
        return 0;
    }

    const auto size = file->size();
    if (!size) return 0;
    return calculate_file_hash(*file, *size);
}

std::uint64_t calculate_file_hash(const dstd::File &file,
                                  std::uint64_t file_size_u64) {
    // Fast fingerprint: hash file_size + first 64KB + last 64KB.
    constexpr std::size_t SAMPLE_SIZE = 64 * 1024;
    const auto file_size = static_cast<std::size_t>(file_size_u64);

    static thread_local dftracer::utils::fileio::hash::HasherUtility hasher;
    hasher.reset();

    // Hash the file size itself
    char size_buf[32];
    const auto size_text =
        dstd::fmt_to<"{}">(size_buf, sizeof(size_buf), file_size);
    hasher.update(dstd::StringView(size_buf, size_text.size));

    unsigned char buf[SAMPLE_SIZE];

    // Hash first SAMPLE_SIZE bytes
    std::size_t head_bytes = std::min(file_size, SAMPLE_SIZE);
    auto rd = file.read_at(0, dstd::Span<std::uint8_t>(buf, head_bytes));
    if (rd && *rd > 0) {
        hasher.update(dstd::StringView(reinterpret_cast<const char *>(buf),
                                       static_cast<std::size_t>(*rd)));
    }

    // Hash last SAMPLE_SIZE bytes, only if file is large enough such that they
    // don't overlap with the head
    if (file_size > SAMPLE_SIZE) {
        const std::uint64_t tail_off =
            file_size - std::min(file_size, SAMPLE_SIZE);
        rd = file.read_at(tail_off, dstd::Span<std::uint8_t>(buf, SAMPLE_SIZE));
        if (rd && *rd > 0) {
            hasher.update(dstd::StringView(reinterpret_cast<const char *>(buf),
                                           static_cast<std::size_t>(*rd)));
        }
    }

    return static_cast<std::uint64_t>(hasher.hash().value);
}

std::uint64_t file_size_bytes(dstd::CStringView path) {
    dftracer::utils::common::InputFileStat st;
    if (dftracer::utils::common::stat_input(path, st) && st.is_regular_file)
        return st.size;

    auto file = dstd::File::open(dstd::PathView(path), {});
    if (!file) return 0;
    const auto size = file->size();
    return size ? *size : 0;
}

}  // namespace dftracer::utils::fileio
