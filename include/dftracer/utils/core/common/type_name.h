#ifndef DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H
#define DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H

#include <functional>
#include <string>
#include <typeinfo>
#include <utility>

#ifdef __GNUG__
#include <cxxabi.h>

#include <cstdlib>
#include <memory>
#endif

namespace dftracer::utils {

/**
 * @brief Get a demangled type name for a given type.
 *
 * On GCC/Clang, uses __cxa_demangle to get readable names.
 * On other compilers, returns typeid(T).name() as-is.
 *
 * @tparam T The type to get the name for
 * @return Demangled type name string
 */
template <typename T>
std::string get_type_name() {
#ifdef __GNUG__
    // GCC/Clang - use cxxabi to demangle
    int status = -1;
    std::unique_ptr<char, void (*)(void*)> res{
        abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status),
        std::free};

    return (status == 0) ? res.get() : typeid(T).name();
#else
    // MSVC or other - just return name() as-is
    // MSVC already returns readable names
    return typeid(T).name();
#endif
}

/**
 * @brief Get a demangled type name for a given object.
 *
 * @param obj Reference to object
 * @return Demangled type name string
 */
template <typename T>
std::string get_type_name(const T& obj) {
#ifdef __GNUG__
    int status = -1;
    std::unique_ptr<char, void (*)(void*)> res{
        abi::__cxa_demangle(typeid(obj).name(), nullptr, nullptr, &status),
        std::free};

    return (status == 0) ? res.get() : typeid(obj).name();
#else
    return typeid(obj).name();
#endif
}

/**
 * @brief Extract template argument from a type string.
 *
 * Example: "std::vector<int, std::allocator<int>>" -> "int"
 */
inline std::string extract_first_template_arg(const std::string& full_name) {
    std::size_t start = full_name.find('<');
    if (start == std::string::npos) return "";

    start++;  // Skip '<'
    int depth = 1;
    std::size_t end = start;

    // Find matching '>' accounting for nested templates
    while (end < full_name.length() && depth > 0) {
        if (full_name[end] == '<') {
            depth++;
            end++;
        } else if (full_name[end] == '>') {
            depth--;
            if (depth > 0) end++;  // Only advance if not done
        } else if (full_name[end] == ',' && depth == 1) {
            break;                 // First arg ends at comma
        } else {
            end++;
        }
    }

    if (end > start) {
        std::string arg = full_name.substr(start, end - start);
        // Trim whitespace
        size_t first = arg.find_first_not_of(" \t");
        size_t last = arg.find_last_not_of(" \t");
        if (first != std::string::npos && last != std::string::npos) {
            return arg.substr(first, last - first + 1);
        }
    }
    return "";
}

/**
 * @brief Extract both key and value types from map-like containers.
 *
 * Example: "std::map<int, string>" -> {"int", "string"}
 */
inline std::pair<std::string, std::string> extract_map_template_args(
    const std::string& full_name) {
    std::size_t start = full_name.find('<');
    if (start == std::string::npos) return {"", ""};

    start++;  // Skip '<'

    // Find first argument (key type)
    int depth = 1;
    std::size_t comma_pos = start;
    while (comma_pos < full_name.length() && depth > 0) {
        if (full_name[comma_pos] == '<')
            depth++;
        else if (full_name[comma_pos] == '>')
            depth--;
        else if (full_name[comma_pos] == ',' && depth == 1)
            break;
        comma_pos++;
    }

    std::string key_type = full_name.substr(start, comma_pos - start);

    // Find second argument (value type)
    if (comma_pos < full_name.length() && full_name[comma_pos] == ',') {
        comma_pos++;  // Skip comma
        // Skip whitespace
        while (comma_pos < full_name.length() &&
               (full_name[comma_pos] == ' ' || full_name[comma_pos] == '\t')) {
            comma_pos++;
        }

        depth = 1;
        std::size_t value_end = comma_pos;
        while (value_end < full_name.length() && depth > 0) {
            if (full_name[value_end] == '<')
                depth++;
            else if (full_name[value_end] == '>')
                depth--;
            else if (full_name[value_end] == ',' && depth == 1)
                break;
            value_end++;
        }

        std::string value_type =
            full_name.substr(comma_pos, value_end - comma_pos);

        // Trim whitespace from both
        auto trim = [](std::string& s) {
            size_t first = s.find_first_not_of(" \t");
            size_t last = s.find_last_not_of(" \t");
            if (first != std::string::npos && last != std::string::npos) {
                s = s.substr(first, last - first + 1);
            }
        };
        trim(key_type);
        trim(value_type);

        return {key_type, value_type};
    }

    return {"", ""};
}

