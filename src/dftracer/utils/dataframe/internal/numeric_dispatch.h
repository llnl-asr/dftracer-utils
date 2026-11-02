// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NUMERIC_DISPATCH_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NUMERIC_DISPATCH_H

// Dispatch a numeric TypeId to FN<T>(...) over untyped buffers. Bool/String/
// Binary are not numeric and hit the default (no-op) case.
#define DF_NUMERIC_DISPATCH(TYPE, FN, ...)                \
    switch (TYPE) {                                       \
        case dftracer::utils::dataframe::TypeId::Int8:    \
            FN<std::int8_t>(__VA_ARGS__);                 \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Int16:   \
            FN<std::int16_t>(__VA_ARGS__);                \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Int32:   \
            FN<std::int32_t>(__VA_ARGS__);                \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Int64:   \
            FN<std::int64_t>(__VA_ARGS__);                \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Uint8:   \
            FN<std::uint8_t>(__VA_ARGS__);                \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Uint16:  \
            FN<std::uint16_t>(__VA_ARGS__);               \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Uint32:  \
            FN<std::uint32_t>(__VA_ARGS__);               \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Uint64:  \
            FN<std::uint64_t>(__VA_ARGS__);               \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Float32: \
            FN<float>(__VA_ARGS__);                       \
            break;                                        \
        case dftracer::utils::dataframe::TypeId::Float64: \
            FN<double>(__VA_ARGS__);                      \
            break;                                        \
        default:                                          \
            break;                                        \
    }

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NUMERIC_DISPATCH_H
