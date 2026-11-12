// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_BASE64_H
#define DFTRACER_UTILS_DSTD_BASE64_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::dstd {

/// Standard base64 (RFC 4648 alphabet) with `=` padding.
String base64_encode(const void* data, std::size_t len);

/// Inverse of base64_encode; nullopt when `sv` is not padded base64.
Optional<String> base64_decode(StringView sv);

/// base64_decode into `out`, reusing its capacity; false (and `out` partial)
/// when `sv` is not padded base64.
bool base64_decode_into(StringView sv, String& out);

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_BASE64_H
