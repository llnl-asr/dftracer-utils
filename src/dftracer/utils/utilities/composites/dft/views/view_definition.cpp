#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <yyjson.h>

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

// ViewPredicate implementation
ViewPredicate& ViewPredicate::with_bloom_dim(
    const std::string& dim, const std::vector<std::string>& values) {
    bloom_dims[dim] = values;
    return *this;
}

ViewPredicate& ViewPredicate::with_time_range(double min_ts, double max_ts) {
    time_range = std::make_pair(min_ts, max_ts);
    return *this;
}

ViewPredicate& ViewPredicate::with_min_duration(double us) {
    min_duration_us = us;
    return *this;
}

ViewPredicate& ViewPredicate::with_max_duration(double us) {
    max_duration_us = us;
    return *this;
}

bool ViewPredicate::has_bloom_dims() const { return !bloom_dims.empty(); }

bool ViewPredicate::has_event_filters() const {
    return time_range.has_value() || min_duration_us.has_value() ||
           max_duration_us.has_value();
}

// ViewDefinition implementation
ViewDefinition& ViewDefinition::with_name(const std::string& n) {
    name = n;
    return *this;
}

ViewDefinition& ViewDefinition::with_description(const std::string& d) {
    description = d;
    return *this;
}

ViewDefinition& ViewDefinition::with_predicate(ViewPredicate pred) {
    predicates.push_back(std::move(pred));
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

    // Add name, description
    yyjson_mut_obj_add_str(doc, root, "name", name.c_str());
    yyjson_mut_obj_add_str(doc, root, "description", description.c_str());

    // Build predicates array
    yyjson_mut_val* preds_arr = yyjson_mut_arr(doc);
    for (const auto& pred : predicates) {
        yyjson_mut_val* pred_obj = yyjson_mut_obj(doc);

        // Add bloom dims as arrays
        for (const auto& [dim, values] : pred.bloom_dims) {
            yyjson_mut_val* vals_arr = yyjson_mut_arr(doc);
            for (const auto& v : values) {
                yyjson_mut_arr_add_str(doc, vals_arr, v.c_str());
            }
            yyjson_mut_obj_add_val(doc, pred_obj, dim.c_str(), vals_arr);
        }

        // Add time_range if set
        if (pred.time_range) {
            yyjson_mut_val* tr = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_real(doc, tr, "min_ts", pred.time_range->first);
            yyjson_mut_obj_add_real(doc, tr, "max_ts", pred.time_range->second);
            yyjson_mut_obj_add_val(doc, pred_obj, "time_range", tr);
        }

        // Add duration filters if set
        if (pred.min_duration_us) {
            yyjson_mut_obj_add_real(doc, pred_obj, "min_duration_us",
                                    *pred.min_duration_us);
        }
        if (pred.max_duration_us) {
            yyjson_mut_obj_add_real(doc, pred_obj, "max_duration_us",
                                    *pred.max_duration_us);
        }

        yyjson_mut_arr_add_val(preds_arr, pred_obj);
    }
    yyjson_mut_obj_add_val(doc, root, "predicates", preds_arr);

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

    // Parse name and description
    yyjson_val* name_val = yyjson_obj_get(root, "name");
    if (name_val && yyjson_is_str(name_val)) {
        view_def.name = yyjson_get_str(name_val);
    }

    yyjson_val* desc_val = yyjson_obj_get(root, "description");
    if (desc_val && yyjson_is_str(desc_val)) {
        view_def.description = yyjson_get_str(desc_val);
    }

    // Parse predicates array
    yyjson_val* preds_arr = yyjson_obj_get(root, "predicates");
    if (preds_arr && yyjson_is_arr(preds_arr)) {
        yyjson_val* pred_obj;
        yyjson_arr_iter pred_iter;
        yyjson_arr_iter_init(preds_arr, &pred_iter);

        while ((pred_obj = yyjson_arr_iter_next(&pred_iter))) {
            if (yyjson_is_obj(pred_obj)) {
                ViewPredicate predicate;

                // Iterate through all keys in the predicate object
                yyjson_obj_iter obj_iter;
                yyjson_obj_iter_init(pred_obj, &obj_iter);
                yyjson_val* key;

                while ((key = yyjson_obj_iter_next(&obj_iter))) {
                    const char* key_str = yyjson_get_str(key);
                    yyjson_val* value = yyjson_obj_iter_get_val(key);

                    // Handle known event filter keys
                    if (strcmp(key_str, "time_range") == 0 &&
                        yyjson_is_obj(value)) {
                        yyjson_val* min_ts_val =
                            yyjson_obj_get(value, "min_ts");
                        yyjson_val* max_ts_val =
                            yyjson_obj_get(value, "max_ts");
                        if (yyjson_is_real(min_ts_val) &&
                            yyjson_is_real(max_ts_val)) {
                            predicate.time_range =
                                std::make_pair(yyjson_get_real(min_ts_val),
                                               yyjson_get_real(max_ts_val));
                        }
                    } else if (strcmp(key_str, "min_duration_us") == 0 &&
                               yyjson_is_real(value)) {
                        predicate.min_duration_us = yyjson_get_real(value);
                    } else if (strcmp(key_str, "max_duration_us") == 0 &&
                               yyjson_is_real(value)) {
                        predicate.max_duration_us = yyjson_get_real(value);
                    } else if (yyjson_is_arr(value)) {
                        // Everything else is a bloom dimension
                        std::vector<std::string> values;
                        yyjson_val* str_val;
                        yyjson_arr_iter arr_iter;
                        yyjson_arr_iter_init(value, &arr_iter);

                        while ((str_val = yyjson_arr_iter_next(&arr_iter))) {
                            if (yyjson_is_str(str_val)) {
                                values.emplace_back(yyjson_get_str(str_val));
                            }
                        }
                        predicate.bloom_dims[key_str] = std::move(values);
                    }
                }
                view_def.predicates.push_back(std::move(predicate));
            }
        }
    }

    yyjson_doc_free(doc);
    return view_def;
}

