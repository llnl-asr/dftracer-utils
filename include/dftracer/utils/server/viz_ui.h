#ifndef DFTRACER_UTILS_SERVER_VIZ_UI_H
#define DFTRACER_UTILS_SERVER_VIZ_UI_H

#include <string_view>

namespace dftracer::utils::server {

class Router;

/// Return the embedded single-page trace viewer. The body is generated at build
/// time from web/dist/index.html by cmake/scripts/embed_asset.cmake.
std::string_view viz_index_html();

/// Return the embedded API explorer page, generated from web/dist/api.html.
std::string_view viz_api_page_html();

/// Register the UI + API-docs routes: the SPA at GET / and /index.html, the
/// API explorer page at GET /api, and the OpenAPI spec at GET
/// /api/openapi.json.
void register_viz_ui(Router& router);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_UI_H
