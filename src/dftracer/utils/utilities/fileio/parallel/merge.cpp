#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <fcntl.h>
#include <unistd.h>

#include <vector>

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

constexpr std::size_t COPY_BUFFER_BYTES = 256 * 1024;

coro::CoroTask<int> stream_shard_to_fd(int out_fd, const std::string& shard) {
    ssize_t in_fd =
        co_await ::dftracer::utils::io::open(shard.c_str(), O_RDONLY, 0);
    if (in_fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("merge_shards: failed to open shard: %s",
                                 shard.c_str());
        co_return -1;
    }
    std::vector<char> buf(COPY_BUFFER_BYTES);
    while (true) {
        auto n = co_await ::dftracer::utils::io::read(static_cast<int>(in_fd),
                                                      buf.data(), buf.size());
        if (n == 0) break;
        if (n < 0) {
            co_await ::dftracer::utils::io::close(static_cast<int>(in_fd));
            DFTRACER_UTILS_LOG_ERROR("merge_shards: read failed on %s",
                                     shard.c_str());
            co_return -1;
        }
        std::size_t remaining = static_cast<std::size_t>(n);
        const char* ptr = buf.data();
        while (remaining > 0) {
            auto w =
                co_await ::dftracer::utils::io::write(out_fd, ptr, remaining);
            if (w <= 0) {
                co_await ::dftracer::utils::io::close(static_cast<int>(in_fd));
                DFTRACER_UTILS_LOG_ERROR(
                    "merge_shards: write failed while draining %s",
                    shard.c_str());
                co_return -1;
            }
            remaining -= static_cast<std::size_t>(w);
            ptr += w;
        }
    }
    co_await ::dftracer::utils::io::close(static_cast<int>(in_fd));
    co_return 0;
}

}  // namespace

coro::CoroTask<int> merge_shards(const std::string& target,
                                 const std::vector<std::string>& shards) {
    if (shards.empty()) co_return 0;

    ssize_t out_fd = co_await ::dftracer::utils::io::open(
        target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        DFTRACER_UTILS_LOG_ERROR("merge_shards: failed to open target: %s",
                                 target.c_str());
        co_return -1;
    }

    for (const auto& shard : shards) {
        if (co_await stream_shard_to_fd(static_cast<int>(out_fd), shard) != 0) {
            co_await ::dftracer::utils::io::close(static_cast<int>(out_fd));
            co_return -1;
        }
    }

    co_await ::dftracer::utils::io::close(static_cast<int>(out_fd));

    for (const auto& shard : shards) {
        ::unlink(shard.c_str());
    }
    co_return 0;
}

}  // namespace dftracer::utils::utilities::fileio::parallel
