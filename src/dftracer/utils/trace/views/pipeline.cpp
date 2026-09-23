#include <dftracer/utils/trace/views/pipeline.h>

#include <stdexcept>
#include <vector>

namespace dftracer::utils::trace::views::detail {

Pipeline lower_fused_folds(const ViewPlan& plan, const ViewDefinition& vdef,
                           std::span<Fold* const> sinks) {
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

coro::CoroTask<ExportStats> execute(Pipeline pipeline,
                                    dftracer::utils::StringIntern& intern,
                                    const CoverageSet* covered,
                                    std::uint64_t limit) {
    if (!pipeline.ops.empty())
        throw std::logic_error("pipeline: streaming ops need the pull driver");
    if (pipeline.source.plan == nullptr || pipeline.source.vdef == nullptr ||
        pipeline.sinks.empty())
        throw std::logic_error("pipeline: incomplete source or no sink");

    std::vector<Fold*> folds;
    folds.reserve(pipeline.sinks.size());
    for (const PipelineNode& s : pipeline.sinks) {
        if (s.fold == nullptr)
            throw std::logic_error("pipeline: sink node carries no fold");
        folds.push_back(s.fold);
    }
    co_return co_await fuse(*pipeline.source.plan, *pipeline.source.vdef, folds,
                            intern, covered, limit);
}

}  // namespace dftracer::utils::trace::views::detail
