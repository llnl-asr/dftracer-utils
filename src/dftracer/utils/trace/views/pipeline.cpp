// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/pipeline.h>

namespace dftracer::utils::trace::views::detail {

Pipeline lower_fused_folds(const ViewPlan& plan, const ViewDefinition& vdef,
                           dstd::Span<Fold* const> sinks) {
    Pipeline p;
    p.source.kind = NodeKind::Source;
    p.source.plan = &plan;
    p.source.vdef = &vdef;
    p.sinks.reserve(sinks.size());
    for (Fold* f : sinks) {
        PipelineNode n;
        n.kind = NodeKind::Sink;
        n.fold = f;
        p.sinks.push_back(n);
    }
    return p;
}

coro::CoroTask<dstd::Result<ExportStats>> execute(
    Pipeline pipeline, dftracer::utils::dstd::StringIntern& intern,
    const CoverageSet* covered, std::uint64_t limit) {
    if (!pipeline.ops.empty())
        dstd::fatal("pipeline: streaming ops need the pull driver");
    if (pipeline.source.plan == nullptr || pipeline.source.vdef == nullptr ||
        pipeline.sinks.empty())
        dstd::fatal("pipeline: incomplete source or no sink");

    dstd::Vector<Fold*> folds;
    folds.reserve(pipeline.sinks.size());
    for (const PipelineNode& s : pipeline.sinks) {
        if (s.fold == nullptr)
            dstd::fatal("pipeline: sink node carries no fold");
        folds.push_back(s.fold);
    }
    auto fused = fuse(*pipeline.source.plan, *pipeline.source.vdef, folds,
                      intern, covered, limit);
    co_return co_await std::move(fused);
}

}  // namespace dftracer::utils::trace::views::detail
