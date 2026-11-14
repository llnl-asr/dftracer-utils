// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/udaf_states.h>
#include <dftracer/utils/dataframe/udf_support.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/memory/detail/heap.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

constexpr std::size_t BLOCK_BYTES = 16 * 1024;
constexpr std::size_t DESTROY_CHUNK = 1024;

dstd::Unexpected<dstd::Error> fail(const UdafRecord& u, dstd::StringView what) {
    return dstd::Unexpected(dstd::Error::from_format<"udaf '{}' {}">(
        dstd::Condition::Internal, u.name(), what));
}

}  // namespace

class UdafStates::Block {
   public:
    Block(std::size_t bytes, std::size_t align) noexcept
        : data_(memory::detail::try_allocate(bytes, align)), bytes_(bytes) {
        if (data_ != nullptr) std::memset(data_, 0, bytes);
    }
    Block(Block&& other) noexcept : data_(other.data_), bytes_(other.bytes_) {
        other.data_ = nullptr;
    }
    Block& operator=(Block&&) = delete;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    ~Block() {
        if (data_ != nullptr) memory::detail::deallocate(data_, bytes_);
    }

    void* data() const noexcept { return data_; }
    std::size_t bytes() const noexcept { return bytes_; }

   private:
    void* data_;
    std::size_t bytes_;
};

UdafStates::UdafStates(UdafBinding binding)
    : UdafStates(dstd::make_shared<const UdafBinding>(std::move(binding))) {}

UdafStates::UdafStates(dstd::SharedPtr<const UdafBinding> binding)
    : binding_(std::move(binding)),
      stride_((binding_->udaf().desc().state_size +
               binding_->udaf().desc().state_align - 1) /
              binding_->udaf().desc().state_align *
              binding_->udaf().desc().state_align),
      per_block_(std::max<std::size_t>(1, BLOCK_BYTES / stride_)) {}

UdafStates::UdafStates(UdafStates&& other) noexcept
    : binding_(std::move(other.binding_)),
      blocks_(std::move(other.blocks_)),
      scratch_(std::move(other.scratch_)),
      stride_(other.stride_),
      per_block_(other.per_block_),
      first_block_(other.first_block_),
      count_(other.count_) {
    other.count_ = 0;
}

UdafStates::~UdafStates() { destroy_all(); }

void* UdafStates::state_at(std::int64_t group) const noexcept {
    const auto g = static_cast<std::size_t>(group);
    if (g < first_block_)
        return static_cast<char*>(blocks_[0].data()) + g * stride_;
    const std::size_t rest = g - first_block_;
    return static_cast<char*>(blocks_[1 + rest / per_block_].data()) +
           (rest % per_block_) * stride_;
}

void UdafStates::fill_all(dstd::Vector<void*>& out) const {
    out.resize(static_cast<std::size_t>(count_));
    for (std::int64_t g = 0; g < count_; ++g)
        out[static_cast<std::size_t>(g)] = state_at(g);
}

void UdafStates::destroy_all() noexcept {
    if (count_ == 0) return;
    const dftu_udaf_desc& d = binding_->udaf().desc();
    if (d.destroy != nullptr) {
        for (std::int64_t from = 0; from < count_;
             from += static_cast<std::int64_t>(DESTROY_CHUNK)) {
            const std::int64_t to = std::min(
                count_, from + static_cast<std::int64_t>(DESTROY_CHUNK));
            scratch_.clear();
            for (std::int64_t g = from; g < to; ++g)
                scratch_.push_back(state_at(g));
            d.destroy(binding_->bound(), scratch_.data(), scratch_.size());
        }
    }
    count_ = 0;
    blocks_.clear();
}

