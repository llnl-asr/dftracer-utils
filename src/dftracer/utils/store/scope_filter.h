#ifndef DFTRACER_UTILS_STORE_SCOPE_FILTER_H
#define DFTRACER_UTILS_STORE_SCOPE_FILTER_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/record.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::store {

/// The filter bits per scope of a file: about 1% of the scopes a file does not
/// hold pass it.
inline constexpr std::size_t SCOPE_FILTER_BITS_PER_SCOPE = 10;
/// The bits one scope sets and a lookup tests.
inline constexpr std::size_t SCOPE_FILTER_PROBES = 7;
/// The most bytes one filter takes, so the tail of a file stays one read: a
/// file of more scopes than this fits lets more absent scopes pass.
inline constexpr std::size_t SCOPE_FILTER_MAX_BYTES = 16384;

/// The hash of one directory key: namespace name, format version, scope kind
/// and paths. The same key gives the same hash on every host.
std::uint64_t scope_hash(dstd::StringView name, std::uint32_t version,
                         ScopeKind kind,
                         dstd::Span<const dstd::StringView> paths);
std::uint64_t scope_hash(dstd::StringView name, std::uint32_t version,
                         ScopeKind kind, dstd::Span<const dstd::String> paths);

/// A Bloom filter over the scope hashes of one file:
/// SCOPE_FILTER_BITS_PER_SCOPE bits per hash, a multiple of 8 bytes, at most
/// SCOPE_FILTER_MAX_BYTES. Empty for no hash.
dstd::Vector<std::uint8_t> build_scope_filter(
    dstd::Span<const std::uint64_t> hashes);

/// False only when the file whose filter is `filter` cannot hold the scope of
/// `hash`. An empty filter, or one that is not a multiple of 8 bytes, holds
/// every scope.
bool scope_filter_may_hold(ByteView filter, std::uint64_t hash);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_SCOPE_FILTER_H
