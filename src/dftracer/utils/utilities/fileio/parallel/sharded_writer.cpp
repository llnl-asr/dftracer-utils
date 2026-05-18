#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <vector>

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

class ShardedWriter final : public ParallelWriter {
   public:
    coro::CoroTask<int> open(std::string path, std::size_t num_workers,
                             bool gzip_extension,
                             CoroScope* /*scope*/) override {
        base_path_ = std::move(path);
        const std::string ext = gzip_extension ? ".gz" : "";
        shard_paths_.resize(num_workers);
        shard_fds_.assign(num_workers, -1);
        shard_offsets_.assign(num_workers, 0);
        per_worker_last_.assign(num_workers, std::nullopt);
        for (std::size_t i = 0; i < num_workers; ++i) {
            shard_paths_[i] = base_path_ + ".shard_" + std::to_string(i) + ext;
            ssize_t fd = co_await ::dftracer::utils::io::open(
                shard_paths_[i].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                DFTRACER_UTILS_LOG_ERROR("Failed to open shard: %s",
                                         shard_paths_[i].c_str());
                co_return -1;
            }
            shard_fds_[i] = static_cast<int>(fd);
        }
        co_return 0;
    }

    coro::CoroTask<int> write_header(ByteView data) override {
        if (shard_fds_.empty()) co_return -1;
        auto rc = co_await write_all(shard_fds_.front(), data);
        if (rc == 0) shard_offsets_.front() += data.size();
        co_return rc;
    }

    coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                    ByteView data) override {
        if (worker_idx >= shard_fds_.size()) co_return -1;
        const auto base = shard_offsets_[worker_idx];
        auto rc = co_await write_all(shard_fds_[worker_idx], data);
        if (rc != 0) co_return rc;
        shard_offsets_[worker_idx] += data.size();
        per_worker_last_[worker_idx] = MemberSpan{base, data.size()};
        co_return 0;
    }

    coro::CoroTask<int> write_footer(ByteView data) override {
        if (shard_fds_.empty()) co_return -1;
        auto rc = co_await write_all(shard_fds_.back(), data);
        if (rc == 0) shard_offsets_.back() += data.size();
        co_return rc;
    }

    coro::CoroTask<int> close() override {
        int status = 0;
        for (auto& fd : shard_fds_) {
            if (fd < 0) continue;
            auto rc = co_await ::dftracer::utils::io::close(fd);
            if (rc < 0) status = -1;
            fd = -1;
        }
        co_return status;
    }

    std::vector<std::string> output_paths() const override {
        return shard_paths_;
    }

    std::optional<MemberSpan> last_member(
        std::size_t worker_idx) const override {
        if (worker_idx >= per_worker_last_.size()) return std::nullopt;
        return per_worker_last_[worker_idx];
    }

    std::vector<std::uint64_t> shard_base_offsets() const override {
        std::vector<std::uint64_t> bases(shard_offsets_.size(), 0);
        std::uint64_t accum = 0;
        for (std::size_t i = 0; i < shard_offsets_.size(); ++i) {
            bases[i] = accum;
            accum += shard_offsets_[i];
        }
        return bases;
    }

   private:
    coro::CoroTask<int> write_all(int fd, ByteView data) {
        if (data.size() == 0) co_return 0;
        const auto* bytes = reinterpret_cast<const char*>(data.data());
        std::size_t written = 0;
        while (written < data.size()) {
            auto n = co_await ::dftracer::utils::io::write(
                fd, bytes + written, data.size() - written);
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR("write failed on shard fd=%d", fd);
                co_return -1;
            }
            written += static_cast<std::size_t>(n);
        }
        co_return 0;
    }

    std::string base_path_;
    std::vector<std::string> shard_paths_;
    std::vector<int> shard_fds_;
    std::vector<std::uint64_t> shard_offsets_;
    std::vector<std::optional<MemberSpan>> per_worker_last_;
};

}  // namespace

std::unique_ptr<ParallelWriter> make_sharded_writer() {
    return std::make_unique<ShardedWriter>();
}

std::unique_ptr<ParallelWriter> make_writer(const WriterConfig& cfg) {
    if (cfg.layout == FileLayout::SHARDED) return make_sharded_writer();
    // Padded striped needs gzip and a large-enough stripe to guarantee a
    // compressed flush fits one slot. Below the minimum, fall back to the
    // atomic-byte-offset writer.
    if (cfg.gzip && cfg.stripe_size >= MIN_PADDED_STRIPE_BYTES) {
        return make_padded_striped_writer(cfg.stripe_size);
    }
    return make_striped_writer();
}

}  // namespace dftracer::utils::utilities::fileio::parallel
