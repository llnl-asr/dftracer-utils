// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/reserved_names.h>
#include <dftracer/utils/plugins/host/state_registry.h>

#include <algorithm>

namespace dftracer::utils::plugins::host {

dftu_status register_state_into(StateRegistry& out, ::dftu_ctx* ctx,
                                const dftu_state_desc* desc, void* self) {
    if (!desc || !desc->name) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "register_state needs a named desc");
    }
    const dstd::StringView name = desc->name;
    if (!plugin_struct_size_ok(
            PluginStruct::StateDesc, desc->struct_size,
            static_cast<std::uint32_t>(sizeof(dftu_state_desc)))) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: struct_size {} is outside [{}, "
                      "{}]">(
                name, desc->struct_size,
                plugin_struct_min_size(PluginStruct::StateDesc),
                static_cast<std::uint32_t>(sizeof(dftu_state_desc))));
    }
    dftu_state_desc desc_copy{};
    common::copy_struct(&desc_copy, sizeof(desc_copy), desc, desc->struct_size,
                        plugin_struct_min_size(PluginStruct::StateDesc));
    if (refuse_plugin_op_name(desc_copy.name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: a state name must be "
                      "'<plugin>.<name>' and the 'dftu.' namespace belongs to "
                      "the host">(name));
    }
    if (!desc_copy.init || !desc_copy.update || !desc_copy.merge ||
        !desc_copy.finalize || !desc_copy.destroy) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: init, update, merge, finalize and "
                      "destroy are all required">(name));
    }
    // Half a pair would let the host write a run it could never read back.
    if (!desc_copy.serialize != !desc_copy.deserialize) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: serialize and deserialize are one "
                      "pair, given together or not at all">(name));
    }
    const dstd::String key = desc_copy.name;
    if (std::any_of(out.begin(), out.end(),
                    [&](const RegisteredState& s) { return s.name == key; })) {
        return host_fail(ctx, DFTU_E_INVALID,
                         dstd::fmt<"state '{}' is already registered">(name));
    }
    out.push_back(RegisteredState{key, desc_copy, self});
    return DFTU_OK;
}

}  // namespace dftracer::utils::plugins::host
