// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_COVERAGE_H
#define DFTRACER_UTILS_TRACE_VIEWS_COVERAGE_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::trace::views::detail {

/// The scan units an artifact has been built over, keyed by (file, checkpoint).
///
/// An absent unit means "not covered", which callers must read as "scan it",
/// never as "nothing matches": a pruning artifact that claims coverage it does
/// not have makes a query skip data that matched, which is a wrong answer
/// rather than a slow one.
class CoverageSet {
   public:
    static std::string key(dstd::StringView file, std::uint64_t checkpoint) {
        std::string k(file);
        k += '\x1f';
        k += std::to_string(checkpoint);
        return k;
    }

    /// Whole-file unit, for artifacts a single member cannot establish. The
    /// marker is not a digit, so it cannot collide with a checkpoint key.
    static std::string file_key(dstd::StringView file) {
        std::string k(file);
        k += '\x1f';
        k += '*';
        return k;
    }

    void add(dstd::StringView file, std::uint64_t checkpoint) {
        units_.insert(key(file, checkpoint));
    }

    bool covers(dstd::StringView file, std::uint64_t checkpoint) const {
        return units_.count(key(file, checkpoint)) != 0;
    }

    void add_file(dstd::StringView file) { units_.insert(file_key(file)); }

    bool covers_file(dstd::StringView file) const {
        return units_.count(file_key(file)) != 0;
    }

    bool empty() const noexcept { return units_.empty(); }
    std::size_t size() const noexcept { return units_.size(); }

    void add_raw(std::string unit) { units_.insert(std::move(unit)); }

    /// Take over `other`'s units, leaving it empty. The set is not
    /// thread-safe, so a parallel scan gives each worker its own and folds
    /// them together after the fan-out joins.
    void absorb(CoverageSet&& other) {
        if (units_.empty()) {
            units_ = std::move(other.units_);
        } else {
            for (auto& u : other.units_) units_.insert(std::move(u));
        }
        other.units_.clear();
    }

   private:
    std::unordered_set<std::string> units_;
};

/// Coverage accumulated by one scan, published only when the scan completes.
///
/// A scan can stop mid-unit (a row limit, a cancelled request), and the
/// artifacts it wrote for that unit are incomplete. Publishing is therefore
/// explicit: `seal()` moves the accumulated units into the target set, and
/// anything else - `abandon()`, an exception, an early return - discards them.
/// Mark a unit only once its artifacts are durable, so a crash leaves orphan
/// artifacts rather than a coverage claim with no data behind it.
class PendingCoverage {
   public:
    explicit PendingCoverage(CoverageSet& target) : target_(&target) {}

    PendingCoverage(const PendingCoverage&) = delete;
    PendingCoverage& operator=(const PendingCoverage&) = delete;

    ~PendingCoverage() { abandon(); }

    /// Record that `checkpoint` of `file` was scanned end to end.
    void mark_complete(dstd::StringView file, std::uint64_t checkpoint) {
        pending_.insert(CoverageSet::key(file, checkpoint));
    }

    /// Record that every checkpoint of `file` was scanned end to end. Required
    /// by artifacts a single member cannot establish: metadata that defines a
    /// hash is emitted at the hash's first use, so a later member referencing
    /// it carries no definition of its own.
    void mark_file_complete(dstd::StringView file) {
        pending_.insert(CoverageSet::file_key(file));
    }

    void seal() {
        for (const auto& u : pending_) target_->add_raw(u);
        pending_.clear();
        sealed_ = true;
    }

    void abandon() noexcept { pending_.clear(); }

    bool sealed() const noexcept { return sealed_; }
    std::size_t pending() const noexcept { return pending_.size(); }

   private:
    CoverageSet* target_;
    std::unordered_set<std::string> pending_;
    bool sealed_ = false;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_COVERAGE_H
