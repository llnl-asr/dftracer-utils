// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/plugins/host/port_hub.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace dftracer::utils::plugins::host {

namespace {

constexpr std::uint64_t PERMITS = std::uint64_t{1} << 40;

bool name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '/' || c == '-';
}

template <class T>
T load(std::uint64_t bits) {
    T v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

template <class T>
std::uint64_t store(T v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof v);
    return bits;
}

template <class T>
std::uint64_t fold(dftu_merge_op op, std::uint64_t a, std::uint64_t b) {
    const T x = load<T>(a);
    const T y = load<T>(b);
    switch (op) {
        case DFTU_MERGE_MIN:
            return store<T>(y < x ? y : x);
        case DFTU_MERGE_MAX:
            return store<T>(y > x ? y : x);
        default:
            break;
    }
    if constexpr (std::is_floating_point_v<T>) {
        return store<T>(x + y);
    } else {
        using U = std::make_unsigned_t<T>;
        return store<T>(static_cast<T>(static_cast<U>(x) + static_cast<U>(y)));
    }
}

std::uint64_t fold_scalar(dftu_merge_op op, dftu_type type, std::uint64_t a,
                          std::uint64_t b) {
    switch (type) {
        case DFTU_T_I64:
            return fold<std::int64_t>(op, a, b);
        case DFTU_T_U64:
            return fold<std::uint64_t>(op, a, b);
        default:
            return fold<double>(op, a, b);
    }
}

bool scalar_type(dftu_type type) {
    return type == DFTU_T_I64 || type == DFTU_T_U64 || type == DFTU_T_F64;
}

dftu_port_status copy_out(const dstd::Vector<std::byte>& from, void* buf,
                          std::uint32_t cap, std::uint32_t* out_len) {
    *out_len = static_cast<std::uint32_t>(from.size());
    if (from.size() > cap) return DFTU_PORT_TOO_BIG;
    if (!from.empty()) std::memcpy(buf, from.data(), from.size());
    return DFTU_PORT_OK;
}

}  // namespace

std::int32_t phase_seed_merge(std::int32_t seed) {
    switch (seed) {
        case DFTU_PHASE_SEED_TS_MIN:
            return DFTU_MERGE_MIN;
        case DFTU_PHASE_SEED_TS_MAX:
            return DFTU_MERGE_MAX;
        case DFTU_PHASE_SEED_EVENT_COUNT:
            return DFTU_MERGE_SUM;
        default:
            return -1;
    }
}

dstd::StringView phase_seed_name(std::int32_t seed) {
    switch (seed) {
        case DFTU_PHASE_SEED_TS_MIN:
            return "TsMin";
        case DFTU_PHASE_SEED_TS_MAX:
            return "TsMax";
        case DFTU_PHASE_SEED_EVENT_COUNT:
            return "EventCount";
        default:
            return "unknown";
    }
}

bool phase_merge_accepts(std::int32_t op, std::int32_t type) {
    switch (op) {
        case DFTU_MERGE_SUM:
        case DFTU_MERGE_MIN:
        case DFTU_MERGE_MAX:
            return scalar_type(type);
        case DFTU_MERGE_UNION:
            return type == DFTU_T_U64;
        case DFTU_MERGE_CONCAT:
            return type == DFTU_T_BYTES;
        default:
            return false;
    }
}

dstd::Result<dstd::String> qualify_port_name(dstd::StringView plugin,
                                             dstd::StringView name,
                                             bool* is_own) {
    if (name.empty()) {
        return common::make_error<"a port name is empty">(
            common::ErrorCode::InvalidArgument);
    }
    for (const char c : name) {
        if (!name_char_ok(c)) {
            return common::make_error<
                "port name '{}' holds a character outside [a-z0-9._/-]">(
                common::ErrorCode::InvalidArgument, name);
        }
    }
    if (name.substr(0, 5) == "dftu.") {
        return common::make_error<
            "port name '{}' is in the 'dftu.' namespace, which belongs to the "
            "host">(common::ErrorCode::InvalidArgument, name);
    }
    if (plugin.empty()) {
        return common::make_error<
            "a plugin needs a name before it can declare port '{}'">(
            common::ErrorCode::InvalidArgument, name);
    }
    const dstd::String prefix = dstd::fmt<"plugin.{}.">(plugin);
    if (name.substr(0, 7) == "plugin.") {
        *is_own = name.substr(0, prefix.size()) == prefix;
        return dstd::String(name);
    }
    *is_own = true;
    return dstd::fmt<"{}{}">(prefix, name);
}

