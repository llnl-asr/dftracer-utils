// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_STRING_ARENA_H
#define DFTRACER_UTILS_DSTD_STRING_ARENA_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace dftracer::utils::dstd {

/// Bump arena for string_views that must survive until a later flush point
/// (e.g. an Arrow builder.finish()).  Not thread-safe.
struct StringArena {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024;
    /// A block is left uninitialized, so an arena that stays small costs no
    /// zero fill.
    struct Block {
        UniquePtr<char[]> data;
        std::size_t size;
        explicit Block(std::size_t n)
            : data(make_unique_for_overwrite<char[]>(n)), size(n) {}
    };
    Vector<Block> blocks;
    std::size_t pos = 0;

    explicit StringArena(std::size_t first_block = BLOCK_SIZE) {
        blocks.emplace_back(first_block);
    }

    StringView push(const char *data, std::size_t len) {
        if (pos + len > blocks.back().size) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char *dst = blocks.back().data.get() + pos;
        std::memcpy(dst, data, len);
        pos += len;
        return {dst, len};
    }

    /// `first` then `second`, next to each other.
    StringView push(StringView first, StringView second) {
        const std::size_t len = first.size() + second.size();
        if (pos + len > blocks.back().size) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char *dst = blocks.back().data.get() + pos;
        if (!first.empty()) std::memcpy(dst, first.data(), first.size());
        if (!second.empty())
            std::memcpy(dst + first.size(), second.data(), second.size());
        pos += len;
        return {dst, len};
    }
    void clear() {
        if (blocks.size() > 1) blocks.erase(blocks.begin() + 1, blocks.end());
        pos = 0;
    }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_STRING_ARENA_H
