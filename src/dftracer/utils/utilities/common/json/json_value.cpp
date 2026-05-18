#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/fileio/file_reader_utility.h>

#include <cstring>

namespace dftracer::utils::utilities::common::json {

JsonValue JsonValue::at(const char* path) const {
    if (!valid_ || !path) return JsonValue();

    JsonValue current = *this;
    const char* start = path;

    while (*start) {
        const char* end = start;
        while (*end && *end != '.') end++;

        size_t key_len = end - start;
        if (key_len == 0) {
            start = (*end == '.') ? end + 1 : end;
            continue;
        }

        std::string_view key_sv(start, key_len);
        current = current[key_sv];

        if (!current.exists()) {
            return JsonValue();
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

coro::CoroTask<StringJsonParserInput> StringJsonParserInput::from_file_async(
    const std::string& file_path) {
    StringJsonParserInput input;
    utilities::fileio::FileReaderUtility file_reader;
    utilities::filesystem::FileEntry file_entry{file_path};
    input.content = co_await file_reader.process(file_entry);
    co_return input;
}

StringJsonParserInput StringJsonParserInput::from_file(
    const std::string& file_path) {
    return from_file_async(file_path).get();
}

StringJsonParserInput StringJsonParserInput::from_string(
    const std::string& json_str) {
    StringJsonParserInput input;
    input.content.content = json_str;
    return input;
}

coro::CoroTask<JsonParserOutput> StringJsonParserUtility::process(
    const StringJsonParserInput& input) {
    content_ = input.content;

    auto result =
        parser_.parse(content_.content.data(), content_.content.size());
    if (result.error()) {
        co_return JsonValue();
    }
    co_return JsonValue(result.value_unsafe());
}

void StringJsonParserUtility::reset() { content_ = utilities::text::Text{}; }

}  // namespace dftracer::utils::utilities::common::json
