// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/plugins/host_status.h>
#include <dftracer/utils/domains/plugins/reserved_names.h>
#include <dftracer/utils/domains/plugins/state_registry.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>

#include <algorithm>

namespace dftracer::utils::domains::plugins {

dftu_status register_state_into(StateRegistry& out, ::dftu_ctx* ctx,
                                const dftu_state_desc* desc, void* self) {
    if (!desc || !desc->name) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "register_state needs a named desc");
    }
    const dstd::StringView name = desc->name;
    if (desc->struct_size != sizeof(dftu_state_desc)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         dstd::fmt<"state '{}' refused: struct_size is not "
                                   "sizeof(dftu_state_desc)">(name));
    }
    if (refuse_plugin_op_name(desc->name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: a state name must be "
                      "'<plugin>.<name>' and the 'dftu.' namespace belongs to "
                      "the host">(name));
    }
    if (!desc->init || !desc->update || !desc->merge || !desc->finalize ||
        !desc->destroy) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: init, update, merge, finalize and "
                      "destroy are all required">(name));
    }
    // Half a pair would let the host write a run it could never read back.
    if (!desc->serialize != !desc->deserialize) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"state '{}' refused: serialize and deserialize are one "
                      "pair, given together or not at all">(name));
    }
    const dstd::String key = desc->name;
    if (std::any_of(out.begin(), out.end(),
                    [&](const RegisteredState& s) { return s.name == key; })) {
        return host_fail(ctx, DFTU_E_INVALID,
                         dstd::fmt<"state '{}' is already registered">(name));
    }
    out.push_back(RegisteredState{key, *desc, self});
    return DFTU_OK;
}

}  // namespace dftracer::utils::domains::plugins
