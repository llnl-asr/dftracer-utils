// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/store/extent_size.h>

#include <string>
#include <string_view>

namespace dftracer::utils::store::engine {

namespace {

constexpr std::uint64_t EXTENT_ALIGNMENT = 4096;

dstd::Unexpected<dstd::Error> override_error(dstd::StringView reason) {
    return dstd::Unexpected<dstd::Error>(dstd::Error(
        dstd::Condition::InvalidArgument,
        common::str_cat(common::constants::store::STORE_EXTENT_BYTES_ENV, " ",
                        reason)));
}

dstd::Unexpected<dstd::Error> override_range_error() {
    return override_error(
        common::str_cat("must be a multiple of ", EXTENT_ALIGNMENT, " in [",
                        common::constants::store::STORE_MIN_EXTENT_BYTES, ", ",
                        common::constants::store::STORE_MAX_EXTENT_BYTES, "]"));
}

}  // namespace

dstd::Expected<std::uint32_t, dstd::Error> choose_extent_size(
    std::size_t stripe_size, dstd::Optional<std::uint64_t> override_bytes) {
    if (override_bytes.has_value()) {
        const std::uint64_t bytes = *override_bytes;
        if (bytes % EXTENT_ALIGNMENT != 0 ||
            bytes < common::constants::store::STORE_MIN_EXTENT_BYTES ||
            bytes > common::constants::store::STORE_MAX_EXTENT_BYTES)
            return override_range_error();
        return static_cast<std::uint32_t>(bytes);
    }
    if (stripe_size == 0)
        return common::constants::store::STORE_DEFAULT_EXTENT_BYTES;
    if (stripe_size > common::constants::store::STORE_MAX_EXTENT_BYTES)
        return common::constants::store::STORE_MAX_EXTENT_BYTES;
    const std::size_t aligned = stripe_size - stripe_size % EXTENT_ALIGNMENT;
    return static_cast<std::uint32_t>(
        aligned < common::constants::store::STORE_MIN_EXTENT_BYTES
            ? common::constants::store::STORE_MIN_EXTENT_BYTES
            : aligned);
}

dstd::Expected<std::uint32_t, dstd::Error> extent_size_for(
    dstd::StringView dir) {
    dstd::Optional<std::uint64_t> override_bytes;
    if (common::Env::get<dstd::StringView>(
            common::constants::store::STORE_EXTENT_BYTES_ENV)
            .has_value()) {
        // A set value that is not a number is a mistake, not an absent setting.
        auto parsed = common::Env::get<std::uint64_t>(
            common::constants::store::STORE_EXTENT_BYTES_ENV);
        if (!parsed.has_value())
            return override_error("must be a plain number of bytes");
        override_bytes = *parsed;
    }
    const std::string path(dir.data(), dir.size());
    return choose_extent_size(common::lustre_stripe(path).size, override_bytes);
}

}  // namespace dftracer::utils::store::engine
