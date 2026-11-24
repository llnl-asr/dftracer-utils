// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_TABLE_ROWS_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_TABLE_ROWS_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::index::build {

/// What the index pass keeps of a dftu.table header so it can tell, row by
/// row, that the file is complete: the declared field names and the row count
/// the header promises.
class TableShape {
   public:
    /// The shape of the table whose first line is `line`, or null when the
    /// line is not a dftu.table header.
    static dstd::SharedPtr<const TableShape> from_header_line(
        dstd::StringView line);

    std::int64_t nrows() const { return nrows_; }

   private:
    friend class TableRowCheck;
    std::int64_t nrows_ = 0;
    dstd::StringViewMap<std::size_t> field_index_;
};

/// Counts the rows of one piece of lines that are whole: `ph` 3 with an
/// `args` object that names every declared field (a null value counts). A
/// check belongs to one thread.
class TableRowCheck {
   public:
    explicit TableRowCheck(const TableShape& shape);

    void row(simdjson::dom::element root);
    void row(json::Value& root);

    std::size_t complete_rows() const { return complete_; }

   private:
    void begin_row();
    void see(dstd::StringView name);
    void end_row();

    const TableShape* shape_;
    dstd::Vector<std::uint32_t> seen_;
    std::uint32_t stamp_ = 0;
    std::size_t matched_ = 0;
    std::size_t complete_ = 0;
};

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_TABLE_ROWS_H
