#ifndef DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_STATE_H
#define DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_STATE_H

#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/state_registry.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::plugins {

/// A self-cleaning temp directory holding one state's spill runs.
class SpillDir {
   public:
    SpillDir() = default;
    ~SpillDir();
    SpillDir(const SpillDir&) = delete;
    SpillDir& operator=(const SpillDir&) = delete;

    /// The directory, created on first use; empty if it could not be created.
    const std::string& path();

   private:
    std::string path_;
};

/// One live instance of a plugin-registered dftu_state_desc: this slice's
/// state plus the runs it spilled. Drives the descriptor's callbacks and
/// nothing else, so a tier-2 state rides PluginFold's existing
/// step/merge/finalize rather than a second execution path.
class StateAccum {
   public:
    /// `budget` is the scan's out-of-core budget in bytes, resolved through
    /// resolve_spill_budget: 0 means auto and NO_SPILL_BUDGET disables
    /// spilling.
    StateAccum(const RegisteredState& reg, std::uint64_t budget);
    ~StateAccum();
    StateAccum(const StateAccum&) = delete;
    StateAccum& operator=(const StateAccum&) = delete;

    const std::string& name() const { return reg_->name; }
    /// Runs written so far; 0 means the state never left memory.
    std::size_t runs() const { return runs_.size(); }
    /// True once the state ran past the budget with no serialize/deserialize
    /// pair, so the host refused it a spill instead of bounding it.
    bool spill_refused() const { return spill_refused_; }

    void update(const dftu_dataframe* df);
    /// Fold `other` in, taking over its runs; `other` keeps its state, which
    /// its own destructor releases.
    void merge(StateAccum& other);
    /// Merge every run back and finalize; false when there is nothing to
    /// finalize or the plugin reported failure.
    bool finalize(dftu_result_value* out);

   private:
    void maybe_spill();
    void adopt_runs(StateAccum& other);
    void drain_runs();
    const std::string& dir_path();

    const RegisteredState* reg_;
    std::uint64_t budget_;
    void* state_ = nullptr;
    bool spill_refused_ = false;
    // shared so a worker's directory outlives the worker once a master fold
    // has adopted the runs inside it.
    std::shared_ptr<SpillDir> dir_;
    std::vector<std::shared_ptr<SpillDir>> adopted_dirs_;
    std::vector<std::string> runs_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_STATE_H
