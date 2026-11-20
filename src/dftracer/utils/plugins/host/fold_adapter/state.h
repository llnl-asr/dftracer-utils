// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_STATE_H
#define DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_STATE_H

#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/state_registry.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

/// A self-cleaning directory under spill_dir() holding one state's spill runs.
class SpillDir {
   public:
    SpillDir() = default;
    SpillDir(const SpillDir&) = delete;
    SpillDir& operator=(const SpillDir&) = delete;

    /// The directory, created on first use; empty if it could not be created.
    const dstd::Path& path();
    /// Charge `bytes` written to a file in the directory to the spill gauge.
    void charge(std::uint64_t bytes) noexcept;

   private:
    dstd::Optional<dftracer::utils::common::ScopedSpillSubdir> dir_;
    dstd::Path empty_;
};

/// One live instance of a plugin-registered dftu_state_desc: this slice's
/// state plus the runs it spilled. Drives the descriptor's callbacks and
/// nothing else, so a tier-2 state rides PluginFold's existing
/// step/merge/finalize rather than a second execution path.
class StateAccum {
   public:
    /// `budget` is the scan's out-of-core budget in bytes, resolved through
    /// memory::resolve_spill_budget: 0 means auto and memory::NO_SPILL_BUDGET
    /// disables spilling. `plugin` names the plugin in error messages. A
    /// failed init is kept and returned by the first update, merge or
    /// finalize.
    StateAccum(const RegisteredState& reg, std::uint64_t budget,
               dstd::StringView plugin);
    ~StateAccum();
    StateAccum(const StateAccum&) = delete;
    StateAccum& operator=(const StateAccum&) = delete;

    const dstd::String& name() const { return reg_->name; }
    /// Runs written so far; 0 means the state never left memory.
    std::size_t runs() const { return runs_.size(); }
    /// True once the state ran past the budget with no serialize/deserialize
    /// pair, so the host refused it a spill instead of bounding it.
    bool spill_refused() const { return spill_refused_; }

    dstd::Result<void> update(const dftu_dataframe* df);
    /// Fold `other` in, taking over its runs; `other` keeps its state, which
    /// its own destructor releases.
    dstd::Result<void> merge(StateAccum& other);
    /// Merge every run back and finalize; false when there is nothing to
    /// finalize, an error when the plugin reported failure.
    dstd::Result<bool> finalize(dftu_result_value* out);

   private:
    dstd::Result<void> init_state();
    dstd::Result<void> maybe_spill();
    void adopt_runs(StateAccum& other);
    dstd::Result<void> drain_runs();
    const dstd::Path& dir_path();
    dstd::Error error_of(const plugins::AbiContext& ctx, dftu_status status,
                         dstd::StringView callback) const;

    const RegisteredState* reg_;
    dstd::String plugin_;
    dstd::Optional<dstd::Error> init_error_;
    std::uint64_t budget_;
    void* state_ = nullptr;
    bool spill_refused_ = false;
    // shared so a worker's directory outlives the worker once a master fold
    // has adopted the runs inside it.
    dstd::SharedPtr<SpillDir> dir_;
    dstd::Vector<dstd::SharedPtr<SpillDir>> adopted_dirs_;
    dstd::Vector<dstd::Path> runs_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_STATE_H
