// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PORT_PLAN_H
#define DFTRACER_UTILS_PLUGINS_HOST_PORT_PLAN_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

/// One channel end a plugin factory declared. `name` is the full name,
/// `plugin.<owner>.<logical>`; `is_ref` is set when the declaring plugin is not
/// the owner.
struct ChannelDecl {
    dstd::String name;
    dstd::String declarer;
    bool is_ref = false;
    bool task_end = false;
    bool sends = false;
    bool receives = false;
    std::uint32_t credits = 0;
    std::int32_t overflow = 0;
    std::uint32_t max_msg_bytes = 0;
};

/// One generation of a phase result a plugin factory declared.
struct PhaseDecl {
    dstd::String name;
    std::uint64_t generation = 0;
    std::int32_t op = 0;
    std::int32_t type = 0;
    /// A dftu_phase_seed the host fills the generation with before the scan,
    /// or 0 when plugins contribute to it.
    std::int32_t seed = 0;
};

/// What one plugin factory declared through dftu_svc_ports.
struct PortDecls {
    dstd::Vector<ChannelDecl> channels;
    dstd::Vector<PhaseDecl> phases;

    bool empty() const { return channels.empty() && phases.empty(); }
};

/// The full name of the logical name `name` as plugin `plugin` uses it: a bare
/// name gets the `plugin.<plugin>.` prefix, and a name that starts with
/// `plugin.` is already full. A name must hold only [a-z0-9._/-] and stay out
/// of the host's `dftu.` namespace. `*is_own` tells whether the full name is
/// in the namespace of `plugin`.
dstd::Result<dstd::String> qualify_port_name(dstd::StringView plugin,
                                             dstd::StringView name,
                                             bool* is_own);

/// The merge a dftu_phase_seed takes (a dftu_merge_op), or -1 for a value that
/// is not a seed.
std::int32_t phase_seed_merge(std::int32_t seed);

/// The name of a dftu_phase_seed for a message.
dstd::StringView phase_seed_name(std::int32_t seed);

/// Whether the merge `op` (a dftu_merge_op) takes the value type `type` (a
/// dftu_type).
bool phase_merge_accepts(std::int32_t op, std::int32_t type);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PORT_PLAN_H
