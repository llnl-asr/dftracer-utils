// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_CHANNEL_H
#define DFTRACER_UTILS_CORO_CHANNEL_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/detail/channel_queue.h>
#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/detail/wake_batch.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

template <typename Queue>
class BasicChannelProducer;

template <typename Queue>
class BasicChannelConsumer;

/**
 * @brief Many-producer, many-consumer queue between coroutines.
 *
 * Use it through @ref Channel, which is bounded, or @ref UnboundedChannel.
 *
 * Send and receive take no lock on the fast path. A coroutine that cannot go on
 * parks under the lock and an opposite operation resumes it. Items from one
 * producer arrive in order.
 *
 * The stream ends for receivers after @ref close or after the last registered
 * producer leaves, and only when every sent item is received.
 *
 * @code
 * auto channel = make_channel<Chunk>(1000);
 *
 * // producer() counts the producer now, so the channel never sees zero
 * // producers while coroutines are still being scheduled.
 * auto produce = [ch = channel->producer()]() mutable -> CoroTask<void> {
 *     auto guard = ch.guard();
 *     for (auto chunk : read_chunks())
 *         co_await ch.send(std::move(chunk));
 * };
 * scope.spawn(std::move(produce));
 * @endcode
 */
template <typename Queue>
class BasicChannel : public std::enable_shared_from_this<BasicChannel<Queue>> {
   public:
    using value_type = typename Queue::value_type;
    using T = value_type;

    class ReceiveAwaitable;
    class SendAwaitable;

    /// Releases a producer slot on destruction. The stream ends when the last
    /// slot is released.
    class ProducerGuard {
       public:
        /// Tag type: adopt an already-registered producer slot (no increment).
        struct Adopt {};

       private:
        BasicChannel* channel_;
        dstd::SharedPtr<BasicChannel> shared_;

        void release() {
            if (channel_) {
                channel_->release_producer();
            }
            channel_ = nullptr;
            shared_.reset();
        }

       public:
        explicit ProducerGuard(BasicChannel* ch) : channel_(ch) {
            if (channel_) {
                channel_->register_producer();
            }
        }

        ProducerGuard(BasicChannel* ch, Adopt) : channel_(ch) {}

        ProducerGuard(BasicChannel* ch, dstd::SharedPtr<BasicChannel> shared,
                      Adopt)
            : channel_(ch), shared_(std::move(shared)) {}

        ~ProducerGuard() { release(); }

        ProducerGuard(const ProducerGuard&) = delete;
        ProducerGuard& operator=(const ProducerGuard&) = delete;

        ProducerGuard(ProducerGuard&& other) noexcept
            : channel_(other.channel_), shared_(std::move(other.shared_)) {
            other.channel_ = nullptr;
        }

        ProducerGuard& operator=(ProducerGuard&& other) noexcept {
            if (this != &other) {
                release();
                channel_ = other.channel_;
                shared_ = std::move(other.shared_);
                other.channel_ = nullptr;
            }
            return *this;
        }
    };

   private:
    struct ReceiveWaiterNode {
        std::coroutine_handle<> handle;
        dstd::Optional<T>* result{nullptr};
        Executor* executor{nullptr};
        ReceiveWaiterNode* next{nullptr};
    };

    struct SendWaiterNode {
        std::coroutine_handle<> handle;
        dstd::Optional<T>* item{nullptr};
        bool* sent{nullptr};
        Executor* executor{nullptr};
        SendWaiterNode* next{nullptr};
    };

   public:
    /// The awaiting task stops when the executor shuts down while it waits.
    class ReceiveAwaitable {
       private:
        BasicChannel* channel_;
        dstd::Optional<T> result_;
        ReceiveWaiterNode waiter_{};
        bool suspended_{false};
        bool cancelled_{false};
        detail::ParkNode* park_{nullptr};

       public:
        explicit ReceiveAwaitable(BasicChannel* channel) : channel_(channel) {}

        ReceiveAwaitable(const ReceiveAwaitable&) = delete;
        ReceiveAwaitable& operator=(const ReceiveAwaitable&) = delete;
        ReceiveAwaitable(ReceiveAwaitable&&) = delete;
        ReceiveAwaitable& operator=(ReceiveAwaitable&&) = delete;

