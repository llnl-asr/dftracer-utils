#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/to_chars.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/// Line formatting for GzipLineWriter::append_fmt, with the format fixed at
/// compile time (FixedString) or parsed once at run time (LineFormat).
/// Only the two characters `{}` are a placeholder and take the next argument;
/// every other brace is literal, so JSON braces need no doubling, and `{{}}` is
/// a literal `{}`. The format must not contain a newline or carriage return:
/// the writer adds the line's '\n'. Integers and floats print in shortest
/// round-trip form, bool as true/false, strings through the Escape policy
/// (PlainEscape copies them; see json_line_format.h for JsonEscape), `raw(sv)`
/// unescaped. Argument types are checked at compile time (concept LineArg) in
/// both forms; the count is checked at compile time for FixedString and at run
/// time for LineFormat.
namespace dftracer::utils::utilities::fileio::line_format {

template <std::size_t N>
struct FixedString {
    char v[N]{};
    consteval FixedString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) v[i] = s[i];
    }
    /// False when the format holds a newline or carriage return; the
    /// templates below require it, so such a format fails to compile.
    constexpr bool line_safe() const {
        for (std::size_t i = 0; i < N; ++i)
            if (v[i] == '\n' || v[i] == '\r') return false;
        return true;
    }
};

/// Escape policy: `bound(s)` is the most bytes `write(dst, s)` may write.
struct PlainEscape {
    static std::size_t bound(std::string_view s) { return s.size(); }
    static char* write(char* d, std::string_view s) {
        std::memcpy(d, s.data(), s.size());
        return d + s.size();
    }
};

/// Copied without escaping, whatever the policy.
struct Raw {
    std::string_view s;
};
inline Raw raw(std::string_view s) { return {s}; }

namespace detail {

template <class T>
inline constexpr bool IS_INT =
    std::is_integral_v<T> && !std::is_same_v<T, bool> &&
    !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
    !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t>;

template <class T>
inline constexpr bool IS_FLOAT =
    std::is_same_v<T, float> || std::is_same_v<T, double>;

template <class T>
inline constexpr bool IS_STR =
    std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string> ||
    std::is_same_v<T, const char*>;

template <class T>
inline constexpr bool LINE_ARG =
    IS_INT<T> || IS_FLOAT<T> || std::is_same_v<T, bool> || IS_STR<T> ||
    std::is_same_v<T, Raw>;

constexpr std::size_t INT_MAX_CHARS = 24;
constexpr std::size_t FLOAT_MAX_CHARS = 32;

/// Walks `f`; `lit(c)` gets each literal char and `hole()` each `{}`. The
/// four characters `{{}}` are the literal `{}`; every other brace is literal.
/// Returns an error text, or nullptr.
template <class Lit, class Hole>
constexpr const char* scan(std::string_view f, Lit&& lit, Hole&& hole) {
    for (std::size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (c == '\n' || c == '\r')
            return "format must not contain a newline; the writer adds it";
        if (f.compare(i, 4, "{{}}") == 0) {
            lit('{');
            lit('}');
            i += 3;
        } else if (f.compare(i, 2, "{}") == 0) {
            hole();
            ++i;
        } else {
            lit(c);
        }
    }
    return nullptr;
}

/// A split format: piece i is text[i == 0 ? 0 : end[i - 1] .. end[i]).
struct View {
    const char* text;
    const std::size_t* end;
    std::size_t holes;
};

template <std::size_t N, std::size_t H>
struct Pieces {
    char text[N]{};
    std::size_t end[H + 1]{};
    std::size_t total = 0;
};

template <std::size_t N>
consteval std::size_t count_holes(const FixedString<N>& f) {
    std::size_t holes = 0;
    if (const char* err =
            scan(std::string_view(f.v, N - 1), [](char) {}, [&] { ++holes; }))
        throw err;
    return holes;
}

template <std::size_t H, std::size_t N>
consteval Pieces<N, H> split(const FixedString<N>& f) {
    Pieces<N, H> p;
    std::size_t hole = 0;
    scan(
        std::string_view(f.v, N - 1), [&](char c) { p.text[p.total++] = c; },
        [&] { p.end[hole++] = p.total; });
    p.end[H] = p.total;
    return p;
}

inline bool is_null(const char* p) { return p == nullptr; }
template <class T>
bool is_null(const T&) {
    return false;
}

template <class E, class T>
std::size_t arg_bound(const T& v) {
    if constexpr (IS_INT<T>)
        return INT_MAX_CHARS;
    else if constexpr (IS_FLOAT<T>)
        return FLOAT_MAX_CHARS;
    else if constexpr (std::is_same_v<T, bool>)
        return 5;
    else if constexpr (std::is_same_v<T, Raw>)
        return v.s.size();
    else
        return E::bound(std::string_view(v));
}

template <class E, class T>
char* put_arg(char* d, const T& v) {
    static_assert(LINE_ARG<T>, "unsupported line argument type");
    if constexpr (IS_INT<T>) {
        return std::to_chars(d, d + INT_MAX_CHARS, v).ptr;
    } else if constexpr (IS_FLOAT<T>) {
#ifdef DFTRACER_UTILS_FP_TO_CHARS_UNAVAILABLE
        return to_chars_double(d, d + FLOAT_MAX_CHARS, static_cast<double>(v));
#else
        return std::to_chars(d, d + FLOAT_MAX_CHARS, v).ptr;
#endif
    } else if constexpr (std::is_same_v<T, bool>) {
        const std::string_view t = v ? "true" : "false";
        std::memcpy(d, t.data(), t.size());
        return d + t.size();
    } else if constexpr (std::is_same_v<T, Raw>) {
        std::memcpy(d, v.s.data(), v.s.size());
        return d + v.s.size();
    } else {
        return E::write(d, std::string_view(v));
    }
}

template <class E, class... A>
std::size_t bound(const View& f, const A&... a) {
    return f.end[f.holes] + 1 + (std::size_t{0} + ... + arg_bound<E>(a));
}

/// `f.holes == sizeof...(a)`; `dst` has room for bound(f, a...).
template <class E, class... A>
std::size_t write(const View& f, char* dst, const A&... a) {
    char* d = dst;
    std::size_t hole = 0;
    const auto piece = [&](std::size_t i) {
        const std::size_t b = i == 0 ? 0 : f.end[i - 1];
        std::memcpy(d, f.text + b, f.end[i] - b);
        d += f.end[i] - b;
    };
    ((piece(hole++), d = put_arg<E>(d, a)), ...);
    piece(hole);
    *d++ = '\n';
    return static_cast<std::size_t>(d - dst);
}

template <FixedString Fmt>
inline constexpr std::size_t HOLES = count_holes(Fmt);

template <FixedString Fmt>
inline constexpr auto PIECES = split<HOLES<Fmt>>(Fmt);

template <FixedString Fmt>
constexpr View view() {
    return {PIECES<Fmt>.text, PIECES<Fmt>.end, HOLES<Fmt>};
}

}  // namespace detail

