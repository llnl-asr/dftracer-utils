#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MERGE_OPERATOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MERGE_OPERATOR_H

#include <rocksdb/merge_operator.h>

#include <string>

namespace dftracer::utils::utilities::composites::dft::aggregators {

class AggregationMergeOperator : public ::rocksdb::MergeOperator {
   public:
    bool FullMergeV2(const MergeOperationInput& merge_in,
                     MergeOperationOutput* merge_out) const override;

    bool PartialMerge(const ::rocksdb::Slice& key,
                      const ::rocksdb::Slice& left_operand,
                      const ::rocksdb::Slice& right_operand,
                      std::string* new_value,
                      ::rocksdb::Logger* logger) const override;

    const char* Name() const override { return "AggregationMergeOperator"; }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_MERGE_OPERATOR_H