        bool await_ready() {
            if (!channel_) return true;
            if (channel_->queue_.try_pop(result_) != detail::PopStatus::Ok) {
                return false;
            }
            channel_->after_pop();
            return true;
        }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) {
            detail::set_awaiting_async(h, true);
            waiter_.executor = detail::executor_for(h);

            BasicChannel* channel = channel_;
            detail::WakeBatch wakes;
            bool parked = false;
            {
                std::lock_guard<std::mutex> lock(channel->state_mutex_);
                channel->recv_waiting_.fetch_add(1, std::memory_order_seq_cst);
                if (channel->send_head_) channel->transfer_locked(wakes);
                detail::PopStatus status = detail::PopStatus::Empty;
                if (!channel->recv_head_) {
                    status = channel->queue_.try_pop(result_);
                }
                if (status == detail::PopStatus::Ok ||
                    channel->is_closed_for_receive_locked()) {
                    channel->recv_waiting_.fetch_sub(1,
                                                     std::memory_order_relaxed);
                    if (channel->send_head_) channel->transfer_locked(wakes);
                } else {
                    waiter_.handle = h;
                    waiter_.result = &result_;
                    channel->enqueue_receive_waiter_locked(&waiter_);
                    suspended_ = true;
                    if (waiter_.executor) {
                        park_ = waiter_.executor->park(
                            this, &ReceiveAwaitable::cancel_thunk);
                    }
                    parked = true;
                }
            }
            // A parked coroutine can run from here on, so only locals are safe.
            wakes.post();
            if (parked) return true;
            detail::set_awaiting_async(h, false);
            return false;
        }

        dstd::Optional<T> await_resume() {
            if (park_) {
                waiter_.executor->unpark(park_);
                park_ = nullptr;
            }
            suspended_ = false;
            if (cancelled_) detail::cancelled_unstoppable("channel receive");
            return std::move(result_);
        }

        static void cancel_thunk(void* self) noexcept {
            static_cast<ReceiveAwaitable*>(self)->cancel_parked();
        }

        void cancel_parked() noexcept {
            {
                std::unique_lock<std::mutex> lock(channel_->state_mutex_,
                                                  std::try_to_lock);
                if (!lock || !channel_->remove_receive_waiter_locked(&waiter_))
                    return;
                cancelled_ = true;
            }
            detail::stop_or_resume(waiter_.executor, park_, waiter_.handle);
        }

