#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_PAGE_SIZE_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_PAGE_SIZE_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine {

/// The page size P of a new data file, a multiple of 4096 in
/// [STORE_MIN_PAGE_BYTES, STORE_MAX_PAGE_BYTES].
///
/// A set `override_bytes` wins and is rejected with InvalidArgument when it is
/// not a multiple of 4096 or out of that range. Without one, `stripe_size`
/// (0 when unknown) becomes P, rounded down to a multiple of 4096 and capped at
/// STORE_MAX_PAGE_BYTES; 0 gives STORE_DEFAULT_PAGE_BYTES.
dstd::Expected<std::uint32_t, dstd::Error> choose_page_size(
    std::size_t stripe_size, dstd::Optional<std::uint64_t> override_bytes);

/// choose_page_size for the data files in `dir`: the Lustre stripe size of
/// `dir` and the DFTRACER_UTILS_STORE_PAGE_BYTES override, both read here. An
/// unreadable stripe is unknown, not an error; a set but unparsable or
/// out-of-range override is InvalidArgument. Never throws.
dstd::Expected<std::uint32_t, dstd::Error> page_size_for(dstd::StringView dir);

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_PAGE_SIZE_H
