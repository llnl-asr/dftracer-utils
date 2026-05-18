#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

class StripedWriter final : public ParallelWriter {
   public:
    coro::CoroTask<int> open(std::string path, std::size_t num_workers,
                             bool /*gzip_extension*/,
                             CoroScope* /*scope*/) override {
        path_ = std::move(path);
        ssize_t fd = co_await ::dftracer::utils::io::open(
            path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open striped output: %s",
                                     path_.c_str());
            co_return -1;
        }
        fd_ = static_cast<int>(fd);
        offset_.store(0, std::memory_order_relaxed);
        per_worker_layout_.assign(std::max<std::size_t>(num_workers, 1),
                                  std::vector<MemberSpan>{});
        merged_layout_.clear();
        merged_layout_built_ = false;
        co_return 0;
    }

    coro::CoroTask<int> write_header(ByteView data) override {
        co_return co_await pwrite_all(data);
    }

    coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                    ByteView data) override {
        if (data.size() == 0) co_return 0;
        const auto base =
            offset_.fetch_add(data.size(), std::memory_order_relaxed);
        // Each worker is sequential (one write_chunk in flight per worker), so
        // no lock needed when appending to its own bucket.
        if (worker_idx < per_worker_layout_.size()) {
            per_worker_layout_[worker_idx].push_back({base, data.size()});
        }
        const auto* bytes = reinterpret_cast<const char*>(data.data());
        std::size_t written = 0;
        while (written < data.size()) {
            auto n = co_await ::dftracer::utils::io::pwrite(
                fd_, bytes + written, data.size() - written,
                static_cast<off_t>(base + written));
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR("pwrite failed on %s (offset=%llu)",
                                         path_.c_str(),
                                         static_cast<unsigned long long>(base));
                co_return -1;
            }
            written += static_cast<std::size_t>(n);
        }
        co_return 0;
    }

    coro::CoroTask<int> write_footer(ByteView data) override {
        co_return co_await pwrite_all(data);
    }

    coro::CoroTask<int> close() override {
        if (fd_ < 0) co_return 0;
        auto rc = co_await ::dftracer::utils::io::close(fd_);
        fd_ = -1;
        co_return static_cast<int>(rc);
    }

    std::vector<std::string> output_paths() const override { return {path_}; }

    std::optional<MemberSpan> last_member(
        std::size_t worker_idx) const override {
        if (worker_idx >= per_worker_layout_.size()) return std::nullopt;
        const auto& v = per_worker_layout_[worker_idx];
        if (v.empty()) return std::nullopt;
        return v.back();
    }

    std::span<const MemberSpan> member_layout() const override {
        // Lazy merge after close: per-worker vectors -> single offset-sorted
        // vector. Caller contract: only invoked after `close()`, no concurrent
        // writers.
        if (!merged_layout_built_) {
            std::size_t total = 0;
            for (const auto& v : per_worker_layout_) total += v.size();
            merged_layout_.clear();
            merged_layout_.reserve(total);
            for (const auto& v : per_worker_layout_) {
                merged_layout_.insert(merged_layout_.end(), v.begin(), v.end());
            }
            std::sort(merged_layout_.begin(), merged_layout_.end(),
                      [](const MemberSpan& a, const MemberSpan& b) {
                          return a.offset < b.offset;
                      });
            merged_layout_built_ = true;
        }
        return std::span<const MemberSpan>(merged_layout_);
    }

   private:
    coro::CoroTask<int> pwrite_all(ByteView data) {
        if (data.size() == 0) co_return 0;
        const auto base =
            offset_.fetch_add(data.size(), std::memory_order_relaxed);
        const auto* bytes = reinterpret_cast<const char*>(data.data());
        std::size_t written = 0;
        while (written < data.size()) {
            auto n = co_await ::dftracer::utils::io::pwrite(
                fd_, bytes + written, data.size() - written,
                static_cast<off_t>(base + written));
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR("pwrite failed on %s (offset=%llu)",
                                         path_.c_str(),
                                         static_cast<unsigned long long>(base));
                co_return -1;
            }
            written += static_cast<std::size_t>(n);
        }
        co_return 0;
    }

    std::string path_;
    int fd_ = -1;
    std::atomic<std::uint64_t> offset_{0};
    std::vector<std::vector<MemberSpan>> per_worker_layout_;
    mutable std::vector<MemberSpan> merged_layout_;
    mutable bool merged_layout_built_ = false;
};

}  // namespace

std::unique_ptr<ParallelWriter> make_striped_writer() {
    return std::make_unique<StripedWriter>();
}

}  // namespace dftracer::utils::utilities::fileio::parallel
