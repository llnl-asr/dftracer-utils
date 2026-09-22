#ifndef DFTRACER_UTILS_TRACE_VIEWS_PIPELINE_H
#define DFTRACER_UTILS_TRACE_VIEWS_PIPELINE_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace dftracer::utils::trace::views::detail {

enum class NodeKind { Source, StreamOp, Reduce, Sink };

/// One node of a lowered pipeline. A node carries only what its kind needs: a
/// Source borrows the scan plan and definition, a Reduce or Sink borrows the
/// fold it drives. Every pointer is borrowed and must outlive `execute`.
struct PipelineNode {
    NodeKind kind = NodeKind::Source;
    const ViewPlan* plan = nullptr;
    const ViewDefinition* vdef = nullptr;
    Fold* fold = nullptr;
};

/// A linear pipeline: one scan source, zero or more streaming ops, and the
/// sinks it feeds. Breakers split pipelines; a Pipeline is one fuse pass or one
/// driver run.
struct Pipeline {
    PipelineNode source;
    std::vector<PipelineNode> ops;
    std::vector<PipelineNode> sinks;
};

/// Lowering (a): the scan as the Source with every fold in `sinks` fused into
/// one pass, no streaming ops. Executing it is exactly
/// `fuse(plan, vdef, sinks, ...)`. `sinks` is borrowed, not copied.
Pipeline lower_fused_folds(const ViewPlan& plan, const ViewDefinition& vdef,
                           std::span<Fold* const> sinks);

/// Run a lowered pipeline. A Source feeding fold Sinks with no streaming ops is
/// lowering (a): one direct `fuse` call. Throws std::logic_error on a shape no
/// lowering handles yet, rather than silently dropping nodes. Taken by value:
/// a coroutine holding a reference to a caller's temporary plan dangles as soon
/// as the call is not itself the awaited full-expression.
coro::CoroTask<ExportStats> execute(Pipeline pipeline,
                                    dftracer::utils::StringIntern& intern,
                                    const CoverageSet* covered = nullptr,
                                    std::uint64_t limit = 0);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_PIPELINE_H
