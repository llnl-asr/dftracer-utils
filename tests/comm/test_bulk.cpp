// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

using Bytes = dstd::Vector<std::byte>;

/// The two providers that must behave alike: the one that copies inside the
/// process, and the one that runs the request, reply and credit protocol over
/// a messaging, here the local one.
enum class Provider { Local, Message };

constexpr Provider PROVIDERS[] = {Provider::Local, Provider::Message};

const char* name_of(Provider provider) {
    return provider == Provider::Local ? "local provider" : "message provider";
}

comm::Communicator make_communicator(Provider provider, std::uint32_t ranks,
                                     coro::Executor& exec) {
    auto started = comm::Communicator::start_local(ranks, exec);
    REQUIRE(started.has_value());
    comm::Communicator communicator = std::move(*started);
    if (provider == Provider::Message)
        comm::detail::Hooks::set_bulk(
            communicator, comm::backend::make_message_bulk(
                              comm::detail::Hooks::messaging(communicator)));
    return communicator;
}

/// A messaging that forwards to another one and can hold back the large
/// messages of one purpose until a test lets them go, so a transfer is parked
/// in a state the test names.
class DelayMessaging final : public comm::backend::Messaging {
   public:
    using Inner = dstd::SharedPtr<comm::backend::Messaging>;

    explicit DelayMessaging(Inner inner) : inner_(std::move(inner)) {}

    const Inner& inner() const { return inner_; }

    /// Holds the messages of @p purpose of at least @p min_bytes from now on.
    void hold(comm::backend::TagPurpose purpose, std::size_t min_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        purpose_ = purpose;
        min_bytes_ = min_bytes;
        remaining_ = std::numeric_limits<std::size_t>::max();
        holding_ = true;
    }

    /// Holds the next message of @p purpose only.
    void hold_next(comm::backend::TagPurpose purpose) {
        std::lock_guard<std::mutex> lock(mutex_);
        purpose_ = purpose;
        min_bytes_ = 0;
        remaining_ = 1;
        holding_ = true;
    }

    std::size_t held() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stash_.size();
    }

    /// Sends what was held, and holds nothing more.
    void release_held() {
        dstd::Vector<Held> sending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            holding_ = false;
            sending = std::move(stash_);
            stash_ = dstd::Vector<Held>{};
        }
        for (Held& one : sending)
            inner_->send(one.key, std::move(one.payload), one.opts,
                         std::move(one.done));
    }

    comm::backend::OpId send(const comm::backend::MessageKey& key,
                             comm::backend::Payload payload,
                             comm::backend::StreamOpts opts,
                             SendDone done) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (holding_ && comm::backend::tag_purpose(key.tag) == purpose_ &&
                payload.size() >= min_bytes_) {
                if (--remaining_ == 0) holding_ = false;
                stash_.push_back(
                    Held{key, std::move(payload), opts, std::move(done)});
                return comm::backend::NO_OP;
            }
        }
        return inner_->send(key, std::move(payload), opts, std::move(done));
    }

    comm::backend::OpId recv(const comm::backend::MessageKey& key,
                             RecvDone done) override {
        return inner_->recv(key, std::move(done));
    }

    comm::backend::OpId recv_any_source(const comm::backend::MessageKey& key,
                                        std::uint32_t senders,
                                        RecvFromDone done) override {
        return inner_->recv_any_source(key, senders, std::move(done));
    }

    comm::backend::OpId recv_into(const comm::backend::MessageKey& key,
                                  std::uint32_t senders,
                                  dstd::Span<std::byte> buffer,
                                  RecvIntoDone done) override {
        return inner_->recv_into(key, senders, buffer, std::move(done));
    }

    bool cancel(const comm::backend::MessageKey& key, comm::backend::OpId op,
                dstd::Error why) override {
        return inner_->cancel(key, op, std::move(why));
    }

    void close(const comm::backend::MessageKey& key) override {
        inner_->close(key);
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        inner_->abort(group, std::move(why));
    }

    void release(std::uint64_t group) override { inner_->release(group); }

    void bind_group(std::uint64_t group,
                    dstd::Span<const std::uint32_t> members) override {
        inner_->bind_group(group, members);
    }

   private:
    struct Held {
        comm::backend::MessageKey key;
        comm::backend::Payload payload;
        comm::backend::StreamOpts opts;
        SendDone done;
    };

    Inner inner_;
    std::mutex mutex_;
    bool holding_ = false;
    comm::backend::TagPurpose purpose_ = comm::backend::TagPurpose::BulkData;
    std::size_t min_bytes_ = 0;
    std::size_t remaining_ = 0;
    dstd::Vector<Held> stash_;
};

