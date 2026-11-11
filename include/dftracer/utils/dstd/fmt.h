// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FMT_H
#define DFTRACER_UTILS_DSTD_FMT_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

/// Text formatting with the format string fixed in the type, so the split into
/// pieces and holes happens at compile time and a call computes one size bound
/// and writes once. Two call forms give the same code:
///   dstd::fmt<"open {}: {}">(path, err)
///   dstd::fmt("open {}: {}"_f, path, err)
/// Only the two characters `{}` and `{:spec}` are holes and take the next
/// argument; every other brace is literal, so JSON braces need no doubling, and
/// `{{}}` is a literal `{}`. A spec is one of `x`, `X` (hex), `0N` (zero-padded
/// width N up to 255, for integers and `f` floats), `.Nf` (fixed float with N
/// decimals up to 99, `f` alone is six); `{:0Nx}` and `{:0N.Pf}` combine them.
/// Integers and floats without a spec print in shortest round-trip form, bool
/// as true/false, char as itself, and strings (anything that views as a
/// @ref StringView, a `const char*` where null prints "(null)", or an object
/// with `str()` such as @ref Path) are copied. A wrong argument count, a bad
/// spec or an argument type that does not fit its hole does not compile.
namespace dftracer::utils::dstd {

/// A string literal as a template argument.
template <std::size_t N>
struct FixedString {
    char v[N]{};
    consteval FixedString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) v[i] = s[i];
    }
};

/// A format fixed in the type; `"..."_f` makes one.
template <FixedString S>
struct Format {};

/// What fmt_to reports: the bytes written and whether the text was cut.
struct FmtResult {
    std::size_t size;
    bool truncated;
};

namespace fmt_detail {

struct Spec {
    char type = 0;
    std::uint8_t width = 0;
    std::uint8_t prec = 0;
    constexpr bool plain() const { return type == 0 && width == 0; }
};

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// Parses the text between `{:` and `}`.
constexpr bool parse_spec(StringView s, Spec& out) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    Spec r;
    if (i < n && s[i] == '0') {
        ++i;
        unsigned width = 0;
        std::size_t digits = 0;
        while (i < n && is_digit(s[i])) {
            width = width * 10 + static_cast<unsigned>(s[i] - '0');
            if (width > 255) return false;
            ++digits;
            ++i;
        }
        if (digits == 0) return false;
        r.width = static_cast<std::uint8_t>(width);
    } else if (i < n && is_digit(s[i])) {
        return false;
    }
    bool has_prec = false;
    if (i < n && s[i] == '.') {
        ++i;
        unsigned prec = 0;
        std::size_t digits = 0;
        while (i < n && is_digit(s[i])) {
            prec = prec * 10 + static_cast<unsigned>(s[i] - '0');
            if (prec > 99) return false;
            ++digits;
            ++i;
        }
        if (digits == 0) return false;
        r.prec = static_cast<std::uint8_t>(prec);
        has_prec = true;
    }
    if (i < n && (s[i] == 'x' || s[i] == 'X' || s[i] == 'f')) r.type = s[i++];
    if (i != n) return false;
    if (has_prec && r.type != 'f') return false;
    if (r.type == 'f' && !has_prec) r.prec = 6;
    if (r.type == 0 && r.width == 0) return false;
    out = r;
    return true;
}

/// Walks `f`; `lit(c)` gets each literal char and `hole(spec)` each hole. The
/// four characters `{{}}` are the literal `{}`. False on a bad spec.
template <class Lit, class Hole>
constexpr bool scan(StringView f, Lit&& lit, Hole&& hole) {
    for (std::size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (f.compare(i, 4, "{{}}") == 0) {
            lit('{');
            lit('}');
            i += 3;
        } else if (f.compare(i, 2, "{}") == 0) {
            hole(Spec{});
            ++i;
        } else if (f.compare(i, 2, "{:") == 0) {
            const std::size_t close = f.find('}', i + 2);
            if (close == StringView::npos) return false;
            Spec spec;
            if (!parse_spec(f.substr(i + 2, close - i - 2), spec)) return false;
            hole(spec);
            i = close;
        } else {
            lit(c);
        }
    }
    return true;
}

/// A split format: piece i is text[i == 0 ? 0 : end[i - 1] .. end[i]).
template <std::size_t N, std::size_t H>
struct Pieces {
    char text[N]{};
    std::size_t end[H + 1]{};
    Spec spec[H + 1]{};
    std::size_t total = 0;
};

