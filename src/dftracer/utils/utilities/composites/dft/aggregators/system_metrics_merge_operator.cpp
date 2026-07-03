#include <dftracer/utils/utilities/composites/dft/aggregators/merge_operator_common.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

bool SystemMetricsMergeOperator::FullMergeV2(
    const MergeOperationInput& merge_in,
    MergeOperationOutput* merge_out) const {
    return full_merge_metrics<SystemAggregationMetrics>(
        merge_in, merge_out, deserialize_system_value, serialize_system_value);
}

bool SystemMetricsMergeOperator::PartialMerge(
    const ::rocksdb::Slice& /*key*/, const ::rocksdb::Slice& left_operand,
    const ::rocksdb::Slice& right_operand, std::string* new_value,
    ::rocksdb::Logger* /*logger*/) const {
    return partial_merge_metrics<SystemAggregationMetrics>(
        left_operand, right_operand, new_value, deserialize_system_value,
        serialize_system_value);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
