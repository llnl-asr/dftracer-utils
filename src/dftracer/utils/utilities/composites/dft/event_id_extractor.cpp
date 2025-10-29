#include <dftracer/utils/utilities/composites/dft/event_id_extractor.h>
#include <yyjson.h>

namespace dftracer::utils::utilities::composites::dft {

EventIdExtractionOutput EventIdExtractor::process(
    const EventIdExtractionInput& input) {
    EventId event;

    yyjson_doc* doc =
        yyjson_read(input.json_data.data(), input.json_data.size(), 0);
    if (!doc) {
        return event;  // Invalid JSON
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return event;  // Not a JSON object
    }

    // Extract id
    yyjson_val* id_val = yyjson_obj_get(root, "id");
    if (id_val && yyjson_is_int(id_val)) {
        event.id = yyjson_get_int(id_val);
    }

    // Extract pid
    yyjson_val* pid_val = yyjson_obj_get(root, "pid");
    if (pid_val && yyjson_is_int(pid_val)) {
        event.pid = yyjson_get_int(pid_val);
    }

    // Extract tid
    yyjson_val* tid_val = yyjson_obj_get(root, "tid");
    if (tid_val && yyjson_is_int(tid_val)) {
        event.tid = yyjson_get_int(tid_val);
    }

    yyjson_doc_free(doc);
    return event;
}

}  // namespace dftracer::utils::utilities::composites::dft