struct Counted {
    std::size_t holes;
    bool ok;
};

template <std::size_t N>
consteval Counted count_holes(const FixedString<N>& f) {
    std::size_t holes = 0;
    const bool ok =
        scan(StringView(f.v, N - 1), [](char) {}, [&](Spec) { ++holes; });
    return {holes, ok};
}

template <std::size_t H, std::size_t N>
consteval Pieces<N, H> split(const FixedString<N>& f) {
    Pieces<N, H> p;
    std::size_t hole = 0;
    scan(
        StringView(f.v, N - 1), [&](char c) { p.text[p.total++] = c; },
        [&](Spec s) {
            p.spec[hole] = s;
            p.end[hole++] = p.total;
        });
    p.end[H] = p.total;
    return p;
}

template <FixedString S>
inline constexpr Counted COUNTED = count_holes(S);

template <FixedString S>
inline constexpr auto PIECES = split<COUNTED<S>.holes>(S);

template <FixedString S>
consteval bool all_plain() {
    for (std::size_t i = 0; i < COUNTED<S>.holes; ++i)
        if (!PIECES<S>.spec[i].plain()) return false;
    return true;
}

/// A format known at compile time: every query is a constant.
template <FixedString S>
struct Static {
    static constexpr std::size_t total() { return PIECES<S>.total; }
    static constexpr const char* text(std::size_t i) {
        return PIECES<S>.text + (i == 0 ? 0 : PIECES<S>.end[i - 1]);
    }
    static constexpr std::size_t size(std::size_t i) {
        return PIECES<S>.end[i] - (i == 0 ? 0 : PIECES<S>.end[i - 1]);
    }
    static constexpr Spec spec(std::size_t i) { return PIECES<S>.spec[i]; }
};

/// A format split elsewhere, with no specs: a runtime format.
struct View {
    const char* base;
    const std::size_t* ends;
    std::size_t holes;

    std::size_t total() const { return ends[holes]; }
    const char* text(std::size_t i) const {
        return base + (i == 0 ? 0 : ends[i - 1]);
    }
    std::size_t size(std::size_t i) const {
        return ends[i] - (i == 0 ? 0 : ends[i - 1]);
    }
    static constexpr Spec spec(std::size_t) { return {}; }
};

/// A string argument that no escape policy touches.
struct RawText {
    StringView s;
};

/// Escape policy: `bound(s)` is the most bytes `write(dst, s)` may write.
struct PlainEscape {
    static std::size_t bound(StringView s) { return s.size(); }
    static char* write(char* d, StringView s) {
        std::memcpy(d, s.data(), s.size());
        return d + s.size();
    }
};

template <class T>
inline constexpr bool IS_INT =
    std::is_integral_v<T> && !std::is_same_v<T, bool> &&
    !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
    !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t> && sizeof(T) <= 8;

template <class T>
inline constexpr bool IS_FLOAT =
    std::is_same_v<T, float> || std::is_same_v<T, double>;

template <class T>
inline constexpr bool IS_POINTER_STR =
    std::is_same_v<T, const char*> || std::is_same_v<T, char*>;

template <class T>
concept StrLike =
    !std::is_same_v<T, std::nullptr_t> &&
    (std::is_convertible_v<const T&, StringView> || requires(const T& t) {
        { t.str() } -> std::convertible_to<StringView>;
    });

template <class T>
inline constexpr bool ARG =
    IS_INT<T> || IS_FLOAT<T> || std::is_same_v<T, bool> ||
    std::is_same_v<T, char> || StrLike<T> || std::is_same_v<T, RawText>;

template <class T>
constexpr bool spec_ok(Spec s) {
    if constexpr (IS_INT<T>)
        return s.type == 0 || s.type == 'x' || s.type == 'X';
    else if constexpr (IS_FLOAT<T>)
        return s.plain() || s.type == 'f';
    else
        return s.plain();
}

template <class T>
StringView to_view(const T& v) {
    if constexpr (IS_POINTER_STR<T> || std::is_array_v<T>) {
        const char* p = v;
        return p != nullptr ? StringView(p) : StringView("(null)");
    } else if constexpr (std::is_convertible_v<const T&, StringView>) {
        return StringView(v);
    } else {
        return StringView(v.str());
    }
}

