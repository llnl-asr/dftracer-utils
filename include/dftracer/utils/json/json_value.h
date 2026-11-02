// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_JSON_VALUE_H
#define DFTRACER_UTILS_JSON_JSON_VALUE_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dftracer::utils::json {

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

    /// The wrapped simdjson element (invalid/empty when !exists()). Lets code
    /// that already parsed reuse the element instead of re-parsing.
    simdjson::dom::element element() const { return elem_; }

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

    JsonValue operator[](dstd::StringView key) const {
        if (!valid_ || !elem_.is_object()) return JsonValue();
        auto result = elem_[key];
        if (result.error()) return JsonValue();
        return JsonValue(result.value_unsafe());
    }

    /// Array element by index (invalid JsonValue if not an array or out of
    /// range). Lets dotted paths address array elements (e.g. "tags.0.name").
    JsonValue operator[](std::size_t index) const {
        if (!valid_ || !elem_.is_array()) return JsonValue();
        auto arr = elem_.get_array();
        if (arr.error()) return JsonValue();
        auto result = arr.value_unsafe().at(index);
        if (result.error()) return JsonValue();
        return JsonValue(result.value_unsafe());
    }

    JsonValue at(const char* path) const;
    JsonValue at(dstd::StringView path) const;

    template <typename T>
    T get(const T& default_val = T{}) const {
        if (!valid_) return default_val;

        if constexpr (std::is_same_v<T, bool>) {
            auto r = elem_.get_bool();
            return r.error() ? default_val : r.value_unsafe();
        } else if constexpr (std::is_same_v<T, dstd::String>) {
            auto r = elem_.get_string();
            return r.error() ? default_val : dstd::String(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, dstd::StringView>) {
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
    dstd::Optional<T> get_optional() const {
        if (!valid_) return dstd::nullopt;

        if constexpr (std::is_same_v<T, dstd::String>) {
            auto r = elem_.get_string();
            return r.error()
                       ? dstd::nullopt
                       : dstd::Optional<T>(dstd::String(r.value_unsafe()));
        } else if constexpr (std::is_same_v<T, dstd::StringView>) {
            auto r = elem_.get_string();
            return r.error() ? dstd::nullopt
                             : dstd::Optional<T>(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, const char*>) {
            auto r = elem_.get_c_str();
            return r.error() ? dstd::nullopt
                             : dstd::Optional<T>(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            auto r = elem_.get_uint64();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error() && ri.value_unsafe() >= 0)
                return static_cast<std::uint64_t>(ri.value_unsafe());
            return dstd::nullopt;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            auto r = elem_.get_int64();
            return r.error() ? dstd::nullopt
                             : dstd::Optional<T>(r.value_unsafe());
        } else if constexpr (std::is_same_v<T, double>) {
            auto r = elem_.get_double();
            if (!r.error()) return r.value_unsafe();
            auto ri = elem_.get_int64();
            if (!ri.error()) return static_cast<double>(ri.value_unsafe());
            auto ru = elem_.get_uint64();
            if (!ru.error()) return static_cast<double>(ru.value_unsafe());
            return dstd::nullopt;
        } else if constexpr (std::is_same_v<T, bool>) {
            auto r = elem_.get_bool();
            return r.error() ? dstd::nullopt
                             : dstd::Optional<T>(r.value_unsafe());
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

/// Coerce a JSON number element (uint/int/double) to double; 0 for non-numbers.
inline double json_number(simdjson::dom::element el) {
    if (el.is_uint64())
        return static_cast<double>(el.get_uint64().value_unsafe());
    if (el.is_int64())
        return static_cast<double>(el.get_int64().value_unsafe());
    if (el.is_double()) return el.get_double().value_unsafe();
    return 0;
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_VALUE_H
