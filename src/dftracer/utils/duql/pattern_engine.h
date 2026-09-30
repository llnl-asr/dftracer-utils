#ifndef DFTRACER_UTILS_DUQL_PATTERN_ENGINE_H
#define DFTRACER_UTILS_DUQL_PATTERN_ENGINE_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/substr_simd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::duql {

/// The outcome of one match: LIMIT when a regex reached its work limit.
enum class MatchResult : std::uint8_t { NO, YES, LIMIT };

namespace detail {

enum class Kind : std::uint8_t {
    EXACT,
    PREFIX,
    SUFFIX,
    CONTAINS,
    SEGMENTS,
    GLOB,
    REGEX
};

enum class GlobKind : std::uint8_t { LITERAL, ANY_RUN, ANY_ONE };

struct GlobToken {
    GlobKind kind;
    char ch = 0;
};

struct RegexCode;

inline char fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c;
}

inline bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

inline std::size_t next_char(std::string_view s, std::size_t i) {
    ++i;
    while (i < s.size() && continuation(s[i])) ++i;
    return i;
}

// Below this haystack length the SIMD dispatch costs more than it saves.
inline constexpr std::size_t SIMD_MIN_HAY = 64;

inline std::int64_t find(std::string_view hay, std::string_view needle,
                         bool icase) {
    if (hay.size() < SIMD_MIN_HAY) {
        if (!icase) {
            const auto at = hay.find(needle);
            return at == std::string_view::npos ? -1
                                                : static_cast<std::int64_t>(at);
        }
        if (needle.size() > hay.size()) return -1;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            std::size_t k = 0;
            while (k < needle.size() && fold(hay[i + k]) == needle[k]) ++k;
            if (k == needle.size()) return static_cast<std::int64_t>(i);
        }
        return -1;
    }
    const auto n = static_cast<std::int64_t>(needle.size());
    const auto h = static_cast<std::int64_t>(hay.size());
    return icase ? detail::substr_find_icase(hay.data(), h, needle.data(), n)
                 : detail::substr_find(hay.data(), h, needle.data(), n);
}

inline bool equal_at(std::string_view s, std::size_t at, std::string_view lit,
                     bool icase) {
    if (at + lit.size() > s.size()) return false;
    if (!icase) return s.compare(at, lit.size(), lit) == 0;
    for (std::size_t k = 0; k < lit.size(); ++k)
        if (fold(s[at + k]) != lit[k]) return false;
    return true;
}

}  // namespace detail

/// A compiled pattern; read-only after compile.
struct CompiledPattern {
    detail::Kind kind = detail::Kind::EXACT;
    bool icase = false;
    std::string literal;
    // SEGMENTS: the literal pieces between `%`; GLOB: the literal runs, which
    // must all occur.
    std::vector<std::string> segments;
    bool anchored_start = false;
    bool anchored_end = false;
    // SEGMENTS and GLOB: the tokens, letters folded when `icase`.
    std::vector<detail::GlobToken> glob;
    std::vector<std::string> required;
    std::shared_ptr<detail::RegexCode> regex;
};

namespace detail {

// Two-pointer wildcard match: `%` any run, `_` one UTF-8 character.
// The byte after the UTF-8 character at `i`.
inline std::size_t step_char(std::string_view s, std::size_t i) {
    if (static_cast<unsigned char>(s[i]) < 0x80) [[likely]]
        return i + 1;
    return next_char(s, i);
}

// Two-pointer wildcard match: `%` any run, `_` one UTF-8 character. Without
// `_` (ANY_ONE false) a restart may land on any byte: a literal never
// matches a UTF-8 continuation byte.
template <bool ICASE, bool ANY_ONE>
bool glob_match_t(const std::vector<GlobToken>& toks, std::string_view s) {
    std::size_t si = 0, pi = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_si = 0;
    const std::size_t n = s.size();
    const std::size_t m = toks.size();
    while (si < n) {
        if (ANY_ONE && pi < m && toks[pi].kind == GlobKind::ANY_ONE) {
            si = step_char(s, si);
            ++pi;
        } else if (pi < m && toks[pi].kind == GlobKind::LITERAL &&
                   (ICASE ? fold(s[si]) : s[si]) == toks[pi].ch) {
            ++si;
            ++pi;
        } else if (pi < m && toks[pi].kind == GlobKind::ANY_RUN) {
            star = pi;
            star_si = si;
            ++pi;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            star_si = ANY_ONE ? step_char(s, star_si) : star_si + 1;
            si = star_si;
        } else {
            return false;
        }
    }
    while (pi < m && toks[pi].kind == GlobKind::ANY_RUN) ++pi;
    return pi == m;
}

inline bool glob_match(const std::vector<GlobToken>& toks, std::string_view s,
                       bool icase, bool any_one) {
    if (any_one)
        return icase ? glob_match_t<true, true>(toks, s)
                     : glob_match_t<false, true>(toks, s);
    return icase ? glob_match_t<true, false>(toks, s)
                 : glob_match_t<false, false>(toks, s);
}

inline bool segments_match(const CompiledPattern& p, std::string_view s) {
    const auto& seg = p.segments;
    std::size_t pos = 0;
    std::size_t first = 0;
    std::size_t last = seg.size();
    if (p.anchored_start) {
        if (!equal_at(s, 0, seg.front(), p.icase)) return false;
        pos = seg.front().size();
        first = 1;
    }
    if (p.anchored_end) {
        const std::string& tail = seg.back();
        if (tail.size() > s.size() || s.size() - tail.size() < pos ||
            !equal_at(s, s.size() - tail.size(), tail, p.icase))
            return false;
        s = s.substr(0, s.size() - tail.size());
        last = seg.size() - 1;
    }
    for (std::size_t i = first; i < last; ++i) {
        const auto at = find(s.substr(pos), seg[i], p.icase);
        if (at < 0) return false;
        pos += static_cast<std::size_t>(at) + seg[i].size();
    }
    return true;
}

/// Shortest value, in bytes, that a Vectorscan-backed regex scans with
/// Vectorscan; shorter values stay on PCRE2.
inline constexpr std::size_t HS_MIN_VALUE = 256;

MatchResult match_regex(const CompiledPattern& p, std::string_view s);

// Strict UTF-8: no overlong forms, surrogates, or code points past U+10FFFF.
bool valid_utf8(std::string_view s);

// The PCRE2 path alone, bypassing Vectorscan; for tests.
MatchResult match_regex_pcre2(const CompiledPattern& p, std::string_view s);

}  // namespace detail

