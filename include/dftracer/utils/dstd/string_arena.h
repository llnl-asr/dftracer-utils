#ifndef DFTRACER_UTILS_DSTD_STRING_ARENA_H
#define DFTRACER_UTILS_DSTD_STRING_ARENA_H

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
        std::unique_ptr<char[]> data;
        std::size_t size;
        explicit Block(std::size_t n) : data(new char[n]), size(n) {}
    };
    std::vector<Block> blocks;
    std::size_t pos = 0;

    explicit StringArena(std::size_t first_block = BLOCK_SIZE) {
        blocks.emplace_back(first_block);
    }

    std::string_view push(const char *data, std::size_t len) {
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
    std::string_view push(std::string_view first, std::string_view second) {
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
