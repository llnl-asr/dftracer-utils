// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_LITERAL_SET_H
#define DFTRACER_UTILS_DUQL_LITERAL_SET_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

struct hs_database;

namespace dftracer::utils::duql {

/// A multi-literal substring matcher over distinct literals; literal `i` has
/// id `i`. Built on Vectorscan, so it is empty when Vectorscan is off.
class LiteralSet {
   public:
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    static constexpr bool AVAILABLE = true;
#else
    /// False when the build has no Vectorscan: @ref make then always returns
    /// an empty set.
    static constexpr bool AVAILABLE = false;
#endif

    /// Empty: @ref scan always returns null.
    LiteralSet() = default;

    /// Compiles `literals`, which must be distinct. The result is empty when
    /// `literals` is empty, Vectorscan is off or the compile fails.
    static LiteralSet make(dstd::Span<const dstd::StringView> literals);

    explicit operator bool() const { return static_cast<bool>(db_); }

    /// Scans `line` and returns `(size + 63) / 64` words with bit `id` set
    /// for each literal found; the scan stops once every literal is found.
    /// The words are thread-local and stay valid until this thread's next
    /// scan. Returns null when the scan could not run (empty set, a line
    /// longer than `unsigned` max, or a Vectorscan error).
    const std::uint64_t* scan(dstd::StringView line) const;

    /// Number of literals.
    std::size_t size() const { return size_; }

   private:
    dstd::SharedPtr<hs_database> db_;
    std::size_t size_ = 0;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_LITERAL_SET_H
