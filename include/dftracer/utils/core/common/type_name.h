#ifndef DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H
#define DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H

#include <string>
#include <typeinfo>

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
 * @brief Extract just the class name from a fully qualified type name.
 *
 * Example: "namespace::ClassName<int>" -> "ClassName"
 *
 * @param full_name Fully qualified type name
 * @return Simple class name
 */
inline std::string extract_class_name(const std::string& full_name) {
    // Find last :: to get class name without namespace
    std::size_t last_colon = full_name.rfind("::");
    std::string name = (last_colon != std::string::npos)
                           ? full_name.substr(last_colon + 2)
                           : full_name;

    // Remove template parameters if any
    std::size_t template_start = name.find('<');
    if (template_start != std::string::npos) {
        name = name.substr(0, template_start);
    }

    return name;
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TYPE_NAME_H
