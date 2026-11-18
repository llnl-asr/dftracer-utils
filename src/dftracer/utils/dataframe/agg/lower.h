// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_AGG_LOWER_H
#define DFTRACER_UTILS_DATAFRAME_AGG_LOWER_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::dataframe {

/// Whether `specs` hold a UDAF that must not run on two threads at once (one
/// without DFTU_AGG_PARALLEL_SAFE, or one that is not registered): no callback
/// of it, bind and deserialize included. A parallel driver then leaves every
/// state of those specs to one thread at a time.
bool agg_specs_serial(dstd::Span<const AggSpec> specs);

/// The engine spec of `a`. `resolve` maps a column name to its index among the
/// value columns the engine reads, and returns that index or a Result of it;
/// the lowering fails only with the error of a `resolve` that fails. A Udaf
/// takes `a.column` and then `a.args` as its arguments; its name is not looked
/// up here, the state reports an unknown name at the first accumulate.
template <class Resolve>
dstd::Result<AggSpec> lower_group_agg(const GroupAgg& a, Resolve&& resolve) {
    auto index = [&](dstd::StringView name) -> dstd::Result<std::int32_t> {
        return resolve(name);
    };
    AggSpec sp;
    sp.op = to_agg_op(a.op);
    sp.out = a.out;
    sp.param = a.param;
    sp.interp = a.interp;
    if (sp.op == AggOp::Udaf) {
        sp.fn = a.fn;
        if (!a.column.empty()) {
            const dstd::Result<std::int32_t> value = index(a.column);
            if (!value) return value.error();
            sp.value_col = *value;
        }
        for (const dstd::String& arg : a.args) {
            const dstd::Result<std::int32_t> at = index(arg);
            if (!at) return at.error();
            sp.args.push_back(*at);
        }
    } else if (sp.op != AggOp::Count) {
        const dstd::Result<std::int32_t> value = index(a.column);
        if (!value) return value.error();
        sp.value_col = *value;
    }
    if (agg_uses_by_col(sp.op)) {
        const dstd::Result<std::int32_t> by = index(a.by);
        if (!by) return by.error();
        sp.by_col = *by;
    }
    return sp;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_LOWER_H
