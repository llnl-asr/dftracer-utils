#ifndef DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_H
#define DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/plugins/state_registry.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Adapts a loaded C-ABI plugin to the internal fold interface so N plugins ride
// one fused scan. A non-null dftu_task from on_batch/on_finalize is a lazy
// CoroTask<void> the fuse worker awaits.
namespace dftracer::utils::plugins {

// Host-owned parallel writer handed to the plugin as an opaque dftu_writer*;
// num_workers/gzip are captured at writer_create for a later writer_open.
struct PluginWriter {
    std::unique_ptr<utilities::fileio::parallel::ParallelWriter> writer;
    std::string path;
    std::size_t num_workers = 1;
    bool gzip = false;
};

// Host-owned trace writer handed to the plugin as an opaque dftu_trace_writer*.
// trace_close compresses `buffer` into gzip members as a re-indexable .pfw.gz.
struct PluginTraceWriter {
    std::string path;
    std::string buffer;
    std::uint64_t next_id = 0;
};

// Host definition of the opaque dftu_op handle: a node in a plugin's compose
// graph. Leaf nodes carry the plugin callback + owned state; combinator nodes
// carry their children. Arena-owned by PluginFold (scan-lifetime).
struct ComposeOp {
    enum class Kind { LEAF, THEN, WHEN_ALL, WHEN_ANY };
    Kind kind = Kind::LEAF;
    dftu_op_fn fn = nullptr;
    void* state = nullptr;
    void (*free_state)(void*) = nullptr;
    dftu_type in_ty = DFTU_T_VOID;
    dftu_type out_ty = DFTU_T_VOID;
    std::uint32_t in_size = 0;
    std::uint32_t out_size = 0;
    std::vector<ComposeOp*> children;

    ComposeOp() = default;
    ComposeOp(const ComposeOp&) = delete;
    ComposeOp& operator=(const ComposeOp&) = delete;
    ~ComposeOp() {
        if (free_state) free_state(state);
    }
};

// dft.ext.agg accumulator; defined in the .cpp to keep agg.h (and its Arrow
// tangle) out of this header.
struct AggAccum;

// One live instance of a plugin-registered dftu_state_desc; defined in
// fold_adapter/state.cpp.
class StateAccum;

// Cross-plugin view of the merged, named accumulators backing dft.ext.agg's
// agg_result. Each master fold publishes its merged AggAccums here at the top
// of finalize, so a plugin finalizing later reads another plugin's whole-scan
// aggregate by name. merge and finalize are serialized, so no locking is
// needed. The registry borrows; the producing fold owns each AggAccum.
struct SharedResultRegistry {
    std::unordered_map<std::uint64_t, AggAccum*> aggs;
    /// Which plugin published each key, so a second publisher of the same name
    /// is refused instead of silently replacing the first. A name declared in
    /// `provides` is already rejected at build(); this catches the accumulator
    /// a plugin creates without declaring it.
    std::unordered_map<std::uint64_t, std::string> owners;
};

// Name-keyed registry of stable-address elements. Deque-backed so an element
// pointer (a dftu_agg* handed to a plugin) never moves; the name-hash index
// gives get-or-create without a linear scan. Insertion order is preserved for
// deterministic materialize.
template <class T>
class StableRegistry {
   public:
    using IndexMap = ankerl::unordered_dense::map<std::uint64_t, std::size_t>;

    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    T& operator[](std::size_t i) { return items_[i]; }
    const T& operator[](std::size_t i) const { return items_[i]; }
    const IndexMap& index() const { return index_; }

    /// Pointer to the element for @p key, or nullptr if absent.
    T* find(std::uint64_t key) {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &items_[it->second];
    }

    /// Append @p value under @p key and return its stable address. The caller
    /// guarantees @p key is not already present.
    T* push(std::uint64_t key, T value) {
        items_.push_back(std::move(value));
        index_.emplace(key, items_.size() - 1);
        return &items_.back();
    }

    /// Return the element for @p key, creating it via @p make() (invoked only
    /// on a miss). Returns nullptr if construction or insertion throws.
    template <class Factory>
    T* get_or_create(std::uint64_t key, Factory&& make) {
        if (T* e = find(key)) return e;
        try {
            return push(key, make());
        } catch (...) {
            return nullptr;
        }
    }

   private:
    std::deque<T> items_;
    IndexMap index_;
};

class PluginFold : public trace::views::detail::Fold {
    using Fold = trace::views::detail::Fold;
    using FoldBatch = trace::views::detail::FoldBatch;
    using ScanShape = trace::views::detail::ScanShape;
    using ScanUnit = trace::views::detail::ScanUnit;
    using CoverageSet = trace::views::detail::CoverageSet;
    using FoldPortBus = trace::views::detail::FoldPortBus;