dstd::Result<void> UdafStates::grow(std::int64_t groups) {
    if (groups <= count_) return {};
    const auto wanted = static_cast<std::size_t>(groups);
    // The first block holds only the groups asked for, up to a full block, so
    // a table of one group does not zero a whole block.
    if (blocks_.empty()) first_block_ = std::min(wanted, per_block_);
    const std::size_t block_count =
        wanted <= first_block_
            ? 1
            : 1 + (wanted - first_block_ + per_block_ - 1) / per_block_;
    blocks_.reserve(block_count);
    while (blocks_.size() < block_count) {
        const std::size_t block_bytes =
            (blocks_.empty() ? first_block_ : per_block_) * stride_;
        Block block(block_bytes, binding_->udaf().desc().state_align);
        if (block.data() == nullptr)
            return dstd::Unexpected(dstd::Error::from_format<
                                    "udaf '{}' cannot allocate {} bytes of "
                                    "state">(dstd::Condition::OutOfMemory,
                                             binding_->udaf().name(),
                                             block_bytes));
        blocks_.push_back(std::move(block));
    }
    scratch_.clear();
    for (std::int64_t g = count_; g < groups; ++g)
        scratch_.push_back(state_at(g));
    binding_->udaf().desc().init(binding_->bound(), scratch_.data(),
                                 scratch_.size());
    count_ = groups;
    return {};
}

dstd::Result<void> UdafStates::update(dstd::Span<const std::int64_t> gids,
                                      dstd::Span<const Series* const> args) {
    return feed(gids, args, false);
}

dstd::Result<void> UdafStates::retract(dstd::Span<const std::int64_t> gids,
                                       dstd::Span<const Series* const> args) {
    return feed(gids, args, true);
}

dstd::Result<void> UdafStates::value(std::int64_t group, dftu_scalar& out) {
    const UdafRecord& u = binding_->udaf();
    if (!u.desc().value) return fail(u, "has no value callback");
    if (group < 0 || group >= count_)
        return fail(u, "value names a group that grow did not make");
    CallContext ctx;
    const dftu_status status =
        u.desc().value(ctx.get(), binding_->bound(), state_at(group), &out);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "value", status, ctx));
    return {};
}

dstd::Result<void> UdafStates::feed(dstd::Span<const std::int64_t> gids,
                                    dstd::Span<const Series* const> args,
                                    bool take_out) {
    const UdafRecord& u = binding_->udaf();
    if (take_out && !u.desc().retract)
        return fail(u, "has no retract callback");
    if (args.size() != binding_->arg_count())
        return fail(u, "bound for a different number of arguments");
    const std::size_t rows = gids.size();
    for (const Series* arg : args)
        if (arg->length() != static_cast<std::int64_t>(rows))
            return fail(u, "update got a column of another length than gids");
    if (rows == 0) return {};

    scratch_.resize(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        const std::int64_t g = gids[i];
        if (g >= count_)
            return fail(u, "update names a group that grow did not make");
        scratch_[i] = g < 0 ? nullptr : state_at(g);
    }

    dstd::Vector<Series> flat;
    dstd::Vector<const dftu_series*> handles;
    flat.reserve(args.size());
    handles.reserve(args.size());
    for (const Series* arg : args) {
        flat.push_back(arg->materialize());
        handles.push_back(flat.back().handle());
    }
    if (!u.is_null_aware()) {
        for (const Series& col : flat) {
            if (col.null_count() == 0) continue;
            for (std::size_t i = 0; i < rows; ++i)
                if (col.is_null(static_cast<std::int64_t>(i)))
                    scratch_[i] = nullptr;
        }
    }

    CallContext ctx;
    const auto call = take_out ? u.desc().retract : u.desc().update;
    const dftu_status status =
        call(ctx.get(), binding_->bound(), scratch_.data(), handles.data(),
             static_cast<std::uint32_t>(handles.size()), rows);
    if (status < 0)
        return dstd::Unexpected(call_error(
            "udaf", u.name(), take_out ? "retract" : "update", status, ctx));
    return {};
}

dstd::Result<void> UdafStates::combine_pairs(
    const UdafStates& other, dstd::Span<const std::int64_t> into,
    dstd::Span<const std::int64_t> from) {
    const UdafRecord& u = binding_->udaf();
    if (&other == this || &other.binding_->udaf() != &u)
        return fail(u, "can only combine with another table of the same udaf");
    if (into.size() != from.size())
        return fail(u, "combine needs one source group per target group");
    if (into.empty()) return {};
    dstd::Vector<void*> sources(from.size());
    scratch_.resize(into.size());
    for (std::size_t i = 0; i < into.size(); ++i) {
        if (into[i] < 0 || into[i] >= count_ || from[i] < 0 ||
            from[i] >= other.count_)
            return fail(u, "combine names a group that grow did not make");
        scratch_[i] = state_at(into[i]);
        sources[i] = other.state_at(from[i]);
    }
    CallContext ctx;
    const dftu_status status =
        u.desc().combine(ctx.get(), binding_->bound(), scratch_.data(),
                         sources.data(), into.size());
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "combine", status, ctx));
    return {};
}

