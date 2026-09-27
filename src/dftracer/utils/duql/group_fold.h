#ifndef DFTRACER_UTILS_DUQL_GROUP_FOLD_H
#define DFTRACER_UTILS_DUQL_GROUP_FOLD_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::duql {

enum class FoldOp : std::uint8_t {
    ENGINE,
    COUNT_DISTINCT,
    COLLECT,
    ARGMAX,
    ARGMIN,
    SKETCH,
    MERGE,
    MERGE_QUANTILE,
};

/// One aggregate of a GroupFold; `in` and `by` index a batch's columns.
/// ENGINE runs `engine` with `param` in the dataframe aggregation. The others
/// skip a null input: COUNT_DISTINCT counts distinct values by value
/// equality; COLLECT lists the values in input order; `arg_max` and `arg_min`
/// give `in` at the row with the greatest or least `by`, the first on ties;
/// SKETCH gives the values' DDSketch and MERGE the merge of stored sketches,
/// both as base64 text; MERGE_QUANTILE the `param` quantile of that merge.
/// `type` is the output type.
struct FoldAgg {
    FoldOp op = FoldOp::ENGINE;
    dataframe::AggOp engine = dataframe::AggOp::Count;
    std::int32_t in = -1;
    std::int32_t by = -1;
    double param = 0;
    dataframe::DataType type;
};

/// A group-by over batches whose first `keys` columns are the group keys,
/// folded as they stream in input order. A null key is a key; groups keep
/// the order they first appear in. Not thread safe.
class GroupFold {
   public:
    /// `key_types` are the key columns' output types.
    GroupFold(std::vector<dataframe::DataType> key_types,
              std::vector<FoldAgg> aggs);
    ~GroupFold();

    /// Throws std::invalid_argument for a key that holds a list or object.
    void add(dataframe::DataFrame batch);

    /// One row per group: the keys, then one column per aggregate, as
    /// `names`.
    dataframe::DataFrame finish(std::vector<std::string> names);

   private:
    struct Ord;
    struct Arg;
    using Sketch = dataframe::BasicDDSketch<2048>;

    void arg(std::size_t a, const dataframe::Series& in,
             const dataframe::Series& by, const std::vector<std::int64_t>& gid);
    void sketches(std::size_t a, const dataframe::Series& in,
                  const std::vector<std::int64_t>& gid);

    std::vector<dataframe::DataType> key_types_;
    std::vector<FoldAgg> aggs_;
    ankerl::unordered_dense::map<std::string, std::int64_t> groups_;
    std::int64_t rows_ = 0;
    std::vector<std::vector<dataframe::Series>> key_parts_;
    dataframe::AggStatePtr engine_;
    std::vector<std::vector<ankerl::unordered_dense::set<std::string>>>
        distinct_;
    std::vector<std::vector<dataframe::Series>> parts_;
    std::vector<std::int64_t> part_rows_;
    std::vector<std::vector<std::vector<std::int64_t>>> members_;
    std::vector<std::vector<Arg>> args_;
    std::vector<std::vector<std::unique_ptr<Sketch>>> sketches_;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_GROUP_FOLD_H
