// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_PARSER_H
#define DFTRACER_UTILS_JSON_PARSER_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/value_access.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::json {

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
 *       parser.for_each_field("args", [](dstd::StringView key, auto& val) {
 *           // process nested fields
 *       });
 *   }
 * @endcode
 *
 * @note StringView values are only valid until the next parse() call.
 */
class JsonParser {
   public:
    /// The longest line the parser reads, simdjson's own limit. A smaller
    /// limit drops every line past it.
    static constexpr std::size_t DEFAULT_CAPACITY =
        simdjson::SIMDJSON_MAXSIZE_BYTES;

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
    bool parse(dstd::StringView json_line);

    /**
     * @brief Check if current document is valid (last parse succeeded).
     */
    bool is_valid() const { return valid_; }

    /// Direct field access from root object
    /// Returns nullopt if field missing or wrong type

    dstd::Optional<std::int64_t> get_int64(dstd::StringView key);
    dstd::Optional<std::uint64_t> get_uint64(dstd::StringView key);
    dstd::Optional<double> get_double(dstd::StringView key);
    dstd::Optional<bool> get_bool(dstd::StringView key);
    dstd::Optional<dstd::StringView> get_string(dstd::StringView key);

    /// The raw field value, for callers that must inspect its type before
    /// choosing an accessor (e.g. a field that may be an int or a string). One
    /// lookup; do not also call a typed get_* for the same key on the same
    /// parse.
    dstd::Optional<simdjson::ondemand::value> get_value(dstd::StringView key);

    /**
     * @brief Iterate over all fields in the root object.
     *
     * @param fn Callback: void(dstd::StringView key, simdjson::ondemand::value
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
     * @param fn Callback: void(dstd::StringView key, simdjson::ondemand::value
     * val)
     * @return true if object found and iterated, false otherwise.
     */
    template <typename Fn>
    bool for_each_field(dstd::StringView object_key, Fn&& fn);

    /**
     * @brief Rewind document for re-iteration.
     *
     * After accessing fields, the document position advances. Call this
     * to reset to the beginning for another pass.
     */
    void rewind();

    /**
     * @brief The text of the parsed line, as the document holds it.
     *
     * Points into the parser's own padded copy, the buffer that the values of
     * the document point into. Valid until the next parse() call.
     */
    dstd::StringView text() const noexcept {
        return dstd::StringView(padbuf_.data(), length_);
    }

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
     * a document_reference from a line-by-line read to consumers that accept a
     * JsonParser&.
     */
    void set_borrowed_document(
        simdjson::ondemand::document_reference ref) noexcept {
        active_ = ref;
        valid_ = true;
        borrowed_ = true;
    }

   private:
    simdjson::ondemand::parser parser_;
    dstd::String
        padbuf_;  ///< reused across parse() calls; grows to fit + padding
    simdjson::ondemand::document doc_;
    simdjson::ondemand::document_reference active_;
    std::size_t length_ = 0;
    bool valid_ = false;
    bool borrowed_ = false;

    // A read error abandons the document, and any later read of it would crash.
    bool readable() {
        return valid_ &&
               static_cast<simdjson::ondemand::document&>(active_).is_alive();
    }
};

// Template implementations

template <typename Fn>
void JsonParser::for_each_field(Fn&& fn) {
    if (!readable()) return;

    auto obj_result = active_.get_object();
    if (obj_result.error()) return;

    for (auto field : obj_result.value_unsafe()) {
        if (field.error()) continue;

        const auto key_result = detail::get_key_or_raw(field);
        if (!key_result) continue;

        auto val_result = field.value();
        if (val_result.error()) continue;

        fn(*key_result, val_result.value_unsafe());
    }
}

template <typename Fn>
bool JsonParser::for_each_field(dstd::StringView object_key, Fn&& fn) {
    if (!readable()) return false;

    auto nested_result = active_[object_key].get_object();
    if (nested_result.error()) return false;

    for (auto field : nested_result.value_unsafe()) {
        if (field.error()) continue;

        const auto key_result = detail::get_key_or_raw(field);
        if (!key_result) continue;

        auto val_result = field.value();
        if (val_result.error()) continue;

        fn(*key_result, val_result.value_unsafe());
    }
    return true;
}

/**
 * @brief Helper to extract typed value from simdjson::ondemand::value.
 *
 * Use in for_each_field callbacks to safely extract values.
 */
struct JsonValueHelper {
    static dstd::Optional<std::int64_t> get_int64(
        simdjson::ondemand::value& val) {
        auto r = val.get_int64();
        return r.error() ? dstd::nullopt
                         : dstd::Optional<std::int64_t>(r.value_unsafe());
    }

    static dstd::Optional<std::uint64_t> get_uint64(
        simdjson::ondemand::value& val) {
        auto r = val.get_uint64();
        return r.error() ? dstd::nullopt
                         : dstd::Optional<std::uint64_t>(r.value_unsafe());
    }

    static dstd::Optional<double> get_double(simdjson::ondemand::value& val) {
        return detail::get_double_or_refused(val);
    }

    static dstd::Optional<bool> get_bool(simdjson::ondemand::value& val) {
        auto r = val.get_bool();
        return r.error() ? dstd::nullopt
                         : dstd::Optional<bool>(r.value_unsafe());
    }

    static dstd::Optional<dstd::StringView> get_string(
        simdjson::ondemand::value& val) {
        return detail::get_string_or_raw(val);
    }

    static bool is_null(simdjson::ondemand::value& val) {
        auto r = val.is_null();
        return r.error() ? false : r.value_unsafe();
    }

    static dstd::Optional<simdjson::ondemand::json_type> get_type(
        simdjson::ondemand::value& val) {
        auto r = val.type();
        return r.error() ? dstd::nullopt
                         : dstd::Optional<simdjson::ondemand::json_type>(
                               r.value_unsafe());
    }

    static dstd::Optional<dstd::String> to_json_string(
        simdjson::ondemand::value& val) {
        auto r = simdjson::to_json_string(val);
        return r.error() ? dstd::nullopt
                         : dstd::Optional<dstd::String>(
                               dstd::StringView(r.value_unsafe()));
    }
};

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_PARSER_H
