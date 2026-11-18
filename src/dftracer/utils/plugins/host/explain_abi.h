// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_EXPLAIN_ABI_H
#define DFTRACER_UTILS_PLUGINS_HOST_EXPLAIN_ABI_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/host/plan_abi.h>

namespace dftracer::utils::plugins::host {

/// The lines one plugin explain callback adds, in the order it added them.
/// Hand out out() as the callback's `dftu_explain_out`; it is valid while this
/// object is.
class ExplainLines {
   public:
    ExplainLines() noexcept;
    ExplainLines(const ExplainLines&) = delete;
    ExplainLines& operator=(const ExplainLines&) = delete;

    const ::dftu_explain_out* out() const noexcept { return &out_; }

    /// Moves the lines out.
    dstd::Vector<dataframe::ExplainLine> take() noexcept {
        return std::move(lines_);
    }

   private:
    static void add(void* self, const char* key, std::uint64_t key_len,
                    const char* value, std::uint64_t value_len);

    ::dftu_explain_out out_;
    dstd::Vector<dataframe::ExplainLine> lines_;
};

/// A ScanRequest as the dftu_scan_request a plugin explain or plan callback
/// reads, with ScanRequest::plan as dftu_scan_request::plan. The request is
/// valid while this object and the ScanRequest are.
class AbiScanRequest {
   public:
    explicit AbiScanRequest(const dataframe::ScanRequest& req);
    ~AbiScanRequest();
    AbiScanRequest(const AbiScanRequest&) = delete;
    AbiScanRequest& operator=(const AbiScanRequest&) = delete;

    const ::dftu_scan_request* get() const noexcept { return &request_; }

   private:
    dstd::Vector<const char*> projection_;
    dstd::Vector<::dftu_expr*> owned_;
    dstd::Vector<const ::dftu_expr*> filters_;
    AbiPlan plan_;
    ::dftu_scan_request request_;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_EXPLAIN_ABI_H
