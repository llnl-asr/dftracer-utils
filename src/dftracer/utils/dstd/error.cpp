#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/memory/heap.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>

namespace dftracer::utils::dstd {

struct Error::Context {
    std::atomic<std::size_t> refs;
    std::size_t length;
};

namespace {

constexpr std::size_t INT32_DIGITS = 11;

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

std::string_view Error::message() const noexcept {
    if (context_ == nullptr) return {};
    return {reinterpret_cast<const char*>(context_ + 1), context_->length};
}

String Error::format() const {
    const std::string_view domain_name = domain().name;
    const std::string_view condition_text = condition_name(condition());
    const std::string_view text = message();

    String out;
    out.reserve(domain_name.size() + 1 + INT32_DIGITS + 1 +
                condition_text.size() + 2 + text.size());
    out.append(domain_name.data(), domain_name.size());
    out.push_back('#');
    char code_buffer[INT32_DIGITS + 1];
    const auto code_end =
        std::to_chars(code_buffer, code_buffer + sizeof(code_buffer), code());
    out.append(code_buffer,
               static_cast<std::size_t>(code_end.ptr - code_buffer));
    out.push_back(' ');
    out.append(condition_text.data(), condition_text.size());
    out.append(": ");
    if (!text.empty()) out.append(text.data(), text.size());
    return out;
}

Error::Context* Error::make_context(std::string_view message) noexcept {
    if (message.size() >
        std::numeric_limits<std::size_t>::max() - sizeof(Context))
        return nullptr;
    const std::size_t size = sizeof(Context) + message.size();
    void* block = memory::try_allocate(size, alignof(Context));
    if (block == nullptr) return nullptr;
    auto* context = ::new (block) Context;
    context->refs.store(1, std::memory_order_relaxed);
    context->length = message.size();
    std::memcpy(reinterpret_cast<char*>(block) + sizeof(Context),
                message.data(), message.size());
    return context;
}

void Error::retain_context(Context* context) noexcept {
    context->refs.fetch_add(1, std::memory_order_relaxed);
}

void Error::release_context(Context* context) noexcept {
    if (context == nullptr) return;
    if (context->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        memory::deallocate(context, sizeof(Context) + context->length);
}

}  // namespace dftracer::utils::dstd
