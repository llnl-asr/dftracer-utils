// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/coro/abi_context.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/state.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

// Tier-2 mergeable state: a plugin registers a dftu_state_desc from its
// factory and the host drives it exactly as it drives a dftu_agg accumulator -
// one instance per worker slice, folded batch by batch, merged at the fan-in,
// spilled past the scan's memory budget, finalized once. Nothing here is a
// second execution path: PluginFold::step/merge/finalize call in.
namespace dftracer::utils::plugins::host {

const dstd::Path& SpillDir::path() {
    if (!dir_) {
        auto made =
            dftracer::utils::common::ScopedSpillSubdir::create("dftu_state");
        if (!made) {
            DFTRACER_UTILS_LOG_ERROR("Plugin state spill directory: {}",
                                     made.error().format());
            return empty_;
        }
        dir_.emplace(std::move(*made));
    }
    return dir_->path();
}

void SpillDir::charge(std::uint64_t bytes) noexcept {
    if (dir_) dir_->charge().add(bytes);
}

const dstd::Path& StateAccum::dir_path() {
    if (!dir_) dir_ = dstd::make_shared<SpillDir>();
    return dir_->path();
}

dstd::Error StateAccum::error_of(const coro::AbiContext& ctx,
                                 dftu_status status,
                                 const char* callback) const {
    return callback_error(plugin_,
                          dstd::fmt<"state '{}' {}">(reg_->name, callback),
                          status, ctx.message());
}

StateAccum::StateAccum(const RegisteredState& reg, std::uint64_t budget,
                       dstd::StringView plugin)
    : reg_(&reg),
      plugin_(plugin),
      budget_(dftracer::utils::memory::resolve_spill_budget(budget)) {
    if (auto started = init_state(); !started) init_error_ = started.error();
}

StateAccum::~StateAccum() {
    if (state_) reg_->desc.destroy(state_);
}

dstd::Result<void> StateAccum::init_state() {
    coro::AbiContext ctx;
    void* fresh = nullptr;
    const dftu_status status = reg_->desc.init(ctx.get(), reg_->self, &fresh);
    if (status < 0) return error_of(ctx, status, "init");
    if (fresh == nullptr) {
        return common::make_error<
            "plugin '{}' state '{}' init: it returned "
            "no state">(common::ErrorCode::InvalidArgument, plugin_,
                        reg_->name);
    }
    state_ = fresh;
    return {};
}

dstd::Result<void> StateAccum::update(const ::dftu_dataframe* df) {
    if (init_error_) return *init_error_;
    if (!state_ || !df) return {};
    coro::AbiContext ctx;
    const dftu_status status = reg_->desc.update(ctx.get(), state_, df);
    if (status < 0) return error_of(ctx, status, "update");
    return maybe_spill();
}

dstd::Result<void> StateAccum::maybe_spill() {
    if (!state_ || budget_ == dftracer::utils::memory::NO_SPILL_BUDGET)
        return {};
    // Without a size hook the host has no measure of the state at all, so it
    // cannot claim the state is over budget, let alone bound it.
    if (!reg_->desc.bytes) return {};
    if (reg_->desc.bytes(state_) <= budget_) return {};

    if (!reg_->desc.serialize) {
        if (!spill_refused_) {
            spill_refused_ = true;
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin state '{}' ran past the {} byte memory budget but "
                "declares no serialize/deserialize pair, so the host cannot "
                "spill it; it stays in memory and the budget is not honoured",
                reg_->name, static_cast<unsigned long long>(budget_));
        }
        return {};
    }

    ::dftu_bytes out{};
    coro::AbiContext ctx;
    const dftu_status status = reg_->desc.serialize(ctx.get(), state_, &out);
    if (status < 0 || !out.data) {
        if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
        if (status < 0) return error_of(ctx, status, "serialize");
        return common::make_error<
            "plugin '{}' state '{}' serialize: it gave "
            "no bytes">(common::ErrorCode::InvalidArgument, plugin_,
                        reg_->name);
    }

    const dstd::Path& dir = dir_path();
    bool written = false;
    if (!dir.empty()) {
        const dstd::Path path =
            dir.view() / dstd::fmt<"run_{}.bin">(runs_.size());
        const dstd::StringView blob(static_cast<const char*>(out.data),
                                    static_cast<std::size_t>(out.len));
        if (dstd::fs::write_file(path, blob)) {
            dir_->charge(out.len);
            runs_.push_back(path);
            written = true;
        }
    }
    if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
    if (!written) {
        return common::make_error<
            "plugin '{}' state '{}': the host could not "
            "write a spill run">(common::ErrorCode::Io, plugin_, reg_->name);
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "Plugin state '{}' spilled run {} past the {} byte budget", reg_->name,
        runs_.size() - 1, static_cast<unsigned long long>(budget_));
    reg_->desc.destroy(state_);
    state_ = nullptr;
    return init_state();
}