/// Writes `mag` (and a leading '-' when `neg`) in the base and padding of
/// `s`; at most max(digits + 1, s.width) bytes. Returns the end.
char* write_integer(char* d, std::uint64_t mag, bool neg, Spec s) noexcept;

/// Writes `v` in fixed notation with `s.prec` decimals, zero-padded to
/// `s.width`, in the `room` bytes at `d`. Returns the end.
char* write_fixed(char* d, std::size_t room, double v, Spec s) noexcept;

template <class T>
constexpr std::size_t int_chars(Spec s) {
    constexpr std::size_t DEC = sizeof(T) == 1   ? 4
                                : sizeof(T) == 2 ? 6
                                : sizeof(T) == 4 ? 11
                                                 : 20;
    const std::size_t natural =
        (s.type == 'x' || s.type == 'X') ? 2 * sizeof(T) : DEC;
    return natural > s.width ? natural : s.width;
}

constexpr std::size_t SHORTEST_FLOAT_CHARS = 32;

inline std::size_t fixed_chars(double v, Spec s) {
    const std::size_t natural =
        (v < 1e15 && v > -1e15) ? 18 + s.prec : 312 + s.prec;
    return natural > s.width ? natural : s.width;
}

template <class E, class T>
std::size_t arg_bound(Spec s, const T& v) {
    using D = std::decay_t<const T>;
    if constexpr (IS_INT<D>)
        return int_chars<D>(s);
    else if constexpr (IS_FLOAT<D>)
        return s.type == 'f' ? fixed_chars(static_cast<double>(v), s)
                             : SHORTEST_FLOAT_CHARS;
    else if constexpr (std::is_same_v<D, bool>)
        return 5;
    else if constexpr (std::is_same_v<D, char>)
        return 1;
    else if constexpr (std::is_same_v<D, RawText>)
        return v.s.size();
    else
        return E::bound(to_view(v));
}

template <class E, class T>
char* put_arg(char* d, Spec s, const T& v) {
    using D = std::decay_t<const T>;
    if constexpr (IS_INT<D>) {
        if (s.plain()) return dstd::to_chars(d, d + int_chars<D>(s), v).ptr;
        if (s.type == 'x' || s.type == 'X')
            return write_integer(
                d, static_cast<std::uint64_t>(std::make_unsigned_t<D>(v)),
                false, s);
        if constexpr (std::is_signed_v<D>) {
            const bool neg = v < 0;
            const std::uint64_t mag =
                neg ? std::uint64_t{0} - static_cast<std::uint64_t>(v)
                    : static_cast<std::uint64_t>(v);
            return write_integer(d, mag, neg, s);
        } else {
            return write_integer(d, static_cast<std::uint64_t>(v), false, s);
        }
    } else if constexpr (IS_FLOAT<D>) {
        if (s.type == 'f')
            return write_fixed(d, fixed_chars(static_cast<double>(v), s),
                               static_cast<double>(v), s);
        return dstd::to_chars(d, d + SHORTEST_FLOAT_CHARS, v).ptr;
    } else if constexpr (std::is_same_v<D, bool>) {
        const StringView t = v ? "true" : "false";
        std::memcpy(d, t.data(), t.size());
        return d + t.size();
    } else if constexpr (std::is_same_v<D, char>) {
        *d = v;
        return d + 1;
    } else if constexpr (std::is_same_v<D, RawText>) {
        std::memcpy(d, v.s.data(), v.s.size());
        return d + v.s.size();
    } else {
        return E::write(d, to_view(v));
    }
}

template <class F, class E, std::size_t... I, class... A>
std::size_t bound_impl(const F& f, std::index_sequence<I...>, const A&... a) {
    return f.total() + (std::size_t{0} + ... + arg_bound<E>(f.spec(I), a));
}

/// The most bytes write() may produce; `f` has sizeof...(A) holes.
template <class E, class F, class... A>
std::size_t bound(const F& f, const A&... a) {
    return bound_impl<F, E>(f, std::index_sequence_for<A...>{}, a...);
}

template <class F>
char* put_piece(const F& f, char* d, std::size_t i) {
    const std::size_t n = f.size(i);
    if (n != 0) std::memcpy(d, f.text(i), n);
    return d + n;
}