   public:
    /// `memory_budget` is the scan's out-of-core aggregation budget in bytes,
    /// the same knob as View::memory_budget: 0 means auto (~1/3 of available
    /// memory) and NO_SPILL_BUDGET disables spilling. Past it, an accumulator's
    /// live group map spills to a sorted temp run instead of growing.
    /// `states` are the state types the plugin's factory registered; the fold
    /// makes one instance of each per slice and drives it like an accumulator.
    /// It must outlive the fold; null means the plugin registered none.
    PluginFold(const dftu_plugin* plugin, dftracer::utils::StringIntern& intern,
               SharedResultRegistry* results = nullptr,
               NamedResultRegistry* named_results = nullptr,
               std::string plugin_name = {}, std::uint64_t memory_budget = 0,
               const StateRegistry* states = nullptr);
    ~PluginFold() override;

    PluginFold(const PluginFold&) = delete;
    PluginFold& operator=(const PluginFold&) = delete;

    bool accepts(const ScanShape&) const override { return true; }
    // The batch materializes every flat arg as a dyn column regardless (see
    // step()), so a plugin fold always needs args extracted.
    bool needs_args() const override { return true; }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<PluginFold>(plugin_, *intern_, results_,
                                            named_results_, plugin_name_,
                                            memory_budget_, states_);
    }

    /// Name this plugin logs under (its load-path stem); empty if unknown.
    const std::string& plugin_name() const { return plugin_name_; }

