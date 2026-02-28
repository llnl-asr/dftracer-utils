#include <dftracer/utils/server/http_response.h>

#include <algorithm>
#include <cctype>

namespace dftracer::utils::server {

namespace {

bool icase_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
}

bool has_content_length(
    const std::vector<std::pair<std::string, std::string>> &headers) {
    return std::any_of(headers.begin(), headers.end(), [](const auto &h) {
        return icase_equal(h.first, "Content-Length");
    });
}

}  // namespace

std::string HttpResponse::serialize_headers() const {
    std::string out;
    out.reserve(256);

    out += "HTTP/1.1 ";
    out += std::to_string(status_code);
    out += ' ';
    out += status_text;
    out += "\r\n";

    for (const auto &[name, value] : headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }

    if (!body.empty() && !has_content_length(headers)) {
        out += "Content-Length: ";
        out += std::to_string(body.size());
        out += "\r\n";
    }

    out += "\r\n";
    return out;
}

std::string HttpResponse::serialize() const {
    auto out = serialize_headers();
    out += body;
    return out;
}

HttpResponse HttpResponse::ok() {
    return HttpResponse{
        .status_code = 200, .status_text = "OK", .headers = {}, .body = {}};
}

HttpResponse HttpResponse::ok(const std::string &body,
                              const std::string &content_type) {
    return HttpResponse{.status_code = 200,
                        .status_text = "OK",
                        .headers = {{"Content-Type", content_type}},
                        .body = body};
}

HttpResponse HttpResponse::not_found() {
    return HttpResponse{.status_code = 404,
                        .status_text = "Not Found",
                        .headers = {{"Content-Type", "text/plain"}},
                        .body = "Not Found"};
}

HttpResponse HttpResponse::bad_request(const std::string &msg) {
    return HttpResponse{.status_code = 400,
                        .status_text = "Bad Request",
                        .headers = {{"Content-Type", "text/plain"}},
                        .body = msg};
}

HttpResponse HttpResponse::internal_error(const std::string &msg) {
    return HttpResponse{.status_code = 500,
                        .status_text = "Internal Server Error",
                        .headers = {{"Content-Type", "text/plain"}},
                        .body = msg};
}

}  // namespace dftracer::utils::server
