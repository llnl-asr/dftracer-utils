// SPDX-License-Identifier: MIT
// GET /api/prov/graph -- the provenance graph of the loaded trace, built by
// domains::analysis::provenance::extract_provenance_graph over the server's
// indexed file set (bloom-cache chunk skipping, max_concurrent worker slots).
// Honors the same file-scoping parameters as the viz endpoints (?file=, ?pid=).
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/domains/analysis/provenance/provenance_graph.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz/handlers.h>
#include <dftracer/utils/domains/server/viz/internal.h>
#include <dftracer/utils/domains/server/viz/scan.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/views/view.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::domains::server {

coro::CoroTask<HttpResponse> handle_prov_graph(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index) {
    auto files = collect_candidate_files(index, params);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    trace::views::View v =
        trace::views::View::from_files(to_view_files(files))
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    domains::analysis::provenance::ProvenanceOptions opts;
    opts.include_io = params.get_int("io", 1) != 0;
    opts.include_all_files = params.get_int("all_files", 0) != 0;
    dstd::StringView ms = params.get("mounts");  // comma-separated prefixes
    while (!ms.empty()) {
        auto c = ms.find(',');
        auto tok = ms.substr(0, c);
        if (!tok.empty()) opts.mounts.emplace_back(tok);
        if (c == dstd::StringView::npos) break;
        ms.remove_prefix(c + 1);
    }
    auto graph =
        co_await domains::analysis::provenance::extract_provenance_graph(
            v, slots, std::move(opts));
    if (!graph) co_return HttpResponse::from_error(graph.error());
    co_return HttpResponse::ok(graph->to_json());
}

}  // namespace dftracer::utils::domains::server
