#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/plugins/reserved_names.h>
#include <dftracer/utils/plugins/state_registry.h>

#include <algorithm>
#include <string>

namespace dftracer::utils::plugins {

int register_state_into(StateRegistry& out, const dftu_state_desc* desc,
                        void* self, const std::string& plugin_name) {
    const char* who = plugin_name.empty() ? "plugin" : plugin_name.c_str();
    if (!desc || !desc->name) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] register_state needs a named desc", who);
        return -1;
    }
    if (refuse_plugin_op_name(desc->name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] state '%s' refused: a state name must be "
            "'<plugin>.<name>' and the 'dftu.' namespace belongs to the host",
            who, desc->name);
        return -1;
    }
    if (!desc->init || !desc->update || !desc->merge || !desc->finalize ||
        !desc->destroy) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] state '%s' refused: init, update, merge, finalize and "
            "destroy are all required",
            who, desc->name);
        return -1;
    }
    // Half a pair would let the host write a run it could never read back.
    if (!desc->serialize != !desc->deserialize) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:%s] state '%s' refused: serialize and deserialize are one "
            "pair, given together or not at all",
            who, desc->name);
        return -1;
    }
    const std::string name = desc->name;
    if (std::any_of(out.begin(), out.end(),
                    [&](const RegisteredState& s) { return s.name == name; })) {
        DFTRACER_UTILS_LOG_ERROR("[plugin:%s] state '%s' is already registered",
                                 who, desc->name);
        return -1;
    }
    out.push_back(RegisteredState{name, *desc, self});
    return 0;
}

}  // namespace dftracer::utils::plugins
