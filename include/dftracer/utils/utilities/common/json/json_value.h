#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/text/shared.h>
#include <yyjson.h>

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
 * Lightweight zero-cost wrapper around yyjson_val* with convenient accessors.
 *
 * Provides pure lazy evaluation:
 * - Fluent chaining: json["args"]["hhash"]
 * - Template get<T>() with auto-casting
 * - Default values for missing/null fields
 * - Zero overhead - just pointer navigation
 *
 * IMPORTANT: JsonValue is only valid while the yyjson_doc is alive.
 */
class JsonValue {
   private:
    yyjson_val* val_;

   public:
    explicit JsonValue(yyjson_val* val = nullptr) : val_(val) {}

    bool is_null() const { return !val_ || yyjson_is_null(val_); }
    bool is_bool() const { return val_ && yyjson_is_bool(val_); }
    bool is_string() const { return val_ && yyjson_is_str(val_); }
    bool is_uint() const { return val_ && yyjson_is_uint(val_); }
    bool is_int() const { return val_ && yyjson_is_int(val_); }
    bool is_number() const { return val_ && yyjson_is_num(val_); }
    bool is_object() const { return val_ && yyjson_is_obj(val_); }
    bool is_array() const { return val_ && yyjson_is_arr(val_); }
    bool exists() const { return val_ != nullptr; }

    JsonValue operator[](const char* key) const {
        return JsonValue(val_ ? yyjson_obj_get(val_, key) : nullptr);
    }

    JsonValue operator[](const std::string& key) const {
        return (*this)[key.c_str()];
    }

    JsonValue operator[](std::string_view key) const {
        std::string key_str(key);
        return (*this)[key_str.c_str()];
    }

    JsonValue at(const char* path) const;
    JsonValue at(const std::string& path) const;
    JsonValue at(std::string_view path) const;

    template <typename T>
    T get(const T& default_val = T{}) const {
        if constexpr (std::is_same_v<T, bool>) {
            return val_ && yyjson_is_bool(val_) ? yyjson_get_bool(val_)
                                                : default_val;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return (val_ && yyjson_is_str(val_))
                       ? std::string(yyjson_get_str(val_))
                       : default_val;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (val_ && yyjson_is_str(val_)) {
                const char* str = yyjson_get_str(val_);
                std::size_t len = yyjson_get_len(val_);
                return std::string_view(str, len);
            }
            return default_val;
        } else if constexpr (std::is_same_v<T, const char*>) {
            return (val_ && yyjson_is_str(val_)) ? yyjson_get_str(val_)
                                                 : default_val;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (!val_) return default_val;
            if (yyjson_is_uint(val_)) return yyjson_get_uint(val_);
            if (yyjson_is_int(val_)) {
                auto v = yyjson_get_int(val_);
                return v >= 0 ? static_cast<std::uint64_t>(v) : default_val;
            }
            return default_val;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            if (!val_) return default_val;
            if (yyjson_is_int(val_)) return yyjson_get_int(val_);
            if (yyjson_is_uint(val_)) {
                auto v = yyjson_get_uint(val_);
                return v <= static_cast<uint64_t>(
                                std::numeric_limits<int64_t>::max())
                           ? static_cast<std::int64_t>(v)
                           : default_val;
            }
            return default_val;
        } else if constexpr (std::is_same_v<T, double>) {
            if (!val_) return default_val;
            if (yyjson_is_real(val_)) return yyjson_get_real(val_);
            if (yyjson_is_int(val_))
                return static_cast<double>(yyjson_get_int(val_));
            if (yyjson_is_uint(val_))
                return static_cast<double>(yyjson_get_uint(val_));
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
        if (!val_) return std::nullopt;

        if constexpr (std::is_same_v<T, std::string>) {
            return yyjson_is_str(val_)
                       ? std::optional(std::string(yyjson_get_str(val_)))
                       : std::nullopt;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (yyjson_is_str(val_)) {
                const char* str = yyjson_get_str(val_);
                std::size_t len = yyjson_get_len(val_);
                return std::optional(std::string_view(str, len));
            }
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, const char*>) {
            return yyjson_is_str(val_) ? std::optional(yyjson_get_str(val_))
                                       : std::nullopt;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (yyjson_is_uint(val_)) return yyjson_get_uint(val_);
            if (yyjson_is_int(val_)) {
                auto v = yyjson_get_int(val_);
                return v >= 0 ? std::optional(static_cast<std::uint64_t>(v))
                              : std::nullopt;
            }
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            if (yyjson_is_int(val_)) return yyjson_get_int(val_);
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, double>) {
            if (yyjson_is_real(val_)) return yyjson_get_real(val_);
            if (yyjson_is_int(val_))
                return static_cast<double>(yyjson_get_int(val_));
            if (yyjson_is_uint(val_))
                return static_cast<double>(yyjson_get_uint(val_));
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, bool>) {
            return yyjson_is_bool(val_) ? std::optional(yyjson_get_bool(val_))
                                        : std::nullopt;
        } else {
            static_assert(!sizeof(T),
                          "Unsupported type for JsonValue::get_optional<T>()");
        }
    }

    yyjson_val* raw() const { return val_; }
    explicit operator yyjson_val*() const { return val_; }
    explicit operator bool() const { return exists(); }
};

using JsonParserInput = yyjson_val*;
using JsonParserOutput = JsonValue;

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
    std::shared_ptr<yyjson_doc> owned_doc_;

   public:
    coro::CoroTask<JsonParserOutput> process(
        const StringJsonParserInput& input) override;
    void reset();
};

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_VALUE_H