        ~ReceiveAwaitable() {
            if (park_) waiter_.executor->unpark(park_);
            if (!suspended_ || !channel_) return;

            std::lock_guard<std::mutex> lock(channel_->state_mutex_);
            if (channel_->remove_receive_waiter_locked(&waiter_)) {
                suspended_ = false;
            }
        }
    };

    /// co_await returns false when the channel is closed, and the item is then
    /// dropped with the awaitable. The awaiting task stops when the executor
    /// shuts down while it waits for room.
    class SendAwaitable {
       private:
        BasicChannel* channel_;
        dstd::Optional<T> item_;
        bool sent_{false};
        SendWaiterNode waiter_{};
        bool suspended_{false};
        bool cancelled_{false};
        detail::ParkNode* park_{nullptr};

       public:
        SendAwaitable(BasicChannel* channel, const T& item)
            : channel_(channel), item_(item) {}

        SendAwaitable(BasicChannel* channel, T&& item)
            : channel_(channel), item_(std::move(item)) {}

        SendAwaitable(const SendAwaitable&) = delete;
        SendAwaitable& operator=(const SendAwaitable&) = delete;
        SendAwaitable(SendAwaitable&&) = delete;
        SendAwaitable& operator=(SendAwaitable&&) = delete;

        bool await_ready() {
            if (!channel_) return true;
            if (channel_->send_waiting_.load(std::memory_order_relaxed) != 0) {
                return false;
            }
            const detail::PushStatus status = channel_->queue_.try_push(*item_);
            if (status == detail::PushStatus::Full) return false;
            sent_ = status == detail::PushStatus::Ok;
            if (sent_) channel_->after_push();
            return true;
        }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) {
            detail::set_awaiting_async(h, true);
            waiter_.executor = detail::executor_for(h);

            BasicChannel* channel = channel_;
            detail::WakeBatch wakes;
            bool parked = false;
            {
                std::lock_guard<std::mutex> lock(channel->state_mutex_);
                channel->send_waiting_.fetch_add(1, std::memory_order_seq_cst);
                if (channel->send_head_) channel->transfer_locked(wakes);
                detail::PushStatus status = detail::PushStatus::Full;
                if (!channel->send_head_) {
                    status = channel->queue_.try_push(*item_);
                }
                if (status == detail::PushStatus::Full) {
                    waiter_.handle = h;
                    waiter_.item = &item_;
                    waiter_.sent = &sent_;
                    channel->enqueue_send_waiter_locked(&waiter_);
                    suspended_ = true;
                    if (waiter_.executor) {
                        park_ = waiter_.executor->park(
                            this, &SendAwaitable::cancel_thunk);
                    }
                    parked = true;
                } else {
                    channel->send_waiting_.fetch_sub(1,
                                                     std::memory_order_relaxed);
                    sent_ = status == detail::PushStatus::Ok;
                    if (channel->recv_head_ || channel->blocking_waiters_ > 0) {
                        channel->transfer_locked(wakes);
                    }
                }
            }
            // A parked coroutine can run from here on, so only locals are safe.
            wakes.post();
            if (parked) return true;
            detail::set_awaiting_async(h, false);
            return false;
        }

        bool await_resume() {
            if (park_) {
                waiter_.executor->unpark(park_);
                park_ = nullptr;
            }
            suspended_ = false;
            if (cancelled_) detail::cancelled_unstoppable("channel send");
            return sent_;
        }

        static void cancel_thunk(void* self) noexcept {
            static_cast<SendAwaitable*>(self)->cancel_parked();
        }

        void cancel_parked() noexcept {
            {
                std::unique_lock<std::mutex> lock(channel_->state_mutex_,
                                                  std::try_to_lock);
                if (!lock || !channel_->remove_send_waiter_locked(&waiter_))
                    return;
                cancelled_ = true;
            }
            detail::stop_or_resume(waiter_.executor, park_, waiter_.handle);
        }

        ~SendAwaitable() {
            if (park_) waiter_.executor->unpark(park_);
            if (!suspended_ || !channel_) return;

            std::lock_guard<std::mutex> lock(channel_->state_mutex_);
            if (channel_->remove_send_waiter_locked(&waiter_)) {
                suspended_ = false;
            }
        }
    };

   private:
    friend class BasicChannelProducer<Queue>;
    friend class BasicChannelConsumer<Queue>;

    Queue queue_;

    // A push or pop writes its queue index, then reads the opposite counter. A
    // waiter raises its counter, then retries the queue. Both orders are
    // seq_cst, so one of the two sides always sees the other and no wake is
    // lost.
    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE)
        std::atomic<std::size_t> send_waiting_{0};
    std::atomic<std::size_t> recv_waiting_{0};

    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) mutable std::mutex state_mutex_;
    std::condition_variable cv_readable_;
    std::size_t blocking_waiters_{0};
    ReceiveWaiterNode* recv_head_{nullptr};
    ReceiveWaiterNode* recv_tail_{nullptr};
    SendWaiterNode* send_head_{nullptr};
    SendWaiterNode* send_tail_{nullptr};
    std::atomic<std::size_t> producer_count_{0};
    bool had_producers_{false};
    bool user_closed_{false};
    std::atomic<bool> closed_{false};

    bool is_closed_for_receive_locked() const {
        return user_closed_ ||
               (had_producers_ &&
                producer_count_.load(std::memory_order_relaxed) == 0);
    }

    void after_push() {
        if (recv_waiting_.load(std::memory_order_seq_cst) > 0) service();
    }

    void after_pop() {
        if (send_waiting_.load(std::memory_order_seq_cst) > 0) service();
    }

    void service() {
        detail::WakeBatch wakes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            transfer_locked(wakes);
        }
        wakes.post();
    }

    // Parked items always go through the queue, never straight to a receiver,
    // so the order of one producer holds. Runs until neither side can move.
    void transfer_locked(detail::WakeBatch& wakes) {
        bool progress = true;
        while (progress) {
            progress = false;
            while (send_head_) {
                const detail::PushStatus status =
                    queue_.try_push(**send_head_->item);
                if (status == detail::PushStatus::Full) break;
                SendWaiterNode* node = pop_send_waiter_locked();
                *node->sent = status == detail::PushStatus::Ok;
                wakes.add(node->handle, node->executor);
                progress = true;
            }
            while (recv_head_) {
                if (queue_.try_pop(*recv_head_->result) !=
                    detail::PopStatus::Ok) {
                    break;
                }
                ReceiveWaiterNode* node = pop_receive_waiter_locked();
                wakes.add(node->handle, node->executor);
                progress = true;
            }
        }
        if (is_closed_for_receive_locked()) {
            while (ReceiveWaiterNode* node = pop_receive_waiter_locked()) {
                wakes.add(node->handle, node->executor);
            }
        }
        if (blocking_waiters_ > 0) cv_readable_.notify_all();
    }

    void enqueue_receive_waiter_locked(ReceiveWaiterNode* node) {
        node->next = nullptr;
        if (recv_tail_) {
            recv_tail_->next = node;
        } else {
            recv_head_ = node;
        }
        recv_tail_ = node;
    }

    void enqueue_send_waiter_locked(SendWaiterNode* node) {
        node->next = nullptr;
        if (send_tail_) {
            send_tail_->next = node;
        } else {
            send_head_ = node;
        }
        send_tail_ = node;
    }

    bool remove_receive_waiter_locked(ReceiveWaiterNode* node) {
        ReceiveWaiterNode* prev = nullptr;
        for (ReceiveWaiterNode* curr = recv_head_; curr;
             prev = curr, curr = curr->next) {
            if (curr != node) continue;
            if (prev) {
                prev->next = curr->next;
            } else {
                recv_head_ = curr->next;
            }
            if (recv_tail_ == curr) recv_tail_ = prev;
            node->next = nullptr;
            recv_waiting_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    bool remove_send_waiter_locked(SendWaiterNode* node) {
        SendWaiterNode* prev = nullptr;
        for (SendWaiterNode* curr = send_head_; curr;
             prev = curr, curr = curr->next) {
            if (curr != node) continue;
            if (prev) {
                prev->next = curr->next;
            } else {
                send_head_ = curr->next;
            }
            if (send_tail_ == curr) send_tail_ = prev;
            node->next = nullptr;
            send_waiting_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    SendWaiterNode* pop_send_waiter_locked() {
        SendWaiterNode* node = send_head_;
        if (!node) return nullptr;
        send_head_ = node->next;
        if (!send_head_) send_tail_ = nullptr;
        node->next = nullptr;
        send_waiting_.fetch_sub(1, std::memory_order_relaxed);
        return node;
    }

    ReceiveWaiterNode* pop_receive_waiter_locked() {
        ReceiveWaiterNode* node = recv_head_;
        if (!node) return nullptr;
        recv_head_ = node->next;
        if (!recv_head_) recv_tail_ = nullptr;
        node->next = nullptr;
        recv_waiting_.fetch_sub(1, std::memory_order_relaxed);
        return node;
    }

    ProducerGuard adopt_producer() {
        return ProducerGuard(this, typename ProducerGuard::Adopt{});
    }

    ProducerGuard adopt_producer(dstd::SharedPtr<BasicChannel> shared) {
        return ProducerGuard(this, std::move(shared),
                             typename ProducerGuard::Adopt{});
    }

    void register_producer() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        had_producers_ = true;
        producer_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void release_producer() {
        detail::WakeBatch wakes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (producer_count_.fetch_sub(1, std::memory_order_relaxed) == 1) {
                closed_.store(true, std::memory_order_release);
                transfer_locked(wakes);
            }
        }
        wakes.post();
    }

   public:
    /// An @ref UnboundedChannel.
    BasicChannel()
        requires std::is_default_constructible_v<Queue>
    = default;

    /// @param capacity The exact capacity of a @ref Channel, at least 1 (0 logs
    ///        and aborts).
    explicit BasicChannel(std::size_t capacity)
        requires std::is_constructible_v<Queue, std::size_t>
        : queue_(capacity) {}

    ~BasicChannel() { close(); }

    BasicChannel(const BasicChannel&) = delete;
    BasicChannel& operator=(const BasicChannel&) = delete;
    BasicChannel(BasicChannel&&) = delete;
    BasicChannel& operator=(BasicChannel&&) = delete;

    static dstd::SharedPtr<BasicChannel> make(std::size_t capacity)
        requires std::is_constructible_v<Queue, std::size_t>
    {
        return dstd::make_shared<BasicChannel>(capacity);
    }

    static dstd::SharedPtr<BasicChannel> make()
        requires std::is_default_constructible_v<Queue>
    {
        return dstd::make_shared<BasicChannel>();
    }

    /// Returns a send handle with a producer slot already counted. Take it
    /// before the coroutine starts and capture it by value.
    BasicChannelProducer<Queue> producer() {
        auto sp = this->weak_from_this().lock();
        if (sp) {
            return BasicChannelProducer<Queue>(std::move(sp));
        }
        return BasicChannelProducer<Queue>(this);
    }

    /// Returns a receive handle that keeps a shared channel alive.
    BasicChannelConsumer<Queue> consumer() {
        auto sp = this->weak_from_this().lock();
        if (sp) {
            return BasicChannelConsumer<Queue>(std::move(sp));
        }
        return BasicChannelConsumer<Queue>(this);
    }

    /// Returns false when the channel is full or closed.
    bool try_send(const T& item) {
        T copy(item);
        return try_send(std::move(copy));
    }

    /// Returns false when the channel is full or closed, and `item` then keeps
    /// its value.
    bool try_send(T&& item) {
        if (queue_.try_push(item) != detail::PushStatus::Ok) return false;
        after_push();
        return true;
    }

    /// Returns false when the channel is empty.
    bool try_receive(T& item) {
        dstd::Optional<T> out;
        if (queue_.try_pop(out) != detail::PopStatus::Ok) return false;
        item = std::move(*out);
        after_pop();
        return true;
    }

    ReceiveAwaitable receive() { return ReceiveAwaitable(this); }

    /// Blocks this thread. Never call it from an executor worker.
    dstd::Optional<T> blocking_receive() {
        dstd::Optional<T> out;
        if (queue_.try_pop(out) == detail::PopStatus::Ok) {
            after_pop();
            return out;
        }

        detail::WakeBatch wakes;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            ++blocking_waiters_;
            recv_waiting_.fetch_add(1, std::memory_order_seq_cst);
            for (;;) {
                if (send_head_) transfer_locked(wakes);
                if (queue_.try_pop(out) == detail::PopStatus::Ok ||
                    is_closed_for_receive_locked()) {
                    break;
                }
                cv_readable_.wait(lock);
            }
            recv_waiting_.fetch_sub(1, std::memory_order_relaxed);
            --blocking_waiters_;
            if (send_head_) transfer_locked(wakes);
        }
        wakes.post();
        return out;
    }

    SendAwaitable send(const T& item) { return SendAwaitable(this, item); }

    SendAwaitable send(T&& item) {
        return SendAwaitable(this, std::move(item));
    }

    /// No send succeeds after this returns. Sent items stay readable.
    void close() {
        detail::WakeBatch wakes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            user_closed_ = true;
            closed_.store(true, std::memory_order_release);
            queue_.mark_closed();
            transfer_locked(wakes);
        }
        wakes.post();
    }

    bool is_closed() const { return closed_.load(std::memory_order_acquire); }

    bool is_closed_and_done() const {
        return is_closed() &&
               producer_count_.load(std::memory_order_acquire) == 0 &&
               queue_.size() == 0;
    }

    std::size_t producer_count() const {
        return producer_count_.load(std::memory_order_acquire);
    }

    std::size_t size() const { return queue_.size(); }

    std::size_t capacity() const { return queue_.capacity(); }

    bool empty() const { return queue_.size() == 0; }

    bool full() const { return queue_.size() >= queue_.capacity(); }
};