template <class F, class E, std::size_t... I, class... A>
char* write_impl(const F& f, char* d, std::index_sequence<I...>,
                 const A&... a) {
    ((d = put_piece(f, d, I), d = put_arg<E>(d, f.spec(I), a)), ...);
    return put_piece(f, d, sizeof...(I));
}

/// `dst` has room for bound(f, a...). Returns the end.
template <class E, class F, class... A>
char* write(const F& f, char* dst, const A&... a) {
    return write_impl<F, E>(f, dst, std::index_sequence_for<A...>{}, a...);
}

constexpr std::size_t CLAMP_SCRATCH = 512;

template <class E, class F, std::size_t... I, class... A>
FmtResult write_clamped_impl(const F& f, char* d, char* end,
                             std::index_sequence<I...>, const A&... a) {
    bool cut = false;
    char* const begin = d;
    const auto put = [&](const char* s, std::size_t n) {
        const std::size_t room = static_cast<std::size_t>(end - d);
        if (n > room) {
            n = room;
            cut = true;
        }
        if (n != 0) std::memcpy(d, s, n);
        d += n;
    };
    const auto piece = [&](std::size_t i) { put(f.text(i), f.size(i)); };
    [[maybe_unused]] const auto arg = [&](Spec s, const auto& v) {
        using D = std::decay_t<decltype(v)>;
        if constexpr (StrLike<D> || std::is_same_v<D, RawText>) {
            if constexpr (std::is_same_v<D, RawText>) {
                put(v.s.data(), v.s.size());
            } else if constexpr (std::is_same_v<E, PlainEscape>) {
                const StringView t = to_view(v);
                put(t.data(), t.size());
            } else {
                // Escape in stack pieces so fmt_to never allocates. This
                // needs a policy that escapes each byte on its own.
                StringView t = to_view(v);
                char escaped[256];
                while (!t.empty() && !cut) {
                    std::size_t n = t.size() < 32 ? t.size() : 32;
                    while (n > 1 && E::bound(t.substr(0, n)) > sizeof escaped)
                        n /= 2;
                    char* const last = E::write(escaped, t.substr(0, n));
                    put(escaped, static_cast<std::size_t>(last - escaped));
                    t.remove_prefix(n);
                }
            }
        } else {
            char scratch[CLAMP_SCRATCH];
            char* e = put_arg<E>(scratch, s, v);
            put(scratch, static_cast<std::size_t>(e - scratch));
        }
    };
    ((piece(I), arg(f.spec(I), a)), ...);
    piece(sizeof...(I));
    return {static_cast<std::size_t>(d - begin), cut};
}

/// Compile-time checks of a call; instantiating it reports the first misuse.
template <FixedString S, class... A>
struct Check {
    static constexpr std::size_t HOLES = COUNTED<S>.holes;
    static_assert(COUNTED<S>.ok,
                  "dstd::fmt: bad hole spec; use {}, {:x}, {:X}, {:0N}, "
                  "{:0Nx}, {:.Nf} or {:0N.Pf}");
    static_assert(sizeof...(A) == HOLES,
                  "dstd::fmt: argument count differs from the hole count");
    static_assert((ARG<std::decay_t<const A>> && ...),
                  "dstd::fmt: unsupported argument type");
    static constexpr bool specs_fit() {
        bool ok = true;
        std::size_t i = 0;
        ((ok = ok && spec_ok<std::decay_t<const A>>(
                         i < HOLES ? PIECES<S>.spec[i] : Spec{}),
          ++i),
         ...);
        return ok;
    }
    static_assert(specs_fit(),
                  "dstd::fmt: a hole spec does not fit its argument type "
                  "(x and X need an integer, f a float, width an integer or "
                  "float, nothing else takes a spec)");
    static constexpr bool OK = true;
};

}  // namespace fmt_detail

/// True for a type that fits a hole.
template <class T>
concept FmtArg = fmt_detail::ARG<std::decay_t<const T>>;

/// The number of holes in the format `S`.
template <FixedString S>
inline constexpr std::size_t FMT_HOLES = fmt_detail::COUNTED<S>.holes;

