#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/runtime/coro/channel.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/long_lived_release.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/fileio/parallel/pwrite_fully.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
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

    p[0] = constants::indexer::GZIP_MAGIC_BYTE_0;
    p[1] = constants::indexer::GZIP_MAGIC_BYTE_1;
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
// Callers leave at least PAD_MEMBER_FIXED_OVERHEAD bytes. Each member leaves
// either nothing or at least that much, so the stripe ends in a whole member:
// a few bytes too small for one would stay zero, and gzip stops at them.
void pad_to_stripe(std::vector<std::uint8_t>& out, std::size_t stripe_size) {
    std::size_t remaining = stripe_size - out.size();
    while (remaining > PAD_MEMBER_MAX_SIZE) {
        const std::size_t take = std::min(
            PAD_MEMBER_MAX_SIZE, remaining - PAD_MEMBER_FIXED_OVERHEAD);
        append_padding_member(
            out, static_cast<std::uint16_t>(take - PAD_MEMBER_FIXED_OVERHEAD));
        remaining -= take;
    }
    if (remaining >= PAD_MEMBER_FIXED_OVERHEAD) {
        append_padding_member(out, static_cast<std::uint16_t>(
                                       remaining - PAD_MEMBER_FIXED_OVERHEAD));
    }
}

struct Chunk {
    std::vector<std::uint8_t> data;
    std::shared_ptr<coro::Channel<ParallelWriter::MemberSpan>> ack;
};

// Everything the packer and its children touch. The writer holds it by
// pointer, so no coroutine points into the writer or the frame that owns it.
struct PaddedStripedImpl {
    explicit PaddedStripedImpl(std::size_t stripe) : stripe_size(stripe) {}

    std::string path;
    int fd = -1;
    std::size_t stripe_size;
    std::atomic<std::uint64_t> next_stripe_idx{0};

    std::unique_ptr<CoroScope> packer_scope;
    std::shared_ptr<coro::Channel<Chunk>> channel;
    std::vector<coro::ChannelProducer<Chunk>> producers;
    std::optional<coro::SpawnFuture<int>> packer_future;
    bool packer_drained = false;

    bool settled() const { return fd < 0 && (packer_drained || !packer_scope); }

    void abandon() {}

    // join_and_clean() stops before the close when a shutdown cancels it.
    ~PaddedStripedImpl() {
        if (fd >= 0) ::close(fd);
    }

    coro::CoroTask<void> join_and_clean() {
        co_await drain_packer();
        if (fd >= 0) {
            co_await ::dftracer::utils::io::close(fd);
            fd = -1;
        }
    }

    // Drop all producer slots so the channel reports EOF to the packer, then
    // wait for the packer to emit its final stripe. Safe to call twice.
    coro::CoroTask<int> drain_packer() {
        if (!packer_drained) {
            producers.clear();
            if (packer_future.has_value()) {
                // A shutdown cancels the wait with an exception; the scope
                // must still be joined before the Impl can be destroyed.
                int rc = 0;
                std::exception_ptr cancelled;
                try {
                    rc = co_await *packer_future;
                } catch (...) {
                    cancelled = std::current_exception();
                }
                packer_future.reset();
                co_await packer_scope->join();
                packer_drained = true;
                if (cancelled) std::rethrow_exception(cancelled);
                if (rc != 0) co_return rc;
            }
            packer_drained = true;
        }
        co_return 0;
    }