/**
 * @brief Send handle that counts a producer slot on construction.
 *
 * Make one with BasicChannel::producer() before you spawn a coroutine, then
 * capture it by value. Inside the coroutine, guard() returns the RAII guard
 * that releases the slot. A handle destroyed without guard() releases the slot
 * itself, such as when the task never ran.
 */
template <typename Queue>
class BasicChannelProducer {
    using ChannelType = BasicChannel<Queue>;
    using T = typename Queue::value_type;

    ChannelType* raw_{nullptr};
    dstd::SharedPtr<ChannelType> shared_;
    bool adopted_{false};

   public:
    explicit BasicChannelProducer(ChannelType* ch) : raw_(ch) {
        if (raw_) {
            raw_->register_producer();
        }
    }

    explicit BasicChannelProducer(dstd::SharedPtr<ChannelType> ch)
        : raw_(ch.get()), shared_(std::move(ch)) {
        if (raw_) {
            raw_->register_producer();
        }
    }

    ~BasicChannelProducer() {
        if (raw_ && !adopted_) {
            raw_->release_producer();
        }
    }

    BasicChannelProducer(BasicChannelProducer&& other) noexcept
        : raw_(other.raw_),
          shared_(std::move(other.shared_)),
          adopted_(other.adopted_) {
        other.raw_ = nullptr;
        other.adopted_ = true;
    }

