// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLAN_ABI_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLAN_ABI_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/providers.h>

namespace dftracer::utils::plugins::host {

/// The plan callbacks of one source or format file, as the vtables spell them
/// (`self` is the source's `self` or the format's `file`). Either may be null.
struct PlanHooks {
    void* self = nullptr;
    dftu_status (*plan)(dftu_ctx*, void*, const ::dftu_scan_request*,
                        ::dftu_plan_result*) = nullptr;
    dftu_status (*plan_duql)(dftu_ctx*, void*, const ::dftu_scan_request*,
                             const ::dftu_plan_duql*,
                             ::dftu_plan_result*) = nullptr;
    /// The registered name; the counters of the call are added under it.
    dstd::StringView owner;
    /// The callback in an error message, such as "provider plan".
    dstd::StringView what;
};

/// Asks the plugin to plan `req`. Calls plan_duql when it is set and some
/// filter of `req` is expressible in duql (`names` are the columns of the
/// source's schema, which the filters read when the projection is empty),
/// otherwise plan. None when no hook ran; a failing hook is its error, with its
/// message.
dstd::Result<dstd::Optional<dataframe::ScanPlan>> run_plan(
    const PlanHooks& hooks, const dataframe::ScanRequest& req,
    dstd::Span<const dstd::String> names);

/// `req` with the plan `planned` gave, for an explain hook; `req` as it is when
/// the plan failed or there is none.
dataframe::ScanRequest with_plan(
    const dataframe::ScanRequest& req,
    const dstd::Result<dstd::Optional<dataframe::ScanPlan>>& planned);

/// The explain lines of a plan: its votes, cost and estimated rows, or the
/// error that failed it. None for a source with no plan.
dstd::Vector<dataframe::ExplainLine> plan_lines(
    const dstd::Result<dstd::Optional<dataframe::ScanPlan>>& planned);

/// A ScanPlan as the dftu_plan_result a plugin scan reads. Valid while this
/// object and the plan are.
class AbiPlan {
   public:
    explicit AbiPlan(const dstd::Optional<dataframe::ScanPlan>& plan);
    AbiPlan(const AbiPlan&) = delete;
    AbiPlan& operator=(const AbiPlan&) = delete;

    /// Null when there is no plan.
    const ::dftu_plan_result* get() const noexcept {
        return present_ ? &result_ : nullptr;
    }

   private:
    dstd::Vector<std::int32_t> votes_;
    ::dftu_plan_result result_;
    bool present_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLAN_ABI_H
