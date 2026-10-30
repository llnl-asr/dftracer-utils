#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/store/scope_filter.h>

#include <algorithm>

namespace dftracer::utils::store {
namespace {

void mix_bytes(std::uint64_t& hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= hash::FNV1A_PRIME;
    }
}

void mix_u32(std::uint64_t& hash, std::uint32_t value) {
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value),
                                   static_cast<std::uint8_t>(value >> 8U),
                                   static_cast<std::uint8_t>(value >> 16U),
                                   static_cast<std::uint8_t>(value >> 24U)};
    mix_bytes(hash, bytes, sizeof(bytes));
}

/// Each string goes in with its length first, so no two keys mix the same
/// bytes.
template <class Path>
std::uint64_t hash_key(dstd::StringView name, std::uint32_t version,
                       ScopeKind kind, dstd::Span<const Path> paths) {
    std::uint64_t hash = hash::FNV1A_OFFSET_BASIS;
    mix_u32(hash, static_cast<std::uint32_t>(name.size()));
    mix_bytes(hash, name.data(), name.size());
    mix_u32(hash, version);
    mix_u32(hash, static_cast<std::uint32_t>(kind));
    mix_u32(hash, static_cast<std::uint32_t>(paths.size()));
    for (const Path& path : paths) {
        mix_u32(hash, static_cast<std::uint32_t>(path.size()));
        mix_bytes(hash, path.data(), path.size());
    }
    return hash::fnv1a_mix(hash);
}

std::uint64_t probe_bit(std::uint64_t hash, std::size_t probe,
                        std::uint64_t bits) {
    const std::uint64_t step = hash::splitmix64(hash) | 1U;
    return (hash + probe * step) % bits;
}

}  // namespace

std::uint64_t scope_hash(dstd::StringView name, std::uint32_t version,
                         ScopeKind kind,
                         dstd::Span<const dstd::StringView> paths) {
    return hash_key(name, version, kind, paths);
}

std::uint64_t scope_hash(dstd::StringView name, std::uint32_t version,
                         ScopeKind kind, dstd::Span<const dstd::String> paths) {
    return hash_key(name, version, kind, paths);
}

dstd::Vector<std::uint8_t> build_scope_filter(
    dstd::Span<const std::uint64_t> hashes) {
    if (hashes.empty()) return {};
    const std::size_t wanted =
        (hashes.size() * SCOPE_FILTER_BITS_PER_SCOPE + 63U) / 64U * 8U;
    const std::size_t bytes = std::min(wanted, SCOPE_FILTER_MAX_BYTES);
    dstd::Vector<std::uint8_t> filter(bytes, std::uint8_t{0});
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes) * 8U;
    for (const std::uint64_t hash : hashes) {
        for (std::size_t probe = 0; probe < SCOPE_FILTER_PROBES; ++probe) {
            const std::uint64_t bit = probe_bit(hash, probe, bits);
            filter[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
        }
    }
    return filter;
}

bool scope_filter_may_hold(ByteView filter, std::uint64_t hash) {
    if (filter.empty() || filter.size() % 8U != 0) return true;
    const auto* data = reinterpret_cast<const std::uint8_t*>(filter.data());
    const std::uint64_t bits = static_cast<std::uint64_t>(filter.size()) * 8U;
    for (std::size_t probe = 0; probe < SCOPE_FILTER_PROBES; ++probe) {
        const std::uint64_t bit = probe_bit(hash, probe, bits);
        if ((data[bit / 8U] & (1U << (bit % 8U))) == 0) return false;
    }
    return true;
}

}  // namespace dftracer::utils::store
