// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dftracer::utils::dstd {

struct Error::Context {
    std::atomic<std::size_t> refs;
    std::size_t length;
    std::size_t capacity;
};

namespace {

constexpr std::size_t INT32_DIGITS = 11;

// A context pointer with the low bit set is a StaticMessage, not a heap block.
constexpr std::uintptr_t STATIC_TAG = 1;

bool is_static(const void* context) noexcept {
    return (reinterpret_cast<std::uintptr_t>(context) & STATIC_TAG) != 0;
}

const StaticMessage* as_static(const void* context) noexcept {
    return reinterpret_cast<const StaticMessage*>(
        reinterpret_cast<std::uintptr_t>(context) & ~STATIC_TAG);
}

}  // namespace

Error::Error() noexcept
    : info_(&error_info(Condition::Unknown)), context_(nullptr) {}

Error::Error(const Error& other) noexcept
    : info_(other.info_), context_(other.context_) {
    if (context_ != nullptr) retain_context(context_);
}

Error& Error::operator=(const Error& other) noexcept {
    if (this != &other) {
        release_context(context_);
        info_ = other.info_;
        context_ = other.context_;
        if (context_ != nullptr) retain_context(context_);
    }
    return *this;
}

Error::Error(Error&& other) noexcept
    : info_(other.info_), context_(other.context_) {
    other.context_ = nullptr;
}

Error& Error::operator=(Error&& other) noexcept {
    if (this != &other) {
        release_context(context_);
        info_ = other.info_;
        context_ = other.context_;
        other.context_ = nullptr;
    }
    return *this;
}

Error::~Error() { release_context(context_); }

StringView Error::message() const noexcept {
    if (context_ == nullptr) return {};
    if (is_static(context_)) {
        const StaticMessage* text = as_static(context_);
        return {text->text, text->length};
    }
    return {reinterpret_cast<const char*>(context_ + 1), context_->length};
}

const char* Error::c_str() const noexcept {
    if (context_ == nullptr) return "";
    if (is_static(context_)) return as_static(context_)->text;
    return reinterpret_cast<const char*>(context_ + 1);
}

String Error::format() const {
    const StringView domain_name = domain().name;
    const StringView condition_text = condition_name(condition());
    const StringView text = message();

    String out;
    out.reserve(domain_name.size() + 1 + INT32_DIGITS + 1 +
                condition_text.size() + 2 + text.size());
    out.append(domain_name.data(), domain_name.size());
    out.push_back('#');
    char code_buffer[INT32_DIGITS + 1];
    const auto code_end =
        to_chars(code_buffer, code_buffer + sizeof(code_buffer), code());
    out.append(code_buffer,
               static_cast<std::size_t>(code_end.ptr - code_buffer));
    out.push_back(' ');
    out.append(condition_text.data(), condition_text.size());
    out.append(": ");
    if (!text.empty()) out.append(text.data(), text.size());
    return out;
}

Error::Context* Error::make_context(StringView message) noexcept {
    if (message.size() >
        std::numeric_limits<std::size_t>::max() - sizeof(Context) - 1)
        return nullptr;
    const std::size_t size = sizeof(Context) + message.size() + 1;
    void* block = memory::detail::try_allocate(size, alignof(Context));
    if (block == nullptr) return nullptr;
    auto* context = ::new (block) Context;
    context->refs.store(1, std::memory_order_relaxed);
    context->length = message.size();
    context->capacity = message.size();
    std::memcpy(reinterpret_cast<char*>(block) + sizeof(Context),
                message.data(), message.size());
    reinterpret_cast<char*>(block)[sizeof(Context) + message.size()] = '\0';
    return context;
}

Error::Context* Error::make_context_buffer(std::size_t capacity) noexcept {
    if (capacity >
        std::numeric_limits<std::size_t>::max() - sizeof(Context) - 1)
        return nullptr;
    const std::size_t size = sizeof(Context) + capacity + 1;
    void* block = memory::detail::try_allocate(size, alignof(Context));
    if (block == nullptr) return nullptr;
    auto* context = ::new (block) Context;
    context->refs.store(1, std::memory_order_relaxed);
    context->length = 0;
    context->capacity = capacity;
    reinterpret_cast<char*>(block)[sizeof(Context)] = '\0';
    return context;
}

char* Error::context_text(Context* context) noexcept {
    return reinterpret_cast<char*>(context + 1);
}

void Error::finish_context(Context* context, std::size_t length) noexcept {
    context->length = length;
    reinterpret_cast<char*>(context + 1)[length] = '\0';
}

Error::Context* Error::make_static_context(
    const StaticMessage* message) noexcept {
    static_assert(alignof(Context) >= 2,
                  "the low pointer bit must be free for the static tag");
    return reinterpret_cast<Context*>(
        reinterpret_cast<std::uintptr_t>(message) | STATIC_TAG);
}

void Error::retain_context(Context* context) noexcept {
    if (is_static(context)) return;
    context->refs.fetch_add(1, std::memory_order_relaxed);
}

void Error::release_context(Context* context) noexcept {
    if (context == nullptr || is_static(context)) return;
    if (context->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        memory::detail::deallocate(context,
                                   sizeof(Context) + context->capacity + 1);
}

}  // namespace dftracer::utils::dstd
