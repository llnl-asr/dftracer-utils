// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// A plan node registered by name. LazyFrame::op stacks it on a plan as a step
/// the engine runs when the plan is driven. One instance serves every plan that
/// names it, so its methods may be called from several threads at once.
class NodeImpl {
   public:
    virtual ~NodeImpl() = default;

    /// The columns this node produces from an input of schema `in`, without
    /// running. A column whose type the node cannot determine statically is
    /// Unknown, never guessed.
    virtual dstd::Result<Schema> output_schema(const Schema& in,
                                               const OpArgs& args) const = 0;

    /// Opens the node's output cursor over `input`, whose frames carry
    /// `input_names`. On success the cursor owns `input`; on failure `input`
    /// dies with the call. `memory_budget` is the plan's budget in bytes,
    /// never 0.
    virtual dstd::Result<dstd::UniquePtr<Cursor>> open(
        dstd::UniquePtr<Cursor> input,
        const dstd::Vector<dstd::String>& input_names, const OpArgs& args,
        std::uint64_t memory_budget) const = 0;
};

/// Registers `impl` under `name`. A name already taken is refused, never
/// replaced.
dstd::Result<void> register_node(dstd::StringView name,
                                 dstd::SharedPtr<NodeImpl> impl);

/// Removes the node registered as `name`; false when there is none. Plans
/// that already hold the node keep it alive.
bool unregister_node(dstd::StringView name);

/// The node registered as `name`, or null.
dstd::SharedPtr<NodeImpl> find_node(dstd::StringView name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NODE_REGISTRY_H
