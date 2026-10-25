#ifndef DFTRACER_UTILS_DSTD_ERROR_H
#define DFTRACER_UTILS_DSTD_ERROR_H

#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/string.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::dstd {

enum class Condition : std::uint16_t {
    Unknown,
    Internal,
    InvalidArgument,
    NotFound,
    Io,
    Parse,
    Compression,
    Timeout,
    Unsupported,
    Cancelled,
};

struct ErrorDomain {
    std::string_view name;
    std::uint64_t id;
};

constexpr ErrorDomain make_error_domain(std::string_view name) noexcept {
    return {name, ::dftracer::utils::hash::fnv1a_hash(name)};
}

struct ErrorCodeInfo {
    const ErrorDomain* domain;
    std::int32_t code;
    Condition condition;
    std::string_view name;
};

inline constexpr ErrorDomain CORE_ERROR_DOMAIN =
    make_error_domain("dftracer.utils.core");

inline constexpr ErrorCodeInfo CORE_ERROR_INFOS[] = {
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Unknown),
     Condition::Unknown, "UNKNOWN"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Internal),
     Condition::Internal, "INTERNAL"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::InvalidArgument),
     Condition::InvalidArgument, "INVALID_ARGUMENT"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::NotFound),
     Condition::NotFound, "NOT_FOUND"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Io),
     Condition::Io, "IO"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Parse),
     Condition::Parse, "PARSE"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Compression),
     Condition::Compression, "COMPRESSION"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Timeout),
     Condition::Timeout, "TIMEOUT"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Unsupported),
     Condition::Unsupported, "UNSUPPORTED"},
    {&CORE_ERROR_DOMAIN, static_cast<std::int32_t>(Condition::Cancelled),
     Condition::Cancelled, "CANCELLED"},
};

constexpr const ErrorCodeInfo& error_info(Condition condition) noexcept {
    const auto at = static_cast<std::size_t>(condition);
    return at < std::size(CORE_ERROR_INFOS) ? CORE_ERROR_INFOS[at]
                                            : CORE_ERROR_INFOS[0];
}

constexpr std::string_view condition_name(Condition condition) noexcept {
    return error_info(condition).name;
}

template <class E>
concept ErrorEnum = std::is_enum_v<E> && requires(E value) {
    { error_info(value) } -> std::same_as<const ErrorCodeInfo&>;
};

class [[nodiscard]] Error {
   public:
    Error() noexcept;
    template <ErrorEnum E>
    Error(E code) noexcept : info_(&error_info(code)) {}
    template <ErrorEnum E>
    Error(E code, std::string_view message) noexcept
        : info_(&error_info(code)) {
        if (!message.empty()) context_ = make_context(message);
    }
    Error(const Error&) noexcept;
    Error& operator=(const Error&) noexcept;
    Error(Error&&) noexcept;
    Error& operator=(Error&&) noexcept;
    ~Error();

    std::int32_t code() const noexcept { return info_->code; }
    Condition condition() const noexcept { return info_->condition; }
    bool is(Condition value) const noexcept { return condition() == value; }
    template <ErrorEnum E>
    bool is_code(E code) const noexcept {
        return info_ == &error_info(code);
    }
    bool in_domain(const ErrorDomain& domain) const noexcept {
        return info_->domain->id == domain.id;
    }
    const ErrorDomain& domain() const noexcept { return *info_->domain; }
    std::string_view name() const noexcept { return info_->name; }
    std::string_view message() const noexcept;
    String format() const;

   private:
    struct Context;

    static Context* make_context(std::string_view message) noexcept;
    static void retain_context(Context* context) noexcept;
    static void release_context(Context* context) noexcept;

    const ErrorCodeInfo* info_;
    Context* context_ = nullptr;
};

static_assert(sizeof(Error) == 16);

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_ERROR_H