    coro::CoroTask<int> run_packer(CoroScope& parent_scope,
                                   coro::ChannelConsumer<Chunk> consumer) {
        std::vector<std::uint8_t> buf;
        buf.reserve(stripe_size);
        std::uint64_t current_stripe_idx =
            next_stripe_idx.fetch_add(1, std::memory_order_relaxed);

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
                    pad_to_stripe(p, stripe_size);
                    const auto offset = (emit_idx + 1) * stripe_size;
                    co_return co_await pwrite_bytes(p.data(), p.size(),
                                                    static_cast<off_t>(offset));
                }));
            co_return;
        };

        for (;;) {
            auto chunk = co_await consumer.receive();
            if (!chunk) break;
            if (!buf.empty() &&
                buf.size() + chunk->data.size() + PAD_MEMBER_FIXED_OVERHEAD >
                    stripe_size) {
                co_await launch_emit(std::move(buf), current_stripe_idx);
                buf.clear();
                buf.reserve(stripe_size);
                current_stripe_idx =
                    next_stripe_idx.fetch_add(1, std::memory_order_relaxed);
            }
            if (chunk->ack) {
                ParallelWriter::MemberSpan span{
                    (current_stripe_idx + 1) * stripe_size + buf.size(),
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

    coro::CoroTask<int> pwrite_bytes(const std::uint8_t* bytes,
                                     std::size_t size, off_t offset) {
        co_return co_await pwrite_fully(fd, path.c_str(), bytes, size, offset);
    }
};

class PaddedStripedWriter : public ParallelWriter {
   public:
    explicit PaddedStripedWriter(std::size_t stripe_size)
        : impl_(std::make_unique<PaddedStripedImpl>(stripe_size)) {}

    PaddedStripedWriter(PaddedStripedWriter&&) noexcept = default;
    PaddedStripedWriter& operator=(PaddedStripedWriter&& o) noexcept {
        if (this != &o) {
            release();
            impl_ = std::move(o.impl_);
            per_worker_last_ = std::move(o.per_worker_last_);
        }
        return *this;
    }
    ~PaddedStripedWriter() override { release(); }

    coro::CoroTask<int> open(std::string path, std::size_t num_workers,
                             bool /*gzip_extension*/,
                             CoroScope* /*scope*/) override {
        PaddedStripedImpl& s = *impl_;
        s.path = std::move(path);
        ssize_t fd = co_await ::dftracer::utils::io::open(
            s.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open padded output: %s",
                                     s.path.c_str());
            co_return -1;
        }
        s.fd = static_cast<int>(fd);
        s.next_stripe_idx.store(0, std::memory_order_relaxed);
        per_worker_last_.assign(num_workers, std::nullopt);

        // Valid-gzip placeholder so callers that skip write_header still
        // produce a file gunzip can walk.
        std::vector<std::uint8_t> pad;
        pad.reserve(s.stripe_size);
        pad_to_stripe(pad, s.stripe_size);
        const auto pwrite_bytes_rc =
            co_await s.pwrite_bytes(pad.data(), pad.size(), 0);
        if (pwrite_bytes_rc != 0) {
            co_return -1;
        }

        // Set up the chunk channel + N pre-registered producers (one per
        // worker). The packer exits once all producers are released.
        s.channel = coro::make_channel<Chunk>(CHUNK_CHANNEL_CAPACITY);
        s.producers.reserve(num_workers);
        for (std::size_t i = 0; i < num_workers; ++i) {
            s.producers.emplace_back(s.channel->producer());
        }

        // The packer runs in a scope of its own: a caller joins its scope
        // before it calls close(), and close() is what lets the packer end,
        // so a packer in the caller's scope would never be joined.
        s.packer_scope = CoroScope::long_lived();
        s.packer_future = s.packer_scope->spawn(
            [impl = impl_.get(), consumer = s.channel->consumer()](
                CoroScope& scope) mutable -> coro::CoroTask<int> {
                co_return co_await impl->run_packer(scope, std::move(consumer));
            });
        co_return 0;
    }

    coro::CoroTask<int> write_header(ByteView data) override {
        PaddedStripedImpl& s = *impl_;
        if (data.size() + PAD_MEMBER_FIXED_OVERHEAD > s.stripe_size) {
            DFTRACER_UTILS_LOG_ERROR(
                "padded writer: header %zu + pad overhead exceeds stripe %zu",
                data.size(), s.stripe_size);
            co_return -1;
        }
        std::vector<std::uint8_t> buf;
        buf.reserve(s.stripe_size);
        buf.insert(
            buf.end(), reinterpret_cast<const std::uint8_t*>(data.data()),
            reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
        pad_to_stripe(buf, s.stripe_size);
        co_return co_await s.pwrite_bytes(buf.data(), buf.size(), 0);
    }

    coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                    ByteView data) override {
        PaddedStripedImpl& s = *impl_;
        if (worker_idx >= s.producers.size()) {
            DFTRACER_UTILS_LOG_ERROR("padded writer: worker_idx %zu >= %zu",
                                     worker_idx, s.producers.size());
            co_return -1;
        }
        if (data.size() + PAD_MEMBER_FIXED_OVERHEAD > s.stripe_size) {
            DFTRACER_UTILS_LOG_ERROR(
                "padded writer: chunk %zu + pad overhead exceeds stripe %zu",
                data.size(), s.stripe_size);
            co_return -1;
        }
        Chunk c;
        c.data.assign(
            reinterpret_cast<const std::uint8_t*>(data.data()),
            reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
        c.ack = coro::make_channel<MemberSpan>(1);
        auto ack = c.ack;
        bool ok = co_await s.producers[worker_idx].send(std::move(c));
        if (!ok) co_return -1;
        auto span = co_await ack->receive();
        if (!span) co_return -1;
        per_worker_last_[worker_idx] = *span;
        co_return 0;
    }

    coro::CoroTask<int> write_footer(ByteView data) override {
        PaddedStripedImpl& s = *impl_;
        const auto drain_rc = co_await s.drain_packer();
        if (drain_rc != 0) co_return -1;
        const auto stripes = s.next_stripe_idx.load(std::memory_order_relaxed);
        const auto offset = (stripes + 1) * s.stripe_size;  // +1 for header
        co_return co_await s.pwrite_bytes(
            reinterpret_cast<const std::uint8_t*>(data.data()), data.size(),
            static_cast<off_t>(offset));
    }

    coro::CoroTask<int> close() override {
        PaddedStripedImpl& s = *impl_;
        const auto drain_packer_rc = co_await s.drain_packer();
        if (drain_packer_rc != 0) {
            if (s.fd >= 0) {
                co_await ::dftracer::utils::io::close(s.fd);
                s.fd = -1;
            }
            co_return -1;
        }
        if (s.fd < 0) co_return 0;
        auto rc = co_await ::dftracer::utils::io::close(s.fd);
        s.fd = -1;
        co_return static_cast<int>(rc);
    }

    std::vector<std::string> output_paths() const override {
        return {impl_->path};
    }

    std::optional<MemberSpan> last_member(
        std::size_t worker_idx) const override {
        if (worker_idx >= per_worker_last_.size()) return std::nullopt;
        return per_worker_last_[worker_idx];
    }

   private:
    void release() {
        release_long_lived(std::move(impl_), "padded_striped_writer_abort");
    }

    std::unique_ptr<PaddedStripedImpl> impl_;
    std::vector<std::optional<MemberSpan>> per_worker_last_;
};

}  // namespace

std::unique_ptr<ParallelWriter> make_padded_striped_writer(
    std::size_t stripe_size) {
    return std::make_unique<PaddedStripedWriter>(stripe_size);
}

}  // namespace dftracer::utils::utilities::fileio::parallel
