#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>

#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

bool AggregationMergeOperator::FullMergeV2(
    const MergeOperationInput& merge_in,
    MergeOperationOutput* merge_out) const {
    AggregationMetrics result;

    if (merge_in.existing_value) {
        try {
            result = deserialize_agg_value(
                std::string_view(merge_in.existing_value->data(),
                                 merge_in.existing_value->size()));
        } catch (...) {
            return false;
        }
    }

    for (const auto& operand : merge_in.operand_list) {
        try {
            auto other = deserialize_agg_value(
                std::string_view(operand.data(), operand.size()));
            result.merge_from(other);
        } catch (...) {
            return false;
        }
    }

    merge_out->new_value = serialize_agg_value(result);
    return true;
}

bool AggregationMergeOperator::PartialMerge(
    const ::rocksdb::Slice& /*key*/, const ::rocksdb::Slice& left_operand,
    const ::rocksdb::Slice& right_operand, std::string* new_value,
    ::rocksdb::Logger* /*logger*/) const {
    try {
        auto left = deserialize_agg_value(
            std::string_view(left_operand.data(), left_operand.size()));
        auto right = deserialize_agg_value(
            std::string_view(right_operand.data(), right_operand.size()));
        left.merge_from(right);
        *new_value = serialize_agg_value(left);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
