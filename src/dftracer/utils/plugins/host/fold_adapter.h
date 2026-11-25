// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_H
#define DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>
#include <dftracer/utils/plugins/host/state_registry.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Adapts a loaded C-ABI plugin to the internal fold interface so N plugins ride
// one fused scan. A task on_finalize hands back is awaited by finalize.
namespace dftracer::utils::plugins::host {

class PortHub;
class PortNames;

// A host-owned object lent to the plugin as an opaque handle (a reader, a
// writer); the fold keeps it until the plugin closes it or the fold ends.
struct PluginHandle {
    virtual ~PluginHandle() = default;
};

// Host-owned trace writer handed to the plugin as an opaque dftu_trace_writer*.
// trace_close compresses `buffer` into gzip members as a re-indexable .pfw.gz.
struct PluginTraceWriter {
    dstd::String path;
    dstd::String buffer;
    std::uint64_t next_id = 0;
};

// The host side of a dftu_job: a leaf runs a plugin body, the rest combine
// children. Arena-owned by PluginFold (scan-lifetime).
struct Job {
    enum class Kind { Leaf, Sleep, Then, All, Any, Map };

    explicit Job(Kind k) : kind(k) {}
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() {
        if (free_state) free_state(state);
    }

    dstd::Vector<Job*> children;
    dftu_job_fn fn = nullptr;
    void* state = nullptr;
    void (*free_state)(void*) = nullptr;
    std::uint64_t sleep_ns = 0;
    Kind kind;
    dftu_job_where where = DFTU_JOB_CPU;
    dftu_type in_ty = DFTU_T_VOID;
    dftu_type out_ty = DFTU_T_VOID;
    std::uint32_t in_size = 0;
    std::uint32_t out_size = 0;
    std::uint32_t max_parallel = 0;
};

// dft.ext.agg accumulator; defined in the .cpp to keep agg.h out of this
// header.
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
    dstd::HashMap<std::uint64_t, AggAccum*> aggs;
    /// Which plugin published each key, so a second publisher of the same name
    /// is refused instead of silently replacing the first. A name declared in
    /// `provides` is already rejected at build(); this catches the accumulator
    /// a plugin creates without declaring it.
    dstd::HashMap<std::uint64_t, dstd::String> owners;
};

// Name-keyed registry of stable-address elements. Deque-backed so an element
// pointer (a dftu_agg* handed to a plugin) never moves; the name-hash index
// gives get-or-create without a linear scan. Insertion order is preserved for
// deterministic materialize.
template <class T>
class StableRegistry {
   public:
    using IndexMap = dstd::HashMap<std::uint64_t, std::size_t>;

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
    /// on a miss).
    template <class Factory>
    T* get_or_create(std::uint64_t key, Factory&& make) {
        if (T* e = find(key)) return e;
        return push(key, make());
    }

   private:
    dstd::Deque<T> items_;
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
    /// memory) and memory::NO_SPILL_BUDGET disables spilling. Past it, an
    /// accumulator's live group map spills to a sorted temp run. `states` are
    /// the state types the plugin's factory registered, one instance per slice.
    /// It must outlive the fold. Null means none. `memory` is the pool the
    /// plugin's blocks and reservations live in, shared by every fold of the
    /// plugin; null gives this fold and its slices one of their own.
    PluginFold(const dftu_plugin* plugin,
               dftracer::utils::dstd::StringIntern& intern,
               SharedResultRegistry* results = nullptr,
               utils::plugins::NamedResultRegistry* named_results = nullptr,
               dstd::String plugin_name = {}, std::uint64_t memory_budget = 0,
               const StateRegistry* states = nullptr,
               dstd::SharedPtr<PluginMemory> memory = nullptr);
    ~PluginFold() override;

    PluginFold(const PluginFold&) = delete;
    PluginFold& operator=(const PluginFold&) = delete;

    bool accepts(const ScanShape&) const override { return true; }
    // The batch materializes every flat arg as a dyn column regardless (see
    // step()), so a plugin fold always needs args extracted.
    bool needs_args() const override { return true; }
    bool reads_every_field() const override { return true; }

