// SPDX-License-Identifier: MIT
#include <dftracer/utils/memory/arena.h>
#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/pool_access.h>
#include <dftracer/utils/memory/heap_system.h>

#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

PoolHandle open_pool(Pool& parent, std::string_view name) {
    PoolHandle pool = parent.child(name, PoolKind::Scratch);
    if (!pool) {
        detail::abort_with(
            "dftracer-utils memory: cannot open the arena pool\n");
    }
    return pool;
}

}  // namespace

Arena::Arena(Pool& parent, std::string_view name)
    : pool_(open_pool(parent, name)),
      reservation_(detail::PoolAccess::reserve(*pool_, 0)) {}

Arena::Arena(std::string_view name) : Arena(detail::current_pool(), name) {}

Arena::~Arena() { release(); }

void* Arena::try_allocate(std::size_t n, std::size_t align) noexcept {
    void* p = bump(n, align);
    if (p != nullptr) [[likely]]
        return p;
    return allocate_chunk(n, align);
}

Arena::Chunk* Arena::new_chunk(std::size_t size) noexcept {
    void* raw = memory::detail::try_allocate_at(size, alignof(Chunk), *pool_,
                                                __builtin_return_address(0));
    if (raw == nullptr) return nullptr;
    auto* chunk = ::new (raw) Chunk{chunks_, size};
    chunks_ = chunk;
    chunk_bytes_ += size;
    detail::asan_poison(chunk + 1, size - sizeof(Chunk));
    return chunk;
}

// A request too big for the next chunk gets a chunk of its own and the current
// chunk keeps serving small requests. It still counts as a step of growth, or
// requests just above the chunk size would never reach a bigger chunk.
void* Arena::allocate_chunk(std::size_t n, std::size_t align) noexcept {
    if (align == 0 || (align & (align - 1)) != 0) {
        detail::abort_with(
            "dftracer-utils memory: alignment is not a power of two\n");
    }
    std::size_t pad = align > alignof(Chunk) ? align - alignof(Chunk) : 0;
    if (n > std::numeric_limits<std::size_t>::max() - sizeof(Chunk) - pad) {
        return nullptr;
    }
    std::size_t need = sizeof(Chunk) + pad + n;
    std::size_t next = next_chunk();
    if (need > next) {
        Chunk* chunk = new_chunk(need);
        if (chunk == nullptr) return nullptr;
        ++grown_;
        auto start = reinterpret_cast<std::uintptr_t>(chunk + 1);
        std::uintptr_t p = (start + align - 1) & ~(align - 1);
        detail::asan_unpoison(reinterpret_cast<void*>(p), n);
        return reinterpret_cast<void*>(p);
    }
    Chunk* chunk = new_chunk(next);
    if (chunk == nullptr) return nullptr;
    ++grown_;
    if (first_ == nullptr) first_ = chunk;
    cur_ = reinterpret_cast<std::uintptr_t>(chunk + 1);
    end_ = reinterpret_cast<std::uintptr_t>(chunk) + next;
    return bump(n, align);
}

// A destructor may put new objects on the list. They run in the next pass.
void Arena::run_destructors() noexcept {
    while (destructors_ != nullptr) {
        Destructor* d = destructors_;
        destructors_ = nullptr;
        for (; d != nullptr; d = d->next) d->destroy(d->object);
    }
}

// A chunk goes back unpoisoned: the backend writes its own state into a freed
// block and poisons it itself.
void Arena::reset() noexcept {
    run_destructors();
    Chunk* c = chunks_;
    while (c != nullptr) {
        Chunk* next = c->next;
        if (c != first_) {
            detail::asan_unpoison(c + 1, c->size - sizeof(Chunk));
            chunk_bytes_ -= c->size;
            memory::detail::deallocate(c, c->size);
        }
        c = next;
    }
    chunks_ = first_;
    if (first_ == nullptr) {
        grown_ = 0;
        cur_ = 1;
        end_ = 0;
        return;
    }
    first_->next = nullptr;
    grown_ = 1;
    detail::asan_poison(first_ + 1, first_->size - sizeof(Chunk));
    cur_ = reinterpret_cast<std::uintptr_t>(first_ + 1);
    end_ = reinterpret_cast<std::uintptr_t>(first_) + first_->size;
}

void Arena::release() noexcept {
    run_destructors();
    first_ = nullptr;
    reset();
    reservation_.release();
}

Status Arena::reserve(std::size_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::size_t>::max() - chunk_bytes_) {
        return Status::OverBudget;
    }
    std::size_t target = chunk_bytes_ + bytes;
    std::size_t have = reservation_.bytes();
    if (target <= have) return Status::Ok;
    if (!reservation_) {
        reservation_ = detail::PoolAccess::reserve(*pool_, target);
        return reservation_.status();
    }
    return memory::detail::PoolAccess::grow(reservation_, target - have);
}

}  // namespace dftracer::utils::memory
