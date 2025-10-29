#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_TRAITS_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_TRAITS_H

#include <type_traits>

namespace dftracer::utils {
// Forward declaration
class TaskContext;
}  // namespace dftracer::utils

namespace dftracer::utils::utilities {

namespace detail {

/**
 * @brief SFINAE trait to detect if a type has process(I, TaskContext&) method.
 *
 * Primary template (false case) - used when the method doesn't exist.
 */
template <typename T, typename I, typename O, typename = void>
struct has_process_with_context_impl : std::false_type {};

/**
 * @brief Specialization for types that have process(I, TaskContext&) method.
 *
 * Uses std::void_t to detect if the expression is valid. If it compiles,
 * this specialization is chosen and inherits from std::true_type.
 */
template <typename T, typename I, typename O>
struct has_process_with_context_impl<
    T, I, O,
    std::void_t<decltype(std::declval<T&>().process(
        std::declval<const I&>(), std::declval<TaskContext&>()))>>
    : std::true_type {};

/**
 * @brief Convenience variable template for has_process_with_context.
 *
 * Usage:
 * @code
 * if constexpr (detail::has_process_with_context_v<MyUtility, Input, Output>) {
 *     // Utility has process(I, TaskContext&)
 * }
 * @endcode
 */
template <typename T, typename I, typename O>
inline constexpr bool has_process_with_context_v =
    has_process_with_context_impl<T, I, O>::value;

/**
 * @brief SFINAE trait to detect if a type has process(I) method.
 *
 * Primary template (false case) - used when the method doesn't exist.
 */
template <typename T, typename I, typename O, typename = void>
struct has_process_impl : std::false_type {};

/**
 * @brief Specialization for types that have process(I) method.
 *
 * Uses std::void_t to detect if the expression is valid.
 */
template <typename T, typename I, typename O>
struct has_process_impl<
    T, I, O,
    std::void_t<decltype(std::declval<T&>().process(std::declval<const I&>()))>>
    : std::true_type {};

/**
 * @brief Convenience variable template for has_process.
 *
 * Usage:
 * @code
 * if constexpr (detail::has_process_v<MyUtility, Input, Output>) {
 *     // Utility has process(I)
 * }
 * @endcode
 */
template <typename T, typename I, typename O>
inline constexpr bool has_process_v = has_process_impl<T, I, O>::value;

/**
 * @brief SFINAE trait to detect if a utility has a specific tag.
 *
 * This checks if Tag exists in the Utility's TagsTuple.
 */
template <typename Tag, typename TagsTuple>
struct has_tag_in_tuple;

// Base case: empty tuple
template <typename Tag>
struct has_tag_in_tuple<Tag, std::tuple<>> : std::false_type {};

// Recursive case: check first element, then rest
template <typename Tag, typename First, typename... Rest>
struct has_tag_in_tuple<Tag, std::tuple<First, Rest...>>
    : std::conditional_t<std::is_same_v<Tag, First>, std::true_type,
                         has_tag_in_tuple<Tag, std::tuple<Rest...>>> {};

/**
 * @brief Convenience variable template for has_tag_in_tuple.
 */
template <typename Tag, typename TagsTuple>
inline constexpr bool has_tag_in_tuple_v =
    has_tag_in_tuple<Tag, TagsTuple>::value;

// SFINAE helper to detect if a type has TagsTuple
template <typename T, typename = void>
struct has_tags_tuple : std::false_type {};

template <typename T>
struct has_tags_tuple<T, std::void_t<typename T::TagsTuple>> : std::true_type {
};

template <typename T>
inline constexpr bool has_tags_tuple_v = has_tags_tuple<T>::value;

}  // namespace detail

/**
 * @brief Check if a Utility type has a specific tag.
 *
 * Usage:
 * @code
 * if constexpr (has_tag<tags::Parallelizable, MyUtility>()) {
 *     // Utility is parallelizable
 * }
 * @endcode
 */
template <typename Tag, typename UtilityType>
constexpr bool has_tag() {
    if constexpr (detail::has_tags_tuple_v<UtilityType>) {
        return detail::has_tag_in_tuple_v<Tag, typename UtilityType::TagsTuple>;
    } else {
        return false;
    }
}

/**
 * @brief Check if a utility instance has a specific tag.
 *
 * Usage:
 * @code
 * auto utility = std::make_shared<MyUtility>();
 * if constexpr (has_tag_v<tags::Parallelizable, decltype(*utility)>) {
 *     // Utility is parallelizable
 * }
 * @endcode
 */
template <typename Tag, typename UtilityType>
inline constexpr bool has_tag_v = has_tag<Tag, UtilityType>();

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITY_TRAITS_H