    dstd::UniquePtr<Fold> slice() const override {
        auto fold = dstd::make_unique<PluginFold>(
            plugin_, *intern_, results_, named_results_, plugin_name_,
            memory_budget_, states_, memory_);
        fold->bind_ports(port_hub_, port_namespace_);
        return fold;
    }

    /// Wires the ports that cross plugins: `hub` is the scan's, and
    /// `port_namespace` is the name the plugin declared its channels under.
    /// Null `hub` leaves those calls refused.
    void bind_ports(PortHub* hub, dstd::String port_namespace);
    PortHub* port_hub() const { return port_hub_; }
    const dstd::String& port_namespace() const { return port_namespace_; }
    /// The qualified names this plugin used; null with no hub.
    PortNames* port_names() const { return port_names_.get(); }

    /// Name this plugin logs under (its load-path stem); empty if unknown.
    const dstd::String& plugin_name() const { return plugin_name_; }

    dstd::Result<void> step(const FoldBatch& batch) override;
    void seal_unit(const ScanUnit& unit) override {
        note_index_path(unit.index_path);
    }
    void drop_unit(const ScanUnit& unit) override {
        note_index_path(unit.index_path);
    }
    dstd::Result<void> merge(Fold& other) override;
    coro::CoroTask<dstd::Result<bool>> finalize(
        const CoverageSet& covered) override;

    void bind_port_bus(FoldPortBus* bus) override { port_bus_ = bus; }

    // No-ops / NULL when no bus is bound (e.g. a unit test without the driver).
    void port_publish(std::uint64_t key, const void* data, std::uint32_t len) {
        if (port_bus_) port_bus_->publish(key, data, len);
    }
    bool has_port_bus() const { return port_bus_ != nullptr; }
    std::size_t port_bus_bytes() const {
        return port_bus_ ? port_bus_->size_bytes() : 0;
    }
    const void* port_consume(std::uint64_t key, std::uint32_t* out_size) const {
        if (port_bus_) return port_bus_->consume(key, out_size);
        if (out_size) *out_size = 0;
        return nullptr;
    }

    // Emit a named, kind-tagged result into the host registry; moves the
    // frame/lazyframe handle carried in `v` (bytes are copied). Fails on
    // a NULL name/v/registry or an unrecognized kind.
    dstd::Result<void> emit_result(dstd::StringView name, dftu_result_value* v);

    // Get-or-create this slice's named dft.svc.agg accumulator; fails on a bad
    // op code or a missing output name. A name seen before keeps its
    // creation-time specs. agg_accumulate skips a batch missing any referenced
    // column.
    dstd::Result<dftu_agg*> agg_new(dstd::StringView name,
                                    dstd::Span<const char* const> key_names,
                                    dstd::Span<const dftu_agg_col> specs);
    dstd::Result<void> agg_accumulate(dftu_agg* a, const dftu_dataframe* df);
    // The merged, finalized accumulator named `name` from any plugin in the
    // fuse, as a new owned handle the caller frees; null when no plugin has
    // published that name yet. Finalize-time read (see SharedResultRegistry).
    dstd::Result<dftu_dataframe*> agg_result(dstd::StringView name) const;
    /// Spill runs written by the accumulator named `name` (0 when it stayed in
    /// memory, or when there is no such accumulator). The observable signal
    /// that the memory budget engaged.
    std::size_t agg_spill_runs(dstd::StringView name) const;

    /// Spill runs written by the registered state named `name`; the same
    /// budget-engaged signal as agg_spill_runs, for a tier-2 state.
    std::size_t state_spill_runs(dstd::StringView name) const;
    /// True when the state named `name` ran past the budget with no
    /// serialize/deserialize pair, so the host had to refuse it a spill.
    bool state_spill_refused(dstd::StringView name) const;

    /// The pool and the reservations of this fold's plugin.
    PluginMemory& memory() { return *memory_; }
    /// The scan's memory budget as given; see the constructor.
    std::uint64_t memory_budget() const { return memory_budget_; }
    /// Starts listening for the memory manager's requests to give bytes back;
    /// called when the plugin makes its first reservation.
    void watch_memory_requests();
    /// At a batch boundary: runs the reclaimers of this fold's reservations
    /// for the bytes another holder asked of the plugin's pool. A no-op until
    /// a reservation exists.
    void poll_reclaim();

