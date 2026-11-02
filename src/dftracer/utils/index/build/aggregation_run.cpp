#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>

namespace dftracer::utils::index::build {

namespace agg = index::schemas::dft::agg;

AggRun::AggRun(std::string path,
               IndexBuildBatchConfig::AggFoldFactory agg_factory)
    : index_path(std::move(path)), factory(std::move(agg_factory)) {}

AggRun start_aggregation(const std::string& index_path,
                         const agg::AggregationConfig& config) {
    auto target = common::unwrap(index::Index::open(
        index::store::internal::normalize_index_root(index_path)));
    auto intern = agg::intern_for_index(index_path);
    common::unwrap(agg_dictionary(common::unwrap(target.snapshot()), *intern));
    agg::tier::write_config(target,
                            {config.time_interval_us, config.params_hash(),
                             config.group_by_file, config.extra_group_keys});
    auto cfg = std::make_shared<agg::AggregationConfig>(config);
    auto factory = [cfg,
                    intern](dftracer::utils::dstd::StringIntern& build_intern) {
        return std::make_unique<agg::AggregationFold>(build_intern, intern,
                                                      *cfg);
    };
    return AggRun(index_path, std::move(factory));
}

void finish_aggregation(AggRun& run, IndexBuildBatchResult& batch) {
    auto target = common::unwrap(index::Index::open(
        index::store::internal::normalize_index_root(run.index_path)));
    agg::merge_aggregation_folds(batch.agg_outputs, target);
}

void clear_aggregation(const std::string& index_path) {
    DFTRACER_UTILS_LOG_INFO("Clearing the aggregation tier of %s",
                            index_path.c_str());
    auto target = common::unwrap(index::Index::open(
        index::store::internal::normalize_index_root(index_path)));
    agg::tier::clear(target);
}

}  // namespace dftracer::utils::index::build
