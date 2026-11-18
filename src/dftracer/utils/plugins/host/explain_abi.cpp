// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/plugins/host/explain_abi.h>

namespace dftracer::utils::plugins::host {

ExplainLines::ExplainLines() noexcept
    : out_{sizeof(::dftu_explain_out), this, &ExplainLines::add} {}

void ExplainLines::add(void* self, const char* key, std::uint64_t key_len,
                       const char* value, std::uint64_t value_len) {
    if (key == nullptr || value == nullptr) return;
    auto* lines = static_cast<ExplainLines*>(self);
    lines->lines_.push_back(dataframe::ExplainLine{
        dstd::String(key, static_cast<std::size_t>(key_len)),
        dstd::String(value, static_cast<std::size_t>(value_len))});
}

AbiScanRequest::AbiScanRequest(const dataframe::ScanRequest& req) : request_{} {
    projection_.reserve(req.projection.size());
    for (const dstd::String& name : req.projection)
        projection_.push_back(name.c_str());
    owned_.reserve(req.filters.size());
    filters_.reserve(req.filters.size());
    for (const dataframe::Expr& f : req.filters) {
        ::dftu_expr* h = dataframe::expr_handle_wrap(f);
        owned_.push_back(h);
        filters_.push_back(h);
    }
    request_.projection = projection_.empty() ? nullptr : projection_.data();
    request_.n_projection = static_cast<std::int32_t>(projection_.size());
    request_.filters = filters_.empty() ? nullptr : filters_.data();
    request_.n_filters = static_cast<std::int32_t>(filters_.size());
    request_.limit = req.limit;
    request_.memory_budget = req.memory_budget;
}

AbiScanRequest::~AbiScanRequest() {
    for (::dftu_expr* h : owned_) ::dftu_expr_free(h);
}

}  // namespace dftracer::utils::plugins::host