    /// Safe on many threads at once: task bodies that run in parallel make
    /// their jobs at the same time.
    Job& new_job(Job::Kind kind) {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        return jobs_.emplace_back(kind);
    }

    dftracer::utils::dstd::StringIntern& intern_table() { return *intern_; }
    /// Keeps @p handle until @ref release_handle or the end of the fold and
    /// returns it, as the opaque pointer the plugin holds.
    PluginHandle* adopt_handle(dstd::UniquePtr<PluginHandle> handle);
    /// Frees the handle @p raw; a pointer the fold does not hold is ignored.
    void release_handle(const void* raw);
    /// The handle @p raw as a T, or null when the fold holds no such handle.
    template <class T>
    T* find_handle(const void* raw) {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        for (auto& handle : handles_) {
            if (static_cast<const void*>(handle.get()) == raw)
                return dynamic_cast<T*>(handle.get());
        }
        return nullptr;
    }
    ::dftu_trace_writer* make_trace_writer(dstd::String path);
    dstd::Result<void> close_trace_writer(::dftu_trace_writer* w);

    /// The store of the scan's index root (dftu.svc.store), opened on the
    /// first call and freed with the fold; fails until a unit names its index.
    dftu_status store_for(::dftu_ctx* ctx, ::dftu_store_t** out);
    /// The store directory this fold lends. Every fold over one index root
    /// names the same one, since the store state is shared per directory, and a
    /// handle the host made carries it so another directory's fold can refuse
    /// it.
    const dstd::String& store_identity();
    /// Flushes the plugin's store once, from finalize, so a reader in another
    /// process sees what the fold committed; the error is the run's. A slice
    /// that used the store left its handle behind, so the master opens the
    /// shared state here and flushes it.
    dstd::Result<void> store_flush();
    /// Notes the index path of a unit the scan hands this fold. The first one
    /// names the index root the store service opens; a unit of a second root is
    /// a conflict the store call reports, since one store serves one root.
    /// Units of one root may name its directory or a file inside it, so the two
    /// index paths are compared by the store directory they resolve to.
    void note_index_path(dstd::StringView index_path) {
        if (index_path.empty() || index_path == last_index_path_) return;
        last_index_path_ = dstd::String(index_path);
        const dstd::String dir = trace::index::store_of(index_path);
        if (dir.empty()) return;
        if (store_dir_.empty()) {
            store_root_ = dstd::String(index_path);
            store_dir_ = dir;
            return;
        }
        if (store_dir_ == dir) return;
        if (store_conflict_.empty()) {
            store_conflict_ = dstd::String(index_path);
            store_conflict_dir_ = dir;
        }
    }

    /// Takes another fold's notes: a slice saw the units, so the master learns
    /// the root, a conflict and that the store was handed out from it.
    void take_store_notes(const PluginFold& other) {
        if (!other.store_dir_.empty()) {
            if (store_dir_.empty()) {
                store_root_ = dstd::String(other.store_root_);
                store_dir_ = dstd::String(other.store_dir_);
            } else if (store_dir_ != other.store_dir_ &&
                       store_conflict_.empty()) {
                store_conflict_ = dstd::String(other.store_root_);
                store_conflict_dir_ = dstd::String(other.store_dir_);
            }
        }
        if (store_conflict_.empty() && !other.store_conflict_.empty()) {
            store_conflict_ = dstd::String(other.store_conflict_);
            store_conflict_dir_ = dstd::String(other.store_conflict_dir_);
        }
        if (other.store_used_) store_used_ = true;
    }

    /// The plugin's name for an error message.
    dstd::StringView plugin_label() const {
        return plugin_name_.empty() ? dstd::StringView("plugin")
                                    : dstd::StringView(plugin_name_);
    }

    // The fully wired host service table.
    ::dftu_plugin_host& host() { return host_; }

    // Valid until this PluginFold is destroyed; fails on a parse error.
    dstd::Result<::dftu_duql*> compile_query(dstd::StringView src);
    int match_query(const duql::Query& q, const dftu_dataframe* df,
                    std::int64_t row);

   private:
    using Query = duql::Query;
    using ValueMap = duql::ValueMap;
    using FoldEvent = trace::views::detail::FoldEvent;

