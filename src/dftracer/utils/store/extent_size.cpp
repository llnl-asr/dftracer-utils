#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/store/extent_size.h>

#include <string>
#include <string_view>

namespace dftracer::utils::store::engine {

namespace {

constexpr std::uint64_t EXTENT_ALIGNMENT = 4096;

dstd::Unexpected<dstd::Error> override_error(std::string_view reason) {
    return dstd::Unexpected<dstd::Error>(dstd::Error(
        dstd::Condition::InvalidArgument,
        str_cat(constants::store::STORE_EXTENT_BYTES_ENV, " ", reason)));
}

dstd::Unexpected<dstd::Error> override_range_error() {
    return override_error(
        str_cat("must be a multiple of ", EXTENT_ALIGNMENT, " in [",
                constants::store::STORE_MIN_EXTENT_BYTES, ", ",
                constants::store::STORE_MAX_EXTENT_BYTES, "]"));
}

}  // namespace

dstd::Expected<std::uint32_t, dstd::Error> choose_extent_size(
    std::size_t stripe_size, dstd::Optional<std::uint64_t> override_bytes) {
    if (override_bytes.has_value()) {
        const std::uint64_t bytes = *override_bytes;
        if (bytes % EXTENT_ALIGNMENT != 0 ||
            bytes < constants::store::STORE_MIN_EXTENT_BYTES ||
            bytes > constants::store::STORE_MAX_EXTENT_BYTES)
            return override_range_error();
        return static_cast<std::uint32_t>(bytes);
    }
    if (stripe_size == 0) return constants::store::STORE_DEFAULT_EXTENT_BYTES;
    if (stripe_size > constants::store::STORE_MAX_EXTENT_BYTES)
        return constants::store::STORE_MAX_EXTENT_BYTES;
    const std::size_t aligned = stripe_size - stripe_size % EXTENT_ALIGNMENT;
    return static_cast<std::uint32_t>(
        aligned < constants::store::STORE_MIN_EXTENT_BYTES
            ? constants::store::STORE_MIN_EXTENT_BYTES
            : aligned);
}

dstd::Expected<std::uint32_t, dstd::Error> extent_size_for(
    dstd::StringView dir) {
    dstd::Optional<std::uint64_t> override_bytes;
    if (Env::get<std::string_view>(constants::store::STORE_EXTENT_BYTES_ENV)
            .has_value()) {
        // A set value that is not a number is a mistake, not an absent setting.
        auto parsed =
            Env::get<std::uint64_t>(constants::store::STORE_EXTENT_BYTES_ENV);
        if (!parsed.has_value())
            return override_error("must be a plain number of bytes");
        override_bytes = *parsed;
    }
    const std::string path(dir.data(), dir.size());
    return choose_extent_size(lustre_stripe(path).size, override_bytes);
}

}  // namespace dftracer::utils::store::engine