dstd::Result<void> UdafStates::combine_from(
    UdafStates& other, dstd::Span<const std::int64_t> mapping) {
    const UdafRecord& u = binding_->udaf();
    if (mapping.size() != static_cast<std::size_t>(other.count_))
        return fail(u, "combine needs one target group per source group");
    dstd::Vector<std::int64_t> from(mapping.size());
    for (std::size_t i = 0; i < from.size(); ++i)
        from[i] = static_cast<std::int64_t>(i);
    dstd::Result<void> merged = combine_pairs(other, mapping, from);
    if (!merged) return merged;
    other.destroy_all();
    return {};
}

dstd::Result<Series> UdafStates::finalize() {
    const UdafRecord& u = binding_->udaf();
    fill_all(scratch_);
    CallContext ctx;
    dftu_series* produced = nullptr;
    const dftu_status status =
        u.desc().finalize(ctx.get(), binding_->bound(), scratch_.data(),
                          scratch_.size(), &produced);
    Series out(produced);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "finalize", status, ctx));
    if (!out.valid()) return fail(u, "finalize returned no column");
    if (out.length() != count_ || out.type() != result_type().id)
        return dstd::Unexpected(dstd::Error::from_format<
                                "udaf '{}' finalize returned {} rows of {}, "
                                "not {} rows of {}">(
            dstd::Condition::Internal, u.name(), out.length(),
            type_name(out.type()), count_, type_name(result_type().id)));
    return out;
}

dstd::Result<void> UdafStates::serialize(std::int64_t group,
                                         dstd::String& out) const {
    const UdafRecord& u = binding_->udaf();
    if (group < 0 || group >= count_)
        return fail(u, "serialize: no such group");
    const dftu_udaf_desc& d = u.desc();
    if (u.is_pod()) {
        out.append(static_cast<const char*>(state_at(group)), d.state_size);
        return {};
    }
    CallContext ctx;
    std::uint64_t need = 0;
    dftu_status status = d.serialize(ctx.get(), binding_->bound(),
                                     state_at(group), nullptr, 0, &need);
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "serialize", status, ctx));
    const std::size_t offset = out.size();
    out.resize(offset + need);
    std::uint64_t written = need;
    status = d.serialize(ctx.get(), binding_->bound(), state_at(group),
                         reinterpret_cast<std::uint8_t*>(out.data()) + offset,
                         need, &written);
    if (status >= 0 && written != need) {
        out.resize(offset);
        return fail(u, "serialize wrote another size than it reported");
    }
    if (status < 0) {
        out.resize(offset);
        return dstd::Unexpected(
            call_error("udaf", u.name(), "serialize", status, ctx));
    }
    return {};
}

dstd::Result<void> UdafStates::deserialize(std::int64_t group,
                                           dstd::StringView bytes) {
    const UdafRecord& u = binding_->udaf();
    if (group < 0 || group >= count_)
        return fail(u, "deserialize: no such group");
    const dftu_udaf_desc& d = u.desc();
    if (u.is_pod()) {
        if (bytes.size() != d.state_size)
            return fail(u, "deserialize got a state of the wrong size");
        std::memcpy(state_at(group), bytes.data(), bytes.size());
        return {};
    }
    CallContext ctx;
    const dftu_status status =
        d.deserialize(ctx.get(), binding_->bound(),
                      reinterpret_cast<const std::uint8_t*>(bytes.data()),
                      bytes.size(), state_at(group));
    if (status < 0)
        return dstd::Unexpected(
            call_error("udaf", u.name(), "deserialize", status, ctx));
    return {};
}

std::size_t UdafStates::approx_bytes() const noexcept {
    std::size_t bytes = sizeof(*this);
    for (const Block& block : blocks_) bytes += block.bytes();
    return bytes + scratch_.capacity() * sizeof(void*);
}

}  // namespace dftracer::utils::dataframe
