#ifndef DFTRACER_UTILS_CORE_COMMON_STRING_ARENA_H
#define DFTRACER_UTILS_CORE_COMMON_STRING_ARENA_H

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace dftracer::utils {

// Bump arena for string_views that must survive until a later flush point
// (e.g. an Arrow builder.finish()).  Not thread-safe.
struct StringArena {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024;
    std::vector<std::vector<char>> blocks;
    std::size_t pos = 0;

    StringArena() { blocks.emplace_back(BLOCK_SIZE); }

    std::string_view push(const char *data, std::size_t len) {
        if (pos + len > blocks.back().size()) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char *dst = blocks.back().data() + pos;
        std::memcpy(dst, data, len);
        pos += len;
        return {dst, len};
    }

    void clear() {
        if (blocks.size() > 1) blocks.resize(1);
        pos = 0;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_STRING_ARENA_H