    BasicChannelProducer& operator=(BasicChannelProducer&& other) noexcept {
        if (this != &other) {
            if (raw_ && !adopted_) {
                raw_->release_producer();
            }
            raw_ = other.raw_;
            shared_ = std::move(other.shared_);
            adopted_ = other.adopted_;
            other.raw_ = nullptr;
            other.adopted_ = true;
        }
        return *this;
    }

    BasicChannelProducer(const BasicChannelProducer&) = delete;
    BasicChannelProducer& operator=(const BasicChannelProducer&) = delete;

    /// Call once inside the coroutine body.
    typename ChannelType::ProducerGuard guard() {
        adopted_ = true;
        if (shared_) {
            return raw_->adopt_producer(shared_);
        }
        return raw_->adopt_producer();
    }

    auto send(const T& item) { return raw_->send(item); }

    auto send(T&& item) { return raw_->send(std::move(item)); }
};

/// Receive handle. It keeps the channel alive when it comes from a shared one.
template <typename Queue>
class BasicChannelConsumer {
    using ChannelType = BasicChannel<Queue>;
    using T = typename Queue::value_type;

    ChannelType* raw_{nullptr};
    dstd::SharedPtr<ChannelType> shared_;

   public:
    explicit BasicChannelConsumer(ChannelType* ch) : raw_(ch) {}