PortChannel::PortChannel(const ChannelDecl& decl)
    : decl_(decl), space_(PERMITS), items_(PERMITS) {
    (void)items_.try_acquire(PERMITS);
    (void)space_.try_acquire(PERMITS - decl_.credits);
}

dftu_port_status PortChannel::push(dstd::Vector<std::byte> message,
                                   bool took_credit) {
    dftu_port_status status = DFTU_PORT_OK;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            status = DFTU_PORT_CLOSED;
        } else if (queue_.size() >= decl_.credits &&
                   decl_.overflow == DFTU_OVERFLOW_DROP_OLDEST) {
            queue_.pop_front();
            queue_.push_back(std::move(message));
        } else if (queue_.size() >= decl_.credits) {
            status = DFTU_PORT_FULL;
        } else {
            queue_.push_back(std::move(message));
            queued = true;
        }
    }
    if (status != DFTU_PORT_OK && took_credit) space_.release(1);
    if (queued) items_.release(1);
    return status;
}

coro::CoroTask<void> PortChannel::send(dstd::Vector<std::byte> message,
                                       dftu_port_status* status) {
    if (message.size() > decl_.max_msg_bytes) {
        *status = DFTU_PORT_TOO_BIG;
        co_return;
    }
    const bool block = decl_.overflow == DFTU_OVERFLOW_BLOCK;
    if (block) co_await space_.acquire(1);
    *status = push(std::move(message), block);
}

bool PortChannel::send_now(dstd::Vector<std::byte>& message,
                           dftu_port_status* status) {
    if (message.size() > decl_.max_msg_bytes) {
        *status = DFTU_PORT_TOO_BIG;
        return true;
    }
    const bool block = decl_.overflow == DFTU_OVERFLOW_BLOCK;
    if (block && !space_.try_acquire(1)) return false;
    *status = push(std::move(message), block);
    return true;
}

dftu_port_status PortChannel::try_send(dstd::Vector<std::byte> message) {
    if (message.size() > decl_.max_msg_bytes) return DFTU_PORT_TOO_BIG;
    const bool block = decl_.overflow == DFTU_OVERFLOW_BLOCK;
    if (block && !space_.try_acquire(1)) return DFTU_PORT_FULL;
    return push(std::move(message), block);
}

void PortChannel::finish_recv(void* buf, std::uint32_t cap,
                              std::uint32_t* out_len, dftu_port_status* status,
                              bool exact) {
    bool give_back = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            *out_len = 0;
            *status = DFTU_PORT_CLOSED;
            give_back = true;
        } else if (exact && queue_.front().size() != cap) {
            *out_len = static_cast<std::uint32_t>(queue_.front().size());
            *status = DFTU_PORT_TOO_BIG;
            queue_.pop_front();
        } else {
            *status = copy_out(queue_.front(), buf, cap, out_len);
            if (*status == DFTU_PORT_OK) {
                queue_.pop_front();
            } else {
                give_back = true;
            }
        }
    }
    if (give_back) {
        items_.release(1);
    } else if (decl_.overflow == DFTU_OVERFLOW_BLOCK) {
        space_.release(1);
    }
}

coro::CoroTask<void> PortChannel::recv(void* buf, std::uint32_t cap,
                                       std::uint32_t* out_len,
                                       dftu_port_status* status, bool exact) {
    co_await items_.acquire(1);
    finish_recv(buf, cap, out_len, status, exact);
}

bool PortChannel::recv_now(void* buf, std::uint32_t cap, std::uint32_t* out_len,
                           dftu_port_status* status, bool exact) {
    if (!items_.try_acquire(1)) return false;
    finish_recv(buf, cap, out_len, status, exact);
    return true;
}

void PortChannel::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
    }
    items_.release(1);
    if (decl_.overflow == DFTU_OVERFLOW_BLOCK) space_.release(PERMITS / 2);
}

std::uint64_t PortLatest::set(const void* data, std::uint32_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* p = static_cast<const std::byte*>(data);
    bytes_.assign(p, p + len);
    return ++generation_;
}

dftu_port_status PortLatest::get(void* buf, std::uint32_t cap,
                                 std::uint32_t* out_len,
                                 std::uint64_t* out_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    *out_generation = generation_;
    if (generation_ == 0) {
        *out_len = 0;
        return DFTU_PORT_NOT_READY;
    }
    return copy_out(bytes_, buf, cap, out_len);
}

