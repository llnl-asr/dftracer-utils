// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/state.h>
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
namespace dftracer::utils::plugins {
namespace {

const char* err_text(const ::dftu_error& e) {
    return e.message ? e.message : "(no message)";
}

}  // namespace

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

StateAccum::StateAccum(const RegisteredState& reg, std::uint64_t budget)
    : reg_(&reg),
      budget_(dftracer::utils::memory::resolve_spill_budget(budget)) {
    state_ = reg.desc.init(reg.self);
    if (!state_)
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' init returned null",
                                 reg.name);
}

StateAccum::~StateAccum() {
    if (state_) reg_->desc.destroy(state_);
}

void StateAccum::update(const ::dftu_dataframe* df) {
    if (!state_ || !df) return;
    ::dftu_error err{};
    if (reg_->desc.update(state_, df, &err) != 0) {
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' update failed: {}",
                                 reg_->name, err_text(err));
        return;
    }
    maybe_spill();
}

void StateAccum::maybe_spill() {
    if (!state_ || budget_ == dftracer::utils::memory::NO_SPILL_BUDGET) return;
    // Without a size hook the host has no measure of the state at all, so it
    // cannot claim the state is over budget, let alone bound it.
    if (!reg_->desc.bytes) return;
    if (reg_->desc.bytes(state_) <= budget_) return;

    if (!reg_->desc.serialize) {
        if (!spill_refused_) {
            spill_refused_ = true;
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin state '{}' ran past the {} byte memory budget but "
                "declares no serialize/deserialize pair, so the host cannot "
                "spill it; it stays in memory and the budget is not honoured",
                reg_->name, static_cast<unsigned long long>(budget_));
        }
        return;
    }

    ::dftu_bytes out{};
    ::dftu_error err{};
    if (reg_->desc.serialize(state_, &out, &err) != 0 || !out.data) {
        if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' serialize failed: {}",
                                 reg_->name, err_text(err));
        return;
    }

    const dstd::Path& dir = dir_path();
    bool written = false;
    if (!dir.empty()) {
        const dstd::Path path =
            dir.view() / ("run_" + dstd::to_string(runs_.size()) + ".bin");
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
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' could not write a run",
                                 reg_->name);
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "Plugin state '{}' spilled run {} past the {} byte budget", reg_->name,
        runs_.size() - 1, static_cast<unsigned long long>(budget_));
    reg_->desc.destroy(state_);
    state_ = reg_->desc.init(reg_->self);
}

void StateAccum::merge(StateAccum& other) {
    if (!state_ || !other.state_) return;
    ::dftu_error err{};
    if (reg_->desc.merge(state_, other.state_, &err) != 0)
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' merge failed: {}",
                                 reg_->name, err_text(err));
    spill_refused_ = spill_refused_ || other.spill_refused_;
    adopt_runs(other);
    maybe_spill();
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

void StateAccum::drain_runs() {
    if (!state_ || runs_.empty() || !reg_->desc.deserialize) return;
    for (const dstd::Path& path : runs_) {
        const auto read = dstd::fs::read_file(path.view());
        if (!read) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin state '{}' could not read run '{}'", reg_->name, path);
            continue;
        }
        const dstd::String& buf = *read;
        ::dftu_bytes in{buf.data(), static_cast<std::uint64_t>(buf.size()),
                        nullptr, nullptr};
        ::dftu_error err{};
        void* run_state = reg_->desc.deserialize(reg_->self, in, &err);
        if (!run_state) {
            DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' deserialize failed: {}",
                                     reg_->name, err_text(err));
            continue;
        }
        if (reg_->desc.merge(state_, run_state, &err) != 0)
            DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' run merge failed: {}",
                                     reg_->name, err_text(err));
        reg_->desc.destroy(run_state);
    }
    runs_.clear();
}

bool StateAccum::finalize(::dftu_result_value* out) {
    if (!state_ || !out) return false;
    drain_runs();
    ::dftu_error err{};
    if (reg_->desc.finalize(state_, out, &err) != 0) {
        DFTRACER_UTILS_LOG_ERROR("Plugin state '{}' finalize failed: {}",
                                 reg_->name, err_text(err));
        return false;
    }
    return true;
}

void PluginFold::states_update(const ::dftu_dataframe* df) {
    for (dstd::UniquePtr<StateAccum>& s : state_accums_) s->update(df);
}

void PluginFold::states_merge(PluginFold& other) {
    const std::size_t n =
        std::min(state_accums_.size(), other.state_accums_.size());
    for (std::size_t i = 0; i < n; ++i)
        state_accums_[i]->merge(*other.state_accums_[i]);
}

void PluginFold::states_finalize() {
    for (dstd::UniquePtr<StateAccum>& s : state_accums_) {
        ::dftu_result_value v{};
        if (s->finalize(&v)) result_emit(s->name().c_str(), &v);
    }
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

}  // namespace dftracer::utils::plugins