    // This plugin's own plan_duql, finer than the union prune (which only
    // skips whole chunks no plugin wants); filters events within a batch.
    bool passes_query(const FoldEvent& ev);
    // Publish this fold's merged accumulators into the shared registry so a
    // later plugin's agg_result can find them.
    void publish_aggs();
    // Finalize each merged accumulator to a frame and emit it under its name.
    dstd::Result<void> materialize_aggs();

    // The registered-state half of step/merge/finalize; see
    // fold_adapter/state.cpp.
    dstd::Result<void> states_update(const dftu_dataframe* df);
    dstd::Result<void> states_merge(PluginFold& other);
    dstd::Result<void> states_finalize();

    const dftu_plugin* plugin_;
    dstd::String plugin_name_;
    dftracer::utils::dstd::StringIntern* intern_;
    void* slice_ = nullptr;
    // What make_slice reported; returned by the first step and by finalize.
    dstd::Optional<dstd::Error> slice_error_;
    dftu_plugin_host host_{};
    FoldPortBus* port_bus_ = nullptr;
    PortHub* port_hub_ = nullptr;
    dstd::UniquePtr<PortNames> port_names_;
    dstd::String port_namespace_;
    SharedResultRegistry* results_ = nullptr;
    utils::plugins::NamedResultRegistry* named_results_ = nullptr;
    std::uint64_t memory_budget_ = 0;
    dstd::SharedPtr<PluginMemory> memory_;
    MemoryWatch memory_watch_;

    // A batch an earlier plugin rewrote, as this plugin sees it.
    dataframe::DataFrame rewritten_batch(
        const dataframe::DataFrame& frame,
        dstd::Span<const dstd::String> projection);
    bool query_on_rewrite_refused_ = false;

    // dft.ext.agg accumulators; unique_ptr keeps each handed-out dftu_agg*
    // stable and lets the header forward-declare AggAccum.
    StableRegistry<dstd::UniquePtr<AggAccum>> aggs_;

    // One live instance per registered state type, parallel to *states_.
    const StateRegistry* states_ = nullptr;
    dstd::Vector<dstd::UniquePtr<StateAccum>> state_accums_;

    // Keeps job addresses stable; jobs live for the scan.
    dstd::Deque<Job> jobs_;
    std::mutex jobs_mutex_;
    std::mutex handles_mutex_;
    dstd::Vector<dstd::UniquePtr<PluginHandle>> handles_;
    dstd::Vector<dstd::UniquePtr<PluginTraceWriter>> trace_writers_;

    // This plugin's own plan_duql, compiled once from the C string the
    // plugin's dftu_plugin::plan_duql returns. Unset means deliver every
    // event.
    dstd::Optional<Query> query_;
    ValueMap qmap_;

    // dftu_svc_duql::duql_compile/duql_matches: queries a plugin compiles
    // itself, independent of plan_duql above.
    dstd::Deque<Query> compiled_queries_;
    ValueMap match_qmap_;
    // duql_matches runs once per row, so the columns each query reads are
    // resolved once per frame and kept here until a frame with another
    // serial comes.
    struct MatchColumns {
        const Query* query;
        std::int64_t rows;
        dstd::Vector<dstd::Pair<dstd::StringView, dataframe::Series>> columns;
    };
    std::uint64_t match_frame_serial_ = 0;
    dstd::Vector<MatchColumns> match_columns_;
    const MatchColumns& match_columns(const Query& q, const dftu_dataframe* df);

    dstd::Vector<FoldEvent> col_scratch_;  // batch materialization

    // The columns dftu_plugin::reads declared, snapshotted once at
    // construction. Empty means the plugin declared none, which build_row_frame
    // reads as "every column".
    dstd::Vector<dstd::String> projection_;

    // The index path of the scan's first unit, which names the index root of
    // the store dftu.svc.store lends, that root's store directory, and the
    // handle once opened. `store_used_` records that this fold or a slice it
    // merged handed the store out, so finalize flushes the shared writer.
    dstd::String store_root_;
    dstd::String store_conflict_;
    dstd::String store_dir_;
    dstd::String store_conflict_dir_;
    /// The index path note_index_path saw last, so a batch of the same unit
    /// skips resolving its store directory again.
    dstd::String last_index_path_;
    bool store_used_ = false;
    ::dftu_store_t* store_ = nullptr;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_H
