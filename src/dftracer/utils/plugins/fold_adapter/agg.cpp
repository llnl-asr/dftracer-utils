#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins {
namespace {

::dftu_agg* host_agg_new(void* h, const char* name,
                         const char* const* key_names, std::uint32_t key_n,
                         const ::dftu_agg_col* specs, std::uint32_t spec_n) {
    return static_cast<PluginFold*>(h)->agg_new(name, key_names, key_n, specs,
                                                spec_n);
}

void host_agg_accumulate(void* h, ::dftu_agg* a, const ::dftu_dataframe* df) {
    static_cast<PluginFold*>(h)->agg_accumulate(a, df);
}

::dftu_dataframe* host_agg_result(void* h, const char* name) {
    return static_cast<PluginFold*>(h)->agg_result(name);
}

// Registration is a build-phase call: by the time a fold runs, the registry
// the host builds its slices from is already settled.
int host_register_state(void* h, const ::dftu_state_desc* desc, void*) {
    DFTRACER_UTILS_LOG_ERROR(
        "[plugin:%s] register_state('%s') refused: a state type is registered "
        "from the factory, not during the scan",
        static_cast<PluginFold*>(h)->plugin_name().c_str(),
        desc && desc->name ? desc->name : "(null)");
    return -1;
}

const dftu_svc_agg g_agg = {host_agg_new, host_agg_accumulate, host_agg_result,
                            host_register_state};

// Finalize `acc` and move its columns into a dftu_dataframe handle (the
// engine's own ABI boundary type) so the result crosses as our DataFrame, no
// Arrow round-trip. agg_finalize reads the state, so this may run more than
// once for the same accumulator. Null if it has no state or finalize throws.
::dftu_dataframe* finalize_to_frame(const AggAccum& acc) {
    if (!acc.state) return nullptr;
    try {
        dataframe::DataFrame out = acc.spiller.drain(*acc.state, acc.key_names);
        std::vector<dftu_series*> handles;
        handles.reserve(out.columns.size());
        for (dataframe::Series& c : out.columns) handles.push_back(c.release());
        std::vector<const char*> names;
        names.reserve(out.names.size());
        for (const std::string& n : out.names) names.push_back(n.c_str());
        return dftu_dataframe_new(names.data(), handles.data(),
                                  static_cast<std::int32_t>(handles.size()));
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

const void* detail::agg_ext_vtable() { return &g_agg; }

::dftu_agg* PluginFold::agg_new(const char* name, const char* const* key_names,
                                std::uint32_t key_n,
                                const ::dftu_agg_col* specs,
                                std::uint32_t spec_n) {
    if (!name || (key_n && !key_names) || spec_n == 0 || !specs) return nullptr;
    if (is_host_namespace(name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin agg '%s' refused: the 'dftu.' namespace belongs to the "
            "host",
            name);
        return nullptr;
    }
    std::uint64_t key = dftracer::utils::hash::fnv1a_hash(name);
    if (std::unique_ptr<AggAccum>* slot = aggs_.find(key))
        return reinterpret_cast<::dftu_agg*>(slot->get());

    std::vector<std::string> value_names;
    auto column_index = [&](const char* col) -> std::int32_t {
        if (!col) return -1;
        for (std::size_t i = 0; i < value_names.size(); ++i)
            if (value_names[i] == col) return static_cast<std::int32_t>(i);
        value_names.emplace_back(col);
        return static_cast<std::int32_t>(value_names.size() - 1);
    };

    std::vector<dataframe::AggSpec> aspecs;
    aspecs.reserve(spec_n);
    for (std::uint32_t i = 0; i < spec_n; ++i) {
        if (specs[i].op < DFTU_AGG_COUNT || specs[i].op > DFTU_AGG_REGR_R2) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin agg '%s' aggregate %u has an out-of-range op code %d",
                name, i, specs[i].op);
            return nullptr;
        }
        if (!specs[i].out || !specs[i].out[0]) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin agg '%s' aggregate %u is missing an output name", name,
                i);
            return nullptr;
        }
        dataframe::AggSpec s;
        s.op = static_cast<dataframe::AggOp>(specs[i].op);
        s.value_col = column_index(specs[i].value);
        s.out = specs[i].out;
        s.param = specs[i].param;
        s.by_col = column_index(specs[i].by);
        aspecs.push_back(std::move(s));
    }

    std::unique_ptr<AggAccum>* slot = aggs_.get_or_create(key, [&] {
        auto acc = std::make_unique<AggAccum>();
        acc->name = name;
        acc->key_names.reserve(key_n);
        for (std::uint32_t i = 0; i < key_n; ++i)
            acc->key_names.emplace_back(key_names[i] ? key_names[i] : "");
        acc->value_names = std::move(value_names);
        acc->state = dataframe::agg_new(std::move(aspecs));
        acc->spiller = dataframe::AggSpiller(memory_budget_);
        return acc;
    });
    return slot ? reinterpret_cast<::dftu_agg*>(slot->get()) : nullptr;
}

