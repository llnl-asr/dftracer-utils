// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_SHARED_STRING_H
#define DFTRACER_UTILS_DSTD_SHARED_STRING_H

#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace dftracer::utils::dstd {

/**
 * @brief An immutable string whose copies share one heap block.
 *
 * The object is one pointer. The block holds an atomic reference count, the
 * byte length and the bytes and a NUL, in one allocation from the memory
 * manager. A copy increments the count and allocates nothing. The empty string,
 * and a default-constructed one, is a null pointer with no block.
 *
 * The bytes never change, so copies and reads on several threads are safe
 * without a lock. One SharedString object is not safe to assign from one thread
 * while another thread reads that same object. The bytes stay valid while any
 * copy of the string lives, and @ref view or @ref data must not be used after
 * the last copy dies.
 */
class SharedString {
   public:
    SharedString() noexcept = default;

    /// Copies @p text into a new block; an empty @p text allocates nothing.
    explicit SharedString(StringView text) : block_(make_block(text)) {}

    SharedString(const SharedString& other) noexcept : block_(other.block_) {
        if (block_ != nullptr) {
            block_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    SharedString(SharedString&& other) noexcept
        : block_(std::exchange(other.block_, nullptr)) {}
    SharedString& operator=(const SharedString& other) noexcept {
        SharedString copy(other);
        swap(copy);
        return *this;
    }
    SharedString& operator=(SharedString&& other) noexcept {
        SharedString moved(std::move(other));
        swap(moved);
        return *this;
    }
    ~SharedString() { release(); }

    void swap(SharedString& other) noexcept { std::swap(block_, other.block_); }

    const char* data() const noexcept {
        return block_ == nullptr ? nullptr : bytes(block_);
    }
    /// The bytes with a terminating NUL; an empty string gives "".
    const char* c_str() const noexcept {
        return block_ == nullptr ? "" : bytes(block_);
    }
    std::size_t size() const noexcept {
        return block_ == nullptr ? 0 : block_->size;
    }
    bool empty() const noexcept { return block_ == nullptr; }
    StringView view() const noexcept { return StringView{data(), size()}; }
    operator StringView() const noexcept { return view(); }

    /// Strings that compare equal have equal text; a pointer test is not made.
    friend bool operator==(const SharedString& a,
                           const SharedString& b) noexcept {
        return a.view() == b.view();
    }
    friend auto operator<=>(const SharedString& a,
                            const SharedString& b) noexcept {
        return a.view() <=> b.view();
    }
    friend bool operator==(const SharedString& a, StringView b) noexcept {
        return a.view() == b;
    }

   private:
    struct Block {
        std::atomic<std::size_t> refs;
        std::size_t size;
    };

    static const char* bytes(const Block* b) noexcept {
        return reinterpret_cast<const char*>(b + 1);
    }
    static Block* make_block(StringView text);
    static void free_block(Block* b) noexcept;

    void release() noexcept {
        if (block_ != nullptr &&
            block_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            free_block(block_);
        }
    }

    Block* block_ = nullptr;
};

template <>
struct Hash<SharedString> : StringHash {};

/// One new @ref SharedString per name, each with its own block.
inline Vector<SharedString> make_shared_strings(Span<const String> names) {
    Vector<SharedString> out;
    out.reserve(names.size());
    for (const String& n : names) out.emplace_back(StringView{n});
    return out;
}

/// One new @ref SharedString per name of @p names.
inline Vector<SharedString> make_shared_strings(
    std::initializer_list<StringView> names) {
    Vector<SharedString> out;
    out.reserve(names.size());
    for (StringView n : names) out.emplace_back(n);
    return out;
}

/// One new @ref String per name.
inline Vector<String> to_strings(Span<const SharedString> names) {
    Vector<String> out;
    out.reserve(names.size());
    for (const SharedString& n : names) out.emplace_back(n.view());
    return out;
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SHARED_STRING_H
