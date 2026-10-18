#include <dftracer/utils/memory/arena.h>
#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_system.h>

#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

PoolHandle open_pool(Pool& parent, std::string_view name) {
    PoolHandle pool = parent.child(name, PoolKind::Scratch);
    if (!pool) throw std::bad_alloc();
    return pool;
}

}  // namespace

Arena::Arena(Pool& parent, std::string_view name)
    : pool_(open_pool(parent, name)), reservation_(pool_->reserve(0)) {}

Arena::Arena(std::string_view name) : Arena(current_pool(), name) {}

Arena::~Arena() { release(); }

void* Arena::try_allocate(std::size_t n, std::size_t align) noexcept {
    try {
        return allocate(n, align);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

Arena::Chunk* Arena::new_chunk(std::size_t size) {
    void* raw = memory::allocate(size, alignof(Chunk), *pool_);
    auto* chunk = ::new (raw) Chunk{chunks_, size};
    chunks_ = chunk;
    chunk_bytes_ += size;
    asan_poison(chunk + 1, size - sizeof(Chunk));
    return chunk;
}

// A request too big for the next chunk gets a chunk of its own, which does
// not move the bump range, so the current chunk keeps serving small requests.
// It still counts as a step of growth, or a run of requests just above the
// chunk size would never reach a bigger chunk.
void* Arena::allocate_chunk(std::size_t n, std::size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) {
        abort_with("dftracer-utils memory: alignment is not a power of two\n");
    }
    std::size_t pad = align > alignof(Chunk) ? align - alignof(Chunk) : 0;
    if (n > std::numeric_limits<std::size_t>::max() - sizeof(Chunk) - pad) {
        throw std::bad_alloc();
    }
    std::size_t need = sizeof(Chunk) + pad + n;
    std::size_t next =
        grown_ >= 6 ? ARENA_MAX_CHUNK : ARENA_FIRST_CHUNK << grown_;
    if (need > next) {
        Chunk* chunk = new_chunk(need);
        ++grown_;
        auto start = reinterpret_cast<std::uintptr_t>(chunk + 1);
        std::uintptr_t p = (start + align - 1) & ~(align - 1);
        asan_unpoison(reinterpret_cast<void*>(p), n);
        return reinterpret_cast<void*>(p);
    }
    Chunk* chunk = new_chunk(next);
    ++grown_;
    if (first_ == nullptr) first_ = chunk;
    cur_ = reinterpret_cast<std::uintptr_t>(chunk + 1);
    end_ = reinterpret_cast<std::uintptr_t>(chunk) + next;
    return allocate(n, align);
}

// A destructor may put new objects on the list; they run in the next pass.
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
            asan_unpoison(c + 1, c->size - sizeof(Chunk));
            chunk_bytes_ -= c->size;
            memory::deallocate(c, c->size);
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
    asan_poison(first_ + 1, first_->size - sizeof(Chunk));
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
        reservation_ = pool_->reserve(target);
        return reservation_.status();
    }
    return reservation_.grow(target - have);
}

}  // namespace dftracer::utils::memory
