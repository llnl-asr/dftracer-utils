// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bulk_support.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/memory/config.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::comm::backend::bulk_support {

using dstd::Condition;
using dstd::Error;

bool Exposure::enter() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    ++uses_;
    return true;
}

void Exposure::leave() noexcept {
    dstd::Vector<Bulk::Drained> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--uses_ != 0 || !closed_) return;
        run = std::move(waiters_);
    }
    for (Bulk::Drained& drained : run)
        if (drained) drained();
}

void Exposure::close(Bulk::Drained done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        if (uses_ != 0) {
            waiters_.push_back(std::move(done));
            return;
        }
    }
    if (done) done();
}

bool Exposure::is_closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

bool Exposure::is_drained() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ && uses_ == 0;
}

Error unknown_exposure_error(std::uint32_t owner, std::uint64_t id) {
    return Error::from_format<"rank {} has no open buffer {}">(
        Condition::NotFound, owner, id);
}

dstd::Result<BufferRef> ExposureTable::add(std::uint64_t group,
                                           std::uint32_t rank,
                                           dstd::Span<std::byte> memory,
                                           Access access, MemoryKind kind) {
    if (kind != MemoryKind::Host)
        return dstd::Unexpected(unsupported_kind_error(kind));
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto ended = state_->ended.find(group);
    if (ended != state_->ended.end()) return dstd::Unexpected(ended->second);
    const std::uint64_t id = state_->next_id++;
    state_->exposures[Key{group, rank, id}] =
        dstd::make_shared<Exposure>(memory, access, kind, group, rank, id);
    BufferRef ref;
    ref.group = group;
    ref.id = id;
    ref.size = memory.size();
    ref.owner = rank;
    ref.access = access;
    ref.kind = kind;
    return ref;
}

dstd::Result<dstd::SharedPtr<Use>> ExposureTable::open(std::uint64_t group,
                                                       std::uint32_t owner,
                                                       std::uint64_t id,
                                                       bool writes) {
    dstd::SharedPtr<Exposure> exposure;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto ended = state_->ended.find(group);
        if (ended != state_->ended.end())
            return dstd::Unexpected(ended->second);
        const auto at = state_->exposures.find(Key{group, owner, id});
        if (at == state_->exposures.end())
            return dstd::Unexpected(unknown_exposure_error(owner, id));
        exposure = at->second;
    }
    if (writes && exposure->access() == Access::ReadOnly)
        return dstd::Unexpected(
            Error::from_format<
                "buffer {} of rank {} is read only, so a push is refused">(
                Condition::InvalidArgument, id, owner));
    if (!writes && exposure->access() == Access::WriteOnly)
        return dstd::Unexpected(
            Error::from_format<
                "buffer {} of rank {} is write only, so a pull is refused">(
                Condition::InvalidArgument, id, owner));
    if (!exposure->enter()) {
        if (exposure->is_drained()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->exposures.erase(Key{group, owner, id});
        }
        return dstd::Unexpected(unknown_exposure_error(owner, id));
    }
    return dstd::make_shared<Use>(std::move(exposure));
}

void ExposureTable::close(const BufferRef& ref, Bulk::Drained done) {
    const Key key{ref.group, ref.owner, ref.id};
    dstd::SharedPtr<Exposure> exposure;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto at = state_->exposures.find(key);
        if (at != state_->exposures.end()) exposure = at->second;
    }
    if (!exposure) {
        if (done) done();
        return;
    }
    // The exposure stays in the table while it drains, so every close of it
    // waits for the same transfers and a transfer that starts finds it closed;
    // the callback that runs when it drained drops it.
    exposure->close(
        [state = dstd::WeakPtr<State>(state_), key, done = std::move(done)] {
            if (const auto held = state.lock()) {
                std::lock_guard<std::mutex> lock(held->mutex);
                held->exposures.erase(key);
            }
            if (done) done();
        });
}

void ExposureTable::end(std::uint64_t group, const Error& why) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->ended.emplace(group, why);
}

dstd::Optional<Error> ExposureTable::ended(std::uint64_t group) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto at = state_->ended.find(group);
    if (at == state_->ended.end()) return {};
    return at->second;
}