    explicit BasicChannelConsumer(dstd::SharedPtr<ChannelType> ch)
        : raw_(ch.get()), shared_(std::move(ch)) {}

    ~BasicChannelConsumer() = default;

    BasicChannelConsumer(BasicChannelConsumer&& other) noexcept
        : raw_(other.raw_), shared_(std::move(other.shared_)) {
        other.raw_ = nullptr;
    }

    BasicChannelConsumer& operator=(BasicChannelConsumer&& other) noexcept {
        if (this != &other) {
            raw_ = other.raw_;
            shared_ = std::move(other.shared_);
            other.raw_ = nullptr;
        }
        return *this;
    }

    BasicChannelConsumer(const BasicChannelConsumer& other)
        : raw_(other.raw_), shared_(other.shared_) {}

    BasicChannelConsumer& operator=(const BasicChannelConsumer& other) {
        if (this != &other) {
            raw_ = other.raw_;
            shared_ = other.shared_;
        }
        return *this;
    }

    auto receive() const { return raw_->receive(); }

    dstd::Optional<T> blocking_receive() const {
        return raw_->blocking_receive();
    }

    bool is_closed() const { return raw_->is_closed(); }
};

/// Bounded channel on a ring with exactly the capacity asked for.
template <typename T>
using Channel = BasicChannel<detail::RingQueue<T>>;

template <typename T>
using ChannelProducer = BasicChannelProducer<detail::RingQueue<T>>;

template <typename T>
using ChannelConsumer = BasicChannelConsumer<detail::RingQueue<T>>;

/// Channel that is never full, so a send never waits. The caller bounds the
/// memory, such as with a semaphore of bytes.
template <typename T>
using UnboundedChannel = BasicChannel<detail::UnboundedQueue<T>>;

/// @param capacity At least 1. Zero logs and aborts.
template <typename T>
dstd::SharedPtr<Channel<T>> make_channel(std::size_t capacity) {
    return Channel<T>::make(capacity);
}

template <typename T>
dstd::SharedPtr<UnboundedChannel<T>> make_unbounded_channel() {
    return UnboundedChannel<T>::make();
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_CHANNEL_H
