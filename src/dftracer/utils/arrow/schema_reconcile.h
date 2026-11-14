// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_SCHEMA_RECONCILE_H
#define DFTRACER_UTILS_ARROW_SCHEMA_RECONCILE_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <nanoarrow/nanoarrow.hpp>

namespace dftracer::utils::arrow {

/// The union schema of batches from producers that each emit a subset of the
/// columns: @ref merge takes one batch schema per batch, @ref finalize locks
/// the union, and @ref reconcile reshapes every later batch into it, so the
/// stream schema stays fixed. A column the locked schema lacks, and a column
/// whose type differs from the locked one, is JSON-encoded into
/// @ref EXTRA_COLUMN_NAME, one object per row, so no data is lost.
///
/// Not thread safe.
class SchemaReconciler {
   public:
    static constexpr const char* EXTRA_COLUMN_NAME = "_extra";

    SchemaReconciler();

    /// Adds the columns of `incoming` the union lacks; false at the end of
    /// the merge or for a schema it cannot read.
    bool merge(const ArrowSchema* incoming);
    /// Locks the union: from here @ref copy_schema and @ref reconcile work.
    /// 0 on success, -1 with @ref last_error set.
    int finalize();
    /// A deep copy of the locked schema into `out`, which the caller releases.
    int copy_schema(ArrowSchema* out) const;
    /// `in_array`, described by `in_schema`, reshaped into the locked schema as
    /// `out`, which the caller releases. 0 on success, -1 with
    /// @ref last_error set.
    int reconcile(const ArrowSchema* in_schema, ArrowArray* in_array,
                  ArrowArray* out) const;

    bool finalized() const { return finalized_; }
    /// The columns of the union, without the trailing extra column.
    std::int64_t known_column_count() const {
        return static_cast<std::int64_t>(child_schemas_.size());
    }
    const dstd::String& last_error() const { return last_error_; }

   private:
    dstd::Vector<dstd::String> names_;
    dstd::Vector<nanoarrow::UniqueSchema> child_schemas_;
    dstd::HashMap<dstd::String, std::int64_t> name_to_idx_;
    nanoarrow::UniqueSchema locked_schema_;
    bool finalized_ = false;
    mutable dstd::String last_error_;
};

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_SCHEMA_RECONCILE_H
