#ifndef DFTRACER_UTILS_TRACE_VIEWS_AGG_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_AGG_FOLD_H

#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/event_source.h>

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Generic per-event field extraction over an EventSource, written once so it
// runs identically over a live simdjson element (DomSource) and the interned
// POD (PodSource).
namespace dftracer::utils::trace::views::detail {

inline void lower_ascii(std::string& s, std::size_t from) {
    for (std::size_t i = from; i < s.size(); ++i)
        s[i] =
            static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
}

// "size" is the io-cat-derived byte size, ret becoming size for POSIX/STDIO
// read|write, so aggregates match dfanalyzer.
template <class Src>
std::optional<double> derived_size_t(const Src& src) {
    namespace dfi = trace::internal;
    auto as_i64 = [&](std::string_view k) -> std::optional<std::int64_t> {
        if (auto n = src.number(k)) return static_cast<std::int64_t>(*n);
        return std::nullopt;
    };
    auto s = dfi::derive_io_size(src.value("cat"), src.value("name"),
                                 as_i64("size_sum"), as_i64("ret"),
                                 as_i64("image_size"));
    return s ? std::optional<double>(static_cast<double>(*s)) : std::nullopt;
}

template <class Src>
std::optional<double> agg_field_t(const Src& src, std::string_view field) {
    if (field == "size") return derived_size_t(src);
    if (auto n = src.number(field)) return n;
    if (field == "te") {
        auto ts = src.number("ts");
        auto dur = src.number("dur");
        if (ts && dur) return *ts + *dur;
    }
    return std::nullopt;
}

// Like agg_field_t but keeps the value's numeric domain (so integer fields stay
// exact). Derived fields (size, te) are computed in the U64 domain to match the
// tier, which stores them as uint64.
template <class Src>
std::optional<dftracer::utils::dataframe::FieldNum> agg_field_typed_t(
    const Src& src, std::string_view field) {
    if (field == "size") {
        if (auto s = derived_size_t(src))
            return dftracer::utils::dataframe::FieldNum::of(
                static_cast<std::uint64_t>(*s));
        return std::nullopt;
    }
    if (auto n = src.number_typed(field)) return n;
    if (field == "te") {
        auto ts = src.number_typed("ts");
        auto dur = src.number_typed("dur");
        if (ts && dur)
            return dftracer::utils::dataframe::FieldNum::of(ts->as_u64() +
                                                            dur->as_u64());
    }
    return std::nullopt;
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_AGG_FOLD_H
