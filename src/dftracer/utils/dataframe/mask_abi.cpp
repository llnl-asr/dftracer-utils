// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/internal/duql_handle.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;

namespace {}  // namespace

namespace dftracer::utils::dataframe {

dstd::Result<Series> DataFrame::mask(const duql::Query& q) const {
    dstd::Result<Series> out =
        evaluate_mask(duql::detail::QueryAccess::root(q), *this);
    if (!out)
        return common::make_error(
            duql::DuqlErrc::Unsupported,
            "DataFrame::mask: predicate has no columnar lowering for this "
            "frame");
    return out;
}

}  // namespace dftracer::utils::dataframe

extern "C" {

dftu_series* dftu_dataframe_mask(const dftu_duql* q,
                                 const dftu_series* const* columns,
                                 const char* const* names, int32_t n) {
    if (!q || n < 0) return nullptr;
    dataframe::DataFrame b;
    b.names.reserve(static_cast<std::size_t>(n));
    b.columns.reserve(static_cast<std::size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        b.names.emplace_back(names[i] ? names[i] : "");
        b.columns.emplace_back(const_cast<dftu_series*>(columns[i]));
    }
    dftu_series* out = nullptr;
    const auto& query = dftracer::utils::duql::duql_handle_unwrap(q);
    dstd::Result<dataframe::Series> mask =
        dftracer::utils::dataframe::evaluate_mask(
            duql::detail::QueryAccess::root(query), b);
    if (mask) out = mask->release();
    for (dataframe::Series& c : b.columns)
        c.release();  // borrowed, do not free
    return out;
}
}
