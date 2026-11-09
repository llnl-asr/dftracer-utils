// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <type_traits>

/// Line formatting for GzipLineWriter::append_fmt on the engine of
/// @ref dstd::fmt, with the format fixed at compile time (@ref
/// dstd::FixedString) or parsed once at run time (LineFormat). Only the two
/// characters `{}` are a placeholder and take the next argument; every other
/// brace is literal, so JSON braces need no doubling, and `{{}}` is a literal
/// `{}`. A line format takes no spec and must not contain a newline or carriage
/// return: the writer adds the line's '\n'. Integers and floats print in
/// shortest round-trip form, bool as true/false, strings through the Escape
/// policy (@ref PlainEscape copies them; see json_line_format.h for @ref
/// JsonEscape), `raw(sv)` unescaped. Argument types are checked at compile time
/// (concept LineArg) in both forms; the count is checked at compile time for
/// FixedString and at run time for LineFormat.
namespace dftracer::utils::utilities::fileio::line_format {

using dstd::fmt_detail::PlainEscape;

/// Copied without escaping, whatever the policy.
using Raw = dstd::fmt_detail::RawText;
inline Raw raw(dstd::StringView s) { return {s}; }

namespace detail {

constexpr bool has_line_break(dstd::StringView f) {
    for (const char c : f)
        if (c == '\n' || c == '\r') return true;
    return false;
}

template <dstd::FixedString Fmt>
consteval bool line_safe() {
    return dstd::fmt_detail::COUNTED<Fmt>.ok &&
           !has_line_break(dstd::StringView(Fmt.v, sizeof(Fmt.v) - 1)) &&
           dstd::fmt_detail::all_plain<Fmt>();
}

using dstd::fmt_detail::View;

inline bool is_null(const char* p) { return p == nullptr; }
inline bool is_null(char* p) { return p == nullptr; }
template <class T>
bool is_null(const T&) {
    return false;
}

}  // namespace detail

/// What append_fmt accepts: integers (not char types or bool), bool, float,
/// double, strings (@ref dstd::StringView, std::string, const char*; null is a
/// returned error) and Raw. Anything else, such as char, other pointers,
/// nullptr and enums, is rejected at compile time; cast explicitly.
template <class T>
concept LineArg =
    dstd::FmtArg<T> && !std::is_same_v<std::decay_t<const T>, char>;

/// A format parsed once at run time, reused for many lines. `Escape` ties it
/// to the append_* methods that use the same policy.
template <class Escape>
class BasicLineFormat {
   public:
    /// INVALID_ARGUMENT for a format containing a newline or carriage return.
    static dstd::Result<BasicLineFormat> parse(dstd::StringView fmt) {
        BasicLineFormat f;
        if (detail::has_line_break(fmt))
            return common::make_error<
                "line format: must not contain a newline; the writer adds it">(
                common::ErrorCode::InvalidArgument);
        bool plain = true;
        const bool ok = dstd::fmt_detail::scan(
            fmt, [&](char c) { f.text_.push_back(c); },
            [&](dstd::fmt_detail::Spec spec) {
                plain = plain && spec.plain();
                f.end_.push_back(f.text_.size());
            });
        if (!ok || !plain)
            return common::make_error<"line format: a hole takes no spec">(
                common::ErrorCode::InvalidArgument);
        f.end_.push_back(f.text_.size());
        return f;
    }

    std::size_t holes() const { return end_.size() - 1; }
    detail::View view() const { return {text_.data(), end_.data(), holes()}; }

   private:
    BasicLineFormat() = default;
    dstd::String text_;
    dstd::Vector<std::size_t> end_;
};

template <class E, LineArg... A>
std::size_t max_line_bytes(const detail::View& f, const A&... a) {
    return dstd::fmt_detail::bound<E>(f, std::decay_t<const A>(a)...) + 1;
}

/// Writes the line to `dst`, which has room for max_line_bytes(f, a...), and
/// returns the bytes written. `f.holes` must equal sizeof...(A).
template <class E, LineArg... A>
std::size_t format_line(const detail::View& f, char* dst, const A&... a) {
    char* d = dstd::fmt_detail::write<E>(f, dst, std::decay_t<const A>(a)...);
    *d++ = '\n';
    return static_cast<std::size_t>(d - dst);
}

/// A format for the append_* templates: one that holds no line break and no
/// spec.
template <dstd::FixedString Fmt>
    requires(detail::line_safe<Fmt>())
constexpr detail::View view() {
    return {dstd::fmt_detail::PIECES<Fmt>.text,
            dstd::fmt_detail::PIECES<Fmt>.end, dstd::FMT_HOLES<Fmt>};
}

template <LineArg... A>
bool any_null(const A&... a) {
    return (detail::is_null(a) || ...);
}

using LineFormat = BasicLineFormat<PlainEscape>;

}  // namespace dftracer::utils::utilities::fileio::line_format

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H