void PluginFold::agg_accumulate(::dftu_agg* a, const ::dftu_dataframe* df) {
    if (!a || !df) return;
    AggAccum& acc = *reinterpret_cast<AggAccum*>(a);
    if (!acc.state) return;

    // Series wraps and frees each shared handle; a missing column yields a null
    // handle, so skip the whole batch rather than accumulate a partial key.
    std::vector<dataframe::Series> owned;
    owned.reserve(acc.key_names.size() + acc.value_names.size());
    auto column = [&](const std::string& n) -> const dataframe::Series* {
        dftu_series* h = dftu_dataframe_column(df, n.c_str());
        if (!h) return nullptr;
        owned.emplace_back(h);
        return &owned.back();
    };

    std::vector<const dataframe::Series*> keys;
    keys.reserve(acc.key_names.size());
    for (const std::string& n : acc.key_names) {
        const dataframe::Series* c = column(n);
        if (!c) return;
        keys.push_back(c);
    }
    std::vector<const dataframe::Series*> values;
    values.reserve(acc.value_names.size());
    for (const std::string& n : acc.value_names) {
        const dataframe::Series* c = column(n);
        if (!c) return;
        values.push_back(c);
    }

    try {
        // Pass the row count explicitly: a zero-key accumulator (the scalar
        // case) has no key column for the engine to size the batch from.
        dataframe::agg_accumulate(*acc.state, keys, values, 0,
                                  dftu_dataframe_num_rows(df));
        acc.spiller.maybe_spill(acc.state);
    } catch (...) {
        DFTRACER_UTILS_LOG_ERROR("Plugin agg '%s' accumulate failed",
                                 acc.name.c_str());
    }
}

// Cross-plugin reads see only what a plugin that already finalized published.
// The fold order puts every provider before its consumers, so a declared name
// is always there; an undeclared one reads back null rather than this fold's
// own partial state.
::dftu_dataframe* PluginFold::agg_result(const char* name) const {
    if (!name || !results_) return nullptr;
    auto it = results_->aggs.find(dftracer::utils::hash::fnv1a_hash(name));
    if (it == results_->aggs.end() || !it->second) return nullptr;
    return finalize_to_frame(*it->second);
}

std::size_t PluginFold::agg_spill_runs(const char* name) const {
    if (!name) return 0;
    const std::uint64_t key = dftracer::utils::hash::fnv1a_hash(name);
    auto it = aggs_.index().find(key);
    if (it == aggs_.index().end()) return 0;
    const std::unique_ptr<AggAccum>& acc = aggs_[it->second];
    return acc ? acc->spiller.runs() : 0;
}

void PluginFold::publish_aggs() {
    if (!results_) return;
    for (const auto& [key, idx] : aggs_.index()) {
        std::unique_ptr<AggAccum>& acc = aggs_[idx];
        if (!acc || !acc->state) continue;
        auto owner = results_->owners.find(key);
        if (owner != results_->owners.end() && owner->second != plugin_name_) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin '%s' accumulator '%s' refused: '%s' already published "
                "that name; agg_result would return whichever finalized last. "
                "Names are lowercased, so two identifiers differing only in "
                "case collide here",
                plugin_name_.c_str(), acc->name.c_str(), owner->second.c_str());
            continue;
        }
        results_->aggs[key] = acc.get();
        results_->owners[key] = plugin_name_;
    }
}

void PluginFold::materialize_aggs() {
    if (!named_results_) return;
    for (std::unique_ptr<AggAccum>& acc : aggs_) {
        if (!acc) continue;
        if (::dftu_dataframe* h = finalize_to_frame(*acc))
            named_results_->emit_frame(acc->name.c_str(), h);
    }
}

}  // namespace dftracer::utils::plugins