/// What append_fmt accepts: integers (not char types or bool), bool, float,
/// double, std::string_view, std::string, const char* (null is a returned
/// error) and Raw. Anything else, such as char, other pointers, nullptr and
/// enums, is rejected at compile time; cast explicitly.
template <class T>
concept LineArg = detail::LINE_ARG<std::decay_t<const T>>;

/// A format parsed once at run time, reused for many lines. `Escape` ties it
/// to the append_* methods that use the same policy.
template <class Escape>
class BasicLineFormat {
   public:
    /// INVALID_ARGUMENT for a format containing a newline or carriage return.
    static Result<BasicLineFormat> parse(std::string_view fmt) {
        BasicLineFormat f;
        const char* err = detail::scan(
            fmt, [&](char c) { f.text_.push_back(c); },
            [&] { f.end_.push_back(f.text_.size()); });
        if (err)
            return make_error(ErrorCode::INVALID_ARGUMENT,
                              std::string("line format: ") + err);
        f.end_.push_back(f.text_.size());
        return f;
    }

    std::size_t holes() const { return end_.size() - 1; }
    detail::View view() const { return {text_.data(), end_.data(), holes()}; }

   private:
    BasicLineFormat() = default;
    std::string text_;
    std::vector<std::size_t> end_;
};

template <class E, LineArg... A>
std::size_t max_line_bytes(const detail::View& f, const A&... a) {
    return detail::bound<E>(f, std::decay_t<const A>(a)...);
}

/// Writes the line to `dst`, which has room for max_line_bytes(f, a...), and
/// returns the bytes written. `f.holes` must equal sizeof...(A).
template <class E, LineArg... A>
std::size_t format_line(const detail::View& f, char* dst, const A&... a) {
    return detail::write<E>(f, dst, std::decay_t<const A>(a)...);
}

template <FixedString Fmt>
    requires(Fmt.line_safe())
constexpr detail::View view() {
    return detail::view<Fmt>();
}

template <LineArg... A>
bool any_null(const A&... a) {
    return (detail::is_null(a) || ...);
}

using LineFormat = BasicLineFormat<PlainEscape>;

}  // namespace dftracer::utils::utilities::fileio::line_format

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINE_FORMAT_H
