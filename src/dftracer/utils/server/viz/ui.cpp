#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/json_builder.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/viz_ui.h>
#include <simdjson.h>

#include <cctype>
#include <string>

namespace dftracer::utils::server {

static coro::CoroTask<HttpResponse> handle_index() {
    co_return HttpResponse::ok(std::string(viz_index_html()),
                               "text/html; charset=utf-8");
}

namespace {

// OpenAPI 3.1 generated from the routes' RouteDoc metadata, serialized with
// simdjson's builder (correct escaping; examples are inlined raw as they are
// already valid JSON).
std::string openapi_json(const Router& router) {
    auto& b = scratch_json_builder();
    b.start_object();
    b.append_key_value("openapi", "3.1.0");
    b.append_comma();
    b.escape_and_append_with_quotes("info");
    b.append_colon();
    b.start_object();
    b.append_key_value("title", "DFTracer Server API");
    b.append_comma();
    b.append_key_value("version", "1");
    b.append_comma();
    b.append_key_value("description", "Query and visualize DFTracer traces.");
    b.end_object();
    b.append_comma();
    b.escape_and_append_with_quotes("servers");
    b.append_colon();
    b.start_array();
    b.start_object();
    b.append_key_value("url", "/");
    b.end_object();
    b.end_array();
    b.append_comma();
    b.escape_and_append_with_quotes("paths");
    b.append_colon();
    b.start_object();
    bool first = true;
    for (const auto& r : router.routes()) {
        if (r.doc.summary.empty()) continue;
        if (!first) b.append_comma();
        first = false;
        b.escape_and_append_with_quotes(r.path);
        b.append_colon();
        b.start_object();
        std::string method;
        for (char c : r.method)
            method +=
                static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        b.escape_and_append_with_quotes(method);
        b.append_colon();
        b.start_object();
        b.escape_and_append_with_quotes("tags");
        b.append_colon();
        b.start_array();
        b.escape_and_append_with_quotes(r.doc.tag);
        b.end_array();
        b.append_comma();
        b.append_key_value("summary", r.doc.summary);
        b.append_comma();
        b.escape_and_append_with_quotes("parameters");
        b.append_colon();
        b.start_array();
        bool first_param = true;
        for (const auto& p : r.doc.params) {
            if (!first_param) b.append_comma();
            first_param = false;
            b.start_object();
            b.append_key_value("name", p.name);
            b.append_comma();
            b.append_key_value("in", "query");
            b.append_comma();
            b.escape_and_append_with_quotes("required");
            b.append_colon();
            b.append(p.required);
            if (!p.desc.empty()) {
                b.append_comma();
                b.append_key_value("description", p.desc);
            }
            b.append_comma();
            b.escape_and_append_with_quotes("schema");
            b.append_colon();
            b.start_object();
            b.append_key_value("type", "string");
            if (!p.example.empty()) {
                b.append_comma();
                b.append_key_value("default", p.example);
            }
            b.end_object();
            b.end_object();
        }
        b.end_array();
        b.append_comma();
        b.escape_and_append_with_quotes("responses");
        b.append_colon();
        b.start_object();
        b.escape_and_append_with_quotes("200");
        b.append_colon();
        b.start_object();
        b.append_key_value("description", "OK");
        if (!r.doc.response_example.empty()) {
            b.append_comma();
            b.escape_and_append_with_quotes("content");
            b.append_colon();
            b.start_object();
            b.escape_and_append_with_quotes("application/json");
            b.append_colon();
            b.start_object();
            b.escape_and_append_with_quotes("example");
            b.append_colon();
            b.append_raw(r.doc.response_example);
            b.end_object();
            b.end_object();
        }
        b.end_object();  // 200
        b.end_object();  // responses
        b.end_object();  // method
        b.end_object();  // path
    }
    b.end_object();      // paths
    b.end_object();      // root
    return std::string(b);
}

}  // namespace

void register_viz_ui(Router& router) {
    RouteHandler serve_index = [](const HttpRequest&, const QueryParams&)
        -> coro::CoroTask<HttpResponse> { co_return co_await handle_index(); };
    router.get("/", serve_index);
    router.get("/index.html", serve_index);

    router.get(
        "/api",
        [](const HttpRequest&,
           const QueryParams&) -> coro::CoroTask<HttpResponse> {
            co_return HttpResponse::ok(std::string(viz_api_page_html()),
                                       "text/html; charset=utf-8");
        },
        RouteDoc{"Interactive API explorer (web page).", "Meta", {}, ""});

    Router* r = &router;
    router.get(
        "/api/openapi.json",
        [r](const HttpRequest&,
            const QueryParams&) -> coro::CoroTask<HttpResponse> {
            co_return HttpResponse::ok(openapi_json(*r), "application/json");
        },
        RouteDoc{"OpenAPI 3.1 specification for this server.", "Meta", {}, ""});
}

}  // namespace dftracer::utils::server