/// Safe from any thread; a compiled pattern is read-only.
inline MatchResult match(const CompiledPattern& p, std::string_view s) {
    using detail::Kind;
    auto yes = [](bool b) { return b ? MatchResult::YES : MatchResult::NO; };
    switch (p.kind) {
        case Kind::EXACT:
            return yes(s.size() == p.literal.size() &&
                       detail::equal_at(s, 0, p.literal, p.icase));
        case Kind::PREFIX:
            return yes(detail::equal_at(s, 0, p.literal, p.icase));
        case Kind::SUFFIX:
            return yes(s.size() >= p.literal.size() &&
                       detail::equal_at(s, s.size() - p.literal.size(),
                                        p.literal, p.icase));
        case Kind::CONTAINS:
            return yes(detail::find(s, p.literal, p.icase) >= 0);
        case Kind::SEGMENTS:
            // A short string is cheaper to walk than to search part by part.
            if (s.size() < detail::SIMD_MIN_HAY)
                return yes(detail::glob_match(p.glob, s, p.icase, false));
            return yes(detail::segments_match(p, s));
        case Kind::GLOB:
            // On a long string, rule out a missing literal part before the
            // character-by-character glob.
            if (s.size() >= detail::SIMD_MIN_HAY)
                for (const auto& seg : p.segments)
                    if (detail::find(s, seg, p.icase) < 0)
                        return MatchResult::NO;
            return yes(detail::glob_match(p.glob, s, p.icase, true));
        case Kind::REGEX:
            break;
    }
    return detail::match_regex(p, s);
}

/// A pattern that failed to compile: the reason and its byte offset in the
/// pattern text.
struct PatternError {
    std::string message;
    std::size_t offset = 0;
};

using PatternPtr = std::shared_ptr<const CompiledPattern>;
using PatternResult = dftracer::utils::expected<PatternPtr, PatternError>;

/// SQL LIKE over the whole string: `%` any run (newlines included), `_` one
/// UTF-8 character; `escape` makes `c%`, `c_` and `cc` literal. `icase` folds
/// ASCII letters.
PatternResult compile_like(std::string_view pattern, bool icase,
                           std::optional<char> escape);

/// Whether `text` occurs in the string; `icase` folds ASCII letters.
PatternResult compile_contains(std::string_view text, bool icase);

/// A regex in the duql dialect, searched anywhere in the string, or matched
/// against the whole string when `whole`. `icase` ignores case (Unicode).
PatternResult compile_regex(std::string_view pattern, bool icase,
                            bool whole = false);

/// Capture `group` (0 = the whole match) of the first match of a regex into
/// `out`; NO when nothing matches or the group took no part.
MatchResult extract(const CompiledPattern& p, std::string_view s,
                    std::size_t group, std::string_view& out);

/// Every non-overlapping match of a regex, in order; stops at LIMIT.
MatchResult findall(const CompiledPattern& p, std::string_view s,
                    const std::function<void(std::string_view)>& fn);

/// The plain string test a pattern reduces to, for callers that inline it
/// in a loop over many strings. `text` is ASCII-folded when `icase`, and
/// lives as long as the pattern.
struct LiteralTest {
    enum class Op : std::uint8_t { EQUALS, STARTS_WITH, ENDS_WITH, CONTAINS };
    Op op;
    std::string_view text;
    bool icase;
};

/// The literal test of `p`, or nullopt when it needs the matcher.
std::optional<LiteralTest> literal_test(const CompiledPattern& p);

/// Capture groups of a regex; 0 for the other kinds.
std::size_t capture_count(const CompiledPattern& p);

/// The named groups of a regex as (group number, name), in group-number
/// order; empty for the other kinds.
std::vector<std::pair<std::size_t, std::string>> capture_names(
    const CompiledPattern& p);

/// A replacement template compiled against one pattern: literal text and
/// group references. Valid for that pattern only.
struct Substitution {
    struct Piece {
        std::string text;
        std::size_t group = 0;
        bool is_group = false;
    };
    std::vector<Piece> pieces;
};

/// Compile `to`: `$n` and `${n}` insert group n, `${name}` a named group,
/// `$$` a dollar sign. A non-regex pattern, an unknown group or name, and a
/// `$` followed by anything else are errors whose offset is in `to`.
dftracer::utils::expected<Substitution, PatternError> compile_substitution(
    const CompiledPattern& p, std::string_view to);

/// Replace every non-overlapping match of a regex in `s` into `out`, left to
/// right; a group that took no part inserts nothing. NO leaves `out` equal to
/// `s`; LIMIT leaves `out` unspecified.
MatchResult regex_replace(const CompiledPattern& p, const Substitution& sub,
                          std::string_view s, std::string& out);

/// Case-sensitive literals every match contains (empty when none is known).
const std::vector<std::string>& required_literals(const CompiledPattern& p);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PATTERN_ENGINE_H
