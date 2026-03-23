#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <yyjson.h>

#include <cstdlib>
#include <string>

namespace dftracer::utils::utilities::composites::dft::views {

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
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "name", name.c_str());
    yyjson_mut_obj_add_str(doc, root, "description", description.c_str());

    if (query) {
        yyjson_mut_obj_add_str(doc, root, "query", query->source().c_str());
    }

    yyjson_mut_obj_add_bool(doc, root, "include_metadata", include_metadata);

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json_str);
    free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

ViewDefinition ViewDefinition::from_json(const std::string& json) {
    yyjson_doc* doc =
        yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
    yyjson_val* root = yyjson_doc_get_root(doc);

    ViewDefinition view_def;

    yyjson_val* name_val = yyjson_obj_get(root, "name");
    if (name_val && yyjson_is_str(name_val)) {
        view_def.name = yyjson_get_str(name_val);
    }

    yyjson_val* desc_val = yyjson_obj_get(root, "description");
    if (desc_val && yyjson_is_str(desc_val)) {
        view_def.description = yyjson_get_str(desc_val);
    }

    yyjson_val* query_val = yyjson_obj_get(root, "query");
    if (query_val && yyjson_is_str(query_val)) {
        view_def.with_query(yyjson_get_str(query_val));
    }

    yyjson_val* meta_val = yyjson_obj_get(root, "include_metadata");
    if (meta_val && yyjson_is_bool(meta_val)) {
        view_def.include_metadata = yyjson_get_bool(meta_val);
    }

    yyjson_doc_free(doc);
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
