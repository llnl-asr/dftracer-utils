#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FINGERPRINT_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FINGERPRINT_H

#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/dataframe/abi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dftracer::utils::dataframe::detail {

// Incremental FNV-1a (core/common/hash) over the fields a caller feeds in.
// Variable-length values are length prefixed so adjacent fields cannot alias.
class Fingerprint {
   public:
    void bytes(const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h_ ^= b[i];
            h_ *= hash::FNV1A_PRIME;
        }
    }

    template <class T>
    void pod(const T& v) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        unsigned char buf[sizeof(T)];
        std::memcpy(buf, &v, sizeof(T));
        bytes(buf, sizeof(T));
    }

    void str(std::string_view s) noexcept {
        pod(static_cast<std::uint64_t>(s.size()));
        bytes(s.data(), s.size());
    }

    void strs(const std::vector<std::string>& v) noexcept {
        pod(static_cast<std::uint64_t>(v.size()));
        for (const std::string& s : v) str(s);
    }

    void scalar(const dftu_scalar& v) noexcept {
        pod(v.kind);
        if (v.kind == DFTU_SCALAR_TAG_STR)
            str(v.value.s ? std::string_view(v.value.s, v.len)
                          : std::string_view());
        else
            pod(v.value.u);
    }

    std::uint64_t value() const noexcept { return h_; }

   private:
    std::uint64_t h_ = hash::FNV1A_OFFSET_BASIS;
};

}  // namespace dftracer::utils::dataframe::detail

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FINGERPRINT_H
