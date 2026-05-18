#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_PARSER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_PARSER_H

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::common::json {

/**
 * @brief On-Demand JSON parser for zero-copy parsing.
 *
 * Key design principles:
 * 1. On-Demand API for lazy field access - only parses what you use
 * 2. Parser is reused across rows (internal buffer management)
 * 3. Zero-copy: string_view points directly into the padded JSON buffer
 * 4. Forward-only iteration: once a field is accessed, it's consumed
 *
 * Usage pattern for batch processing:
 * @code
 *   JsonParser parser;
 *
 *   for (auto& line : input_lines) {
 *       // parse() copies to internal padded buffer
 *       if (!parser.parse(line)) continue;
 *
 *       // Access fields directly from parser
 *       auto name = parser.get_string("name");
 *       auto ts = parser.get_int64("ts");
 *
 *       // Iterate over 'args' object
 *       parser.for_each_field("args", [](std::string_view key, auto& val) {
 *           // process nested fields
 *       });
 *   }
 * @endcode
 *
 * @note string_view values are only valid until the next parse() call.
 */
class JsonParser {
   public:
    static constexpr std::size_t DEFAULT_CAPACITY = 1 << 20;  // 1MB

    explicit JsonParser(std::size_t capacity = DEFAULT_CAPACITY);

    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;
    JsonParser(JsonParser&&) = default;
    JsonParser& operator=(JsonParser&&) = default;

    /**
     * @brief Parse a JSON line.
     *
     * Copies the input to an internal padded buffer for SIMD processing.
     * Previous parse results become invalid after this call.
     *
     * @param json_line The JSON string to parse.
     * @return true on success, false on parse error.
     */
    bool parse(std::string_view json_line);

    /**
     * @brief Parse from pre-padded string (avoids copy).
     */
    bool parse_padded(simdjson::padded_string_view json);

    /**
     * @brief Check if current document is valid (last parse succeeded).
     */
    bool is_valid() const { return valid_; }

    // Direct field access from root object
    // Returns nullopt if field missing or wrong type

    std::optional<std::int64_t> get_int64(std::string_view key);
    std::optional<std::uint64_t> get_uint64(std::string_view key);
    std::optional<double> get_double(std::string_view key);
    std::optional<bool> get_bool(std::string_view key);
    std::optional<std::string_view> get_string(std::string_view key);

    /**
     * @brief Iterate over all fields in the root object.
     *
     * @param fn Callback: void(std::string_view key, simdjson::ondemand::value
     * val)
     *
     * @note This consumes the document. After calling, field access methods
     *       will return nullopt. Call parse() again to re-parse.
     */
    template <typename Fn>
    void for_each_field(Fn&& fn);

    /**
     * @brief Iterate over fields of a nested object.
     *
     * @param object_key The field containing the nested object.
     * @param fn Callback: void(std::string_view key, simdjson::ondemand::value
     * val)
     * @return true if object found and iterated, false otherwise.
     */
    template <typename Fn>
    bool for_each_field(std::string_view object_key, Fn&& fn);

    /**
     * @brief Rewind document for re-iteration.
     *
     * After accessing fields, the document position advances. Call this
     * to reset to the beginning for another pass.
     */
    void rewind();

    /**
     * @brief Get raw document for advanced usage.
     */
    simdjson::ondemand::document& raw_document() { return doc_; }

    /**
     * @brief Borrow an externally-owned parsed document.
     *
     * After this call, for_each_field/rewind/get_* operate on the borrowed
     * reference. The caller must keep the underlying document alive until
     * another parse() / set_borrowed_document() call. Intended for bridging
     * iterate_many output (document_reference) to consumers that accept a
     * JsonParser&.
     */
    void set_borrowed_document(
        simdjson::ondemand::document_reference ref) noexcept {
        active_ = ref;
        valid_ = true;
    }

   private:
    simdjson::ondemand::parser parser_;
    simdjson::padded_string padded_json_;
    simdjson::ondemand::document doc_;
    simdjson::ondemand::document_reference active_;
    bool valid_ = false;
};

// Template implementations

template <typename Fn>
void JsonParser::for_each_field(Fn&& fn) {
    if (!valid_) return;

    auto obj_result = active_.get_object();
    if (obj_result.error()) return;

    for (auto field : obj_result.value()) {
        if (field.error()) continue;

        auto key_result = field.unescaped_key();
        if (key_result.error()) continue;

        auto val_result = field.value();
        if (val_result.error()) continue;

        fn(key_result.value(), val_result.value());
    }
}

template <typename Fn>
bool JsonParser::for_each_field(std::string_view object_key, Fn&& fn) {
    if (!valid_) return false;

    auto nested_result = active_[object_key].get_object();
    if (nested_result.error()) return false;

    for (auto field : nested_result.value()) {
        if (field.error()) continue;

        auto key_result = field.unescaped_key();
        if (key_result.error()) continue;

        auto val_result = field.value();
        if (val_result.error()) continue;

        fn(key_result.value(), val_result.value());
    }
    return true;
}

/**
 * @brief Helper to extract typed value from simdjson::ondemand::value.
 *
 * Use in for_each_field callbacks to safely extract values.
 */
struct JsonValueHelper {
    static std::optional<std::int64_t> get_int64(
        simdjson::ondemand::value& val) {
        auto r = val.get_int64();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static std::optional<std::uint64_t> get_uint64(
        simdjson::ondemand::value& val) {
        auto r = val.get_uint64();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static std::optional<double> get_double(simdjson::ondemand::value& val) {
        auto r = val.get_double();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static std::optional<bool> get_bool(simdjson::ondemand::value& val) {
        auto r = val.get_bool();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static std::optional<std::string_view> get_string(
        simdjson::ondemand::value& val) {
        auto r = val.get_string();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static bool is_null(simdjson::ondemand::value& val) {
        auto r = val.is_null();
        return r.error() ? false : r.value();
    }

    static std::optional<simdjson::ondemand::json_type> get_type(
        simdjson::ondemand::value& val) {
        auto r = val.type();
        return r.error() ? std::nullopt : std::optional(r.value());
    }

    static std::optional<std::string> to_json_string(
        simdjson::ondemand::value& val) {
        auto r = simdjson::to_json_string(val);
        return r.error() ? std::nullopt : std::optional(std::string(r.value()));
    }
};

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_PARSER_H
