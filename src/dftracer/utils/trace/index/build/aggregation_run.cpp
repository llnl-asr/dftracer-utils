// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/build/aggregation_run.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

namespace dftracer::utils::trace::index::build {

AggRun::AggRun(dstd::String path,
               IndexBuildBatchConfig::AggFoldFactory agg_factory)
    : index_path(std::move(path)), factory(std::move(agg_factory)) {}

dstd::Result<AggRun> start_aggregation(
    dstd::StringView index_path,
    const trace::index::schemas::dft::agg::AggregationConfig& config) {
    auto target = trace::index::Index::open(
        trace::index::normalize_index_root(index_path));
    if (!target) return target.error();
    auto intern = trace::index::schemas::dft::agg::intern_for_index(index_path);
    auto snapshot = target->snapshot();
    if (!snapshot) return snapshot.error();
    auto dictionary = agg_dictionary(*snapshot, *intern);
    if (!dictionary) return dictionary.error();
    auto written = trace::index::schemas::dft::agg::tier::write_config(
        *target, {config.time_interval_us, config.params_hash(),
                  config.group_by_file, config.extra_group_keys});
    if (!written) return written.error();
    auto cfg =
        dstd::make_shared<trace::index::schemas::dft::agg::AggregationConfig>(
            config);
    auto factory = [cfg,
                    intern](dftracer::utils::dstd::StringIntern& build_intern) {
        return dstd::make_unique<
            trace::index::schemas::dft::agg::AggregationFold>(build_intern,
                                                              intern, *cfg);
    };
    return AggRun(dstd::String(index_path), std::move(factory));
}

dstd::Result<void> finish_aggregation(AggRun& run,
                                      IndexBuildBatchResult& batch) {
    auto target = trace::index::Index::open(
        trace::index::normalize_index_root(run.index_path));
    if (!target) return target.error();
    return trace::index::schemas::dft::agg::merge_aggregation_folds(
        batch.agg_outputs, *target);
}

dstd::Result<void> clear_aggregation(const dstd::String& index_path) {
    DFTRACER_UTILS_LOG_INFO("Clearing the aggregation tier of {}", index_path);
    auto target = trace::index::Index::open(
        trace::index::normalize_index_root(index_path));
    if (!target) return target.error();
    return trace::index::schemas::dft::agg::tier::clear(*target);
}

}  // namespace dftracer::utils::trace::index::build
