// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <cstring>

namespace dftracer::utils::common {

namespace {

constexpr std::int32_t LAST_CONDITION =
    static_cast<std::int32_t>(dstd::Condition::OutOfMemory);

void release_message(dftu_error* e) {
    if (e->message != nullptr) {
        memory::detail::deallocate(const_cast<char*>(e->message),
                                   std::strlen(e->message) + 1);
    }
    e->message = nullptr;
    e->release = nullptr;
}

}  // namespace

dftu_status to_status(dstd::Condition condition) noexcept {
    return -(static_cast<dftu_status>(condition) + 1);
}

dstd::Condition to_condition(dftu_status status) noexcept {
    if (status >= 0 || status < -(LAST_CONDITION + 1)) {
        return dstd::Condition::Internal;
    }
    return static_cast<dstd::Condition>(-(status + 1));
}

dftu_status fill_error(dftu_error* out, const dstd::Error& error) noexcept {
    const dftu_status status = to_status(error.condition());
    if (out == nullptr) return status;
    dftu_error_release(out);
    out->domain = error.domain().id;
    out->code = error.code();
    out->status = status;
    out->message = nullptr;
    out->release = nullptr;
    const dstd::StringView text =
        error.message().empty() ? error.name() : error.message();
    // Cut at an embedded NUL, so release() can size the block with strlen.
    const std::size_t n = ::strnlen(text.data(), text.size());
    if (n != 0) {
        auto* copy = static_cast<char*>(
            memory::detail::try_allocate(n + 1, alignof(char)));
        if (copy != nullptr) {
            std::memcpy(copy, text.data(), n);
            copy[n] = '\0';
            out->message = copy;
            out->release = &release_message;
        }
    }
    return status;
}

bool copy_struct(void* dst, std::size_t dst_size, const void* src,
                 std::uint32_t src_size, std::size_t min_size) noexcept {
    if (src_size < min_size) return false;
    std::memcpy(dst, src, src_size < dst_size ? src_size : dst_size);
    return true;
}

}  // namespace dftracer::utils::common