ViewDefinition ViewDefinition::io_view() {
    ViewDefinition view;
    view.name = "io";
    view.description = "POSIX, STDIO I/O operations";

    ViewPredicate predicate;
    predicate.bloom_dims = {
        {"cat", {"POSIX", "STDIO"}},
        {"name",
         {"read", "write", "open", "close", "pread", "pwrite", "pread64",
          "pwrite64", "readv", "writev", "fopen", "fclose", "fread", "fwrite",
          "lseek", "stat", "fstat", "lseek64", "fstat64"}}};
    view.predicates.push_back(std::move(predicate));

    return view;
}

ViewDefinition ViewDefinition::compute_view() {
    ViewDefinition view;
    view.name = "compute";
    view.description = "AI/HPC compute and framework operations";

    ViewPredicate predicate;
    predicate.bloom_dims = {
        {"cat", {"compute", "comm", "device", "ai_framework", "ai_root"}}};
    view.predicates.push_back(std::move(predicate));

    return view;
}

ViewDefinition ViewDefinition::dlio_view() {
    ViewDefinition view;
    view.name = "dlio";
    view.description = "DLIO benchmark operations";

    ViewPredicate predicate;
    predicate.bloom_dims = {
        {"cat",
         {"compute", "data", "dataloader", "comm", "device", "checkpoint",
          "pipeline", "ai_framework", "ai_root", "dlio_benchmark", "reader",
          "storage", "config", "data_loader"}}};
    view.predicates.push_back(std::move(predicate));

    return view;
}

std::string resolve_bloom_dimension(const std::string& dim) {
    static const std::unordered_map<std::string, std::string> ALIASES = {
        {"host", "hhash"},   {"file", "fhash"},     {"script", "shash"},
        {"category", "cat"}, {"process_id", "pid"}, {"thread_id", "tid"}};
    auto it = ALIASES.find(dim);
    return it != ALIASES.end() ? it->second : dim;
}

}  // namespace dftracer::utils::utilities::composites::dft::views
