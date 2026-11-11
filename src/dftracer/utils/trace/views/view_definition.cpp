// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <simdjson.h>

#include <cstdlib>

namespace dftracer::utils::trace::views {

using json::escape_json_string;

ViewDefinition& ViewDefinition::with_name(const dstd::String& n) {
    name = n;
    return *this;
}

ViewDefinition& ViewDefinition::with_description(const dstd::String& d) {
    description = d;
    return *this;
}

ViewDefinition& ViewDefinition::with_query(const dstd::String& query_str) {
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

dstd::String ViewDefinition::to_json() const {
    dstd::String out;
    out += "{\n";
    out += "  \"name\": \"";
    internal::append_value(out, escape_json_string(name));
    out += "\",\n";
    out += "  \"description\": \"";
    internal::append_value(out, escape_json_string(description));
    out += "\"";

    if (query) {
        out += ",\n  \"duql\": \"";
        internal::append_value(out, escape_json_string(query->source()));
        out += "\"";
    }

    out += ",\n  \"all\": ";
    internal::append_value(out, (metadata_records ? "true" : "false"));
    out += "\n";
    out += "}";
    return out;
}

ViewDefinition ViewDefinition::from_json(const dstd::String& json) {
    ViewDefinition view_def;

    simdjson::dom::parser parser;
    auto result = parser.parse(json.data(), json.size());
    if (result.error()) {
        return view_def;
    }

    auto root = result.value_unsafe();
    if (!root.is_object()) {
        return view_def;
    }

    auto name_result = root["name"];
    if (!name_result.error() && name_result.value_unsafe().is_string()) {
        view_def.name = dstd::String(
            name_result.value_unsafe().get_string().value_unsafe());
    }

    auto desc_result = root["description"];
    if (!desc_result.error() && desc_result.value_unsafe().is_string()) {
        view_def.description = dstd::String(
            desc_result.value_unsafe().get_string().value_unsafe());
    }

    auto query_result = root["duql"];
    if (!query_result.error() && query_result.value_unsafe().is_string()) {
        view_def.with_query(dstd::String(
            query_result.value_unsafe().get_string().value_unsafe()));
    }

    auto all_result = root["all"];
    if (!all_result.error() && all_result.value_unsafe().is_bool()) {
        view_def.metadata_records =
            all_result.value_unsafe().get_bool().value_unsafe();
        view_def.include_metadata = view_def.metadata_records;
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

}  // namespace dftracer::utils::trace::views