PortPhase::PortPhase(dstd::String name, dftu_merge_op op, dftu_type type,
                     std::uint64_t generations)
    : name_(std::move(name)),
      op_(op),
      type_(type),
      generations_(static_cast<std::size_t>(generations)) {}

dstd::Unexpected<dstd::Error> PortPhase::undeclared(
    std::uint64_t generation) const {
    return common::make_error<
        "phase result '{}' generation {} was not declared">(
        common::ErrorCode::NotFound, name_, generation);
}

dstd::Result<dftu_port_status> PortPhase::contribute(std::uint64_t generation,
                                                     dftu_merge_op op,
                                                     dftu_type type,
                                                     const void* data,
                                                     std::uint32_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation > generations_.size()) {
        return undeclared(generation);
    }
    if (op != op_ || type != type_) {
        return common::make_error<
            "phase result '{}' was declared with another merge or type">(
            common::ErrorCode::InvalidArgument, name_);
    }
    Generation& gen = generations_[static_cast<std::size_t>(generation - 1)];
    if (gen.seeded) {
        return common::make_error<
            "phase result '{}' generation {} is seeded by the host; a plugin "
            "cannot contribute to it">(common::ErrorCode::InvalidArgument,
                                       name_, generation);
    }
    if (gen.sealed) return DFTU_PORT_CLOSED;
    if (len > common::constants::plugins::PORT_VALUE_MAX_BYTES) {
        return DFTU_PORT_TOO_BIG;
    }
    if (!phase_merge_accepts(op, type)) {
        return common::make_error<
            "phase result '{}': this merge does not take this value type">(
            common::ErrorCode::InvalidArgument, name_);
    }
    if (op == DFTU_MERGE_CONCAT) {
        const auto* p = static_cast<const std::byte*>(data);
        gen.bytes.insert(gen.bytes.end(), p, p + len);
    } else if (op == DFTU_MERGE_UNION) {
        if (len % 8 != 0) return DFTU_PORT_TOO_BIG;
        for (std::uint32_t at = 0; at < len; at += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, static_cast<const char*>(data) + at, 8);
            const auto pos =
                std::lower_bound(gen.set.begin(), gen.set.end(), value);
            if (pos == gen.set.end() || *pos != value)
                gen.set.insert(pos, value);
        }
    } else {
        if (len != 8) return DFTU_PORT_TOO_BIG;
        std::uint64_t bits = 0;
        std::memcpy(&bits, data, 8);
        gen.scalar = gen.any ? fold_scalar(op, type, gen.scalar, bits) : bits;
    }
    gen.any = true;
    return DFTU_PORT_OK;
}

dstd::Result<void> PortPhase::seal(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation > generations_.size()) {
        return undeclared(generation);
    }
    const std::size_t at = static_cast<std::size_t>(generation - 1);
    if (at > 0 && !generations_[at - 1].sealed) {
        return common::make_error<
            "phase result '{}': generation {} cannot seal before generation "
            "{}; generations seal in order">(common::ErrorCode::InvalidArgument,
                                             name_, generation, generation - 1);
    }
    generations_[at].sealed = true;
    return {};
}

dstd::Result<void> PortPhase::seed(std::uint64_t generation,
                                   std::uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation > generations_.size()) {
        return undeclared(generation);
    }
    Generation& gen = generations_[static_cast<std::size_t>(generation - 1)];
    gen.scalar = value;
    gen.any = true;
    gen.seeded = true;
    gen.sealed = true;
    return {};
}

dstd::Result<dftu_port_status> PortPhase::result(std::uint64_t generation,
                                                 void* buf, std::uint32_t cap,
                                                 std::uint32_t* out_len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation > generations_.size()) {
        return undeclared(generation);
    }
    const Generation& gen =
        generations_[static_cast<std::size_t>(generation - 1)];
    *out_len = 0;
    if (!gen.sealed) return DFTU_PORT_NOT_READY;
    dstd::Vector<std::byte> out;
    if (op_ == DFTU_MERGE_CONCAT) {
        out = gen.bytes;
    } else if (op_ == DFTU_MERGE_UNION) {
        out.resize(gen.set.size() * 8);
        if (!out.empty()) std::memcpy(out.data(), gen.set.data(), out.size());
    } else if (gen.any) {
        out.resize(8);
        std::memcpy(out.data(), &gen.scalar, 8);
    }
    return copy_out(out, buf, cap, out_len);
}