dstd::Result<void> StateAccum::merge(StateAccum& other) {
    if (init_error_) return *init_error_;
    if (other.init_error_) return *other.init_error_;
    if (!state_ || !other.state_) return {};
    coro::AbiContext ctx;
    const dftu_status status =
        reg_->desc.merge(ctx.get(), state_, other.state_);
    if (status < 0) return error_of(ctx, status, "merge");
    spill_refused_ = spill_refused_ || other.spill_refused_;
    adopt_runs(other);
    return maybe_spill();
}

void StateAccum::adopt_runs(StateAccum& other) {
    if (other.runs_.empty()) return;
    // The worker's spilled runs are part of its state, so the master takes
    // them over along with the in-memory half; the worker's temp directory has
    // to outlive the worker, hence the shared handle.
    if (other.dir_) adopted_dirs_.push_back(other.dir_);
    for (dstd::Path& r : other.runs_) runs_.push_back(std::move(r));
    other.runs_.clear();
}

dstd::Result<void> StateAccum::drain_runs() {
    if (!state_ || runs_.empty() || !reg_->desc.deserialize) return {};
    for (const dstd::Path& path : runs_) {
        const auto read = dstd::fs::read_file(path.view());
        if (!read) {
            return common::make_error<
                "plugin '{}' state '{}': the host could "
                "not read spill run '{}'">(common::ErrorCode::Io, plugin_,
                                           reg_->name, path);
        }
        const dstd::String& buf = *read;
        ::dftu_bytes in{buf.data(), static_cast<std::uint64_t>(buf.size()),
                        nullptr, nullptr};
        coro::AbiContext ctx;
        void* run_state = nullptr;
        const dftu_status loaded =
            reg_->desc.deserialize(ctx.get(), reg_->self, in, &run_state);
        if (loaded < 0) return error_of(ctx, loaded, "deserialize");
        if (run_state == nullptr) {
            return common::make_error<
                "plugin '{}' state '{}' deserialize: it gave no state">(
                common::ErrorCode::InvalidArgument, plugin_, reg_->name);
        }
        const dftu_status merged =
            reg_->desc.merge(ctx.get(), state_, run_state);
        reg_->desc.destroy(run_state);
        if (merged < 0) return error_of(ctx, merged, "merge of a spill run");
    }
    runs_.clear();
    return {};
}

dstd::Result<bool> StateAccum::finalize(::dftu_result_value* out) {
    if (init_error_) return *init_error_;
    if (!state_ || !out) return false;
    if (auto drained = drain_runs(); !drained) return drained.error();
    coro::AbiContext ctx;
    const dftu_status status = reg_->desc.finalize(ctx.get(), state_, out);
    if (status < 0) return error_of(ctx, status, "finalize");
    return true;
}

dstd::Result<void> PluginFold::states_update(const ::dftu_dataframe* df) {
    for (dstd::UniquePtr<StateAccum>& s : state_accums_) {
        auto updated = s->update(df);
        if (!updated) return updated.error();
    }
    return {};
}

dstd::Result<void> PluginFold::states_merge(PluginFold& other) {
    const std::size_t n =
        std::min(state_accums_.size(), other.state_accums_.size());
    for (std::size_t i = 0; i < n; ++i) {
        auto merged = state_accums_[i]->merge(*other.state_accums_[i]);
        if (!merged) return merged.error();
    }
    return {};
}

dstd::Result<void> PluginFold::states_finalize() {
    for (dstd::UniquePtr<StateAccum>& s : state_accums_) {
        ::dftu_result_value v{};
        auto produced = s->finalize(&v);
        if (!produced.has_value()) return produced.error();
        if (!*produced) continue;
        auto emitted = emit_result(s->name().c_str(), &v);
        if (!emitted) return emitted.error();
    }
    return {};
}

std::size_t PluginFold::state_spill_runs(const char* name) const {
    if (!name) return 0;
    for (const dstd::UniquePtr<StateAccum>& s : state_accums_)
        if (s->name() == name) return s->runs();
    return 0;
}

bool PluginFold::state_spill_refused(const char* name) const {
    if (!name) return false;
    for (const dstd::UniquePtr<StateAccum>& s : state_accums_)
        if (s->name() == name) return s->spill_refused();
    return false;
}

}  // namespace dftracer::utils::plugins::host
