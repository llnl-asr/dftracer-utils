#ifndef DFTRACER_UTILS_PYTHON_SCHEMA_RECONCILE_H
#define DFTRACER_UTILS_PYTHON_SCHEMA_RECONCILE_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <nanoarrow/nanoarrow.h>

#include <nanoarrow/nanoarrow.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::python {

// Build a union schema over batches from producers that each emit a subset
// of columns; after finalize(), surprise columns are JSON-encoded into
// _extra so no data is lost and the stream schema stays stable.
class SchemaReconciler {
   public:
    static constexpr const char *EXTRA_COLUMN_NAME = "_extra";

    SchemaReconciler();

    bool merge(const ArrowSchema *incoming);
    int finalize();
    int copy_schema(ArrowSchema *out) const;
    int reconcile(const ArrowSchema *in_schema, ArrowArray *in_array,
                  ArrowArray *out) const;

    bool finalized() const { return finalized_; }
    int64_t num_known_columns() const {
        return static_cast<int64_t>(child_schemas_.size());
    }
    const std::string &last_error() const { return last_error_; }

   private:
    std::vector<std::string> names_;
    std::vector<nanoarrow::UniqueSchema> child_schemas_;
    std::unordered_map<std::string, int64_t> name_to_idx_;
    nanoarrow::UniqueSchema locked_schema_;
    bool finalized_ = false;
    mutable std::string last_error_;
};

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_PYTHON_SCHEMA_RECONCILE_H
