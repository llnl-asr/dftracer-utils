#ifndef DFTRACER_UTILS_INDEX_SCHEMA_CLASS_H
#define DFTRACER_UTILS_INDEX_SCHEMA_CLASS_H

#include <dftracer/utils/index/record_schema.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::index {

/// A string literal usable as a template argument.
template <std::size_t N>
struct FixedString {
    char value[N]{};
    constexpr FixedString(const char (&s)[N]) { std::copy_n(s, N, value); }
    constexpr std::string_view view() const { return {value, N - 1}; }
};

/// Field options: index past the path budget, or play a trace role.
struct AlwaysIndex {};
template <TimeUnit UNIT = TimeUnit::US>
struct TimeRole {};
template <TimeUnit UNIT = TimeUnit::US>
struct DurationRole {};
struct EntityRole {};
struct LaneRole {};
struct NameRole {};

/// A json field's value: canonical JSON text.
struct Json {
    std::string text;
};

/// One field of a schema class: a value of `T` (bool, an integer, a floating
/// point type, std::string, Json, or std::optional of one for an optional
/// field) at the JSON path `PATH`, with `Options`.
template <typename T, FixedString PATH, typename... Options>
struct Field {
    T value{};

    operator const T&() const { return value; }
    const T& operator*() const { return value; }
    const T* operator->() const { return &value; }
};

namespace schemas {
/// The built-in schemas, for `using extends = ...`.
struct DFTracer {
    static constexpr std::string_view id = "dftracer";
};
struct Generic {
    static constexpr std::string_view id = "generic";
};
}  // namespace schemas

namespace detail {

template <typename T>
struct IsField : std::false_type {};
template <typename T, FixedString PATH, typename... Options>
struct IsField<Field<T, PATH, Options...>> : std::true_type {};

template <typename T>
struct Unwrap {
    using type = T;
    static constexpr bool OPTIONAL = false;
};
template <typename T>
struct Unwrap<std::optional<T>> {
    using type = T;
    static constexpr bool OPTIONAL = true;
};

template <typename T>
consteval FieldType field_type() {
    if constexpr (std::same_as<T, bool>)
        return FieldType::BOOL;
    else if constexpr (std::integral<T>)
        return FieldType::INT;
    else if constexpr (std::floating_point<T>)
        return FieldType::FLOAT;
    else if constexpr (std::same_as<T, std::string>)
        return FieldType::STRING;
    else if constexpr (std::same_as<T, Json>)
        return FieldType::JSON;
    else
        static_assert(sizeof(T) == 0,
                      "a schema field holds bool, an integer, a floating "
                      "point type, std::string, Json or std::optional of one");
}

template <FieldType TYPE>
inline constexpr bool NUMERIC =
    TYPE == FieldType::INT || TYPE == FieldType::FLOAT;

template <FieldType TYPE>
void set_option(index::FieldSpec& f, AlwaysIndex*) {
    f.always_index = true;
}
template <FieldType TYPE>
void set_option(index::FieldSpec& f, EntityRole*) {
    f.role = Role::ENTITY;
}
template <FieldType TYPE>
void set_option(index::FieldSpec& f, LaneRole*) {
    f.role = Role::LANE;
}
template <FieldType TYPE>
void set_option(index::FieldSpec& f, NameRole*) {
    f.role = Role::NAME;
}
template <FieldType TYPE, TimeUnit UNIT>
void set_option(index::FieldSpec& f, TimeRole<UNIT>*) {
    static_assert(NUMERIC<TYPE>,
                  "a time field is an integer or floating point");
    f.role = Role::TIME;
    f.unit = UNIT;
}
template <FieldType TYPE, TimeUnit UNIT>
void set_option(index::FieldSpec& f, DurationRole<UNIT>*) {
    static_assert(NUMERIC<TYPE>,
                  "a duration field is an integer or floating point");
    f.role = Role::DURATION;
    f.unit = UNIT;
}

template <typename F>
struct FieldOf;
template <typename T, FixedString PATH, typename... Options>
struct FieldOf<Field<T, PATH, Options...>> {
    static index::FieldSpec make() {
        constexpr FieldType TYPE = field_type<typename Unwrap<T>::type>();
        index::FieldSpec f;
        f.name = std::string(PATH.view());
        f.path = f.name;
        f.type = TYPE;
        f.optional = Unwrap<T>::OPTIONAL;
        (set_option<TYPE>(f, static_cast<Options*>(nullptr)), ...);
        return f;
    }
};

// Converts to any member type, for counting an aggregate's members.
struct AnyMember {
    template <typename U>
    operator U() const;
};

template <typename T, typename... A>
consteval std::size_t member_count() {
    if constexpr (requires { T{A{}..., AnyMember{}}; })
        return member_count<T, A..., AnyMember>();
    else
        return sizeof...(A);
}

// Converts to the member it initializes, recording that member's field.
struct MemberProbe {
    std::vector<index::FieldSpec>* out;
    template <typename U>
    operator U() const {
        static_assert(IsField<U>::value,
                      "schema class members must be index::Field<...>");
        out->push_back(FieldOf<U>::make());
        return U{};
    }
};

template <std::size_t>
MemberProbe probe(std::vector<index::FieldSpec>& out) {
    return {&out};
}

template <typename T, std::size_t... I>
void record_fields(std::vector<index::FieldSpec>& out,
                   std::index_sequence<I...>) {
    // Braced initializers run left to right, so fields keep member order.
    [[maybe_unused]] const T members{probe<I>(out)...};
}

}  // namespace detail

/// The spec of schema class `T`: an aggregate whose members are all
/// index::Field, with a `static constexpr std::string_view id`, optionally
/// `using extends =` another schema class (schemas::Generic when absent)
/// and optionally a `static constexpr std::string_view SOURCE` holding its
/// duql source members. A member of another type does not compile.
template <typename T>
SchemaSpec schema_spec() {
    static_assert(std::is_aggregate_v<T>, "a schema class is an aggregate");
    SchemaSpec spec;
    spec.id = std::string(T::id);
    if constexpr (requires { typename T::extends; })
        spec.extends = std::string(T::extends::id);
    if constexpr (requires { T::SOURCE; }) spec.source = std::string(T::SOURCE);
    detail::record_fields<T>(
        spec.fields, std::make_index_sequence<detail::member_count<T>()>{});
    return spec;
}

/// Registers schema class `T` (see schema_spec) under `source`; throws as
/// register_schema(const SchemaSpec&, std::string_view).
template <typename T>
const RecordSchema& register_schema(std::string_view source) {
    return register_schema(schema_spec<T>(), source);
}

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_SCHEMA_CLASS_H