/// @ref fmt with an escape policy `E` for string arguments: `E::bound(s)` is
/// the most bytes `E::write(dst, s)` may write for the view `s`. Numbers, bools
/// and chars print as in @ref fmt and a fmt_detail::RawText argument is not
/// escaped.
template <class E, FixedString S, class... A>
String fmt_with(const A&... a) {
    static_assert(fmt_detail::Check<S, A...>::OK);
    constexpr fmt_detail::Static<S> FORMAT{};
    String out;
    out.resize(fmt_detail::bound<E>(FORMAT, a...));
    char* const first = out.data();
    char* const end = fmt_detail::write<E>(FORMAT, first, a...);
    out.resize(static_cast<std::size_t>(end - first));
    return out;
}

/// Formats into a dstd::String sized once from the bound of the arguments.
template <FixedString S, class... A>
String fmt(const A&... a) {
    return fmt_with<fmt_detail::PlainEscape, S>(a...);
}

template <FixedString S, class... A>
String fmt(Format<S>, const A&... a) {
    return fmt<S>(a...);
}

/// @ref fmt_to with an escape policy, as @ref fmt_with. The cut falls on the
/// escaped text.
template <class E, FixedString S, class... A>
FmtResult fmt_to_with(char* buf, std::size_t cap, const A&... a) {
    static_assert(fmt_detail::Check<S, A...>::OK);
    constexpr fmt_detail::Static<S> FORMAT{};
    if (fmt_detail::bound<E>(FORMAT, a...) <= cap)
        return {static_cast<std::size_t>(
                    fmt_detail::write<E>(FORMAT, buf, a...) - buf),
                false};
    return fmt_detail::write_clamped_impl<E>(
        FORMAT, buf, buf + cap, std::index_sequence_for<A...>{}, a...);
}

/// Formats into `buf`, never allocates and never writes past `cap` bytes. No
/// terminating NUL is written. When the whole text fits, the result is its
/// size; otherwise `size` is `cap`, the text is cut at `cap` bytes and
/// `truncated` is set.
template <FixedString S, class... A>
FmtResult fmt_to(char* buf, std::size_t cap, const A&... a) {
    return fmt_to_with<fmt_detail::PlainEscape, S>(buf, cap, a...);
}

template <FixedString S, class... A>
FmtResult fmt_to(char* buf, std::size_t cap, Format<S>, const A&... a) {
    return fmt_to<S>(buf, cap, a...);
}

namespace fmt_detail {

template <FixedString Sep, std::size_t K>
consteval auto join_format() {
    constexpr std::size_t SEP = sizeof(Sep.v) - 1;
    constexpr std::size_t N = K == 0 ? 1 : 2 * K + (K - 1) * SEP + 1;
    char out[N]{};
    std::size_t d = 0;
    for (std::size_t i = 0; i < K; ++i) {
        if (i > 0)
            for (std::size_t j = 0; j < SEP; ++j) out[d++] = Sep.v[j];
        out[d++] = '{';
        out[d++] = '}';
    }
    return FixedString<N>(out);
}

template <FixedString Sep>
consteval bool plain_separator() {
    for (std::size_t i = 0; i + 1 < sizeof(Sep.v); ++i)
        if (Sep.v[i] == '{' || Sep.v[i] == '}') return false;
    return true;
}

}  // namespace fmt_detail

/// Formats each argument as a plain `{}` hole with `Sep` between them, such as
/// one TSV row: `fmt_join<"\t">(a, b, c)` is `fmt<"{}\t{}\t{}">(a, b, c)`.
template <FixedString Sep, class... A>
String fmt_join(const A&... a) {
    static_assert(fmt_detail::plain_separator<Sep>(),
                  "dstd::fmt_join: the separator must not hold a brace");
    return fmt<fmt_detail::join_format<Sep, sizeof...(A)>()>(a...);
}

/// @ref fmt_join into `buf`, with the contract of @ref fmt_to.
template <FixedString Sep, class... A>
FmtResult fmt_join_to(char* buf, std::size_t cap, const A&... a) {
    static_assert(fmt_detail::plain_separator<Sep>(),
                  "dstd::fmt_join_to: the separator must not hold a brace");
    return fmt_to<fmt_detail::join_format<Sep, sizeof...(A)>()>(buf, cap, a...);
}

}  // namespace dftracer::utils::dstd

namespace dftracer::utils {

/// `"open {}"_f`: a format for the call form `dstd::fmt("open {}"_f, path)`.
/// Defined here so code in any namespace under dftracer::utils finds it.
template <dstd::FixedString S>
consteval dstd::Format<S> operator""_f() {
    return {};
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_DSTD_FMT_H
