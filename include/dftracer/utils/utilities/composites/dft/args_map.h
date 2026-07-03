#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_ARGS_MAP_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_ARGS_MAP_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace dftracer::utils::utilities::composites::dft {

using ArgsValue = std::variant<std::monostate, std::string, std::int64_t,
                               std::uint64_t, double, bool>;

class ArgsValueProxy {
    const ArgsValue* val_;

   public:
    explicit ArgsValueProxy(const ArgsValue* v = nullptr) : val_(v) {}

    bool exists() const {
        return val_ != nullptr &&
               !std::holds_alternative<std::monostate>(*val_);
    }
    explicit operator bool() const { return exists(); }
    bool is_null() const { return !exists(); }
    bool is_string() const {
        return val_ && std::holds_alternative<std::string>(*val_);
    }
    bool is_uint() const {
        return val_ && std::holds_alternative<std::uint64_t>(*val_);
    }
    bool is_int() const {
        return val_ && std::holds_alternative<std::int64_t>(*val_);
    }
    bool is_number() const {
        return val_ && (std::holds_alternative<std::int64_t>(*val_) ||
                        std::holds_alternative<std::uint64_t>(*val_) ||
                        std::holds_alternative<double>(*val_));
    }
    bool is_bool() const { return val_ && std::holds_alternative<bool>(*val_); }
    bool is_object() const { return false; }
    bool is_array() const { return false; }

    template <typename T>
    T get(const T& default_val = T{}) const {
        if (!val_ || std::holds_alternative<std::monostate>(*val_))
            return default_val;

        if constexpr (std::is_same_v<T, bool>) {
            if (auto* p = std::get_if<bool>(val_)) return *p;
            return default_val;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (auto* p = std::get_if<std::string>(val_)) return *p;
            return default_val;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (auto* p = std::get_if<std::string>(val_))
                return std::string_view(*p);
            return default_val;
        } else if constexpr (std::is_same_v<T, const char*>) {
            if (auto* p = std::get_if<std::string>(val_)) return p->c_str();
            return default_val;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (auto* p = std::get_if<std::uint64_t>(val_)) return *p;
            if (auto* p = std::get_if<std::int64_t>(val_)) {
                if (*p >= 0) return static_cast<std::uint64_t>(*p);
            }
            if (auto* p = std::get_if<double>(val_)) {
                if (*p >= 0 && *p <= static_cast<double>(
                                         std::numeric_limits<uint64_t>::max()))
                    return static_cast<std::uint64_t>(*p);
            }
            return default_val;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            if (auto* p = std::get_if<std::int64_t>(val_)) return *p;
            if (auto* p = std::get_if<std::uint64_t>(val_)) {
                if (*p <=
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    return static_cast<std::int64_t>(*p);
            }
            if (auto* p = std::get_if<double>(val_)) {
                return static_cast<std::int64_t>(*p);
            }
            return default_val;
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto* p = std::get_if<double>(val_)) return *p;
            if (auto* p = std::get_if<std::int64_t>(val_))
                return static_cast<double>(*p);
            if (auto* p = std::get_if<std::uint64_t>(val_))
                return static_cast<double>(*p);
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
                          "Unsupported type for ArgsValueProxy::get<T>()");
        }
    }

    template <typename T>
    std::optional<T> get_optional() const {
        if (!val_ || std::holds_alternative<std::monostate>(*val_))
            return std::nullopt;

        if constexpr (std::is_same_v<T, std::string>) {
            if (auto* p = std::get_if<std::string>(val_)) return *p;
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            if (auto* p = std::get_if<std::string>(val_))
                return std::string_view(*p);
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            if (auto* p = std::get_if<std::uint64_t>(val_)) return *p;
            if (auto* p = std::get_if<std::int64_t>(val_)) {
                if (*p >= 0) return static_cast<std::uint64_t>(*p);
            }
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            if (auto* p = std::get_if<std::int64_t>(val_)) return *p;
            if (auto* p = std::get_if<std::uint64_t>(val_)) {
                if (*p <=
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    return static_cast<std::int64_t>(*p);
            }
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto* p = std::get_if<double>(val_)) return *p;
            if (auto* p = std::get_if<std::int64_t>(val_))
                return static_cast<double>(*p);
            if (auto* p = std::get_if<std::uint64_t>(val_))
                return static_cast<double>(*p);
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (auto* p = std::get_if<bool>(val_)) return *p;
            return std::nullopt;
        } else {
            static_assert(
                !sizeof(T),
                "Unsupported type for ArgsValueProxy::get_optional<T>()");
        }
    }
};

class ArgsMap {
    // Keys are interned views into the process-lifetime key_intern() pool (see
    // insert): each interned string lives in a heap node that is never moved or
    // freed until exit, so the view stays valid for the map's whole life,
    // including long-lived maps stored in call-tree nodes. Storing the view
    // (not a copy) is what makes interning actually reduce memory.
    // INVARIANT: only insert a view from key_intern().intern(); a transient
    // string_view would dangle.
    using Map = dftracer::utils::InternedStringViewMap<ArgsValue>;
    Map data_;
    bool valid_ = false;

    static dftracer::utils::StringIntern& key_intern() {
        static dftracer::utils::StringIntern instance;
        return instance;
    }

   public:
    ArgsMap() = default;

    bool exists() const { return valid_; }
    explicit operator bool() const { return valid_; }

    void set_valid(bool v) { valid_ = v; }

    void insert(std::string_view key, ArgsValue value) {
        // Store the interned view directly (permanent storage); no key copy.
        data_.emplace(key_intern().intern(key), std::move(value));
    }

    void clear() {
        data_.clear();
        valid_ = false;
    }

    ArgsValueProxy operator[](std::string_view key) const {
        if (!valid_) return ArgsValueProxy{};
        auto it = data_.find(key);
        return it != data_.end() ? ArgsValueProxy{&it->second}
                                 : ArgsValueProxy{};
    }

    ArgsValueProxy operator[](const char* key) const {
        return (*this)[std::string_view(key)];
    }

    ArgsValueProxy operator[](const std::string& key) const {
        return (*this)[std::string_view(key)];
    }

    ArgsValueProxy at(const char* key) const { return (*this)[key]; }
    ArgsValueProxy at(const std::string& key) const { return (*this)[key]; }
    ArgsValueProxy at(std::string_view key) const { return (*this)[key]; }

    template <typename Fn>
    void for_each_member(Fn&& fn) const {
        if (!valid_) return;
        for (const auto& [k, v] : data_) {
            fn(std::string_view(k), ArgsValueProxy{&v});
        }
    }

    const Map& raw() const { return data_; }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_ARGS_MAP_H
