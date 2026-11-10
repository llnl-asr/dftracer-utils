// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

#include <cstdint>
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
        "[plugin:{}] register_state('{}') refused: a state type is registered "
        "from the factory, not during the scan",
        static_cast<PluginFold*>(h)->plugin_name(),
        desc && desc->name ? desc->name : "(null)");
    return -1;
}

const dftu_svc_agg g_agg = {host_agg_new, host_agg_accumulate, host_agg_result,
                            host_register_state};

// Finalize `acc` and move its columns into a dftu_dataframe handle (the
// engine's own ABI boundary type) so the result crosses as our DataFrame, no
// Arrow round-trip. agg_finalize reads the state, so this may run more than
// once for the same accumulator. Null if it has no state or finalize fails.
::dftu_dataframe* finalize_to_frame(const AggAccum& acc) {
    if (!acc.state) return nullptr;
    auto drained = acc.spiller.drain(*acc.state, acc.key_names);
    if (!drained) return nullptr;
    dataframe::DataFrame& out = *drained;
    dstd::Vector<dftu_series*> handles;
    handles.reserve(out.columns.size());
    for (dataframe::Series& c : out.columns) handles.push_back(c.release());
    dstd::Vector<const char*> names;
    names.reserve(out.names.size());
    for (const dstd::String& n : out.names) names.push_back(n.c_str());
    return dftu_dataframe_new(names.data(), handles.data(),
                              static_cast<std::int32_t>(handles.size()));
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
            "Plugin agg '{}' refused: the 'dftu.' namespace belongs to the "
            "host",
            name);
        return nullptr;
    }
    std::uint64_t key = dftracer::utils::dstd::hash::fnv1a_hash(name);
    if (dstd::UniquePtr<AggAccum>* slot = aggs_.find(key))
        return reinterpret_cast<::dftu_agg*>(slot->get());

    dstd::Vector<dstd::String> value_names;
    auto column_index = [&](const char* col) -> std::int32_t {
        if (!col) return -1;
        for (std::size_t i = 0; i < value_names.size(); ++i)
            if (value_names[i] == col) return static_cast<std::int32_t>(i);
        value_names.emplace_back(col);
        return static_cast<std::int32_t>(value_names.size() - 1);
    };

    dstd::Vector<dataframe::AggSpec> aspecs;
    aspecs.reserve(spec_n);
    for (std::uint32_t i = 0; i < spec_n; ++i) {
        if (specs[i].op < DFTU_AGG_COUNT || specs[i].op > DFTU_AGG_REGR_R2) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin agg '{}' aggregate {} has an out-of-range op code {}",
                name, i, specs[i].op);
            return nullptr;
        }
        if (!specs[i].out || !specs[i].out[0]) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin agg '{}' aggregate {} is missing an output name", name,
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

    dstd::UniquePtr<AggAccum>* slot = aggs_.get_or_create(key, [&] {
        auto acc = dstd::make_unique<AggAccum>();
        acc->name = name;
        acc->key_names.reserve(key_n);
        for (std::uint32_t i = 0; i < key_n; ++i)
            acc->key_names.emplace_back(key_names[i] ? key_names[i] : "");
        acc->value_names = std::move(value_names);
        acc->state = dataframe::agg_new(std::move(aspecs));
        acc->spiller = dataframe::AggSpiller(memory_budget_);
        return acc;
    });
    return reinterpret_cast<::dftu_agg*>(slot->get());
}

void PluginFold::agg_accumulate(::dftu_agg* a, const ::dftu_dataframe* df) {
    if (!a || !df) return;
    AggAccum& acc = *reinterpret_cast<AggAccum*>(a);
    if (!acc.state) return;

    // Series wraps and frees each shared handle; a missing column yields a null
    // handle, so skip the whole batch rather than accumulate a partial key.
    dstd::Vector<dataframe::Series> owned;
    owned.reserve(acc.key_names.size() + acc.value_names.size());
    auto column = [&](const auto& n) -> const dataframe::Series* {
        dftu_series* h = dftu_dataframe_column(df, n.c_str());
        if (!h) return nullptr;
        owned.emplace_back(h);
        return &owned.back();
    };

    dstd::Vector<const dataframe::Series*> keys;
    keys.reserve(acc.key_names.size());
    for (const dstd::String& n : acc.key_names) {
        const dataframe::Series* c = column(n);
        if (!c) return;
        keys.push_back(c);
    }
    dstd::Vector<const dataframe::Series*> values;
    values.reserve(acc.value_names.size());
    for (const dstd::String& n : acc.value_names) {
        const dataframe::Series* c = column(n);
        if (!c) return;
        values.push_back(c);
    }

    // Pass the row count explicitly: a zero-key accumulator (the scalar
    // case) has no key column for the engine to size the batch from.
    const auto accumulated = dataframe::agg_accumulate(
        *acc.state, keys, values, 0, dftu_dataframe_num_rows(df));
    if (!accumulated) {
        DFTRACER_UTILS_LOG_ERROR("Plugin agg '{}' accumulate failed", acc.name);
        return;
    }
    const auto spilled = acc.spiller.maybe_spill(acc.state);
    if (!spilled.has_value())
        DFTRACER_UTILS_LOG_ERROR("Plugin agg '{}' spill failed: {}", acc.name,
                                 spilled.error().c_str());
}

// Cross-plugin reads see only what a plugin that already finalized published.
// The fold order puts every provider before its consumers, so a declared name
// is always there; an undeclared one reads back null rather than this fold's
// own partial state.
::dftu_dataframe* PluginFold::agg_result(const char* name) const {
    if (!name || !results_) return nullptr;
    auto it =
        results_->aggs.find(dftracer::utils::dstd::hash::fnv1a_hash(name));
    if (it == results_->aggs.end() || !it->second) return nullptr;
    return finalize_to_frame(*it->second);
}

std::size_t PluginFold::agg_spill_runs(const char* name) const {
    if (!name) return 0;
    const std::uint64_t key = dftracer::utils::dstd::hash::fnv1a_hash(name);
    auto it = aggs_.index().find(key);
    if (it == aggs_.index().end()) return 0;
    const dstd::UniquePtr<AggAccum>& acc = aggs_[it->second];
    return acc ? acc->spiller.runs() : 0;
}

void PluginFold::publish_aggs() {
    if (!results_) return;
    for (const auto& [key, idx] : aggs_.index()) {
        dstd::UniquePtr<AggAccum>& acc = aggs_[idx];
        if (!acc || !acc->state) continue;
        auto owner = results_->owners.find(key);
        if (owner != results_->owners.end() && owner->second != plugin_name_) {
            DFTRACER_UTILS_LOG_ERROR(
                "Plugin '{}' accumulator '{}' refused: '{}' already published "
                "that name; agg_result would return whichever finalized last. "
                "Names are lowercased, so two identifiers differing only in "
                "case collide here",
                plugin_name_, acc->name, owner->second);
            continue;
        }
        results_->aggs[key] = acc.get();
        results_->owners[key] = plugin_name_;
    }
}

void PluginFold::materialize_aggs() {
    if (!named_results_) return;
    for (dstd::UniquePtr<AggAccum>& acc : aggs_) {
        if (!acc) continue;
        if (::dftu_dataframe* h = finalize_to_frame(*acc))
            named_results_->emit_frame(acc->name.c_str(), h);
    }
}

}  // namespace dftracer::utils::plugins
