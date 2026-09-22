#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/state.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Tier-2 mergeable state: a plugin registers a dftu_state_desc from its
// factory and the host drives it exactly as it drives a dftu_agg accumulator -
// one instance per worker slice, folded batch by batch, merged at the fan-in,
// spilled past the scan's memory budget, finalized once. Nothing here is a
// second execution path: PluginFold::step/merge/finalize call in.
namespace dftracer::utils::plugins {
namespace {

namespace fs = std::filesystem;

const char* err_text(const ::dftu_error& e) {
    return e.message ? e.message : "(no message)";
}

}  // namespace

SpillDir::~SpillDir() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
}

const std::string& SpillDir::path() {
    if (!path_.empty()) return path_;
    std::error_code ec;
    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path p = fs::temp_directory_path(ec) /
                     ("dftu_state_" + std::to_string(::getpid()) + "_" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                      "_" + std::to_string(attempt));
        if (fs::create_directory(p, ec)) {
            path_ = p.string();
            break;
        }
    }
    return path_;
}

const std::string& StateAccum::dir_path() {
    if (!dir_) dir_ = std::make_shared<SpillDir>();
    return dir_->path();
}

StateAccum::StateAccum(const RegisteredState& reg, std::uint64_t budget)
    : reg_(&reg), budget_(dftracer::utils::resolve_spill_budget(budget)) {
    state_ = reg.desc.init(reg.self);
    if (!state_)
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' init returned null",
                                 reg.name.c_str());
}

StateAccum::~StateAccum() {
    if (state_) reg_->desc.destroy(state_);
}

void StateAccum::update(const ::dftu_dataframe* df) {
    if (!state_ || !df) return;
    ::dftu_error err{};
    if (reg_->desc.update(state_, df, &err) != 0) {
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' update failed: %s",
                                 reg_->name.c_str(), err_text(err));
        return;
    }
    maybe_spill();
}

void StateAccum::maybe_spill() {
    if (!state_ || budget_ == dftracer::utils::NO_SPILL_BUDGET) return;
    // Without a size hook the host has no measure of the state at all, so it
    // cannot claim the state is over budget, let alone bound it.
    if (!reg_->desc.bytes) return;
    if (reg_->desc.bytes(state_) <= budget_) return;

    if (!reg_->desc.serialize) {
        if (!spill_refused_) {
            spill_refused_ = true;
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin state '%s' ran past the %llu byte memory budget but "
                "declares no serialize/deserialize pair, so the host cannot "
                "spill it; it stays in memory and the budget is not honoured",
                reg_->name.c_str(), static_cast<unsigned long long>(budget_));
        }
        return;
    }

    ::dftu_bytes out{};
    ::dftu_error err{};
    if (reg_->desc.serialize(state_, &out, &err) != 0 || !out.data) {
        if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' serialize failed: %s",
                                 reg_->name.c_str(), err_text(err));
        return;
    }

    const std::string dir = dir_path();
    bool written = false;
    if (!dir.empty()) {
        const std::string path =
            dir + "/run_" + std::to_string(runs_.size()) + ".bin";
        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        os.write(static_cast<const char*>(out.data),
                 static_cast<std::streamsize>(out.len));
        os.close();
        if (os) {
            runs_.push_back(path);
            written = true;
        }
    }
    if (out.free_fn) out.free_fn(const_cast<void*>(out.data), out.ud);
    if (!written) {
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' could not write a run",
                                 reg_->name.c_str());
        return;
    }

    DFTRACER_UTILS_LOG_DEBUG(
        "Plugin state '%s' spilled run %zu past the %llu byte budget",
        reg_->name.c_str(), runs_.size() - 1,
        static_cast<unsigned long long>(budget_));
    reg_->desc.destroy(state_);
    state_ = reg_->desc.init(reg_->self);
}

void StateAccum::merge(StateAccum& other) {
    if (!state_ || !other.state_) return;
    ::dftu_error err{};
    if (reg_->desc.merge(state_, other.state_, &err) != 0)
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' merge failed: %s",
                                 reg_->name.c_str(), err_text(err));
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
    for (std::string& r : other.runs_) runs_.push_back(std::move(r));
    other.runs_.clear();
}

void StateAccum::drain_runs() {
    if (!state_ || runs_.empty() || !reg_->desc.deserialize) return;
    for (const std::string& path : runs_) {
        std::ifstream is(path, std::ios::binary);
        std::string buf((std::istreambuf_iterator<char>(is)),
                        std::istreambuf_iterator<char>());
        if (!is && !is.eof()) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin state '%s' could not read run '%s'", reg_->name.c_str(),
                path.c_str());
            continue;
        }
        ::dftu_bytes in{buf.data(), static_cast<std::uint64_t>(buf.size()),
                        nullptr, nullptr};
        ::dftu_error err{};
        void* run_state = reg_->desc.deserialize(reg_->self, in, &err);
        if (!run_state) {
            DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' deserialize failed: %s",
                                     reg_->name.c_str(), err_text(err));
            continue;
        }
        if (reg_->desc.merge(state_, run_state, &err) != 0)
            DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' run merge failed: %s",
                                     reg_->name.c_str(), err_text(err));
        reg_->desc.destroy(run_state);
    }
    runs_.clear();
}

bool StateAccum::finalize(::dftu_result_value* out) {
    if (!state_ || !out) return false;
    drain_runs();
    ::dftu_error err{};
    if (reg_->desc.finalize(state_, out, &err) != 0) {
        DFTRACER_UTILS_LOG_ERROR("Plugin state '%s' finalize failed: %s",
                                 reg_->name.c_str(), err_text(err));
        return false;
    }
    return true;
}

void PluginFold::states_update(const ::dftu_dataframe* df) {
    for (std::unique_ptr<StateAccum>& s : state_accums_) s->update(df);
}

void PluginFold::states_merge(PluginFold& other) {
    const std::size_t n =
        std::min(state_accums_.size(), other.state_accums_.size());
    for (std::size_t i = 0; i < n; ++i)
        state_accums_[i]->merge(*other.state_accums_[i]);
}

void PluginFold::states_finalize() {
    for (std::unique_ptr<StateAccum>& s : state_accums_) {
        ::dftu_result_value v{};
        if (s->finalize(&v)) result_emit(s->name().c_str(), &v);
    }
}

std::size_t PluginFold::state_spill_runs(const char* name) const {
    if (!name) return 0;
    for (const std::unique_ptr<StateAccum>& s : state_accums_)
        if (s->name() == name) return s->runs();
    return 0;
}

bool PluginFold::state_spill_refused(const char* name) const {
    if (!name) return false;
    for (const std::unique_ptr<StateAccum>& s : state_accums_)
        if (s->name() == name) return s->spill_refused();
    return false;
}

}  // namespace dftracer::utils::plugins
