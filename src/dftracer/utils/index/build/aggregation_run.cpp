#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>

namespace dftracer::utils::index::build {

namespace agg = index::schemas::dft::agg;

AggRun start_aggregation(const std::string& index_path,
                         const agg::AggregationConfig& config) {
    AggRun run;
    run.db = std::make_shared<index::store::IndexDatabase>(
        index_path, index::store::IndexOpenMode::ReadWrite);
    auto intern = agg::intern_for_index(index_path);
    agg::tier::load_dictionary(*run.db, *intern);
    agg::tier::write_config(*run.db,
                            {config.time_interval_us, config.params_hash(),
                             config.group_by_file, config.extra_group_keys});
    auto cfg = std::make_shared<agg::AggregationConfig>(config);
    run.factory = [cfg, intern](
                      dftracer::utils::dstd::StringIntern& build_intern,
                      int file_id) {
        return std::make_unique<agg::AggregationFold>(build_intern, intern,
                                                      *cfg, file_id);
    };
    return run;
}

void finish_aggregation(AggRun& run, IndexBuildBatchResult& batch) {
    agg::merge_aggregation_folds(batch.agg_outputs, *run.db);
}

void clear_aggregation(const std::string& index_path) {
    DFTRACER_UTILS_LOG_INFO("Clearing the aggregation tier of %s",
                            index_path.c_str());
    index::store::IndexDatabase db(index_path,
                                   index::store::IndexOpenMode::ReadWrite);
    agg::tier::clear(db);
}

}  // namespace dftracer::utils::index::build
