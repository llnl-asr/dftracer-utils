#ifndef DFTRACER_UTILS_CORE_COMMON_EXTERNAL_SORT_H
#define DFTRACER_UTILS_CORE_COMMON_EXTERNAL_SORT_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/spill_file.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils {

/// Sorts fixed-size records that do not fit in memory. Records are buffered up
/// to `budget_bytes`; a full buffer is sorted and written as a run to a
/// SpillFile, and drain() merges the runs. Input that fits the budget never
/// touches disk. The merge reads each run through a buffer sized so that all of
/// them together stay within `budget_bytes` (down to 64 records per run).
/// `Less` must be a strict total order for a deterministic result; equal
/// records come out run by run. A record has no padding bytes (checked at
/// compile time). Not thread-safe. Errors are SpillFile's.
template <class Rec, class Less>
class ExternalSorter {
    static_assert(std::is_trivially_copyable_v<Rec>);
    static_assert(std::has_unique_object_representations_v<Rec>,
                  "records go to disk as raw bytes, so they must have no "
                  "padding; add an explicit zeroed pad member");

   public:
    ExternalSorter(Less less, std::uint64_t budget_bytes)
        : less_(std::move(less)),
          cap_(std::max<std::size_t>(1, budget_bytes / sizeof(Rec))) {}

    Result<void> add(const Rec& r) {
        if (buf_.size() == buf_.capacity())
            buf_.reserve(std::min(
                cap_, std::max<std::size_t>(1024, buf_.capacity() * 2)));
        buf_.push_back(r);
        if (buf_.size() >= cap_) return flush();
        return {};
    }

    std::size_t runs() const noexcept { return runs_.size(); }

    /// Calls `f(const Rec&)` for every record in order. One-shot.
    template <class F>
    Result<void> drain(F&& f) {
        if (runs_.empty()) {
            std::sort(buf_.begin(), buf_.end(), less_);
            for (const Rec& r : buf_) f(r);
            std::vector<Rec>().swap(buf_);
            return {};
        }
        if (!buf_.empty()) {
            auto flushed = flush();
            if (!flushed) return flushed;
        }
        std::vector<Rec>().swap(buf_);
        return merge(f);
    }

   private:
    struct Run {
        std::uint64_t offset;
        std::uint64_t count;
    };

    struct Reader {
        std::vector<Rec> buf;
        std::size_t pos = 0;
        std::uint64_t next = 0;
    };

    Result<void> flush() {
        std::sort(buf_.begin(), buf_.end(), less_);
        if (!file_) {
            auto created = SpillFile::create();
            if (!created) return unexpected(created.error());
            file_ = std::move(*created);
        }
        auto at = file_->append(buf_.data(), buf_.size() * sizeof(Rec));
        if (!at) return unexpected(at.error());
        runs_.push_back({*at, buf_.size()});
        buf_.clear();
        return {};
    }

    /// 1 when a chunk was read, 0 at the end of the run.
    Result<bool> refill(std::size_t run, Reader& rd, std::size_t chunk) {
        const Run& r = runs_[run];
        if (rd.next >= r.count) return false;
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk, r.count - rd.next));
        rd.buf.resize(n);
        auto got = file_->read(r.offset + rd.next * sizeof(Rec), rd.buf.data(),
                               n * sizeof(Rec));
        if (!got) return unexpected(got.error());
        rd.next += n;
        rd.pos = 0;
        return true;
    }

    template <class F>
    Result<void> merge(F& f) {
        const std::size_t chunk =
            std::max<std::size_t>(64, cap_ / runs_.size());
        std::vector<Reader> readers(runs_.size());
        auto after = [&](std::size_t a, std::size_t b) {
            const Rec& x = readers[a].buf[readers[a].pos];
            const Rec& y = readers[b].buf[readers[b].pos];
            if (less_(x, y)) return false;
            if (less_(y, x)) return true;
            return a > b;
        };
        std::priority_queue<std::size_t, std::vector<std::size_t>,
                            decltype(after)>
            heap(after);
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            auto more = refill(i, readers[i], chunk);
            if (!more) return unexpected(more.error());
            if (*more) heap.push(i);
        }
        while (!heap.empty()) {
            const std::size_t i = heap.top();
            heap.pop();
            Reader& rd = readers[i];
            f(static_cast<const Rec&>(rd.buf[rd.pos]));
            if (++rd.pos < rd.buf.size()) {
                heap.push(i);
                continue;
            }
            auto more = refill(i, rd, chunk);
            if (!more) return unexpected(more.error());
            if (*more) heap.push(i);
        }
        return {};
    }

    Less less_;
    std::size_t cap_;
    std::vector<Rec> buf_;
    std::vector<Run> runs_;
    std::unique_ptr<SpillFile> file_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_EXTERNAL_SORT_H
