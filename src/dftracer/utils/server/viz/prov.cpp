// GET /api/prov/graph -- the provenance graph of the loaded trace, built by
// trace::provenance::extract_provenance_graph over the server's indexed
// file set (bloom-cache chunk skipping, max_concurrent worker slots).
// Honors the same file-scoping parameters as the viz endpoints (?file=, ?pid=).
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz/handlers.h>
#include <dftracer/utils/server/viz/internal.h>
#include <dftracer/utils/server/viz/scan.h>
#include <dftracer/utils/trace/provenance/provenance_graph.h>
#include <dftracer/utils/trace/views/view.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::server {

coro::CoroTask<HttpResponse> handle_prov_graph(const HttpRequest& req,
                                               const QueryParams& params,
                                               TraceIndex& index) {
    auto files = collect_candidate_files(index, params);
    std::size_t slots = std::max<std::size_t>(1, index.max_concurrent());
    trace::views::View v =
        trace::views::View::from_files(to_view_files(files))
            .cancel_when([&req]() { return req.cancel_token.cancelled(); });
    trace::provenance::ProvenanceOptions opts;
    opts.include_io = params.get_int("io", 1) != 0;
    opts.include_all_files = params.get_int("all_files", 0) != 0;
    std::string_view ms = params.get("mounts");  // comma-separated prefixes
    while (!ms.empty()) {
        auto c = ms.find(',');
        auto tok = ms.substr(0, c);
        if (!tok.empty()) opts.mounts.emplace_back(tok);
        if (c == std::string_view::npos) break;
        ms.remove_prefix(c + 1);
    }
    auto graph = co_await trace::provenance::extract_provenance_graph(
        v, slots, std::move(opts));
    co_return HttpResponse::ok(graph.to_json());
}

}  // namespace dftracer::utils::server
