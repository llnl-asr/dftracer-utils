#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/merge_operator_common.h>

namespace dftracer::utils::utilities::composites::dft::aggregators {

bool AggregationMergeOperator::FullMergeV2(
    const MergeOperationInput& merge_in,
    MergeOperationOutput* merge_out) const {
    return full_merge_metrics<AggregationMetrics>(
        merge_in, merge_out, deserialize_agg_value, serialize_agg_value);
}

bool AggregationMergeOperator::PartialMerge(
    const ::rocksdb::Slice& /*key*/, const ::rocksdb::Slice& left_operand,
    const ::rocksdb::Slice& right_operand, std::string* new_value,
    ::rocksdb::Logger* /*logger*/) const {
    return partial_merge_metrics<AggregationMetrics>(
        left_operand, right_operand, new_value, deserialize_agg_value,
        serialize_agg_value);
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