PortHub::PortHub(const dstd::Vector<ChannelDecl>& channels,
                 const dstd::Vector<PhaseDecl>& phases,
                 const dstd::Vector<ChannelDecl>& senders) {
    for (const ChannelDecl& decl : channels) {
        if (decl.is_ref) continue;
        const std::uint64_t handle = dstd::hash::fnv1a_hash(decl.name);
        handles_.emplace(decl.name, handle);
        channels_.emplace(handle, dstd::make_unique<PortChannel>(decl));
    }
    for (const ChannelDecl& decl : senders) {
        const auto handle = handles_.find(decl.name);
        if (handle == handles_.end()) continue;
        dstd::Vector<std::uint64_t>& mine = sends_of_[decl.declarer];
        if (std::find(mine.begin(), mine.end(), handle->second) != mine.end())
            continue;
        mine.push_back(handle->second);
        ++senders_left_[handle->second];
    }
    dstd::HashMap<dstd::String, std::uint64_t> generations;
    for (const PhaseDecl& decl : phases) {
        std::uint64_t& most = generations[decl.name];
        if (decl.generation > most) most = decl.generation;
    }
    for (const PhaseDecl& decl : phases) {
        if (phases_.find(decl.name) != phases_.end()) continue;
        phases_.emplace(
            decl.name,
            dstd::make_unique<PortPhase>(
                decl.name, static_cast<dftu_merge_op>(decl.op),
                static_cast<dftu_type>(decl.type), generations[decl.name]));
    }
}

PortHub::~PortHub() { close_all(); }

dstd::Optional<std::uint64_t> PortHub::find_channel(
    dstd::StringView name) const {
    const auto it = handles_.find(name);
    if (it == handles_.end()) return dstd::nullopt;
    return it->second;
}

PortChannel* PortHub::channel(std::uint64_t handle) const {
    const auto it = channels_.find(handle);
    return it == channels_.end() ? nullptr : it->second.get();
}

void PortHub::close_all() {
    for (auto& entry : channels_) entry.second->close();
}

void PortHub::sender_finished(dstd::StringView declarer) {
    dstd::Vector<std::uint64_t> to_close;
    {
        std::lock_guard<std::mutex> lock(senders_mutex_);
        const auto it = sends_of_.find(declarer);
        if (it == sends_of_.end()) return;
        for (const std::uint64_t handle : it->second) {
            if (--senders_left_[handle] == 0) to_close.push_back(handle);
        }
        sends_of_.erase(it);
    }
    for (const std::uint64_t handle : to_close) {
        if (PortChannel* ch = channel(handle)) ch->close();
    }
}

PortLatest* PortHub::latest(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    auto it = latest_.find(name);
    if (it == latest_.end()) {
        it =
            latest_.emplace(dstd::String(name), dstd::make_unique<PortLatest>())
                .first;
    }
    return it->second.get();
}

PortPhase* PortHub::phase(dstd::StringView name) const {
    const auto it = phases_.find(name);
    return it == phases_.end() ? nullptr : it->second.get();
}

PortNames::PortNames(PortHub* hub, dstd::String space)
    : hub_(hub), space_(std::move(space)) {}

dstd::Result<PortNames::Entry*> PortNames::resolve(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(name);
    if (found != entries_.end()) return found->second.get();
    bool own = false;
    auto full = qualify_port_name(space_, name, &own);
    if (!full) return std::move(full).error();
    auto entry = dstd::make_unique<Entry>();
    entry->phase = hub_->phase(*full);
    entry->full = std::move(*full);
    Entry* raw = entry.get();
    entries_.emplace(dstd::String(name), std::move(entry));
    return raw;
}

PortLatest* PortNames::latest(Entry* entry) {
    PortLatest* slot = entry->latest.load(std::memory_order_acquire);
    if (slot == nullptr) {
        slot = hub_->latest(entry->full);
        entry->latest.store(slot, std::memory_order_release);
    }
    return slot;
}

namespace {
thread_local bool t_in_batch = false;
}

bool in_batch_callback() noexcept { return t_in_batch; }

BatchCallbackScope::BatchCallbackScope() noexcept : saved_(t_in_batch) {
    t_in_batch = true;
}

BatchCallbackScope::~BatchCallbackScope() { t_in_batch = saved_; }

}  // namespace dftracer::utils::plugins::host
