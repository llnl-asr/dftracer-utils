// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/shared_string.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <cstring>
#include <new>

namespace dftracer::utils::dstd {

SharedString::Block* SharedString::make_block(StringView text) {
    if (text.empty()) return nullptr;
    const std::size_t bytes_size = sizeof(Block) + text.size() + 1;
    void* raw = memory::detail::allocate(bytes_size, alignof(Block));
    Block* b = new (raw) Block{{1}, text.size()};
    std::memcpy(reinterpret_cast<char*>(b + 1), text.data(), text.size());
    reinterpret_cast<char*>(b + 1)[text.size()] = '\0';
    return b;
}

void SharedString::free_block(Block* b) noexcept {
    const std::size_t bytes_size = sizeof(Block) + b->size + 1;
    b->~Block();
    memory::detail::deallocate(b, bytes_size);
}

}  // namespace dftracer::utils::dstd
