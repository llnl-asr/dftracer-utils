#ifndef DFTRACER_UTILS_TRACE_AGGREGATORS_MERGE_OPERATOR_COMMON_H
#define DFTRACER_UTILS_TRACE_AGGREGATORS_MERGE_OPERATOR_COMMON_H

#include <rocksdb/merge_operator.h>

#include <string>
#include <string_view>

namespace dftracer::utils::trace::aggregators {

// Shared bodies for the RocksDB merge operators; the aggregation and
// system-metrics operators differ only in Metrics type and (de)serialize
// functions. `Metrics` must provide merge_from(const Metrics&). A
// deserialization failure aborts the merge (returns false).

template <typename Metrics, typename Deserialize, typename Serialize>
bool full_merge_metrics(
    const ::rocksdb::MergeOperator::MergeOperationInput& merge_in,
    ::rocksdb::MergeOperator::MergeOperationOutput* merge_out,
    Deserialize&& deserialize, Serialize&& serialize) {
    Metrics result;

    if (merge_in.existing_value) {
        try {
            result =
                deserialize(std::string_view(merge_in.existing_value->data(),
                                             merge_in.existing_value->size()));
        } catch (...) {
            return false;
        }
    }

    for (const auto& operand : merge_in.operand_list) {
        try {
            result.merge_from(
                deserialize(std::string_view(operand.data(), operand.size())));
        } catch (...) {
            return false;
        }
    }

    merge_out->new_value = serialize(result);
    return true;
}

template <typename Metrics, typename Deserialize, typename Serialize>
bool partial_merge_metrics(const ::rocksdb::Slice& left_operand,
                           const ::rocksdb::Slice& right_operand,
                           std::string* new_value, Deserialize&& deserialize,
                           Serialize&& serialize) {
    try {
        auto left = deserialize(
            std::string_view(left_operand.data(), left_operand.size()));
        auto right = deserialize(
            std::string_view(right_operand.data(), right_operand.size()));
        left.merge_from(right);
        *new_value = serialize(left);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace dftracer::utils::trace::aggregators

#endif  // DFTRACER_UTILS_TRACE_AGGREGATORS_MERGE_OPERATOR_COMMON_H
