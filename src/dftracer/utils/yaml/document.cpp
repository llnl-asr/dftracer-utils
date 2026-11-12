// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/line_value.h>
#include <dftracer/utils/yaml/scalar.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace dftracer::utils::yaml {

Document::Document(dstd::UniquePtr<DocumentStorage> storage)
    : storage_(std::move(storage)) {}
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;
Document::~Document() = default;

dstd::Result<Document> Document::parse(dstd::String text,
                                       dstd::StringView source_name) {
    auto storage = dstd::make_unique<DocumentStorage>();
    storage->text = std::move(text);
    const std::size_t bom =
        bom_size(storage->text.data(), storage->text.size());
    storage->base = storage->text.data() + bom;
    storage->size = storage->text.size() - bom;
    if (storage->lines.build(storage->base, storage->size) ==
        BuildResult::Invalid)
        return line_problem_error(storage->lines.problem, source_name, 1);
    return Document(std::move(storage));
}

dstd::Result<Document> Document::load_file(dstd::PathView path) {
    auto text = dstd::fs::read_file(path);
    if (!text) return text.error();
    return parse(std::move(*text), path.str());
}

Value Document::root() const { return LineAccess::root(storage_.get()); }

bool Value::is_object() const { return LineAccess::is_object(*this); }

bool Value::is_array() const { return LineAccess::is_array(*this); }

bool Value::is_string() const { return LineAccess::is_string(*this); }

bool Value::is_null() const { return LineAccess::is_null(*this); }

dstd::Optional<Value> Value::find(dstd::StringView key) const {
    return LineAccess::find(*this, key);
}

void Value::for_each_member(
    dstd::FunctionRef<void(dstd::StringView, Value)> fn) const {
    LineAccess::for_each_member(*this, fn);
}

void Value::for_each_element(dstd::FunctionRef<void(Value)> fn) const {
    LineAccess::for_each_element(*this, fn);
}

dstd::StringView Value::raw() const { return LineAccess::raw(*this); }

dstd::Optional<dstd::StringView> Value::as_string() const {
    if (!is_string()) return dstd::nullopt;
    return raw();
}

dstd::Optional<bool> Value::as_bool() const {
    if (!is_string()) return dstd::nullopt;
    return parse_bool(raw());
}

dstd::Optional<double> Value::as_double() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (text == ".inf" || text == ".Inf" || text == ".INF" || text == "+.inf" ||
        text == "+.Inf" || text == "+.INF")
        return std::numeric_limits<double>::infinity();
    if (text == "-.inf" || text == "-.Inf" || text == "-.INF")
        return -std::numeric_limits<double>::infinity();
    if (text == ".nan" || text == ".NaN" || text == ".NAN")
        return std::numeric_limits<double>::quiet_NaN();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<double>(text);
}

dstd::Optional<std::int64_t> Value::as_int() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<std::int64_t>(text);
}

dstd::Optional<std::uint64_t> Value::as_uint() const {
    if (!is_string()) return dstd::nullopt;
    dstd::StringView text = raw();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return dstd::from_chars<std::uint64_t>(text);
}

}  // namespace dftracer::utils::yaml
