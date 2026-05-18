#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::fileio::parallel {

namespace {

// FEXTRA-only gzip padding member layout (RFC 1952):
//   hdr(10) + xlen(2) + xlen FEXTRA bytes + empty_stored_block(5) + trailer(8)
constexpr std::size_t PAD_MEMBER_FIXED_OVERHEAD = 25;
constexpr std::size_t PAD_MEMBER_MAX_SIZE = PAD_MEMBER_FIXED_OVERHEAD + 65535;

// Worst-case channel depth. One per worker is usually enough; we allow a
// little slack so bursts don't block producers.
constexpr std::size_t CHUNK_CHANNEL_CAPACITY = 64;

// Bound on concurrent stripe pwrites. Each stripe routes to a different OST
// (offset = (idx+1) * stripe_size, OST = (idx+1) % stripe_count), so this
// keeps multiple OSTs busy without unbounded outstanding I/O.
constexpr std::size_t MAX_INFLIGHT_PWRITES = 16;

// Append a FEXTRA-only padding member (decompresses to zero bytes).
void append_padding_member(std::vector<std::uint8_t>& out, std::uint16_t xlen) {
    const std::size_t start = out.size();
    out.resize(start + PAD_MEMBER_FIXED_OVERHEAD + xlen);
    std::uint8_t* p = out.data() + start;

    p[0] = 0x1f;
    p[1] = 0x8b;
    p[2] = 0x08;                    // CM = deflate
    p[3] = 0x04;                    // FLG = FEXTRA
    p[4] = p[5] = p[6] = p[7] = 0;  // MTIME
    p[8] = 0;                       // XFL
    p[9] = 0xff;                    // OS = unknown

    p[10] = static_cast<std::uint8_t>(xlen & 0xff);
    p[11] = static_cast<std::uint8_t>((xlen >> 8) & 0xff);
    std::memset(p + 12, 0, xlen);

    // Empty deflate stored block: BFINAL=1, BTYPE=00, LEN=0, NLEN=0xffff.
    p[12 + xlen + 0] = 0x01;
    p[12 + xlen + 1] = 0x00;
    p[12 + xlen + 2] = 0x00;
    p[12 + xlen + 3] = 0xff;
    p[12 + xlen + 4] = 0xff;

    // Trailer: CRC32=0, ISIZE=0.
    std::memset(p + 12 + xlen + 5, 0, 8);
}

// Fill `out` with padding members until its size reaches exactly stripe_size.
void pad_to_stripe(std::vector<std::uint8_t>& out, std::size_t stripe_size) {
    while (stripe_size - out.size() >= PAD_MEMBER_MAX_SIZE) {
        append_padding_member(out, 65535);
    }
    std::size_t remaining = stripe_size - out.size();
    if (remaining >= PAD_MEMBER_FIXED_OVERHEAD) {
        append_padding_member(out, static_cast<std::uint16_t>(
                                       remaining - PAD_MEMBER_FIXED_OVERHEAD));
    }
    // < 25 bytes leftover is dropped; next slot still starts at the
    // declared stripe offset.
}

class PaddedStripedWriter : public ParallelWriter {
   public:
    struct Chunk {
        std::vector<std::uint8_t> data;
        std::shared_ptr<coro::Channel<MemberSpan>> ack;
    };

    explicit PaddedStripedWriter(std::size_t stripe_size)
        : stripe_size_(stripe_size) {}

    coro::CoroTask<int> open(std::string path, std::size_t num_workers,
                             bool /*gzip_extension*/,
                             CoroScope* scope) override {
        if (!scope) {
            DFTRACER_UTILS_LOG_ERROR(
                "PaddedStripedWriter requires a CoroScope to spawn its packer");
            co_return -1;
        }
        path_ = std::move(path);
        ssize_t fd = co_await ::dftracer::utils::io::open(
            path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open padded output: %s",
                                     path_.c_str());
            co_return -1;
        }
        fd_ = static_cast<int>(fd);
        next_stripe_idx_.store(0, std::memory_order_relaxed);
        per_worker_last_.assign(num_workers, std::nullopt);

