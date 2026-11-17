// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_ARRIVAL_FOLD_H
#define DFTRACER_UTILS_COMM_BACKEND_ARRIVAL_FOLD_H

#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

/**
 * @brief The running fold of the contributions of one slot, in arrival order.
 *
 * A provider calls the @c _locked members under its own mutex and
 * @ref apply outside it, so a user function never runs under that mutex. One
 * caller at a time is the folder: it takes the contributions that are ready,
 * folds them outside the lock, and takes more until none is left. A
 * contribution that arrives while another caller folds is queued for that
 * folder, so none is lost and none is folded twice. The merge function is set
 * once, by the member the provider picks, and a contribution that arrives
 * before it waits, except the first, which only starts the fold.
 */
class ArrivalFold {
   public:
    /// One contribution: owned bytes, or a view that a keep-alive holds.
    struct Held {
        dstd::Vector<std::byte> owned;
        dstd::Span<const std::byte> view;
        dstd::SharedPtr<const void> keep;
        bool is_owned = false;

        static Held borrowed(dstd::Span<const std::byte> view,
                             dstd::SharedPtr<const void> keep) {
            Held held;
            held.view = view;
            held.keep = std::move(keep);
            return held;
        }

        static Held owning(dstd::Vector<std::byte> bytes) {
            Held held;
            held.owned = std::move(bytes);
            held.is_owned = true;
            return held;
        }

        dstd::Span<const std::byte> bytes() const noexcept {
            return is_owned
                       ? dstd::Span<const std::byte>(owned.data(), owned.size())
                       : view;
        }
    };

    /// What one folder folds before it comes back for more.
    struct Batch {
        dstd::Vector<Held> items;
        /// The first item of the batch starts the fold instead of merging.
        bool starts = false;
    };

    /// Sets the merge, once. Called under the provider mutex.
    void set_combine_locked(Combine combine) {
        if (!combine_ && combine) combine_ = std::move(combine);
    }

    /// Ends the fold: the queue is dropped and no merge is taken from it. A
    /// merge that runs may finish, and so may one that passed its check in
    /// @ref apply just before this call: the check and the merge are not one
    /// step, because holding a lock across a user function could deadlock. The
    /// bytes such a merge reads stay valid, since the folder owns its batch.
    /// Called under the provider mutex, by a provider that dropped, released
    /// or ended the slot.
    void cancel_locked() {
        {
            std::lock_guard<std::mutex> lock(gate_);
            cancelled_ = true;
        }
        pending_.clear();
    }

    /// Adds @p held. Gives true when the caller became the folder, with its
    /// work in @p out. Called under the provider mutex.
    bool take_locked(Held held, Batch& out) {
        if (is_cancelled()) return false;
        const bool ready = static_cast<bool>(combine_) || !started_;
        if (folding_ || !ready) {
            pending_.push_back(std::move(held));
            return false;
        }
        folding_ = true;
        out.starts = !started_;
        started_ = true;
        if (combine_) drain_locked(out);
        out.items.push_back(std::move(held));
        return true;
    }

    /// After a fold: takes the next batch, or ends the folder's turn and gives
    /// false. Called under the provider mutex.
    bool refill_locked(Batch& out) {
        out.items.clear();
        out.starts = false;
        if (is_cancelled()) pending_.clear();
        if (!combine_ || pending_.empty()) {
            folding_ = false;
            return false;
        }
        drain_locked(out);
        return true;
    }

    /// True while a folder works or a queued contribution can still be taken.
    /// A queue that waits for a merge no member supplied does not count.
    bool busy_locked() const noexcept {
        return folding_ || (combine_ && !pending_.empty());
    }

    /// Folds @p batch. Only the folder calls it, and never under the provider
    /// mutex.
    void apply(Batch& batch) {
        for (Held& held : batch.items) {
            const dstd::Span<const std::byte> bytes = held.bytes();
            if (batch.starts) {
                folded_.assign(bytes.begin(), bytes.end());
                batch.starts = false;
            } else if (bytes.size() == folded_.size()) {
                // A member of another size is refused by the caller, which
                // reads the counts of the records.
                if (is_cancelled()) return;
                combine_(folded_.data(), bytes.data(), folded_.size());
            }
        }
    }

    /// The fold, once the fold is not busy.
    dstd::Vector<std::byte> take_folded() { return std::move(folded_); }

   private:
    bool is_cancelled() {
        std::lock_guard<std::mutex> lock(gate_);
        return cancelled_;
    }

    void drain_locked(Batch& out) {
        for (Held& held : pending_) out.items.push_back(std::move(held));
        pending_.clear();
    }

    dstd::Vector<Held> pending_;
    dstd::Vector<std::byte> folded_;
    Combine combine_;
    bool started_ = false;
    bool folding_ = false;
    /// Set once by @ref cancel_locked; read outside the provider mutex by the
    /// folder before each merge.
    std::mutex gate_;
    bool cancelled_ = false;
};

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_ARRIVAL_FOLD_H
