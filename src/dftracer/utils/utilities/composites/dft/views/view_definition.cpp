#include <dftracer/utils/utilities/common/json/json_escape.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <simdjson.h>

#include <cstdlib>
#include <sstream>
#include <string>

namespace dftracer::utils::utilities::composites::dft::views {

using common::json::escape_json_string;

ViewDefinition& ViewDefinition::with_name(const std::string& n) {
    name = n;
    return *this;
}

ViewDefinition& ViewDefinition::with_description(const std::string& d) {
    description = d;
    return *this;
}

ViewDefinition& ViewDefinition::with_query(const std::string& query_str) {
    if (!query_str.empty()) {
        auto result = Query::from_string(query_str);
        if (result) query = std::move(*result);
    }
    return *this;
}

ViewDefinition& ViewDefinition::with_query(Query q) {
    query = std::move(q);
    return *this;
}

ViewDefinition& ViewDefinition::with_include_metadata(bool v) {
    include_metadata = v;
    return *this;
}

std::string ViewDefinition::to_json() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": \"" << escape_json_string(name) << "\",\n";
    out << "  \"description\": \"" << escape_json_string(description) << "\"";

    if (query) {
        out << ",\n  \"query\": \"" << escape_json_string(query->source())
            << "\"";
    }

    out << ",\n  \"include_metadata\": "
        << (include_metadata ? "true" : "false") << "\n";
    out << "}";
    return out.str();
}

ViewDefinition ViewDefinition::from_json(const std::string& json) {
    ViewDefinition view_def;

    simdjson::dom::parser parser;
    auto result = parser.parse(json);
    if (result.error()) {
        return view_def;
    }

    auto root = result.value_unsafe();
    if (!root.is_object()) {
        return view_def;
    }

    auto name_result = root["name"];
    if (!name_result.error() && name_result.value_unsafe().is_string()) {
        view_def.name =
            std::string(name_result.value_unsafe().get_string().value());
    }

    auto desc_result = root["description"];
    if (!desc_result.error() && desc_result.value_unsafe().is_string()) {
        view_def.description =
            std::string(desc_result.value_unsafe().get_string().value());
    }

    auto query_result = root["query"];
    if (!query_result.error() && query_result.value_unsafe().is_string()) {
        view_def.with_query(
            std::string(query_result.value_unsafe().get_string().value()));
    }

    auto meta_result = root["include_metadata"];
    if (!meta_result.error() && meta_result.value_unsafe().is_bool()) {
        view_def.include_metadata =
            meta_result.value_unsafe().get_bool().value();
    }

    return view_def;
}

ViewDefinition ViewDefinition::io_view() {
    ViewDefinition view;
    view.name = "io";
    view.description = "POSIX, STDIO I/O operations";
    view.with_query(
        R"(cat in ["POSIX", "STDIO"] and name in ["read", "write", "open", "close", "pread", "pwrite", "pread64", "pwrite64", "readv", "writev", "fopen", "fclose", "fread", "fwrite", "lseek", "stat", "fstat", "lseek64", "fstat64"])");
    return view;
}

ViewDefinition ViewDefinition::compute_view() {
    ViewDefinition view;
    view.name = "compute";
    view.description = "AI/HPC compute and framework operations";
    view.with_query(
        R"(cat in ["compute", "comm", "device", "ai_framework", "ai_root"])");
    return view;
}

ViewDefinition ViewDefinition::dlio_view() {
    ViewDefinition view;
    view.name = "dlio";
    view.description = "DLIO benchmark operations";
    view.with_query(
        R"(cat in ["compute", "data", "dataloader", "comm", "device", "checkpoint", "pipeline", "ai_framework", "ai_root", "dlio_benchmark", "reader", "storage", "config", "data_loader"])");
    return view;
}

}  // namespace dftracer::utils::utilities::composites::dft::views
