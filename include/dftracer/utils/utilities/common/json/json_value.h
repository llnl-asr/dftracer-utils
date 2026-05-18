#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/text/shared.h>
#include <simdjson.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::utilities::common::json {

/**
 * Lightweight wrapper around simdjson::dom::element with convenient accessors.
 *
 * Provides:
 * - Fluent chaining: json["args"]["hhash"]
 * - Template get<T>() with auto-casting
 * - Default values for missing/null fields
 * - Zero overhead - just element navigation
 *
 * IMPORTANT: JsonValue is only valid while the simdjson::dom::document is
 * alive.
 */
class JsonValue {
   private:
    simdjson::dom::element elem_;
    bool valid_ = false;

   public:
    JsonValue() : valid_(false) {}
    explicit JsonValue(simdjson::dom::element elem)
        : elem_(elem), valid_(true) {}

    bool is_null() const { return !valid_ || elem_.is_null(); }
    bool is_bool() const { return valid_ && elem_.is_bool(); }
    bool is_string() const { return valid_ && elem_.is_string(); }
    bool is_uint() const { return valid_ && elem_.is_uint64(); }
    bool is_int() const { return valid_ && elem_.is_int64(); }
    bool is_number() const {
        return valid_ &&
               (elem_.is_int64() || elem_.is_uint64() || elem_.is_double());
    }
    bool is_object() const { return valid_ && elem_.is_object(); }
    bool is_array() const { return valid_ && elem_.is_array(); }
    bool exists() const { return valid_; }

    JsonValue operator[](const char* key) const {
        if (!valid_ || !elem_.is_object()) return JsonValue();
        auto result = elem_[key];
        if (result.error()) return JsonValue();
        return JsonValue(result.value_unsafe());
    }

    JsonValue operator[](const std::string& key) const {
        return (*this)[key.c_str()];
    }

    JsonValue operator[](std::string_view key) const {
        if (!valid_ || !elem_.is_object()) return JsonValue();
        auto result = elem_[key];
        if (result.error()) return JsonValue();
        return JsonValue(result.value_unsafe());
    }

    JsonValue at(const char* path) const;
    JsonValue at(const std::string& path) const;
    JsonValue at(std::string_view path) const;

    template <typename T>
    T get(const T& default_val = T{}) const {
        if (!valid_) return default_val;

        if constexpr (std::is_same_v<T, bool>) {
            auto r = elem_.get_bool();
            return r.error() ? default_val : r.value_unsafe();
        } else if constexpr (std::is_same_v<T, std::string>) {
            auto r = elem_.get_string();
            return r.error() ? default_val : std::string(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            auto r = elem_.get_string();
            return r.error() ? default_val : r.value_unsafe();
        } else if constexpr (std::is_same_v<T, const char*>) {
            auto r = elem_.get_c_str();
            return r.error() ? default_val : r.value_unsafe();
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            auto r = elem_.get_uint64();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error() && ri.value_unsafe() >= 0)
                return static_cast<std::uint64_t>(ri.value_unsafe());
            return default_val;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            auto r = elem_.get_int64();
            if (!r.error()) return r.value_unsafe();
            auto ru = elem_.get_uint64();
            if (!ru.error() &&
                ru.value_unsafe() <=
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return static_cast<std::int64_t>(ru.value_unsafe());
            return default_val;
        } else if constexpr (std::is_same_v<T, double>) {
            auto r = elem_.get_double();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error()) return static_cast<double>(ri.value_unsafe());
            auto ru = elem_.get_uint64();
            if (!ru.error()) return static_cast<double>(ru.value_unsafe());
            return default_val;
        } else if constexpr (std::is_same_v<T, float>) {
            return static_cast<float>(
                get<double>(static_cast<double>(default_val)));
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            return static_cast<T>(
                get<std::uint64_t>(static_cast<std::uint64_t>(default_val)));
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            return static_cast<T>(
                get<std::int64_t>(static_cast<std::int64_t>(default_val)));
        } else {
            static_assert(!sizeof(T),
                          "Unsupported type for JsonValue::get<T>(). "
                          "Supported: string, integral types, double, bool");
        }
    }

    template <typename T>
    std::optional<T> get_optional() const {
        if (!valid_) return std::nullopt;

        if constexpr (std::is_same_v<T, std::string>) {
            auto r = elem_.get_string();
            return r.error() ? std::nullopt
                             : std::optional(std::string(r.value_unsafe()));
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            auto r = elem_.get_string();
            return r.error() ? std::nullopt : std::optional(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, const char*>) {
            auto r = elem_.get_c_str();
            return r.error() ? std::nullopt : std::optional(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            auto r = elem_.get_uint64();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error() && ri.value_unsafe() >= 0)
                return static_cast<std::uint64_t>(ri.value_unsafe());
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            auto r = elem_.get_int64();
            return r.error() ? std::nullopt : std::optional(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, double>) {
            auto r = elem_.get_double();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error()) return static_cast<double>(ri.value_unsafe());
            auto ru = elem_.get_uint64();
            if (!ru.error()) return static_cast<double>(ru.value_unsafe());
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, bool>) {
            auto r = elem_.get_bool();
            return r.error() ? std::nullopt : std::optional(r.value_unsafe());
        } else {
            static_assert(!sizeof(T),
                          "Unsupported type for JsonValue::get_optional<T>()");
        }
    }

    template <typename Fn>
    void for_each_member(Fn&& fn) const {
        if (!valid_ || !elem_.is_object()) return;
        auto obj = elem_.get_object();
        if (obj.error()) return;
        for (auto field : obj.value_unsafe()) {
            fn(field.key, JsonValue(field.value));
        }
    }

    simdjson::dom::element raw() const { return elem_; }
    explicit operator bool() const { return exists(); }
};

using JsonParserOutput = JsonValue;
using JsonParserInput = simdjson::dom::element;

class JsonParserUtility
    : public utilities::Utility<JsonParserInput, JsonParserOutput> {
   public:
    coro::CoroTask<JsonParserOutput> process(
        const JsonParserInput& input) override {
        co_return JsonValue(input);
    }
};

struct StringJsonParserInput {
    utilities::text::Text content;

    static coro::CoroTask<StringJsonParserInput> from_file_async(
        const std::string& file_path);
    static StringJsonParserInput from_file(const std::string& file_path);
    static StringJsonParserInput from_string(const std::string& json_str);
};

class StringJsonParserUtility
    : public utilities::Utility<StringJsonParserInput, JsonParserOutput> {
   private:
    utilities::text::Text content_;
    simdjson::dom::parser parser_;
    simdjson::dom::document doc_;

   public:
    coro::CoroTask<JsonParserOutput> process(
        const StringJsonParserInput& input) override;
    void reset();
};

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H
