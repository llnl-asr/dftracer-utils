// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_GROUP_FOLD_H
#define DFTRACER_UTILS_DUQL_GROUP_FOLD_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::duql {

enum class FoldOp : std::uint8_t {
    Engine,
    CountDistinct,
    Collect,
    Argmax,
    Argmin,
    Sketch,
    Merge,
    MergeQuantile,
    Plugin,
};

/// One aggregate of a GroupFold; `in` and `by` index a batch's columns.
/// ENGINE runs `engine` with `param` in the dataframe aggregation. The others
/// skip a null input: COUNT_DISTINCT counts distinct values by value
/// equality; COLLECT lists the values in input order; `arg_max` and `arg_min`
/// give `in` at the row with the greatest or least `by`, the first on ties;
/// SKETCH gives the values' DDSketch and MERGE the merge of stored sketches,
/// both as base64 text; MERGE_QUANTILE the `param` quantile of that merge;
/// PLUGIN the registered reducer `plugin` over the group's values, `params`
/// filling its operands after the column, null for a group with none.
/// `type` is the output type.
struct FoldAgg {
    FoldOp op = FoldOp::Engine;
    dataframe::AggOp engine = dataframe::AggOp::Count;
    std::int32_t in = -1;
    std::int32_t by = -1;
    double param = 0;
    dataframe::DataType type;
    dstd::String plugin;
    dstd::Vector<double> params;
};

/// A group-by over batches whose first `keys` columns are the group keys,
/// folded as they stream in input order. A null key is a key; groups keep
/// the order they first appear in. Not thread safe.
class GroupFold {
   public:
    /// `key_types` are the key columns' output types.
    GroupFold(dstd::Vector<dataframe::DataType> key_types,
              dstd::Vector<FoldAgg> aggs);
    ~GroupFold();

    /// An InvalidArgument error for a key that holds a list or object, or the
    /// error of the engine aggregation. After an error the fold is unusable.
    dstd::Result<void> add(dataframe::DataFrame batch);

    /// One row per group: the keys, then one column per aggregate, as
    /// `names`. An InvalidArgument error when a plugin reducer is not
    /// registered or fails on a group.
    dstd::Result<dataframe::DataFrame> finish(dstd::Vector<dstd::String> names);

   private:
    struct Ord;
    struct Arg;
    using Sketch = dataframe::MergeSketch;

    void arg(std::size_t a, const dataframe::Series& in,
             const dataframe::Series& by,
             const dstd::Vector<std::int64_t>& gid);
    void sketches(std::size_t a, const dataframe::Series& in,
                  const dstd::Vector<std::int64_t>& gid);

    dstd::Vector<dataframe::DataType> key_types_;
    dstd::Vector<FoldAgg> aggs_;
    dstd::HashMap<dstd::String, std::int64_t> groups_;
    dstd::StringViewMap<std::int64_t> str_groups_;
    std::int64_t null_group_ = -1;
    std::int64_t group_count_ = 0;
    std::int64_t rows_ = 0;
    dstd::Vector<dstd::Vector<dataframe::Series>> key_parts_;
    dataframe::AggStatePtr engine_;
    struct Distinct {
        dstd::StringViewSet text;
        dstd::StringViewSet tagged;
    };
    dstd::Vector<dstd::Vector<Distinct>> distinct_;
    dstd::Vector<dstd::Vector<dataframe::Series>> parts_;
    dstd::Vector<std::int64_t> part_rows_;
    dstd::Vector<dstd::Vector<dstd::Vector<std::int64_t>>> members_;
    dstd::Vector<dstd::Vector<Arg>> args_;
    dstd::Vector<dstd::Vector<dstd::UniquePtr<Sketch>>> sketches_;
};

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_GROUP_FOLD_H
