#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DISTINCT_SKETCH_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DISTINCT_SKETCH_H

// HyperLogLog++: a mergeable distinct-count sketch.
//
// Based on Heule, Nunkesser and Hall (EDBT 2013), "HyperLogLog in Practice":
// https://research.google/pubs/pub40671/
//
// Over plain HyperLogLog it adds the two things that matter for counting a
// dimension left out of a grouping key:
//   - a sparse representation, so a group touching few distinct values costs
//     bytes rather than a full register array, and is counted *exactly*;
//   - 64-bit hashing, which removes HLL's large-range correction.
// Both forms merge (sparse pairs union, dense registers take the maximum), so
// per-file and per-worker sketches combine without keeping the values.

#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

/// `Precision` sets the register count (2^Precision) and so the accuracy:
/// the standard error is about 1.04/sqrt(registers) once dense. Sketches only
/// merge with others of the same precision.
template <std::uint32_t Precision = 13>
class BasicDistinctSketch {
   public:
    // Default 8192 registers: 8 KB dense, ~1.15% standard error once dense.
    static constexpr std::uint32_t PRECISION = Precision;
    static constexpr std::uint32_t REGISTERS = 1u << PRECISION;
    // Stay sparse (and exact) while the encoded pairs cost less than dense.
    static constexpr std::size_t SPARSE_LIMIT = REGISTERS / 4;

    void add(std::string_view value) {
        if (!value.empty()) add_hash(hash64(value));
    }

    /// For values that are already a well-distributed hash (dftracer's fhash
    /// and hhash are), skipping the extra hashing.
    void add_hash(std::uint64_t h) {
        if (dense_.empty()) {
            sparse_.push_back(h);
            if (sparse_.size() >= SPARSE_LIMIT) compact_sparse();
            return;
        }
        apply_dense(h);
    }

    void merge_from(const BasicDistinctSketch& other) {
        if (other.dense_.empty() && other.sparse_.empty()) return;
        if (!other.dense_.empty()) {
            to_dense();
            for (std::uint32_t i = 0; i < REGISTERS; ++i)
                dense_[i] = std::max(dense_[i], other.dense_[i]);
        }
        for (std::uint64_t h : other.sparse_) add_hash(h);
    }

    bool empty() const { return dense_.empty() && sparse_.empty(); }

    std::uint64_t estimate() const {
        if (dense_.empty()) {
            // Sparse: the distinct hashes are still held, so this is exact.
            auto uniq = sparse_;
            std::sort(uniq.begin(), uniq.end());
            uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
            return uniq.size();
        }
        double sum = 0;
        std::uint32_t zeros = 0;
        for (std::uint32_t i = 0; i < REGISTERS; ++i) {
            sum += std::ldexp(1.0, -static_cast<int>(dense_[i]));
            if (dense_[i] == 0) ++zeros;
        }
        const double m = REGISTERS;
        double est = ALPHA * m * m / sum;
        // Linear counting is the better estimator while registers are empty;
        // with 64-bit hashes no large-range correction is needed.
        if (zeros > 0) {
            double linear = m * std::log(m / static_cast<double>(zeros));
            if (linear <= LINEAR_THRESHOLD) est = linear;
        }
        return static_cast<std::uint64_t>(est + 0.5);
    }

    /// Dense registers, empty while the sketch is sparse. Serialization keeps
    /// both forms so a sparse sketch stays small (and exact) on disk.
    const std::vector<std::uint8_t>& dense_registers() const { return dense_; }
    const std::vector<std::uint64_t>& sparse_hashes() const { return sparse_; }

    void set_dense_registers(std::vector<std::uint8_t> regs) {
        dense_ = std::move(regs);
        sparse_.clear();
    }
    void set_sparse_hashes(std::vector<std::uint64_t> hashes) {
        sparse_ = std::move(hashes);
        dense_.clear();
    }

   private:
    static constexpr double ALPHA = 0.7213 / (1.0 + 1.079 / REGISTERS);
    // Below ~2.5m the linear-counting estimator is the more accurate one.
    static constexpr double LINEAR_THRESHOLD = 2.5 * REGISTERS;

    void apply_dense(std::uint64_t h) {
        auto idx = static_cast<std::uint32_t>(h >> (64 - PRECISION));
        std::uint64_t rest = (h << PRECISION) | (1ULL << (PRECISION - 1));
        auto rank = static_cast<std::uint8_t>(leading_zeros(rest) + 1);
        if (rank > dense_[idx]) dense_[idx] = rank;
    }

    void compact_sparse() {
        std::sort(sparse_.begin(), sparse_.end());
        sparse_.erase(std::unique(sparse_.begin(), sparse_.end()),
                      sparse_.end());
        // Only promote once the distinct values really outgrow the sparse form;
        // duplicates alone should not cost accuracy.
        if (sparse_.size() >= SPARSE_LIMIT) to_dense();
    }

    void to_dense() {
        if (!dense_.empty()) return;
        dense_.assign(REGISTERS, 0);
        for (std::uint64_t h : sparse_) apply_dense(h);
        sparse_.clear();
        sparse_.shrink_to_fit();
    }

    static std::uint32_t leading_zeros(std::uint64_t v) {
        if (v == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<std::uint32_t>(__builtin_clzll(v));
#else
        std::uint32_t n = 0;
        while ((v & (1ULL << 63)) == 0) {
            v <<= 1;
            ++n;
        }
        return n;
#endif
    }

    static std::uint64_t hash64(std::string_view s) {
        return hash::fnv1a_mix(hash::fnv1a_hash(s));
    }

    std::vector<std::uint64_t> sparse_;
    std::vector<std::uint8_t> dense_;
};

using DistinctSketch = BasicDistinctSketch<>;

}  // namespace dftracer::utils::utilities::common::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_DISTINCT_SKETCH_H