/// Runs the bulk transfers of @p communicator on the message provider over a
/// messaging that can hold messages back.
dstd::SharedPtr<DelayMessaging> delay_bulk(comm::Communicator& communicator) {
    auto delay = dstd::make_shared<DelayMessaging>(
        comm::detail::Hooks::messaging(communicator));
    comm::detail::Hooks::set_bulk(communicator,
                                  comm::backend::make_message_bulk(delay));
    return delay;
}

Bytes pattern(std::size_t size, std::uint8_t seed = 1) {
    Bytes bytes(size);
    for (std::size_t at = 0; at < size; ++at)
        bytes[at] = static_cast<std::byte>((at * 31 + seed) & 0xFF);
    return bytes;
}

dstd::Span<std::byte> view(Bytes& bytes) {
    return dstd::Span<std::byte>(bytes.data(), bytes.size());
}

dstd::Span<const std::byte> cview(const Bytes& bytes) {
    return dstd::Span<const std::byte>(bytes.data(), bytes.size());
}

bool same(const Bytes& a, const Bytes& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

}  // namespace

TEST_SUITE("CommBulk") {
    TEST_CASE("a pull copies the exposed bytes and a push writes them") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            comm::detail::Hooks::bulk(communicator)->limit_range_bytes(4096);
            // Below the eager threshold, above it, and above many ranges.
            for (const std::size_t size :
                 {std::size_t{1}, std::size_t{100}, std::size_t{65536},
                  std::size_t{200000}}) {
                CAPTURE(size);
                Bytes exposed = pattern(size, 3);
                auto ref = owner.expose(view(exposed), comm::Access::ReadWrite);
                REQUIRE(ref.has_value());
                CHECK(ref->owner == 0);
                CHECK(ref->size == size);
                Bytes got(size);
                auto pulled = peer.pull(*ref, view(got)).wait();
                if (!pulled) FAIL(std::string(pulled.error().c_str()));
                CHECK(*pulled == size);
                CHECK(same(got, exposed));

                const Bytes fresh = pattern(size / 2, 9);
                auto pushed =
                    peer.push(*ref, cview(fresh), size - size / 2).wait();
                REQUIRE(pushed.has_value());
                CHECK(*pushed == fresh.size());
                CHECK(std::memcmp(exposed.data() + (size - size / 2),
                                  fresh.data(), fresh.size()) == 0);
                CHECK(owner.unexpose(*ref).wait().has_value());
            }
        }
    }

    TEST_CASE("an offset pull takes the rest of the buffer") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            Bytes exposed = pattern(1000, 5);
            auto ref = owner.expose(view(exposed));
            REQUIRE(ref.has_value());
            Bytes got(400);
            auto pulled = peer.pull(*ref, view(got), 600).wait();
            REQUIRE(pulled.has_value());
            CHECK(*pulled == 400);
            CHECK(std::memcmp(got.data(), exposed.data() + 600, 400) == 0);
            Bytes none;
            auto at_end = peer.pull(*ref, view(none), 1000).wait();
            REQUIRE(at_end.has_value());
            CHECK(*at_end == 0);
            // A larger buffer than the range gets the range and its count.
            Bytes roomy(900, std::byte{0x7F});
            auto roomy_pull = peer.pull(*ref, view(roomy), 500).wait();
            REQUIRE(roomy_pull.has_value());
            CHECK(*roomy_pull == 500);
            CHECK(roomy[500] == std::byte{0x7F});
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("a range that does not fit is TooBig and copies nothing") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            Bytes exposed = pattern(1000, 5);
            const Bytes before = exposed;
            auto ref = owner.expose(view(exposed));
            REQUIRE(ref.has_value());

            Bytes short_buffer(999, std::byte{0x55});
            auto pulled = peer.pull(*ref, view(short_buffer)).wait();
            REQUIRE_FALSE(pulled.has_value());
            CHECK(pulled.error().is_code(comm::ChannelError::TooBig));
            CHECK(short_buffer[0] == std::byte{0x55});
            CHECK(short_buffer[998] == std::byte{0x55});

            Bytes small(10);
            auto past = peer.pull(*ref, view(small), 1001).wait();
            REQUIRE_FALSE(past.has_value());
            CHECK(past.error().is_code(comm::ChannelError::TooBig));

            const Bytes too_long = pattern(1001);
            auto pushed = peer.push(*ref, cview(too_long)).wait();
            REQUIRE_FALSE(pushed.has_value());
            CHECK(pushed.error().is_code(comm::ChannelError::TooBig));
            const Bytes ten = pattern(10);
            auto crossing = peer.push(*ref, cview(ten), 995).wait();
            REQUIRE_FALSE(crossing.has_value());
            CHECK(crossing.error().is_code(comm::ChannelError::TooBig));
            auto beyond = peer.push(*ref, cview(ten), 1001).wait();
            REQUIRE_FALSE(beyond.has_value());
            CHECK(beyond.error().is_code(comm::ChannelError::TooBig));
            CHECK(same(exposed, before));
            // The same buffers fit when they are the right size.
            auto exact = peer.push(*ref, cview(ten), 990).wait();
            REQUIRE(exact.has_value());
            CHECK(*exact == 10);
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("an access mode refuses the call it does not allow") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            Bytes memory = pattern(256, 2);
            const Bytes before = memory;

            auto read_only = owner.expose(view(memory), comm::Access::ReadOnly);
            REQUIRE(read_only.has_value());
            const Bytes fresh = pattern(16, 8);
            auto refused_push = peer.push(*read_only, cview(fresh)).wait();
            REQUIRE_FALSE(refused_push.has_value());
            CHECK(refused_push.error().is(dstd::Condition::InvalidArgument));
            CHECK(dftu_comm_test::has_text(refused_push.error(), "read only"));
            CHECK(same(memory, before));
            Bytes got(256);
            CHECK(peer.pull(*read_only, view(got)).wait().has_value());

            auto write_only =
                owner.expose(view(memory), comm::Access::WriteOnly);
            REQUIRE(write_only.has_value());
            auto refused_pull = peer.pull(*write_only, view(got)).wait();
            REQUIRE_FALSE(refused_pull.has_value());
            CHECK(refused_pull.error().is(dstd::Condition::InvalidArgument));
            CHECK(dftu_comm_test::has_text(refused_pull.error(), "write only"));
            CHECK(peer.push(*write_only, cview(fresh)).wait().has_value());

            const Bytes& frozen = before;
            auto readable = owner.expose(cview(frozen), comm::Access::ReadOnly);
            CHECK(readable.has_value());
            auto bad_const =
                owner.expose(cview(frozen), comm::Access::ReadWrite);
            REQUIRE_FALSE(bad_const.has_value());
            CHECK(bad_const.error().is(dstd::Condition::InvalidArgument));
            CHECK(owner.unexpose(*read_only).wait().has_value());
            CHECK(owner.unexpose(*write_only).wait().has_value());
            CHECK(owner.unexpose(*readable).wait().has_value());
        }
    }

    TEST_CASE("a device memory kind is refused as unsupported") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            Bytes memory = pattern(64);
            auto device = owner.expose(view(memory), comm::Access::ReadWrite,
                                       comm::MemoryKind::Cuda);
            REQUIRE_FALSE(device.has_value());
            CHECK(device.error().is(dstd::Condition::Unsupported));

            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            comm::BufferRef forged = *ref;
            forged.kind = comm::MemoryKind::Hip;
            Bytes got(64);
            auto pulled = peer.pull(forged, view(got)).wait();
            REQUIRE_FALSE(pulled.has_value());
            CHECK(pulled.error().is(dstd::Condition::Unsupported));
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("an unknown or closed buffer and a foreign ref fail by name") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 3, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            Bytes memory = pattern(128);
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());

            comm::BufferRef unknown = *ref;
            unknown.id = 4242;
            Bytes got(128);
            auto missing = peer.pull(unknown, view(got)).wait();
            REQUIRE_FALSE(missing.has_value());
            INFO(std::string(missing.error().c_str()));
            CHECK(missing.error().is(dstd::Condition::NotFound));
            CHECK(dftu_comm_test::has_text(missing.error(), "4242"));

            comm::BufferRef other_group = *ref;
            other_group.group += 1;
            auto foreign = peer.pull(other_group, view(got)).wait();
            REQUIRE_FALSE(foreign.has_value());
            CHECK(foreign.error().is(dstd::Condition::InvalidArgument));

            comm::BufferRef nobody = *ref;
            nobody.owner = 9;
            CHECK_FALSE(peer.pull(nobody, view(got)).wait().has_value());

            // Only the owner closes its buffer.
            auto not_owner = peer.unexpose(*ref).wait();
            REQUIRE_FALSE(not_owner.has_value());
            CHECK(not_owner.error().is(dstd::Condition::InvalidArgument));

            CHECK(owner.unexpose(*ref).wait().has_value());
            auto closed = peer.pull(*ref, view(got)).wait();
            REQUIRE_FALSE(closed.has_value());
            CHECK(closed.error().is(dstd::Condition::NotFound));
            // Closing a closed buffer completes at once.
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("unexpose waits for a copy in flight and stops new ones") {
        // The local provider holds the exposure for a whole copy; the message
        // provider holds it per range, see the next case.
        for (const Provider provider : {Provider::Local}) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 3, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            comm::Group other = communicator.world(2);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = 200000;
            Bytes memory = pattern(SIZE, 4);
            const Bytes expected = memory;
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            bulk->limit_credit_bytes(4096);
            // The pull waits for credit that the test holds, so it is in
            // flight for as long as the test wants.
            auto held = bulk->hold_credit(ref->group, 1, 0, 4096);
            Bytes got(SIZE);
            auto pull = peer.pull(*ref, view(got));
            CHECK_FALSE(pull.test());
            auto closing = owner.unexpose(*ref);
            CHECK_FALSE(closing.test());
            // A second close of the ref that still drains waits as well.
            auto closing_again = owner.unexpose(*ref);
            CHECK_FALSE(closing_again.test());
            // Another rank has its own credit, so its pull reaches the owner
            // at once and finds the exposure closed.
            Bytes late(SIZE);
            auto refused = other.pull(*ref, view(late)).wait();
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().is(dstd::Condition::NotFound));
            CHECK_FALSE(closing.test());
            held.reset();
            auto pulled = pull.wait();
            REQUIRE(pulled.has_value());
            CHECK(*pulled == SIZE);
            CHECK(same(got, expected));
            CHECK(closing.wait().has_value());
            CHECK(closing_again.wait().has_value());
            // The pull ended before the exposure drained.
            CHECK(pull.test());
        }
    }

    TEST_CASE("a pull never reads memory after its unexpose completed") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            constexpr std::size_t SIZE = std::size_t{6} << 20;
            const Bytes expected = pattern(SIZE, 17);
            for (int round = 0; round < 12; ++round) {
                // A communicator of its own, so its credit peak says that this
                // round's pull started.
                comm::Communicator communicator =
                    make_communicator(provider, 2, *rt.executor());
                comm::Group owner = communicator.world(0);
                comm::Group peer = communicator.world(1);
                auto bulk = comm::detail::Hooks::bulk(communicator);
                bulk->limit_range_bytes(std::uint64_t{64} << 10);
                Bytes memory = expected;
                auto ref = owner.expose(view(memory), comm::Access::ReadOnly);
                REQUIRE(ref.has_value());
                Bytes got(SIZE);
                dstd::Optional<dstd::Error> failed;
                bool copied = false;
                std::thread puller([&] {
                    auto pulled = peer.pull(*ref, view(got)).wait();
                    if (pulled)
                        copied = *pulled == SIZE;
                    else
                        failed = pulled.error();
                });
                CHECK(dftu_comm_test::spin_until(
                    [&] { return bulk->peak_in_flight() > 0; }));
                CHECK(owner.unexpose(*ref).wait().has_value());
                // The memory is the owner's again: poison it.
                std::memset(memory.data(), 0xEE, memory.size());
                puller.join();
                if (failed) {
                    CHECK(failed->is(dstd::Condition::NotFound));
                } else {
                    CHECK(copied);
                    CHECK(same(got, expected));
                }
            }
        }
    }

    TEST_CASE("a cancel ends a waiting pull once and leaves the group alone") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = 200000;
            Bytes memory = pattern(SIZE, 6);
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            bulk->limit_credit_bytes(4096);
            auto held = bulk->hold_credit(ref->group, 1, 0, 4096);
            Bytes got(SIZE, std::byte{0x11});
            auto pull = peer.pull(*ref, view(got));
            CHECK_FALSE(pull.test());
            pull.cancel();
            auto outcome = pull.wait();
            REQUIRE_FALSE(outcome.has_value());
            CHECK(outcome.error().is(dstd::Condition::Cancelled));
            CHECK(pull.status() == comm::OpStatus::Canceled);
            pull.cancel();
            CHECK(pull.wait().error().is(dstd::Condition::Cancelled));
            CHECK(got[0] == std::byte{0x11});
            held.reset();

            // The group still works, and a finished call keeps its outcome.
            Bytes again(SIZE);
            auto done = peer.pull(*ref, view(again));
            CHECK(done.wait().has_value());
            done.cancel();
            CHECK(done.status() == comm::OpStatus::Ok);
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("a cancelled push gives back its credit and its exposure") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = 200000;
            Bytes memory(SIZE);
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            bulk->limit_credit_bytes(4096);
            // The receiving rank's credit toward the pusher is taken, so the
            // push waits for it.
            auto held = bulk->hold_credit(ref->group, 0, 1, 4096);
            const Bytes source = pattern(SIZE, 4);
            auto push = peer.push(*ref, cview(source));
            CHECK_FALSE(push.test());
            push.cancel();
            auto outcome = push.wait();
            REQUIRE_FALSE(outcome.has_value());
            CHECK(outcome.error().is(dstd::Condition::Cancelled));
            auto closing = owner.unexpose(*ref);
            CHECK(dftu_comm_test::spin_until([&] { return closing.test(); }));
            CHECK(bulk->credit_in_flight() == 4096);
            held.reset();
            CHECK(bulk->credit_in_flight() == 0);

            // The same source pushes again, and nothing is left blocked.
            auto again = owner.expose(view(memory));
            REQUIRE(again.has_value());
            auto pushed = peer.push(*again, cview(source)).wait();
            REQUIRE(pushed.has_value());
            CHECK(*pushed == SIZE);
            CHECK(same(memory, source));
            CHECK(owner.unexpose(*again).wait().has_value());
        }
    }

    TEST_CASE("a push cancelled after the owner granted a range is undone") {
        Runtime rt(4);
        comm::Communicator communicator =
            make_communicator(Provider::Local, 2, *rt.executor());
        comm::Group owner = communicator.world(0);
        comm::Group peer = communicator.world(1);
        auto delay = delay_bulk(communicator);
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(4096);
        bulk->limit_credit_bytes(8192);
        constexpr std::size_t SIZE = 200000;
        Bytes memory(SIZE);
        auto ref = owner.expose(view(memory));
        REQUIRE(ref.has_value());
        // The owner's standing receive for requests is the one that parks.
        const std::size_t standing =
            comm::backend::local_counts(*delay->inner()).parked;
        // The bytes of the first range stay on the way, so the owner waits for
        // them with its credit taken.
        delay->hold(comm::backend::TagPurpose::BulkData, 4096);
        const Bytes source = pattern(SIZE, 8);
        auto push = peer.push(*ref, cview(source));
        REQUIRE(dftu_comm_test::spin_until([&] { return delay->held() == 1; }));
        CHECK(bulk->credit_in_flight() == 4096);
        push.cancel();
        CHECK(dftu_comm_test::spin_until(
            [&] { return bulk->credit_in_flight() == 0; }));
        auto closing = owner.unexpose(*ref);
        CHECK(dftu_comm_test::spin_until([&] { return closing.test(); }));
        // The bytes that were on the way go to a receive that drops them, which
        // is what frees the memory of the pusher.
        delay->release_held();
        auto outcome = push.wait();
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().is(dstd::Condition::Cancelled));
        CHECK(comm::backend::local_counts(*delay->inner()).parked == standing);

        auto again = owner.expose(view(memory));
        REQUIRE(again.has_value());
        auto pushed = peer.push(*again, cview(source)).wait();
        REQUIRE(pushed.has_value());
        CHECK(same(memory, source));
        CHECK(owner.unexpose(*again).wait().has_value());
    }

    TEST_CASE("a cancel that reaches the owner before its write is kept") {
        Runtime rt(4);
        comm::Communicator communicator =
            make_communicator(Provider::Local, 2, *rt.executor());
        comm::Group owner = communicator.world(0);
        comm::Group peer = communicator.world(1);
        auto delay = delay_bulk(communicator);
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(4096);
        bulk->limit_credit_bytes(8192);
        constexpr std::size_t SIZE = 200000;
        Bytes memory(SIZE);
        auto ref = owner.expose(view(memory));
        REQUIRE(ref.has_value());
        // The write request is held, so the cancel gets to the owner first.
        delay->hold_next(comm::backend::TagPurpose::BulkRequest);
        const Bytes source = pattern(SIZE, 6);
        auto push = peer.push(*ref, cview(source));
        REQUIRE(delay->held() == 1);
        push.cancel();
        auto outcome = push.wait();
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().is(dstd::Condition::Cancelled));
        delay->release_held();
        auto closing = owner.unexpose(*ref);
        CHECK(dftu_comm_test::spin_until([&] { return closing.test(); }));
        CHECK(bulk->credit_in_flight() == 0);

        auto again = owner.expose(view(memory));
        REQUIRE(again.has_value());
        auto pushed = peer.push(*again, cview(source)).wait();
        REQUIRE(pushed.has_value());
        CHECK(same(memory, source));
        CHECK(owner.unexpose(*again).wait().has_value());
    }

    TEST_CASE("a pull cancelled between ranges leaves nothing parked") {
        Runtime rt(4);
        comm::Communicator communicator =
            make_communicator(Provider::Local, 2, *rt.executor());
        comm::Group owner = communicator.world(0);
        comm::Group peer = communicator.world(1);
        auto delay = delay_bulk(communicator);
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(4096);
        bulk->limit_credit_bytes(8192);
        constexpr std::size_t SIZE = 200000;
        Bytes memory = pattern(SIZE, 2);
        auto ref = owner.expose(view(memory), comm::Access::ReadOnly);
        REQUIRE(ref.has_value());
        // The owner's standing receive for requests is the one that parks.
        const std::size_t standing =
            comm::backend::local_counts(*delay->inner()).parked;
        delay->hold(comm::backend::TagPurpose::BulkPullData, 4096);
        Bytes got(SIZE);
        auto pull = peer.pull(*ref, view(got));
        REQUIRE(dftu_comm_test::spin_until([&] { return delay->held() == 1; }));
        CHECK(bulk->credit_in_flight() == 4096);
        pull.cancel();
        auto outcome = pull.wait();
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().is(dstd::Condition::Cancelled));
        CHECK(bulk->credit_in_flight() == 0);
        // The range that was on the way is taken off the stream.
        delay->release_held();
        auto closing = owner.unexpose(*ref);
        CHECK(closing.wait().has_value());
        CHECK(comm::backend::local_counts(*delay->inner()).parked == standing);

        auto again = owner.expose(view(memory), comm::Access::ReadOnly);
        REQUIRE(again.has_value());
        Bytes second(SIZE);
        auto pulled = peer.pull(*again, view(second)).wait();
        REQUIRE(pulled.has_value());
        CHECK(same(second, memory));
        CHECK(owner.unexpose(*again).wait().has_value());
    }

    TEST_CASE("a deadline ends a waiting pull with a timeout") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = 200000;
            Bytes memory = pattern(SIZE);
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            bulk->limit_credit_bytes(4096);
            auto held = bulk->hold_credit(ref->group, 1, 0, 4096);
            Bytes got(SIZE);
            comm::CallOpts opts;
            opts.deadline_ns = dftu_comm_test::deadline_after(30'000'000);
            auto outcome = peer.pull(*ref, view(got), 0, opts).wait();
            REQUIRE_FALSE(outcome.has_value());
            CHECK(outcome.error().is(dstd::Condition::Timeout));
            held.reset();
            CHECK(peer.pull(*ref, view(got)).wait().has_value());

            comm::CallOpts stopped;
            coro::StopSource source;
            source.request_stop();
            stopped.stop = source.token();
            auto never = peer.pull(*ref, view(got), 0, stopped).wait();
            REQUIRE_FALSE(never.has_value());
            CHECK(never.error().is(dstd::Condition::Cancelled));
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("an abort fails a waiting pull with the error of the group") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = 200000;
            Bytes memory = pattern(SIZE);
            auto ref = owner.expose(view(memory));
            REQUIRE(ref.has_value());
            bulk->limit_credit_bytes(4096);
            auto held = bulk->hold_credit(ref->group, 1, 0, 4096);
            Bytes got(SIZE);
            auto pull = peer.pull(*ref, view(got));
            CHECK_FALSE(pull.test());
            owner.abort(dstd::Error{dstd::Condition::Internal, "boom"});
            auto outcome = pull.wait();
            REQUIRE_FALSE(outcome.has_value());
            CHECK(
                dftu_comm_test::has_text(outcome.error(), "aborted by rank 0"));
            CHECK(dftu_comm_test::has_text(outcome.error(), "boom"));
            auto later = peer.pull(*ref, view(got)).wait();
            REQUIRE_FALSE(later.has_value());
            CHECK(dftu_comm_test::has_text(later.error(), "boom"));
            auto exposing = owner.expose(view(memory));
            CHECK_FALSE(exposing.has_value());
            held.reset();
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("many large pulls keep each peer inside its credit") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = std::size_t{6} << 20;
            constexpr std::uint64_t CREDIT = std::uint64_t{2} << 20;
            constexpr std::uint32_t PULLS = 8;
            Bytes memory = pattern(SIZE, 12);
            auto ref = owner.expose(view(memory), comm::Access::ReadOnly);
            REQUIRE(ref.has_value());
            bulk->limit_range_bytes(std::uint64_t{1} << 20);
            bulk->limit_credit_bytes(CREDIT);
            dstd::Vector<Bytes> out(PULLS);
            for (Bytes& bytes : out) bytes.resize(SIZE);
            std::atomic<std::uint32_t> good{0};
            dstd::Vector<std::thread> threads;
            for (std::uint32_t at = 0; at < PULLS; ++at)
                threads.emplace_back([&, at] {
                    auto pulled = peer.pull(*ref, view(out[at])).wait();
                    if (pulled && *pulled == SIZE) good.fetch_add(1);
                });
            for (std::thread& thread : threads) thread.join();
            CHECK(good.load() == PULLS);
            for (const Bytes& bytes : out) CHECK(same(bytes, memory));
            CHECK(bulk->peak_in_flight() > 0);
            CHECK(bulk->peak_in_flight() <= CREDIT);
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }

    TEST_CASE("a push larger than the credit streams in ranges") {
        for (const Provider provider : PROVIDERS) {
            INFO(std::string(name_of(provider)));
            Runtime rt(4);
            comm::Communicator communicator =
                make_communicator(provider, 2, *rt.executor());
            comm::Group owner = communicator.world(0);
            comm::Group peer = communicator.world(1);
            auto bulk = comm::detail::Hooks::bulk(communicator);
            constexpr std::size_t SIZE = std::size_t{3} << 20;
            Bytes memory(SIZE);
            auto ref = owner.expose(view(memory), comm::Access::WriteOnly);
            REQUIRE(ref.has_value());
            bulk->limit_range_bytes(std::uint64_t{256} << 10);
            bulk->limit_credit_bytes(std::uint64_t{512} << 10);
            const Bytes source = pattern(SIZE, 21);
            auto pushed = peer.push(*ref, cview(source)).wait();
            REQUIRE(pushed.has_value());
            CHECK(*pushed == SIZE);
            CHECK(same(memory, source));
            CHECK(bulk->peak_in_flight() <= (std::uint64_t{512} << 10));
            CHECK(owner.unexpose(*ref).wait().has_value());
        }
    }
}