/**
 * @brief Extract just the class name from a fully qualified type name with
 * smart container formatting.
 *
 * Examples:
 *   "std::vector<int>" -> "V[int]"
 *   "std::map<int, string>" -> "M[int,string]"
 *   "namespace::ClassName<int>" -> "ClassName"
 *
 * @param full_name Fully qualified type name
 * @return Formatted class name with container notation
 */
inline std::string extract_class_name(const std::string& full_name) {
    // Recursively process template arguments
    std::function<std::string(const std::string&)> process_type;
    process_type = [&](const std::string& type_name) -> std::string {
        // Handle basic string types
        if (type_name.find("std::basic_string") != std::string::npos ||
            type_name.find("std::__cxx11::basic_string") != std::string::npos) {
            return "string";
        }

        // Handle vector: V[T]
        if (type_name.find("std::vector") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "V[" + process_type(elem) + "]";
            }
            return "vector";
        }

        // Handle map: M[K,V]
        if (type_name.find("std::map") != std::string::npos &&
            type_name.find("std::unordered_map") == std::string::npos) {
            auto [key, val] = extract_map_template_args(type_name);
            if (!key.empty() && !val.empty()) {
                return "M[" + process_type(key) + "," + process_type(val) + "]";
            }
            return "map";
        }

        // Handle unordered_map: UM[K,V]
        if (type_name.find("std::unordered_map") != std::string::npos) {
            auto [key, val] = extract_map_template_args(type_name);
            if (!key.empty() && !val.empty()) {
                return "UM[" + process_type(key) + "," + process_type(val) +
                       "]";
            }
            return "unordered_map";
        }

        // Handle set: S[T]
        if (type_name.find("std::set") != std::string::npos &&
            type_name.find("std::unordered_set") == std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "S[" + process_type(elem) + "]";
            }
            return "set";
        }

        // Handle unordered_set: US[T]
        if (type_name.find("std::unordered_set") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "US[" + process_type(elem) + "]";
            }
            return "unordered_set";
        }

        // Handle list: L[T]
        if (type_name.find("std::list") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "L[" + process_type(elem) + "]";
            }
            return "list";
        }

        // Handle array: A[T]
        if (type_name.find("std::array") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "A[" + process_type(elem) + "]";
            }
            return "array";
        }

        // Handle shared_ptr: SP[T]
        if (type_name.find("std::shared_ptr") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "SP[" + process_type(elem) + "]";
            }
            return "shared_ptr";
        }

        // Handle unique_ptr: UP[T]
        if (type_name.find("std::unique_ptr") != std::string::npos) {
            std::string elem = extract_first_template_arg(type_name);
            if (!elem.empty()) {
                return "UP[" + process_type(elem) + "]";
            }
            return "unique_ptr";
        }

        // Default: extract class name without namespace and templates
        std::size_t last_colon = type_name.rfind("::");
        std::string name = (last_colon != std::string::npos)
                               ? type_name.substr(last_colon + 2)
                               : type_name;

        // Remove template parameters
        std::size_t template_start = name.find('<');
        if (template_start != std::string::npos) {
            name = name.substr(0, template_start);
        }

        return name;
    };

    return process_type(full_name);
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H