void ExposureTable::release(std::uint64_t group) {
    dstd::Vector<dstd::SharedPtr<Exposure>> closing;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->ended.erase(group);
        for (auto at = state_->exposures.lower_bound(Key{group, 0, 0});
             at != state_->exposures.end() && at->first.group == group;) {
            closing.push_back(std::move(at->second));
            at = state_->exposures.erase(at);
        }
    }
    for (const auto& exposure : closing) exposure->close({});
}

std::uint64_t CreditGate::in_flight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t total = 0;
    for (const auto& entry : accounts_) total += entry.second.in_flight;
    return total;
}

std::uint64_t CreditGate::acquire(const Key& key, std::uint64_t bytes,
                                  Granted granted, std::uint64_t credit) {
    std::lock_guard<std::mutex> lock(mutex_);
    Account& account = accounts_[key];
    account.credit = credit;
    if (account.queue.empty() &&
        (account.in_flight == 0 || account.in_flight + bytes <= credit)) {
        account.in_flight += bytes;
        if (account.in_flight > peak_.load(std::memory_order_relaxed))
            peak_.store(account.in_flight, std::memory_order_relaxed);
        return 0;
    }
    const std::uint64_t ticket = next_ticket_++;
    account.queue.push_back(Waiter{ticket, bytes, std::move(granted)});
    return ticket;
}

bool CreditGate::withdraw(const Key& key, std::uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto at = accounts_.find(key);
    if (at == accounts_.end()) return false;
    dstd::Vector<Waiter>& queue = at->second.queue;
    for (auto waiter = queue.begin(); waiter != queue.end(); ++waiter) {
        if (waiter->ticket != ticket) continue;
        queue.erase(waiter);
        if (queue.empty() && at->second.in_flight == 0) accounts_.erase(at);
        return true;
    }
    return false;
}

void CreditGate::release(const Key& key, std::uint64_t bytes) {
    dstd::Vector<Granted> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto at = accounts_.find(key);
        if (at == accounts_.end()) return;
        Account& account = at->second;
        account.in_flight -= std::min(bytes, account.in_flight);
        while (!account.queue.empty()) {
            Waiter& next = account.queue.front();
            if (account.in_flight != 0 &&
                account.in_flight + next.bytes > account.credit)
                break;
            account.in_flight += next.bytes;
            if (account.in_flight > peak_.load(std::memory_order_relaxed))
                peak_.store(account.in_flight, std::memory_order_relaxed);
            run.push_back(std::move(next.granted));
            account.queue.erase(account.queue.begin());
        }
        if (account.in_flight == 0 && account.queue.empty())
            accounts_.erase(at);
    }
    for (Granted& granted : run) granted();
}

namespace {

struct Hold {
    Hold(CreditGate& gate_, const CreditGate::Key& key_, std::uint64_t bytes_)
        : gate(gate_), key(key_), bytes(bytes_) {}
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    ~Hold() {
        if (ticket == 0 || !gate.withdraw(key, ticket))
            gate.release(key, bytes);
    }
    CreditGate& gate;
    CreditGate::Key key;
    std::uint64_t bytes;
    std::uint64_t ticket = 0;
};

}  // namespace

dstd::SharedPtr<void> CreditGate::hold(const Key& key, std::uint64_t bytes,
                                       std::uint64_t credit) {
    auto held = dstd::make_shared<Hold>(*this, key, bytes);
    held->ticket = acquire(key, bytes, [] {}, credit);
    return held;
}

std::uint64_t credit_from_budget(std::uint32_t members) noexcept {
    const std::uint64_t budget = memory::config().budget;
    const std::uint64_t peers = members > 1 ? members - 1 : 1;
    const std::uint64_t share =
        budget / 100 *
        common::constants::comm_settings::BULK_CREDIT_BUDGET_PERCENT / peers;
    return std::clamp(share,
                      common::constants::comm_settings::BULK_MIN_CREDIT_BYTES,
                      common::constants::comm_settings::BULK_MAX_CREDIT_BYTES);
}

}  // namespace dftracer::utils::comm::backend::bulk_support
