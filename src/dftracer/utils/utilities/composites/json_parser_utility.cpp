#include <dftracer/utils/utilities/composites/json_parser_utility.h>
#include <dftracer/utils/utilities/io/file_reader_utility.h>

#include <cstring>

namespace dftracer::utils::utilities::composites {

JsonValue JsonValue::at(const char* path) const {
    if (!val_ || !path) return JsonValue(nullptr);

    JsonValue current(val_);
    const char* start = path;

    while (*start) {
        const char* end = start;
        while (*end && *end != '.') end++;

        size_t key_len = end - start;
        if (key_len == 0) {
            start = (*end == '.') ? end + 1 : end;
            continue;
        }

        char key_buf[256];
        if (key_len >= sizeof(key_buf)) {
            std::string key_str(start, key_len);
            current = current[key_str.c_str()];
        } else {
            std::memcpy(key_buf, start, key_len);
            key_buf[key_len] = '\0';
            current = current[key_buf];
        }

        if (!current.exists()) {
            return JsonValue(nullptr);
        }

        start = (*end == '.') ? end + 1 : end;
    }

    return current;
}

JsonValue JsonValue::at(const std::string& path) const {
    return at(path.c_str());
}

JsonValue JsonValue::at(std::string_view path) const {
    std::string path_str(path);
    return at(path_str.c_str());
}

StringJsonParserInput StringJsonParserInput::from_file(
    const std::string& file_path) {
    StringJsonParserInput input;

    utilities::io::FileReaderUtility file_reader;
    utilities::filesystem::FileEntry file_entry{file_path};
    input.content = file_reader.process(file_entry);

    return input;
}

StringJsonParserInput StringJsonParserInput::from_string(
    const std::string& json_str) {
    StringJsonParserInput input;
    input.content.content = json_str;
    return input;
}

JsonParserOutput StringJsonParserUtility::process(
    const StringJsonParserInput& input) {
    content_ = input.content;

    yyjson_doc* doc =
        yyjson_read(content_.content.data(), content_.content.size(), 0);

    yyjson_val* json_object = nullptr;
    if (doc) {
        json_object = yyjson_doc_get_root(doc);
        owned_doc_ = std::shared_ptr<yyjson_doc>(doc, [](yyjson_doc* d) {
            if (d) yyjson_doc_free(d);
        });
    }

    return JsonValue(json_object);
}

void StringJsonParserUtility::reset() {
    owned_doc_.reset();
    content_ = utilities::text::Text{};
}

}  // namespace dftracer::utils::utilities::composites