    void step(const FoldBatch& batch) override;
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold& other) override;
    coro::CoroTask<bool> finalize(const CoverageSet& covered) override;

    // The fuse worker awaits this after step(); consumed once, then null.
    ::dftu_task* take_pending() override {
        ::dftu_task* t = pending_;
        pending_ = nullptr;
        return t;
    }

    void bind_port_bus(FoldPortBus* bus) override { port_bus_ = bus; }

    // No-ops / NULL when no bus is bound (e.g. a unit test without the driver).
    void port_publish(std::uint64_t key, const void* data, std::uint32_t len) {
        if (port_bus_) port_bus_->publish(key, data, len);
    }
    const void* port_consume(std::uint64_t key, std::uint32_t* out_len) const {
        if (port_bus_) return port_bus_->consume(key, out_len);
        if (out_len) *out_len = 0;
        return nullptr;
    }

    // Emit a named, kind-tagged result into the host registry; moves the
    // Arrow/frame/lazyframe handle carried in `v` (bytes are copied). -1 on a
    // NULL name/v/registry or an unrecognized kind.
    int result_emit(const char* name, dftu_result_value* v);

    // Get-or-create this slice's named dft.ext.agg accumulator; null on a bad
    // op code, a missing output name, or an allocation failure. A name seen
    // before keeps its creation-time specs. agg_accumulate skips a batch
    // missing any referenced column.
    dftu_agg* agg_new(const char* name, const char* const* key_names,
                      std::uint32_t key_n, const dftu_agg_col* specs,
                      std::uint32_t spec_n);
    void agg_accumulate(dftu_agg* a, const dftu_dataframe* df);
    // The merged, finalized accumulator named `name` from any plugin in the
    // fuse, as a new owned handle the caller frees; null when no plugin has
    // published that name yet. Finalize-time read (see SharedResultRegistry).
    dftu_dataframe* agg_result(const char* name) const;
    /// Spill runs written by the accumulator named `name` (0 when it stayed in
    /// memory, or when there is no such accumulator). The observable signal
    /// that the memory budget engaged.
    std::size_t agg_spill_runs(const char* name) const;

    /// Spill runs written by the registered state named `name`; the same
    /// budget-engaged signal as agg_spill_runs, for a tier-2 state.
    std::size_t state_spill_runs(const char* name) const;
    /// True when the state named `name` ran past the budget with no
    /// serialize/deserialize pair, so the host had to refuse it a spill.
    bool state_spill_refused(const char* name) const;

    /// Times the host asked the slice to reclaim (dftu_plugin::reclaim)
    /// after a step or merge left it past the budget; the budget-engaged
    /// signal for a plugin's own slice.
    std::size_t slice_reclaims() const { return slice_reclaims_; }
    /// True once a reclaim freed nothing while the slice was still over the
    /// budget, or the plugin measures its slice but declares no reclaim.
    bool slice_reclaim_refused() const { return reclaim_refused_; }

    // Allocate a lazy CoroTask into this step's arena, returning its stable
    // address as an opaque dftu_task*; null if allocation throws.
    ::dftu_task* emplace_task(coro::CoroTask<void>&& task);

    // dftu_svc_compose backing. Ops are arena-owned (scan-lifetime); the
    // combinators validate value sizes and return null on a mismatch.
    ::dftu_op* compose_make(dftu_op_fn fn, void* state,
                            void (*free_state)(void*), dftu_type in_ty,
                            std::uint32_t in_size, dftu_type out_ty,
                            std::uint32_t out_size);
    ::dftu_op* compose_then(::dftu_op* a, ::dftu_op* b);
    ::dftu_op* compose_when_all(::dftu_op* const* ops, std::uint32_t n);
    ::dftu_op* compose_when_any(::dftu_op* const* ops, std::uint32_t n);
    ::dftu_task* compose_run(::dftu_op* op, const void* in, void* out, int* rc);
    // A compose leaf wrapping registered host utility `util_id` (single-value
    // only); null for a stream-only or unknown id. Lets then()/when_all pipe
    // host utilities through the dftu_op engine.

    dftracer::utils::StringIntern& intern_table() { return *intern_; }
    ::dftu_writer* create_writer(const char* path, std::uint32_t num_workers,
                                 int gzip);
    ::dftu_trace_writer* create_trace_writer(const char* path);
    int close_trace_writer(::dftu_trace_writer* w);

    // The fully wired host service table.
    ::dftu_plugin_host& host() { return host_; }

    // Valid until this PluginFold is destroyed; null on parse error.
    ::dftu_query* compile_query(const char* src, std::uint32_t len);
    int match_query(const query::Query& q, const dftu_dataframe* df,
                    std::int64_t row);

   private:
    using Query = query::Query;
    using ValueMap = query::ValueMap;
    using FoldEvent = trace::views::detail::FoldEvent;

    // This plugin's own plan_query, finer than the union prune (which only
    // skips whole chunks no plugin wants); filters events within a batch.
    bool passes_query(const FoldEvent& ev);
    // Publish this fold's merged accumulators into the shared registry so a
    // later plugin's agg_result can find them.
    void publish_aggs();
    // Finalize each merged accumulator to a frame and emit it under its name.
    void materialize_aggs();

    // The registered-state half of step/merge/finalize; see
    // fold_adapter/state.cpp.
    void states_update(const dftu_dataframe* df);
    void states_merge(PluginFold& other);
    void states_finalize();

    const dftu_plugin* plugin_;
    std::string plugin_name_;
    dftracer::utils::StringIntern* intern_;
    void* slice_;
    dftu_plugin_host host_{};
    ::dftu_task* pending_ = nullptr;
    FoldPortBus* port_bus_ = nullptr;
    SharedResultRegistry* results_ = nullptr;
    NamedResultRegistry* named_results_ = nullptr;
    std::uint64_t memory_budget_ = 0;
    std::size_t slice_reclaims_ = 0;
    bool reclaim_refused_ = false;

    // Asks the slice to shrink when its declared size runs past the budget.
    void reclaim_slice();

    // A batch an earlier plugin rewrote, as this plugin sees it.
    dataframe::DataFrame rewritten_batch(
        const dataframe::DataFrame& frame,
        const std::vector<std::string>& projection);
    bool query_on_rewrite_refused_ = false;

    // dft.ext.agg accumulators; unique_ptr keeps each handed-out dftu_agg*
    // stable and lets the header forward-declare AggAccum.
    StableRegistry<std::unique_ptr<AggAccum>> aggs_;

    // One live instance per registered state type, parallel to *states_.
    const StateRegistry* states_ = nullptr;
    std::vector<std::unique_ptr<StateAccum>> state_accums_;

    // unique_ptr keeps each CoroTask address stable so when_all/then can
    // reference them; cleared once the awaited root completes.
    std::vector<std::unique_ptr<coro::CoroTask<void>>> task_arena_;
    // deque keeps op node addresses stable as combinators reference children.
    std::deque<ComposeOp> compose_ops_;
    std::vector<std::unique_ptr<PluginWriter>> writers_;
    std::vector<std::unique_ptr<PluginTraceWriter>> trace_writers_;

    // This plugin's own plan_query, compiled once from the C string the
    // plugin's dftu_plugin::plan_query returns. Unset means deliver every
    // event.
    std::optional<Query> query_;
    ValueMap qmap_;

    // dftu_svc_query::query_compile/query_matches: queries a plugin compiles
    // itself, independent of plan_query above.
    std::deque<Query> compiled_queries_;
    ValueMap match_qmap_;

    std::vector<FoldEvent> col_scratch_;  // batch materialization

    // The columns dftu_plugin::reads declared, snapshotted once at
    // construction. Empty means the plugin declared none, which build_row_frame
    // reads as "every column".
    std::vector<std::string> projection_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_FOLD_ADAPTER_H
