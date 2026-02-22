#ifndef DFTRACER_UTILS_CORE_CORO_CHANNEL_H
#define DFTRACER_UTILS_CORE_CORO_CHANNEL_H

#include <concurrentqueue.h>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "dftracer/utils/core/coro/resumption_helper.h"

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::coro {

/**
 * Channel<T> - Producer-consumer queue for streaming data
 *
 * Usage (simple -- guard registers and releases automatically):
 * @code
 * auto channel = make_channel<Chunk>(1000);
 *
 * auto producer = make_task([&](CoroScope& ctx) -> CoroTask<void> {
 *     auto guard = channel->producer_guard();  // registers
 *     for (auto chunk : read_chunks())
 *         co_await channel->send(std::move(chunk));
 *     // ~ProducerGuard auto-releases; channel closes when last exits
 * });
 * @endcode
 *
 * Usage (coroutines -- pre-register then adopt for RAII release):
 * @code
 * auto channel = make_channel<Chunk>(0);
 *
 * // Pre-register before spawning so consumers see producers immediately
 * for (std::size_t i = 0; i < N; ++i)
 *     channel->register_producer();
 *
 * for (std::size_t i = 0; i < N; ++i) {
 *     scope.spawn([&](CoroScope& ctx) -> CoroTask<void> {
 *         auto guard = channel->adopt_producer();  // no increment
 *         // ... work ...
 *         co_return;  // ~ProducerGuard auto-releases
 *     });
 * }
 * @endcode
 */
template <typename T>
class Channel : public std::enable_shared_from_this<Channel<T>> {
   public:
    struct ReceiveWaiterNode {
        std::coroutine_handle<> handle;
        std::optional<T>* result{nullptr};
        dftracer::utils::Executor* executor{nullptr};
        ReceiveWaiterNode* next{nullptr};
    };
    struct SendWaiterNode {
        std::coroutine_handle<> handle;
        std::optional<T>* item{nullptr};
        bool* sent{nullptr};
        dftracer::utils::Executor* executor{nullptr};
        SendWaiterNode* next{nullptr};
    };
    class ReceiveAwaitable;
    class SendAwaitable;

    /**
     * RAII guard for producer tracking
     * Automatically closes channel when last producer exits
     */
    class ProducerGuard {
       public:
        /// Tag type: adopt an already-registered producer slot (no increment).
        struct Adopt {};

       private:
        Channel* channel_;

       public:
        /// Register a new producer slot.
        explicit ProducerGuard(Channel* ch) : channel_(ch) {
            if (channel_) {
                channel_->had_producers_.store(true, std::memory_order_release);
                channel_->num_producers_.fetch_add(1,
                                                   std::memory_order_release);
            }
        }

        /// Adopt an existing producer registration (no increment).
        /// Use after register_producer() when you need RAII release only.
        ProducerGuard(Channel* ch, Adopt) : channel_(ch) {}

        ~ProducerGuard() {
            if (!channel_) return;
            std::size_t prev = channel_->num_producers_.fetch_sub(
                1, std::memory_order_acq_rel);
            if (prev == 1) {
                // Mark closed for consumers only if user hasn't closed already
                if (!channel_->user_closed_.load(std::memory_order_acquire))
                    channel_->closed_.store(true, std::memory_order_release);
                channel_->notify_all_waiters();
            }
        }

        // Non-copyable
        ProducerGuard(const ProducerGuard&) = delete;
        ProducerGuard& operator=(const ProducerGuard&) = delete;

        // Movable
        ProducerGuard(ProducerGuard&& other) noexcept
            : channel_(other.channel_) {
            other.channel_ = nullptr;
        }

        ProducerGuard& operator=(ProducerGuard&& other) noexcept {
            if (this != &other) {
                if (channel_) {
                    // Release current channel
                    std::size_t remaining = channel_->num_producers_.fetch_sub(
                        1, std::memory_order_acq_rel);
                    if (remaining == 1) {
                        if (!channel_->user_closed_.load(
                                std::memory_order_acquire)) {
                            channel_->closed_.store(true,
                                                    std::memory_order_release);
                        }
                        channel_->notify_all_waiters();
                    }
                }
                channel_ = other.channel_;
                other.channel_ = nullptr;
            }
            return *this;
        }
    };

    class ReceiveAwaitable {
       private:
        Channel* channel_;
        std::optional<T> result_;
        ReceiveWaiterNode waiter_{};
        bool suspended_{false};

       public:
        explicit ReceiveAwaitable(Channel* channel) : channel_(channel) {}

        ReceiveAwaitable(const ReceiveAwaitable&) = delete;
        ReceiveAwaitable& operator=(const ReceiveAwaitable&) = delete;
        ReceiveAwaitable(ReceiveAwaitable&&) = delete;
        ReceiveAwaitable& operator=(ReceiveAwaitable&&) = delete;

        bool await_ready() {
            if (!channel_) {
                result_ = std::nullopt;
                return true;
            }

            T item;
            if (channel_->try_receive(item)) {
                result_ = std::optional<T>(std::move(item));
                return true;
            }

            bool consumed = false;
            {
                std::lock_guard<std::mutex> lock(channel_->state_mutex_);
                if (channel_->try_receive_locked(item)) {
                    result_ = std::optional<T>(std::move(item));
                    consumed = true;
                } else if (channel_->is_terminal_locked()) {
                    result_ = std::nullopt;
                    return true;
                }
            }

            if (consumed) {
                if (!channel_->wake_one_send_waiter_after_receive()) {
                    channel_->cv_writable_.notify_one();
                }
                return true;
            }

            return false;
        }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) {
            if (!channel_) {
                result_ = std::nullopt;
                return false;
            }

            if constexpr (requires(Promise& p) { p.awaiting_async_ = true; }) {
                h.promise().awaiting_async_ = true;
            }

            if constexpr (requires(Promise& p) {
                              p.get_root_promise();
                              p.get_root_promise()->awaiting_async_ = true;
                          }) {
                auto* root = h.promise().get_root_promise();
                if (root) {
                    root->awaiting_async_ = true;
                    waiter_.executor = root->get_executor();
                } else {
                    waiter_.executor = nullptr;
                }
            } else {
                waiter_.executor = nullptr;
            }

            T item;
            bool consumed = false;
            bool consumed_from_queue = false;
            std::coroutine_handle<> sender_handle;
            dftracer::utils::Executor* sender_executor = nullptr;
            {
                std::lock_guard<std::mutex> lock(channel_->state_mutex_);
                if (channel_->try_receive_locked(item)) {
                    result_ = std::optional<T>(std::move(item));
                    consumed = true;
                    consumed_from_queue = true;
                } else if (SendWaiterNode* sender_waiter =
                               channel_->pop_send_waiter_locked()) {
                    if (channel_->user_closed_.load(
                            std::memory_order_acquire)) {
                        if (sender_waiter->sent) {
                            *(sender_waiter->sent) = false;
                        }
                    } else {
                        if (sender_waiter->item &&
                            sender_waiter->item->has_value()) {
                            result_ = std::move(*(sender_waiter->item));
                            consumed = true;
                        } else {
                            result_ = std::nullopt;
                        }
                        if (sender_waiter->sent) {
                            *(sender_waiter->sent) = true;
                        }
                    }
                    sender_handle = sender_waiter->handle;
                    sender_executor = sender_waiter->executor;
                } else if (channel_->is_terminal_locked()) {
                    result_ = std::nullopt;
                    h.promise().awaiting_async_ = false;
                    if constexpr (requires(Promise& p) {
                                      p.get_root_promise();
                                      p.get_root_promise()->awaiting_async_ =
                                          false;
                                  }) {
                        if (auto* root = h.promise().get_root_promise()) {
                            root->awaiting_async_ = false;
                        }
                    }
                    return false;
                } else {
                    waiter_.handle = h;
                    waiter_.result = &result_;
                    waiter_.next = nullptr;
                    channel_->enqueue_receive_waiter_locked(&waiter_);
                    suspended_ = true;
                    return true;
                }
            }

            if (sender_handle) {
                schedule_coroutine_resumption_helper(sender_executor,
                                                     sender_handle);
                if (!consumed) {
                    result_ = std::nullopt;
                }
            }

            if (consumed) {
                if (consumed_from_queue) {
                    if (!channel_->wake_one_send_waiter_after_receive()) {
                        channel_->cv_writable_.notify_one();
                    }
                }
                h.promise().awaiting_async_ = false;
                if constexpr (requires(Promise& p) {
                                  p.get_root_promise();
                                  p.get_root_promise()->awaiting_async_ = false;
                              }) {
                    if (auto* root = h.promise().get_root_promise()) {
                        root->awaiting_async_ = false;
                    }
                }
                return false;
            }

            h.promise().awaiting_async_ = false;
            if constexpr (requires(Promise& p) {
                              p.get_root_promise();
                              p.get_root_promise()->awaiting_async_ = false;
                          }) {
                if (auto* root = h.promise().get_root_promise()) {
                    root->awaiting_async_ = false;
                }
            }

            return false;
        }

        std::optional<T> await_resume() {
            suspended_ = false;
            return std::move(result_);
        }

        ~ReceiveAwaitable() {
            if (!suspended_ || !channel_) return;

            std::lock_guard<std::mutex> lock(channel_->state_mutex_);
            ReceiveWaiterNode* prev = nullptr;
            ReceiveWaiterNode* curr = channel_->recv_waiters_head_;
            while (curr) {
                if (curr == &waiter_) {
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        channel_->recv_waiters_head_ = curr->next;
                    }
                    if (channel_->recv_waiters_tail_ == curr) {
                        channel_->recv_waiters_tail_ = prev;
                    }
                    waiter_.next = nullptr;
                    suspended_ = false;
                    return;
                }
                prev = curr;
                curr = curr->next;
            }
        }
    };

    class SendAwaitable {
       private:
        Channel* channel_;
        std::optional<T> item_;
        bool sent_{false};
        SendWaiterNode waiter_{};
        bool suspended_{false};

       public:
        SendAwaitable(Channel* channel, const T& item)
            : channel_(channel), item_(item) {}

        SendAwaitable(Channel* channel, T&& item)
            : channel_(channel), item_(std::move(item)) {}

        SendAwaitable(const SendAwaitable&) = delete;
        SendAwaitable& operator=(const SendAwaitable&) = delete;
        SendAwaitable(SendAwaitable&&) = delete;
        SendAwaitable& operator=(SendAwaitable&&) = delete;

        bool await_ready() {
            if (!channel_ ||
                channel_->user_closed_.load(std::memory_order_acquire)) {
                sent_ = false;
                return true;
            }
            return false;
        }

        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> h) {
            if (!channel_ ||
                channel_->user_closed_.load(std::memory_order_acquire)) {
                sent_ = false;
                return false;
            }

            if constexpr (requires(Promise& p) { p.awaiting_async_ = true; }) {
                h.promise().awaiting_async_ = true;
            }

            if constexpr (requires(Promise& p) {
                              p.get_root_promise();
                              p.get_root_promise()->awaiting_async_ = true;
                          }) {
                auto* root = h.promise().get_root_promise();
                if (root) {
                    root->awaiting_async_ = true;
                    waiter_.executor = root->get_executor();
                } else {
                    waiter_.executor = nullptr;
                }
            } else {
                waiter_.executor = nullptr;
            }

            std::coroutine_handle<> receive_handle;
            dftracer::utils::Executor* receive_executor = nullptr;
            bool notify_readable = false;

            {
                std::lock_guard<std::mutex> lock(channel_->state_mutex_);

                if (channel_->user_closed_.load(std::memory_order_acquire)) {
                    sent_ = false;
                    if constexpr (requires(Promise& p) {
                                      p.get_root_promise();
                                      p.get_root_promise()->awaiting_async_ =
                                          false;
                                  }) {
                        if (auto* root = h.promise().get_root_promise()) {
                            root->awaiting_async_ = false;
                        }
                    }
                    h.promise().awaiting_async_ = false;
                    return false;
                }

                ReceiveWaiterNode* waiter =
                    channel_->pop_receive_waiter_locked();
                if (waiter) {
                    if (waiter->result) {
                        *(waiter->result) = std::move(item_);
                    }
                    sent_ = true;
                    receive_handle = waiter->handle;
                    receive_executor = waiter->executor;

                    if constexpr (requires(Promise& p) {
                                      p.get_root_promise();
                                      p.get_root_promise()->awaiting_async_ =
                                          false;
                                  }) {
                        if (auto* root = h.promise().get_root_promise()) {
                            root->awaiting_async_ = false;
                        }
                    }
                    h.promise().awaiting_async_ = false;
                } else {
                    if (channel_->capacity_ != SIZE_MAX &&
                        channel_->available_slots_ == 0) {
                        waiter_.handle = h;
                        waiter_.item = &item_;
                        waiter_.sent = &sent_;
                        waiter_.next = nullptr;
                        channel_->enqueue_send_waiter_locked(&waiter_);
                        suspended_ = true;
                        return true;
                    }

                    if (channel_->capacity_ != SIZE_MAX) {
                        --channel_->available_slots_;
                    }
                    channel_->pending_items_.fetch_add(
                        1, std::memory_order_acq_rel);
                    channel_->queue_.enqueue(std::move(*item_));
                    sent_ = true;
                    notify_readable = true;

                    if constexpr (requires(Promise& p) {
                                      p.get_root_promise();
                                      p.get_root_promise()->awaiting_async_ =
                                          false;
                                  }) {
                        if (auto* root = h.promise().get_root_promise()) {
                            root->awaiting_async_ = false;
                        }
                    }
                    h.promise().awaiting_async_ = false;
                }
            }

            if (receive_handle) {
                schedule_coroutine_resumption_helper(receive_executor,
                                                     receive_handle);
            } else if (notify_readable) {
                channel_->cv_readable_.notify_one();
            }

            return false;
        }

        bool await_resume() {
            suspended_ = false;
            return sent_;
        }

        ~SendAwaitable() {
            if (!suspended_ || !channel_) return;

            std::lock_guard<std::mutex> lock(channel_->state_mutex_);
            SendWaiterNode* prev = nullptr;
            SendWaiterNode* curr = channel_->send_waiters_head_;
            while (curr) {
                if (curr == &waiter_) {
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        channel_->send_waiters_head_ = curr->next;
                    }
                    if (channel_->send_waiters_tail_ == curr) {
                        channel_->send_waiters_tail_ = prev;
                    }
                    waiter_.next = nullptr;
                    suspended_ = false;
                    return;
                }
                prev = curr;
                curr = curr->next;
            }
        }
    };

   private:
    moodycamel::ConcurrentQueue<T> queue_;

    std::size_t capacity_;
    std::size_t available_slots_;
    mutable std::mutex state_mutex_;
    std::condition_variable cv_readable_;
    std::condition_variable cv_writable_;
    std::atomic<std::size_t> num_producers_{0};
    std::atomic<bool> had_producers_{false};
    std::atomic<std::size_t> active_sends_{0};
    std::atomic<std::size_t> pending_items_{0};
    std::atomic<bool> user_closed_{false};
    std::atomic<bool> closed_{false};
    ReceiveWaiterNode* recv_waiters_head_{nullptr};
    ReceiveWaiterNode* recv_waiters_tail_{nullptr};
    SendWaiterNode* send_waiters_head_{nullptr};
    SendWaiterNode* send_waiters_tail_{nullptr};

    bool is_terminal_locked() const {
        const bool no_producers =
            num_producers_.load(std::memory_order_acquire) == 0;
        const bool had_producers =
            had_producers_.load(std::memory_order_acquire);
        const bool no_active_sends =
            active_sends_.load(std::memory_order_acquire) == 0;
        const bool no_pending =
            pending_items_.load(std::memory_order_acquire) == 0;
        const bool user_closed = user_closed_.load(std::memory_order_acquire);
        return (user_closed || (had_producers && no_producers)) &&
               no_active_sends && no_pending;
    }

    void mark_item_consumed() {
        const std::size_t prev =
            pending_items_.fetch_sub(1, std::memory_order_acq_rel);
        assert(prev > 0 && "Channel pending_items underflow");
        (void)prev;
    }

    void release_slot_if_bounded_locked() {
        if (capacity_ != SIZE_MAX) {
            ++available_slots_;
        }
    }

    void release_slot_if_bounded() {
        if (capacity_ == SIZE_MAX) {
            return;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        release_slot_if_bounded_locked();
        cv_writable_.notify_one();
    }

    bool try_receive_locked(T& item) {
        const bool ok = queue_.try_dequeue(item);
        if (ok) {
            mark_item_consumed();
        }
        return ok;
    }

    void enqueue_receive_waiter_locked(ReceiveWaiterNode* node) {
        node->next = nullptr;
        if (recv_waiters_tail_) {
            recv_waiters_tail_->next = node;
        } else {
            recv_waiters_head_ = node;
        }
        recv_waiters_tail_ = node;
    }

    void enqueue_send_waiter_locked(SendWaiterNode* node) {
        node->next = nullptr;
        if (send_waiters_tail_) {
            send_waiters_tail_->next = node;
        } else {
            send_waiters_head_ = node;
        }
        send_waiters_tail_ = node;
    }

    void enqueue_send_waiter_front_locked(SendWaiterNode* node) {
        node->next = send_waiters_head_;
        send_waiters_head_ = node;
        if (!send_waiters_tail_) {
            send_waiters_tail_ = node;
        }
    }

    SendWaiterNode* pop_send_waiter_locked() {
        SendWaiterNode* node = send_waiters_head_;
        if (!node) return nullptr;
        send_waiters_head_ = node->next;
        if (!send_waiters_head_) send_waiters_tail_ = nullptr;
        node->next = nullptr;
        return node;
    }

    void enqueue_receive_waiter_front_locked(ReceiveWaiterNode* node) {
        node->next = recv_waiters_head_;
        recv_waiters_head_ = node;
        if (!recv_waiters_tail_) {
            recv_waiters_tail_ = node;
        }
    }

    ReceiveWaiterNode* pop_receive_waiter_locked() {
        ReceiveWaiterNode* node = recv_waiters_head_;
        if (!node) return nullptr;
        recv_waiters_head_ = node->next;
        if (!recv_waiters_head_) recv_waiters_tail_ = nullptr;
        node->next = nullptr;
        return node;
    }

    void wake_all_receive_waiters_terminal() {
        for (;;) {
            ReceiveWaiterNode* waiter = nullptr;
            std::coroutine_handle<> handle;
            dftracer::utils::Executor* executor = nullptr;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                waiter = pop_receive_waiter_locked();
                if (!waiter) {
                    return;
                }

                T item;
                if (waiter->result) {
                    if (queue_.try_dequeue(item)) {
                        mark_item_consumed();
                        release_slot_if_bounded_locked();
                        *(waiter->result) = std::optional<T>(std::move(item));
                    } else {
                        *(waiter->result) = std::nullopt;
                    }
                }
                handle = waiter->handle;
                executor = waiter->executor;
            }

            if (handle) {
                schedule_coroutine_resumption_helper(executor, handle);
            }
        }
    }

    void wake_all_send_waiters_closed() {
        for (;;) {
            SendWaiterNode* waiter = nullptr;
            std::coroutine_handle<> handle;
            dftracer::utils::Executor* executor = nullptr;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                waiter = pop_send_waiter_locked();
                if (!waiter) {
                    return;
                }
                if (waiter->sent) {
                    *(waiter->sent) = false;
                }
                handle = waiter->handle;
                executor = waiter->executor;
            }

            if (handle) {
                schedule_coroutine_resumption_helper(executor, handle);
            }
        }
    }

    bool wake_one_send_waiter_after_receive() {
        std::coroutine_handle<> sender_handle;
        std::coroutine_handle<> receiver_handle;
        dftracer::utils::Executor* sender_executor = nullptr;
        dftracer::utils::Executor* receiver_executor = nullptr;
        bool notify_readable = false;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            release_slot_if_bounded_locked();

            SendWaiterNode* sender_waiter = pop_send_waiter_locked();
            if (!sender_waiter) {
                return false;
            }

            if (user_closed_.load(std::memory_order_acquire)) {
                if (sender_waiter->sent) {
                    *(sender_waiter->sent) = false;
                }
                sender_handle = sender_waiter->handle;
                sender_executor = sender_waiter->executor;
            } else {
                ReceiveWaiterNode* recv_waiter = pop_receive_waiter_locked();
                if (recv_waiter) {
                    if (recv_waiter->result) {
                        if (sender_waiter->item) {
                            *(recv_waiter->result) =
                                std::move(*(sender_waiter->item));
                        } else {
                            *(recv_waiter->result) = std::nullopt;
                        }
                    }
                    receiver_handle = recv_waiter->handle;
                    receiver_executor = recv_waiter->executor;
                } else {
                    if (capacity_ != SIZE_MAX && available_slots_ == 0) {
                        enqueue_send_waiter_front_locked(sender_waiter);
                        return false;
                    }
                    if (capacity_ != SIZE_MAX) {
                        --available_slots_;
                    }
                    if (sender_waiter->item &&
                        sender_waiter->item->has_value()) {
                        pending_items_.fetch_add(1, std::memory_order_acq_rel);
                        queue_.enqueue(std::move(*(*sender_waiter->item)));
                    }
                    notify_readable = true;
                }

                if (sender_waiter->sent) {
                    *(sender_waiter->sent) = true;
                }
                sender_handle = sender_waiter->handle;
                sender_executor = sender_waiter->executor;
            }
        }

        if (receiver_handle) {
            schedule_coroutine_resumption_helper(receiver_executor,
                                                 receiver_handle);
        } else if (notify_readable) {
            cv_readable_.notify_one();
        }

        if (sender_handle) {
            schedule_coroutine_resumption_helper(sender_executor,
                                                 sender_handle);
        }

        return true;
    }

    bool try_handoff_to_receive_waiter(T& item) {
        std::coroutine_handle<> resume_handle;
        dftracer::utils::Executor* resume_executor = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ReceiveWaiterNode* waiter = pop_receive_waiter_locked();
            if (!waiter) {
                return false;
            }
            if (waiter->result) {
                *(waiter->result) = std::optional<T>(std::move(item));
            }
            resume_handle = waiter->handle;
            resume_executor = waiter->executor;
        }
        if (resume_handle) {
            schedule_coroutine_resumption_helper(resume_executor,
                                                 resume_handle);
        }
        return true;
    }

    void notify_all_waiters() {
        cv_readable_.notify_all();
        cv_writable_.notify_all();

        bool terminal = false;
        bool closed_for_receive = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            terminal = is_terminal_locked();
            const bool user_closed =
                user_closed_.load(std::memory_order_acquire);
            const bool had_producers =
                had_producers_.load(std::memory_order_acquire);
            const bool no_producers =
                num_producers_.load(std::memory_order_acquire) == 0;
            closed_for_receive = user_closed || (had_producers && no_producers);
        }

        if (terminal || closed_for_receive) {
            wake_all_receive_waiters_terminal();
        }

        if (terminal || user_closed_.load(std::memory_order_acquire)) {
            wake_all_send_waiters_closed();
        }
    }

    void maybe_notify_terminal() {
        if (num_producers_.load(std::memory_order_acquire) == 0 &&
            active_sends_.load(std::memory_order_acquire) == 0 &&
            pending_items_.load(std::memory_order_acquire) == 0) {
            cv_readable_.notify_all();
            wake_all_receive_waiters_terminal();
        }
    }

   public:
    /**
     * Constructor
     * @param capacity Maximum number of items in queue (0 = unlimited)
     */
    explicit Channel(std::size_t capacity = 0)
        : queue_(capacity == 0 ? 1024 : capacity),
          capacity_(capacity == 0 ? SIZE_MAX : capacity),
          available_slots_(capacity_ == SIZE_MAX ? SIZE_MAX : capacity_) {}

    ~Channel() { close(); }

    // Non-copyable, non-movable (moodycamel queue is non-movable)
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    /**
     * Helper to create shared_ptr Channel<T>
     */
    static std::shared_ptr<Channel<T>> make(std::size_t capacity = 0) {
        return std::make_shared<Channel<T>>(capacity);
    }

    /**
     * Get producer guard (RAII)
     * Use this to automatically close channel when last producer exits
     */
    ProducerGuard producer_guard() { return ProducerGuard(this); }

    /**
     * Adopt an already-registered producer slot as an RAII guard.
     * Call register_producer() first, then adopt_producer() inside the
     * coroutine/thread to get automatic release on scope exit.
     */
    ProducerGuard adopt_producer() {
        return ProducerGuard(this, typename ProducerGuard::Adopt{});
    }

    /**
     * Pre-register a single producer before the task actually starts
     */
    void register_producer() {
        had_producers_.store(true, std::memory_order_release);
        num_producers_.fetch_add(1, std::memory_order_release);
    }

    /**
     * Pre-register multiple producers at once
     */
    void register_producers(std::size_t n) {
        if (n > 0) {
            had_producers_.store(true, std::memory_order_release);
            num_producers_.fetch_add(n, std::memory_order_release);
        }
    }

    /**
     * Release a pre-registered producer slot
     */
    void release_producer() {
        std::size_t prev =
            num_producers_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            if (!user_closed_.load(std::memory_order_acquire)) {
                closed_.store(true, std::memory_order_release);
            }
            notify_all_waiters();
        }
    }

    /**
     * Try to send without blocking
     *
     * @param item Item to send
     * @return true if sent, false if queue full or closed
     */
    bool try_send(const T& item) { return try_send_impl(item); }

    bool try_send(T&& item) { return try_send_impl(std::move(item)); }

   private:
    template <typename U>
    bool try_send_impl(U&& item) {
        if (user_closed_.load(std::memory_order_acquire)) return false;
        active_sends_.fetch_add(1, std::memory_order_acq_rel);

        std::coroutine_handle<> resume_handle;
        dftracer::utils::Executor* resume_executor = nullptr;
        bool enqueued = false;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (user_closed_.load(std::memory_order_acquire)) {
                active_sends_.fetch_sub(1, std::memory_order_acq_rel);
                maybe_notify_terminal();
                return false;
            }

            ReceiveWaiterNode* waiter = pop_receive_waiter_locked();
            if (waiter) {
                if (waiter->result) {
                    *(waiter->result) = std::optional<T>(std::forward<U>(item));
                }
                resume_handle = waiter->handle;
                resume_executor = waiter->executor;
            } else {
                if (capacity_ != SIZE_MAX) {
                    if (available_slots_ == 0) {
                        active_sends_.fetch_sub(1, std::memory_order_acq_rel);
                        maybe_notify_terminal();
                        return false;
                    }
                    --available_slots_;
                }

                pending_items_.fetch_add(1, std::memory_order_acq_rel);
                enqueued = queue_.try_enqueue(std::forward<U>(item));
                if (enqueued) {
                } else if (capacity_ != SIZE_MAX) {
                    pending_items_.fetch_sub(1, std::memory_order_acq_rel);
                    ++available_slots_;
                } else {
                    pending_items_.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
        }

        active_sends_.fetch_sub(1, std::memory_order_acq_rel);

        if (resume_handle) {
            schedule_coroutine_resumption_helper(resume_executor,
                                                 resume_handle);
        } else if (enqueued) {
            cv_readable_.notify_one();
        }

        maybe_notify_terminal();
        return resume_handle || enqueued;
    }

   public:
    /**
     * Try to receive without blocking
     *
     * @param item Output parameter for received item
     * @return true if received, false if queue empty
     */
    bool try_receive(T& item) {
        const bool ok = queue_.try_dequeue(item);
        if (ok) {
            mark_item_consumed();
            if (!wake_one_send_waiter_after_receive()) {
                cv_writable_.notify_one();
            }
            maybe_notify_terminal();
        }
        return ok;
    }

    ReceiveAwaitable receive() { return ReceiveAwaitable(this); }

    SendAwaitable send(const T& item) { return SendAwaitable(this, item); }

    SendAwaitable send(T&& item) {
        return SendAwaitable(this, std::move(item));
    }

    /**
     * Close channel
     * No more items can be sent after this
     */
    void close() {
        user_closed_.store(true, std::memory_order_release);
        closed_.store(true, std::memory_order_release);
        notify_all_waiters();
    }

    /**
     * Check if channel is closed
     */
    bool is_closed() const { return closed_.load(std::memory_order_acquire); }

    /**
     * Check if channel is closed and no producers remain
     */
    bool is_closed_and_done() const {
        return is_closed() &&
               num_producers_.load(std::memory_order_acquire) == 0 &&
               pending_items_.load(std::memory_order_acquire) == 0;
    }

    /**
     * Get number of active producers
     */
    std::size_t num_producers() const {
        return num_producers_.load(std::memory_order_acquire);
    }

    /**
     * Get current queue size (approximate, lock-free)
     */
    std::size_t size() const { return queue_.size_approx(); }

    /**
     * Get channel capacity
     */
    std::size_t capacity() const { return capacity_; }

    /**
     * Check if queue is empty (approximate)
     */
    bool empty() const { return queue_.size_approx() == 0; }

    /**
     * Check if queue is full (approximate)
     */
    bool full() const { return queue_.size_approx() >= capacity_; }
};

/**
 * Helper to create shared_ptr Channel<T>
 */
template <typename T>
std::shared_ptr<Channel<T>> make_channel(std::size_t capacity) {
    return Channel<T>::make(capacity);
}
}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_CHANNEL_H
