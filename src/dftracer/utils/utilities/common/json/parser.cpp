#include <dftracer/utils/utilities/common/json/parser.h>

namespace dftracer::utils::utilities::common::json {

JsonParser::JsonParser(std::size_t capacity) : parser_(capacity) {}

bool JsonParser::parse(std::string_view json_line) {
    padded_json_ = simdjson::padded_string(json_line);
    auto result = parser_.iterate(padded_json_);
    if (result.error()) {
        valid_ = false;
        return false;
    }
    doc_ = std::move(result.value());
    active_ = simdjson::ondemand::document_reference(doc_);
    valid_ = true;
    return true;
}

bool JsonParser::parse_padded(simdjson::padded_string_view json) {
    auto result = parser_.iterate(json);
    if (result.error()) {
        valid_ = false;
        return false;
    }
    doc_ = std::move(result.value());
    active_ = simdjson::ondemand::document_reference(doc_);
    valid_ = true;
    return true;
}

void JsonParser::rewind() {
    if (valid_) {
        active_.rewind();
    }
}

std::optional<std::int64_t> JsonParser::get_int64(std::string_view key) {
    if (!valid_) return std::nullopt;
    auto result = active_[key].get_int64();
    if (result.error()) return std::nullopt;
    return result.value();
}

std::optional<std::uint64_t> JsonParser::get_uint64(std::string_view key) {
    if (!valid_) return std::nullopt;
    auto result = active_[key].get_uint64();
    if (result.error()) return std::nullopt;
    return result.value();
}

std::optional<double> JsonParser::get_double(std::string_view key) {
    if (!valid_) return std::nullopt;
    auto result = active_[key].get_double();
    if (result.error()) return std::nullopt;
    return result.value();
}

std::optional<bool> JsonParser::get_bool(std::string_view key) {
    if (!valid_) return std::nullopt;
    auto result = active_[key].get_bool();
    if (result.error()) return std::nullopt;
    return result.value();
}

std::optional<std::string_view> JsonParser::get_string(std::string_view key) {
    if (!valid_) return std::nullopt;
    auto result = active_[key].get_string();
    if (result.error()) return std::nullopt;
    return result.value();
}

}  // namespace dftracer::utils::utilities::common::json