        // Valid-gzip placeholder so callers that skip write_header still
        // produce a file gunzip can walk.
        std::vector<std::uint8_t> pad;
        pad.reserve(stripe_size_);
        pad_to_stripe(pad, stripe_size_);
        if (co_await pwrite_bytes(pad.data(), pad.size(), 0) != 0) {
            co_return -1;
        }

        // Set up the chunk channel + N pre-registered producers (one per
        // worker). The packer exits once all producers are released.
        channel_ = coro::make_channel<Chunk>(CHUNK_CHANNEL_CAPACITY);
        producers_.reserve(num_workers);
        for (std::size_t i = 0; i < num_workers; ++i) {
            producers_.emplace_back(channel_->producer());
        }

        packer_future_ =
            scope->spawn([this, consumer = channel_->consumer()](
                             CoroScope& s) mutable -> coro::CoroTask<int> {
                co_return co_await run_packer(s, std::move(consumer));
            });
        co_return 0;
    }

    coro::CoroTask<int> write_header(ByteView data) override {
        if (data.size() + PAD_MEMBER_FIXED_OVERHEAD > stripe_size_) {
            DFTRACER_UTILS_LOG_ERROR(
                "padded writer: header %zu + pad overhead exceeds stripe %zu",
                data.size(), stripe_size_);
            co_return -1;
        }
        std::vector<std::uint8_t> buf;
        buf.reserve(stripe_size_);
        buf.insert(
            buf.end(), reinterpret_cast<const std::uint8_t*>(data.data()),
            reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
        pad_to_stripe(buf, stripe_size_);
        co_return co_await pwrite_bytes(buf.data(), buf.size(), 0);
    }

    coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                    ByteView data) override {
        if (worker_idx >= producers_.size()) {
            DFTRACER_UTILS_LOG_ERROR("padded writer: worker_idx %zu >= %zu",
                                     worker_idx, producers_.size());
            co_return -1;
        }
        if (data.size() + PAD_MEMBER_FIXED_OVERHEAD > stripe_size_) {
            DFTRACER_UTILS_LOG_ERROR(
                "padded writer: chunk %zu + pad overhead exceeds stripe %zu",
                data.size(), stripe_size_);
            co_return -1;
        }
        Chunk c;
        c.data.assign(
            reinterpret_cast<const std::uint8_t*>(data.data()),
            reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
        c.ack = coro::make_channel<MemberSpan>(1);
        auto ack = c.ack;
        bool ok = co_await producers_[worker_idx].send(std::move(c));
        if (!ok) co_return -1;
        auto span = co_await ack->receive();
        if (!span) co_return -1;
        per_worker_last_[worker_idx] = *span;
        co_return 0;
    }

    coro::CoroTask<int> write_footer(ByteView data) override {
        if (co_await drain_packer() != 0) co_return -1;
        const auto stripes = next_stripe_idx_.load(std::memory_order_relaxed);
        const auto offset = (stripes + 1) * stripe_size_;  // +1 for header
        co_return co_await pwrite_all(data, static_cast<off_t>(offset));
    }

    coro::CoroTask<int> close() override {
        if (co_await drain_packer() != 0) {
            if (fd_ >= 0) {
                co_await ::dftracer::utils::io::close(fd_);
                fd_ = -1;
            }
            co_return -1;
        }
        if (fd_ < 0) co_return 0;
        auto rc = co_await ::dftracer::utils::io::close(fd_);
        fd_ = -1;
        co_return static_cast<int>(rc);
    }

    std::vector<std::string> output_paths() const override { return {path_}; }

    std::optional<MemberSpan> last_member(
        std::size_t worker_idx) const override {
        if (worker_idx >= per_worker_last_.size()) return std::nullopt;
        return per_worker_last_[worker_idx];
    }

   private:
    // Drop all producer slots so the channel reports EOF to the packer, then
    // wait for the packer to emit its final stripe. Safe to call twice.
    coro::CoroTask<int> drain_packer() {
        if (!packer_drained_) {
            producers_.clear();
            if (packer_future_.has_value()) {
                auto rc = co_await *packer_future_;
                packer_future_.reset();
                if (rc != 0) co_return rc;
            }
            packer_drained_ = true;
        }
        co_return 0;
    }

    coro::CoroTask<int> run_packer(CoroScope& parent_scope,
                                   coro::ChannelConsumer<Chunk> consumer) {
        std::vector<std::uint8_t> buf;
        buf.reserve(stripe_size_);
        std::uint64_t current_stripe_idx =
            next_stripe_idx_.fetch_add(1, std::memory_order_relaxed);

        std::deque<coro::SpawnFuture<int>> in_flight;
        int final_rc = 0;

        auto await_one = [&]() -> coro::CoroTask<void> {
            auto f = std::move(in_flight.front());
            in_flight.pop_front();
            auto rc = co_await std::move(f);
            if (rc != 0 && final_rc == 0) final_rc = rc;
        };

        auto launch_emit = [&](std::vector<std::uint8_t>&& payload,
                               std::uint64_t emit_idx) -> coro::CoroTask<void> {
            while (in_flight.size() >= MAX_INFLIGHT_PWRITES) {
                co_await await_one();
            }
            in_flight.push_back(parent_scope.spawn(
                [this, p = std::move(payload),
                 emit_idx](CoroScope&) mutable -> coro::CoroTask<int> {
                    pad_to_stripe(p, stripe_size_);
                    const auto offset = (emit_idx + 1) * stripe_size_;
                    co_return co_await pwrite_bytes(p.data(), p.size(),
                                                    static_cast<off_t>(offset));
                }));
            co_return;
        };

        while (auto chunk = co_await consumer.receive()) {
            if (!buf.empty() &&
                buf.size() + chunk->data.size() + PAD_MEMBER_FIXED_OVERHEAD >
                    stripe_size_) {
                co_await launch_emit(std::move(buf), current_stripe_idx);
                buf.clear();
                buf.reserve(stripe_size_);
                current_stripe_idx =
                    next_stripe_idx_.fetch_add(1, std::memory_order_relaxed);
            }
            if (chunk->ack) {
                MemberSpan span{
                    (current_stripe_idx + 1) * stripe_size_ + buf.size(),
                    static_cast<std::uint64_t>(chunk->data.size())};
                co_await chunk->ack->send(std::move(span));
                chunk->ack->close();
            }
            buf.insert(buf.end(), chunk->data.begin(), chunk->data.end());
        }
        if (!buf.empty()) {
            co_await launch_emit(std::move(buf), current_stripe_idx);
        }

        while (!in_flight.empty()) {
            co_await await_one();
        }
        co_return final_rc;
    }

    coro::CoroTask<int> pwrite_all(ByteView data, off_t offset) {
        co_return co_await pwrite_bytes(
            reinterpret_cast<const std::uint8_t*>(data.data()), data.size(),
            offset);
    }

    coro::CoroTask<int> pwrite_bytes(const std::uint8_t* bytes,
                                     std::size_t size, off_t offset) {
        if (size == 0) co_return 0;
        std::size_t written = 0;
        while (written < size) {
            auto n = co_await ::dftracer::utils::io::pwrite(
                fd_, bytes + written, size - written,
                offset + static_cast<off_t>(written));
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR(
                    "padded writer pwrite failed at %lld on %s",
                    static_cast<long long>(offset), path_.c_str());
                co_return -1;
            }
            written += static_cast<std::size_t>(n);
        }
        co_return 0;
    }

    std::string path_;
    int fd_ = -1;
    std::size_t stripe_size_;
    std::atomic<std::uint64_t> next_stripe_idx_{0};

    std::shared_ptr<coro::Channel<Chunk>> channel_;
    std::vector<coro::ChannelProducer<Chunk>> producers_;
    std::optional<coro::SpawnFuture<int>> packer_future_;
    bool packer_drained_ = false;
    std::vector<std::optional<MemberSpan>> per_worker_last_;
};

}  // namespace

std::unique_ptr<ParallelWriter> make_padded_striped_writer(
    std::size_t stripe_size) {
    return std::make_unique<PaddedStripedWriter>(stripe_size);
}

}  // namespace dftracer::utils::utilities::fileio::parallel
