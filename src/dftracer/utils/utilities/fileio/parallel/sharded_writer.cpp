// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <fcntl.h>
#include <unistd.h>

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

class ShardedWriter final : public ParallelWriter {
   public:
    coro::CoroTask<int> open(dstd::String path, std::size_t worker_count,
                             bool gzip_extension,
                             coro::Scope* /*scope*/) override {
        base_path_ = std::move(path);
        const dstd::String ext = gzip_extension ? ".gz" : "";
        shard_paths_.resize(worker_count);
        shard_fds_.assign(worker_count, -1);
        shard_offsets_.assign(worker_count, 0);
        per_worker_last_.assign(worker_count, dstd::nullopt);
        for (std::size_t i = 0; i < worker_count; ++i) {
            shard_paths_[i] = base_path_ + ".shard_" + dstd::to_string(i) + ext;
            ssize_t fd = co_await ::dftracer::utils::runtime::io::open(
                shard_paths_[i].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                DFTRACER_UTILS_LOG_ERROR("Failed to open shard: {}",
                                         shard_paths_[i]);
                co_return -1;
            }
            shard_fds_[i] = static_cast<int>(fd);
        }
        co_return 0;
    }

    coro::CoroTask<int> write_header(dstd::ByteView data) override {
        if (shard_fds_.empty()) co_return -1;
        auto rc = co_await write_all(shard_fds_.front(), data);
        if (rc == 0) shard_offsets_.front() += data.size();
        co_return rc;
    }

    coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                    dstd::ByteView data) override {
        if (worker_idx >= shard_fds_.size()) co_return -1;
        const auto base = shard_offsets_[worker_idx];
        auto rc = co_await write_all(shard_fds_[worker_idx], data);
        if (rc != 0) co_return rc;
        shard_offsets_[worker_idx] += data.size();
        per_worker_last_[worker_idx] = MemberSpan{base, data.size()};
        co_return 0;
    }

    coro::CoroTask<int> write_footer(dstd::ByteView data) override {
        if (shard_fds_.empty()) co_return -1;
        auto rc = co_await write_all(shard_fds_.back(), data);
        if (rc == 0) shard_offsets_.back() += data.size();
        co_return rc;
    }

    coro::CoroTask<int> close() override {
        int status = 0;
        for (auto& fd : shard_fds_) {
            if (fd < 0) continue;
            auto rc = co_await ::dftracer::utils::runtime::io::close(fd);
            if (rc < 0) status = -1;
            fd = -1;
        }
        co_return status;
    }

    dstd::Vector<dstd::String> output_paths() const override {
        return shard_paths_;
    }

    dstd::Optional<MemberSpan> last_member(
        std::size_t worker_idx) const override {
        if (worker_idx >= per_worker_last_.size()) return dstd::nullopt;
        return per_worker_last_[worker_idx];
    }

    dstd::Vector<std::uint64_t> shard_base_offsets() const override {
        dstd::Vector<std::uint64_t> bases(shard_offsets_.size(), 0);
        std::uint64_t accum = 0;
        for (std::size_t i = 0; i < shard_offsets_.size(); ++i) {
            bases[i] = accum;
            accum += shard_offsets_[i];
        }
        return bases;
    }

   private:
    coro::CoroTask<int> write_all(int fd, dstd::ByteView data) {
        if (data.size() == 0) co_return 0;
        const auto* bytes = reinterpret_cast<const char*>(data.data());
        std::size_t written = 0;
        while (written < data.size()) {
            auto n = co_await ::dftracer::utils::runtime::io::write(
                fd, bytes + written, data.size() - written);
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR("write failed on shard fd={}", fd);
                co_return -1;
            }
            written += static_cast<std::size_t>(n);
        }
        co_return 0;
    }

    dstd::String base_path_;
    dstd::Vector<dstd::String> shard_paths_;
    dstd::Vector<int> shard_fds_;
    dstd::Vector<std::uint64_t> shard_offsets_;
    dstd::Vector<dstd::Optional<MemberSpan>> per_worker_last_;
};

}  // namespace

dstd::UniquePtr<ParallelWriter> make_sharded_writer() {
    return dstd::make_unique<ShardedWriter>();
}

dstd::UniquePtr<ParallelWriter> make_writer(const WriterConfig& cfg) {
    if (cfg.layout == FileLayout::Sharded) return make_sharded_writer();
    // Padded striped needs gzip and a large-enough stripe to guarantee a
    // compressed flush fits one slot. Below the minimum, fall back to the
    // atomic-byte-offset writer.
    LayoutInfo info{cfg.layout, common::FilesystemKind::Unknown,
                    cfg.stripe_size, 0};
    if (uses_padded_layout(info, cfg.gzip)) {
        return make_padded_striped_writer(cfg.stripe_size);
    }
    return make_striped_writer();
}

ConfiguredWriter make_writer_for_path(const WriterRequest& req) {
    LayoutInfo info = detect_layout(req.path);
    // A striped layout with no usable stripe size cannot slot workers by
    // offset; degrade to sharded so writes still land correctly.
    if (info.layout == FileLayout::Striped && info.stripe_size == 0) {
        info.layout = FileLayout::Sharded;
    }
    const bool padded = uses_padded_layout(info, req.gzip);
    WriterSizing sizing = compute_writer_sizing(
        info, req.baseline_workers, req.default_flush_bytes,
        req.buffer_headroom_bytes, padded);
    WriterConfig cfg;
    cfg.layout = info.layout;
    cfg.stripe_size = info.stripe_size;
    cfg.gzip = req.gzip;
    return ConfiguredWriter{make_writer(cfg), info, sizing};
}

}  // namespace dftracer::utils::utilities::fileio::parallel
