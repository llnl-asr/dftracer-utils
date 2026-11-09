// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/json/parser.h>

namespace dftracer::utils::json {

JsonParser::JsonParser(std::size_t capacity) : parser_(capacity) {}

bool JsonParser::parse(dstd::StringView json_line) {
    // Reuse padbuf_'s capacity instead of allocating a padded_string per line;
    // resize() zero-fills the SIMDJSON_PADDING tail On-Demand requires.
    borrowed_ = false;
    length_ = json_line.size();
    padbuf_.assign(json_line.data(), json_line.size());
    padbuf_.resize(json_line.size() + simdjson::SIMDJSON_PADDING);
    auto result =
        parser_.iterate(padbuf_.data(), json_line.size(), padbuf_.size());
    if (detail::is_repairable(result.error())) {
        detail::repair_line(padbuf_.data(), json_line.size());
        result =
            parser_.iterate(padbuf_.data(), json_line.size(), padbuf_.size());
    }
    if (result.error()) {
        valid_ = false;
        return false;
    }
    doc_ = std::move(result.value_unsafe());
    active_ = simdjson::ondemand::document_reference(doc_);
    valid_ = true;
    return true;
}

void JsonParser::rewind() {
    // A document a read error abandoned cannot be rewound.
    if (readable()) {
        active_.rewind();
    }
}

dstd::Optional<std::int64_t> JsonParser::get_int64(dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key].get_int64();
    if (result.error()) return dstd::nullopt;
    return result.value_unsafe();
}

dstd::Optional<std::uint64_t> JsonParser::get_uint64(dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key].get_uint64();
    if (result.error()) return dstd::nullopt;
    return result.value_unsafe();
}

dstd::Optional<double> JsonParser::get_double(dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key];
    if (result.error()) return dstd::nullopt;
    auto value = result.value_unsafe();
    return detail::get_double_or_refused(value);
}

dstd::Optional<bool> JsonParser::get_bool(dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key].get_bool();
    if (result.error()) return dstd::nullopt;
    return result.value_unsafe();
}

dstd::Optional<dstd::StringView> JsonParser::get_string(dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key];
    if (result.error()) return dstd::nullopt;
    auto value = result.value_unsafe();
    return detail::get_string_or_raw(value);
}

dstd::Optional<simdjson::ondemand::value> JsonParser::get_value(
    dstd::StringView key) {
    if (!readable()) return dstd::nullopt;
    auto result = active_[key];
    if (result.error()) return dstd::nullopt;
    return result.value_unsafe();
}

}  // namespace dftracer::utils::json
