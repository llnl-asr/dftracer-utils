#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>
#include <hwy/highway.h>
#include <pcre2.h>

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
#include <hs.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::duql {

using namespace detail;

namespace detail {

struct RegexCode {
    pcre2_code* code = nullptr;
    std::size_t groups = 0;
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    hs_database_t* hs = nullptr;
    bool ascii_only = false;
    bool gap = false;
#endif
    ~RegexCode() {
        pcre2_code_free(code);
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
        hs_free_database(hs);
#endif
    }
};

}  // namespace detail

namespace {

constexpr std::uint32_t MATCH_LIMIT = 1'000'000;
constexpr std::uint32_t DEPTH_LIMIT = 10'000;
constexpr std::size_t JIT_STACK_MAX = 1 << 20;

std::string folded(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = fold(c);
    return out;
}

}  // namespace

namespace {

// Scans a regex in the duql dialect: refuses what RE2, PCRE2 and Vectorscan
// do not share, and collects literals every match contains.
class DialectScan {
   public:
    explicit DialectScan(std::string_view p) : p_(p) {}

    std::optional<PatternError> run() {
        branches_ = alternation();
        if (error_) return error_;
        if (i_ < p_.size()) fail("unbalanced ')'");
        return error_;
    }

    std::vector<std::string> literals() {
        if (icase_ || branches_) return {};
        std::sort(
            found_.begin(), found_.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
        if (found_.size() > 2) found_.resize(2);
        return found_;
    }

   private:
    std::string_view p_;
    std::size_t i_ = 0;
    std::optional<PatternError> error_;
    std::vector<std::string> found_;
    std::string run_;
    bool icase_ = false;
    bool branches_ = false;

    void fail(std::string msg) {
        if (!error_) error_ = PatternError{std::move(msg), i_};
    }

    void end_run() {
        if (!run_.empty()) found_.push_back(std::move(run_));
        run_.clear();
    }

    // Whether the alternation had more than one branch.
    bool alternation() {
        bool branches = false;
        sequence();
        while (!error_ && i_ < p_.size() && p_[i_] == '|') {
            branches = true;
            end_run();
            ++i_;
            sequence();
        }
        end_run();
        return branches;
    }

    void sequence() {
        while (!error_ && i_ < p_.size() && p_[i_] != '|' && p_[i_] != ')') {
            if (p_[i_] == '(') {
                // A group's literals are required only when the group is.
                end_run();
                const std::size_t before = found_.size();
                group();
                if (error_) return;
                if (quantifier() == 0) found_.resize(before);
                continue;
            }
            std::string lit;
            const bool literal = atom(lit);
            if (error_) return;
            const auto q = quantifier();
            if (error_) return;
            if (literal && q == 1) {
                run_ += lit;
            } else {
                end_run();
                if (literal && q == 2) found_.push_back(lit);
            }
        }
    }

    // 0: none or optional (?, *, {0,...}); 1: exactly once; 2: at least once.
    int quantifier() {
        if (i_ >= p_.size()) return 1;
        int q = 1;
        const char c = p_[i_];
        if (c == '*' || c == '?') {
            q = 0;
            ++i_;
        } else if (c == '+') {
            q = 2;
            ++i_;
        } else if (c == '{') {
            const auto close = p_.find('}', i_);
            if (close == std::string_view::npos) return 1;
            const std::string_view body = p_.substr(i_ + 1, close - i_ - 1);
            if (body.empty() ||
                !((body[0] >= '0' && body[0] <= '9') || body[0] == ','))
                return 1;
            q = body[0] == ',' ||
                        (body[0] == '0' && (body.size() == 1 || body[1] == ','))
                    ? 0
                    : 2;
            i_ = close + 1;
        } else {
            return 1;
        }
        if (i_ < p_.size() && p_[i_] == '+') fail("possessive quantifier");
        if (i_ < p_.size() && p_[i_] == '?') ++i_;
        return q;
    }

    // Consumes one atom; returns whether it is a literal (its text in `lit`).
    bool atom(std::string& lit) {
        const char c = p_[i_];
        if (c == '[') {
            char_class();
            return false;
        }
        if (c == '\\') return escape(lit);
        if (c == '.' || c == '^' || c == '$') {
            ++i_;
            return false;
        }
        if (c == '*' || c == '+' || c == '?' || c == '{') {
            fail("quantifier without a target");
            return false;
        }
        const std::size_t start = i_;
        i_ = next_char(p_, i_);
        lit.assign(p_.substr(start, i_ - start));
        return true;
    }

    bool escape(std::string& lit) {
        if (i_ + 1 >= p_.size()) {
            fail("trailing '\\'");
            return false;
        }
        const char e = p_[i_ + 1];
        if (e >= '1' && e <= '9') {
            fail("backreference");
            return false;
        }
        switch (e) {
            case 'g':
            case 'k':
                fail("backreference");
                return false;
            case 'C':
                fail("\\C");
                return false;
            case 'K':
                fail("\\K (reset match start)");
                return false;
            case 'G':
                fail("\\G");
                return false;
            case 'Q': {
                const auto end = p_.find("\\E", i_ + 2);
                const std::size_t stop =
                    end == std::string_view::npos ? p_.size() : end;
                lit.assign(p_.substr(i_ + 2, stop - i_ - 2));
                i_ = end == std::string_view::npos ? p_.size() : end + 2;
                return !lit.empty();
            }
            case 'p':
            case 'P':
            case 'x': {
                i_ += 2;
                if (i_ < p_.size() && p_[i_] == '{') {
                    const auto close = p_.find('}', i_);
                    i_ =
                        close == std::string_view::npos ? p_.size() : close + 1;
                } else if (e == 'x') {
                    i_ = std::min(i_ + 2, p_.size());
                } else if (i_ < p_.size()) {
                    ++i_;
                }
                return false;
            }
            default:
                break;
        }
        i_ += 2;
        if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') ||
            (e >= '0' && e <= '9'))
            return false;
        lit.assign(1, e);
        return true;
    }

    void char_class() {
        ++i_;
        if (i_ < p_.size() && p_[i_] == '^') ++i_;
        if (i_ < p_.size() && p_[i_] == ']') ++i_;
        while (i_ < p_.size() && p_[i_] != ']') {
            if (p_[i_] == '\\') {
                i_ += 2;
            } else if (p_.substr(i_, 2) == "[:") {
                const auto close = p_.find(":]", i_ + 2);
                i_ = close == std::string_view::npos ? p_.size() : close + 2;
            } else {
                ++i_;
            }
        }
        if (i_ >= p_.size()) {
            fail("unterminated character class");
            return;
        }
        ++i_;
    }

    void group() {
        const std::size_t open = i_;
        ++i_;
        if (i_ < p_.size() && p_[i_] == '*') {
            fail("verb '(*'");
            return;
        }
        if (i_ < p_.size() && p_[i_] == '?') {
            ++i_;
            if (!group_prefix(open)) return;
        }
        if (error_) return;
        const std::size_t before = found_.size();
        const bool branches = alternation();
        if (branches) found_.resize(before);
        if (error_) return;
        if (i_ >= p_.size() || p_[i_] != ')') {
            i_ = open;
            fail("unterminated group");
            return;
        }
        ++i_;
    }

    // After "(?": a non-capturing group, a named group or inline options.
    // Returns false when the group ended here (options alone).
    bool group_prefix(std::size_t open) {
        if (i_ >= p_.size()) {
            fail("unterminated group");
            return false;
        }
        const char c = p_[i_];
        if (c == ':') {
            ++i_;
            return true;
        }
        if (c == 'P' && i_ + 1 < p_.size() && p_[i_ + 1] == '<') ++i_;
        if (p_[i_] == '<' && i_ + 1 < p_.size() && p_[i_ + 1] != '=' &&
            p_[i_ + 1] != '!') {
            const auto close = p_.find('>', i_);
            if (close == std::string_view::npos) {
                fail("unterminated group name");
                return false;
            }
            i_ = close + 1;
            return true;
        }
        if (c == '=' || c == '!' || c == '<') {
            i_ = open;
            fail("lookaround");
            return false;
        }
        if (c == '>') {
            i_ = open;
            fail("atomic group");
            return false;
        }
        if (c == 'R' || c == '&' || c == '+' || (c >= '0' && c <= '9') ||
            (c == 'P' && i_ + 1 < p_.size() &&
             (p_[i_ + 1] == '>' || p_[i_ + 1] == '='))) {
            i_ = open;
            fail("recursion or backreference");
            return false;
        }
        if (c == 'C') {
            i_ = open;
            fail("callout");
            return false;
        }
        bool off = false;
        while (i_ < p_.size() && p_[i_] != ')' && p_[i_] != ':') {
            const char f = p_[i_];
            if (f == '-') {
                off = true;
            } else if (f == 'i' || f == 'm' || f == 's') {
                if (f == 'i' && !off) icase_ = true;
            } else {
                fail(std::string("inline option '") + f + "'");
                return false;
            }
            ++i_;
        }
        if (i_ >= p_.size()) {
            fail("unterminated group");
            return false;
        }
        if (p_[i_] == ':') {
            ++i_;
            return true;
        }
        ++i_;
        return false;
    }
};

struct ThreadMatch {
    pcre2_match_context* context = nullptr;
    pcre2_jit_stack* stack = nullptr;
    pcre2_match_data* data = nullptr;
    std::size_t pairs = 0;

    ThreadMatch() {
        context = pcre2_match_context_create(nullptr);
        pcre2_set_match_limit(context, MATCH_LIMIT);
        pcre2_set_depth_limit(context, DEPTH_LIMIT);
        stack = pcre2_jit_stack_create(32 * 1024, JIT_STACK_MAX, nullptr);
        if (stack) pcre2_jit_stack_assign(context, nullptr, stack);
    }
    ~ThreadMatch() {
        pcre2_match_data_free(data);
        pcre2_jit_stack_free(stack);
        pcre2_match_context_free(context);
    }
    ThreadMatch(const ThreadMatch&) = delete;
    ThreadMatch& operator=(const ThreadMatch&) = delete;

    pcre2_match_data* for_groups(std::size_t groups) {
        if (!data || pairs < groups + 1) {
            pcre2_match_data_free(data);
            pairs = std::max<std::size_t>(groups + 1, 16);
            data = pcre2_match_data_create(static_cast<std::uint32_t>(pairs),
                                           nullptr);
        }
        return data;
    }
};

ThreadMatch& thread_match() {
    thread_local ThreadMatch tm;
    return tm;
}

// pcre2_match at `offset`; the ovector stays in the thread's match data.
MatchResult run_regex(const RegexCode& code, std::string_view s,
                      std::size_t offset, pcre2_match_data*& md) {
    ThreadMatch& tm = thread_match();
    md = tm.for_groups(code.groups);
    const int rc =
        pcre2_match(code.code, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(),
                    offset, 0, md, tm.context);
    if (rc >= 0) return MatchResult::YES;
    if (rc == PCRE2_ERROR_NOMATCH) return MatchResult::NO;
    return MatchResult::LIMIT;
}

bool has_required(const CompiledPattern& p, std::string_view s) {
    for (const auto& lit : p.required)
        if (find(s, lit, false) < 0) return false;
    return true;
}

bool scan_utf8(std::string_view s, bool& wide);

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
struct ThreadScratch {
    hs_scratch_t* scratch = nullptr;
    ~ThreadScratch() { hs_free_scratch(scratch); }
};

int hit(unsigned, unsigned long long, unsigned long long, unsigned, void* ctx) {
    *static_cast<bool*>(ctx) = true;
    return 1;
}

// Vectorscan and PCRE2 disagree on Unicode property escapes and \X, on case
// folding of \p{Lu} and \p{Ll}, and on case folding of some non-ASCII
// letters. Property patterns with case folding never use Vectorscan; other
// property patterns, \X, and case-folded patterns with non-ASCII literals keep
// non-ASCII values on PCRE2.
enum class HsUse { ALL, ASCII_ONLY, NEVER };

// Whether `pattern` has an unbounded dot gap (`.*`, `.+`, `.{n,}`), the shape
// on which Vectorscan beats the PCRE2 JIT on long values; on alternations and
// classes the JIT stays ahead at every length measured (benchmarks.md).
bool has_dot_gap(std::string_view pattern) {
    bool in_class = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\') {
            ++i;
        } else if (in_class) {
            in_class = c != ']';
        } else if (c == '[') {
            in_class = true;
        } else if (c == '.' && i + 1 < pattern.size() &&
                   (pattern[i + 1] == '*' || pattern[i + 1] == '+' ||
                    pattern[i + 1] == '{')) {
            return true;
        }
    }
    return false;
}

HsUse hs_use(std::string_view pattern, bool icase) {
    bool wide_literal = false;
    bool props = false;
    bool inline_flags = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (static_cast<unsigned char>(c) >= 0x80) wide_literal = true;
        if (c == '(' && i + 1 < pattern.size() && pattern[i + 1] == '?')
            inline_flags = true;
        if (c != '\\' || i + 1 == pattern.size()) continue;
        switch (pattern[++i]) {
            case 'p':
            case 'P':
            case 'X':
                props = true;
                break;
            case 'x':
            case 'o':
            case 'N':
                wide_literal = true;
                break;
            default:
                break;
        }
    }
    const bool folds = icase || inline_flags;
    if (props && folds) return HsUse::NEVER;
    if (props || (wide_literal && folds)) return HsUse::ASCII_ONLY;
    return HsUse::ALL;
}

hs_database_t* compile_hs(std::string_view pattern, bool icase, bool whole) {
    if (pattern.find('\0') != std::string_view::npos) return nullptr;
    unsigned flags = HS_FLAG_SINGLEMATCH | HS_FLAG_UTF8 | HS_FLAG_ALLOWEMPTY;
    if (icase) flags |= HS_FLAG_CASELESS;
    std::string text(pattern);
    if (whole) text = "\\A(?:" + text + ")\\z";
    hs_database_t* db = nullptr;
    hs_compile_error_t* err = nullptr;
    if (hs_compile(text.c_str(), flags, HS_MODE_BLOCK, nullptr, &db, &err) !=
        HS_SUCCESS) {
        hs_free_compile_error(err);
        return nullptr;
    }
    return db;
}

// hs_alloc_scratch allocates and checks on every call, so scan first and
// grow the scratch only when the scan reports it missing or too small.
hs_error_t hs_scan_grow(const hs_database_t* db, std::string_view s,
                        match_event_handler fn, void* ctx) {
    thread_local ThreadScratch ts;
    const char* data = s.empty() ? "" : s.data();
    const auto n = static_cast<unsigned>(s.size());
    hs_error_t rc = hs_scan(db, data, n, 0, ts.scratch, fn, ctx);
    if (rc != HS_INVALID || hs_alloc_scratch(db, &ts.scratch) != HS_SUCCESS)
        return rc;
    return hs_scan(db, data, n, 0, ts.scratch, fn, ctx);
}

bool hs_match(const RegexCode& code, std::string_view s, MatchResult& out) {
    bool found = false;
    const hs_error_t rc = hs_scan_grow(code.hs, s, hit, &found);
    if (rc != HS_SUCCESS && rc != HS_SCAN_TERMINATED) return false;
    out = found ? MatchResult::YES : MatchResult::NO;
    return true;
}

bool hs_allowed(const RegexCode& code, std::string_view s) {
    bool wide;
    return s.size() <= UINT32_MAX && scan_utf8(s, wide) &&
           !(wide && code.ascii_only);
}

// A PCRE2 match that reached its work limit, again on Vectorscan for the
// exact result when the value allows it.
[[gnu::noinline]] MatchResult exact_after_limit(const RegexCode& rc,
                                                std::string_view s) {
    MatchResult r;
    if (rc.hs && hs_allowed(rc, s) && hs_match(rc, s, r)) return r;
    return MatchResult::LIMIT;
}
#endif

PatternPtr make(CompiledPattern p) {
    return std::make_shared<const CompiledPattern>(std::move(p));
}

}  // namespace

PatternResult compile_like(std::string_view pattern, bool icase,
                           std::optional<char> escape) {
    std::vector<GlobToken> toks;
    // An int, not the optional: GCC fuses the two tests of an optional into
    // a read of its unset payload, which Valgrind reports.
    const int esc = escape ? *escape : -1;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == esc) {
            if (i + 1 >= pattern.size() ||
                (pattern[i + 1] != '%' && pattern[i + 1] != '_' &&
                 pattern[i + 1] != esc))
                return dftracer::utils::unexpected(PatternError{
                    "the escape character must precede '%', '_' or itself", i});
            toks.push_back({GlobKind::LITERAL, pattern[++i]});
        } else if (c == '%') {
            if (toks.empty() || toks.back().kind != GlobKind::ANY_RUN)
                toks.push_back({GlobKind::ANY_RUN});
        } else if (c == '_') {
            toks.push_back({GlobKind::ANY_ONE});
        } else {
            toks.push_back({GlobKind::LITERAL, c});
        }
    }
    CompiledPattern p;
    p.icase = icase;
    std::vector<std::string> runs(1);
    bool any_one = false;
    for (const GlobToken& t : toks) {
        if (t.kind == GlobKind::LITERAL) {
            runs.back() += icase ? fold(t.ch) : t.ch;
        } else {
            any_one |= t.kind == GlobKind::ANY_ONE;
            if (!runs.back().empty()) runs.emplace_back();
        }
    }
    if (runs.back().empty()) runs.pop_back();
    if (!icase)
        for (const auto& r : runs)
            if (p.required.empty() || r.size() > p.required[0].size())
                p.required = {r};
    const bool starts_run =
        !toks.empty() && toks.front().kind == GlobKind::ANY_RUN;
    const bool ends_run =
        !toks.empty() && toks.back().kind == GlobKind::ANY_RUN;
    if (icase)
        for (auto& t : toks)
            if (t.kind == GlobKind::LITERAL) t.ch = fold(t.ch);
    if (any_one) {
        p.kind = Kind::GLOB;
        p.glob = std::move(toks);
        p.segments = std::move(runs);
    } else if (runs.empty()) {
        p.kind = starts_run ? Kind::CONTAINS : Kind::EXACT;
    } else if (runs.size() == 1 && !starts_run && !ends_run) {
        p.kind = Kind::EXACT;
        p.literal = std::move(runs[0]);
    } else if (runs.size() == 1 && !starts_run) {
        p.kind = Kind::PREFIX;
        p.literal = std::move(runs[0]);
    } else if (runs.size() == 1 && !ends_run) {
        p.kind = Kind::SUFFIX;
        p.literal = std::move(runs[0]);
    } else if (runs.size() == 1) {
        p.kind = Kind::CONTAINS;
        p.literal = std::move(runs[0]);
    } else {
        p.kind = Kind::SEGMENTS;
        p.glob = std::move(toks);
        p.segments = std::move(runs);
        p.anchored_start = !starts_run;
        p.anchored_end = !ends_run;
    }
    return make(std::move(p));
}

PatternResult compile_contains(std::string_view text, bool icase) {
    CompiledPattern p;
    p.kind = Kind::CONTAINS;
    p.icase = icase;
    p.literal = icase ? folded(text) : std::string(text);
    if (!icase && !text.empty()) p.required = {std::string(text)};
    return make(std::move(p));
}

PatternResult compile_regex(std::string_view pattern, bool icase, bool whole) {
    DialectScan scan(pattern);
    if (auto err = scan.run())
        return dftracer::utils::unexpected(PatternError{
            "the duql regex dialect has no " + err->message, err->offset});
    std::uint32_t options =
        PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | PCRE2_NEVER_BACKSLASH_C;
    if (icase) options |= PCRE2_CASELESS;
    if (whole) options |= PCRE2_ANCHORED | PCRE2_ENDANCHORED;
    int err = 0;
    PCRE2_SIZE offset = 0;
    auto code = std::make_shared<RegexCode>();
    code->code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                               pattern.size(), options, &err, &offset, nullptr);
    if (!code->code) {
        PCRE2_UCHAR buf[256];
        pcre2_get_error_message(err, buf, sizeof(buf));
        return dftracer::utils::unexpected(
            PatternError{reinterpret_cast<const char*>(buf), offset});
    }
    std::uint32_t groups = 0;
    pcre2_pattern_info(code->code, PCRE2_INFO_CAPTURECOUNT, &groups);
    code->groups = groups;
    // Without JIT support the interpreter runs; results are the same. Under
    // Valgrind the JIT code lives in anonymous pages that Memcheck cannot
    // attribute, so its reads of the match buffer report as uninitialised.
#ifndef DFTRACER_UTILS_VALGRIND_MODE
    pcre2_jit_compile(code->code, PCRE2_JIT_COMPLETE);
#endif
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    const HsUse use = hs_use(pattern, icase);
    if (use != HsUse::NEVER) code->hs = compile_hs(pattern, icase, whole);
    code->ascii_only = use == HsUse::ASCII_ONLY;
    code->gap = has_dot_gap(pattern);
#endif
    CompiledPattern p;
    p.kind = Kind::REGEX;
    p.icase = icase;
    p.regex = std::move(code);
    if (!icase) p.required = scan.literals();
    return make(std::move(p));
}

namespace {

// Offset of the first vector block holding a byte at or above 0x80, or the
// start of the tail shorter than a block.
std::size_t ascii_prefix(const unsigned char* d, std::size_t n) {
    namespace hn = hwy::HWY_NAMESPACE;
    const hn::ScalableTag<std::uint8_t> v8;
    const std::size_t lanes = hn::Lanes(v8);
    const auto high = hn::Set(v8, std::uint8_t{0x80});
    std::size_t i = 0;
    for (; i + 4 * lanes <= n; i += 4 * lanes) {
        const auto a =
            hn::Or(hn::LoadU(v8, d + i), hn::LoadU(v8, d + i + lanes));
        const auto b = hn::Or(hn::LoadU(v8, d + i + 2 * lanes),
                              hn::LoadU(v8, d + i + 3 * lanes));
        if (!hn::AllTrue(v8, hn::Lt(hn::Or(a, b), high))) break;
    }
    return i;
}

bool scan_utf8(std::string_view s, bool& wide) {
    wide = false;
    const auto* d = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = ascii_prefix(d, n);
    while (i < n) {
        if (i + 8 <= n) {
            std::uint64_t w;
            std::memcpy(&w, d + i, 8);
            if ((w & 0x8080808080808080ULL) == 0) {
                i += 8;
                continue;
            }
        }
        const unsigned c = d[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        wide = true;
        std::size_t len;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            return false;
        }
        if (i + len > n || d[i + 1] < lo || d[i + 1] > hi) return false;
        for (std::size_t k = 2; k < len; ++k)
            if ((d[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

}  // namespace

bool detail::valid_utf8(std::string_view s) {
    bool wide;
    return scan_utf8(s, wide);
}

MatchResult detail::match_regex_pcre2(const CompiledPattern& p,
                                      std::string_view s) {
    if (!has_required(p, s)) return MatchResult::NO;
    pcre2_match_data* md = nullptr;
    return run_regex(*p.regex, s, 0, md);
}

MatchResult detail::match_regex(const CompiledPattern& p, std::string_view s) {
    if (!has_required(p, s)) return MatchResult::NO;
    const RegexCode& rc = *p.regex;
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    if (s.size() >= HS_MIN_VALUE && rc.gap && rc.hs) [[unlikely]] {
        MatchResult r;
        if (hs_allowed(rc, s) && hs_match(rc, s, r)) return r;
    }
#endif
    pcre2_match_data* md = nullptr;
    const MatchResult r = run_regex(rc, s, 0, md);
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    if (r == MatchResult::LIMIT) [[unlikely]]
        return exact_after_limit(rc, s);
#endif
    return r;
}

MatchResult extract(const CompiledPattern& p, std::string_view s,
                    std::size_t group, std::string_view& out) {
    if (p.kind != Kind::REGEX || group > p.regex->groups || !has_required(p, s))
        return MatchResult::NO;
    pcre2_match_data* md = nullptr;
    const MatchResult r = run_regex(*p.regex, s, 0, md);
    if (r != MatchResult::YES) return r;
    const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
    if (ov[2 * group] == PCRE2_UNSET) return MatchResult::NO;
    out = s.substr(ov[2 * group], ov[2 * group + 1] - ov[2 * group]);
    return MatchResult::YES;
}

MatchResult findall(const CompiledPattern& p, std::string_view s,
                    const std::function<void(std::string_view)>& fn) {
    if (p.kind != Kind::REGEX || !has_required(p, s)) return MatchResult::NO;
    std::size_t offset = 0;
    bool any = false;
    while (offset <= s.size()) {
        pcre2_match_data* md = nullptr;
        const MatchResult r = run_regex(*p.regex, s, offset, md);
        if (r == MatchResult::LIMIT) return r;
        if (r == MatchResult::NO) break;
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        fn(s.substr(ov[0], ov[1] - ov[0]));
        any = true;
        offset = ov[1] > ov[0]      ? ov[1]
                 : ov[1] < s.size() ? next_char(s, ov[1])
                                    : s.size() + 1;
    }
    return any ? MatchResult::YES : MatchResult::NO;
}

std::optional<LiteralTest> literal_test(const CompiledPattern& p) {
    switch (p.kind) {
        case Kind::EXACT:
            return LiteralTest{LiteralTest::Op::EQUALS, p.literal, p.icase};
        case Kind::PREFIX:
            return LiteralTest{LiteralTest::Op::STARTS_WITH, p.literal,
                               p.icase};
        case Kind::SUFFIX:
            return LiteralTest{LiteralTest::Op::ENDS_WITH, p.literal, p.icase};
        case Kind::CONTAINS:
            return LiteralTest{LiteralTest::Op::CONTAINS, p.literal, p.icase};
        case Kind::SEGMENTS:
        case Kind::GLOB:
        case Kind::REGEX:
            break;
    }
    return std::nullopt;
}

std::size_t capture_count(const CompiledPattern& p) {
    return p.kind == Kind::REGEX ? p.regex->groups : 0;
}

std::vector<std::pair<std::size_t, std::string>> capture_names(
    const CompiledPattern& p) {
    std::vector<std::pair<std::size_t, std::string>> names;
    if (p.kind != Kind::REGEX) return names;
    std::uint32_t count = 0, size = 0;
    PCRE2_SPTR table = nullptr;
    pcre2_pattern_info(p.regex->code, PCRE2_INFO_NAMECOUNT, &count);
    pcre2_pattern_info(p.regex->code, PCRE2_INFO_NAMEENTRYSIZE, &size);
    pcre2_pattern_info(p.regex->code, PCRE2_INFO_NAMETABLE, &table);
    for (std::uint32_t i = 0; i < count; ++i) {
        const PCRE2_SPTR e = table + static_cast<std::size_t>(i) * size;
        names.emplace_back((static_cast<std::size_t>(e[0]) << 8) | e[1],
                           reinterpret_cast<const char*>(e + 2));
    }
    std::sort(names.begin(), names.end());
    return names;
}

dftracer::utils::expected<Substitution, PatternError> compile_substitution(
    const CompiledPattern& p, std::string_view to) {
    auto fail = [](std::string msg, std::size_t at) {
        return dftracer::utils::unexpected(PatternError{std::move(msg), at});
    };
    if (p.kind != Kind::REGEX)
        return fail("a replacement needs a regex pattern", 0);
    const auto names = capture_names(p);
    Substitution sub;
    std::string lit;
    auto flush = [&] {
        if (lit.empty()) return;
        sub.pieces.push_back({std::move(lit), 0, false});
        lit.clear();
    };
    auto group = [&](std::size_t g,
                     std::size_t at) -> std::optional<PatternError> {
        if (g > p.regex->groups)
            return PatternError{"replacement refers to group " +
                                    std::to_string(g) +
                                    " but the pattern has " +
                                    std::to_string(p.regex->groups) + " groups",
                                at};
        flush();
        sub.pieces.push_back({std::string(), g, true});
        return std::nullopt;
    };
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    constexpr const char* BAD_DOLLAR =
        "'$' must be followed by a group number, {group} or $";
    for (std::size_t i = 0; i < to.size(); ++i) {
        if (to[i] != '$') {
            lit += to[i];
            continue;
        }
        const std::size_t at = i;
        if (i + 1 >= to.size()) return fail(BAD_DOLLAR, at);
        const char c = to[i + 1];
        if (c == '$') {
            lit += '$';
            ++i;
        } else if (digit(c)) {
            std::size_t g = 0, j = i + 1;
            for (; j < to.size() && digit(to[j]); ++j)
                g = std::min<std::size_t>(g * 10 + (to[j] - '0'), 1u << 30);
            if (auto e = group(g, at)) return dftracer::utils::unexpected(*e);
            i = j - 1;
        } else if (c == '{') {
            const std::size_t close = to.find('}', i + 2);
            if (close == std::string_view::npos || close == i + 2)
                return fail(BAD_DOLLAR, at);
            const std::string_view ref = to.substr(i + 2, close - i - 2);
            const bool numeric = std::all_of(ref.begin(), ref.end(), digit);
            std::size_t g = 0;
            if (numeric) {
                for (const char d : ref)
                    g = std::min<std::size_t>(g * 10 + (d - '0'), 1u << 30);
            } else {
                const auto it = std::find_if(
                    names.begin(), names.end(),
                    [&](const auto& n) { return n.second == ref; });
                if (it == names.end())
                    return fail("unknown group name '" + std::string(ref) + "'",
                                at);
                g = it->first;
            }
            if (auto e = group(g, at)) return dftracer::utils::unexpected(*e);
            i = close;
        } else {
            return fail(BAD_DOLLAR, at);
        }
    }
    flush();
    return sub;
}

MatchResult regex_replace(const CompiledPattern& p, const Substitution& sub,
                          std::string_view s, std::string& out) {
    out.clear();
    if (p.kind != Kind::REGEX || !has_required(p, s)) {
        out.assign(s);
        return MatchResult::NO;
    }
    std::size_t offset = 0;
    std::size_t copied = 0;
    bool any = false;
    while (offset <= s.size()) {
        pcre2_match_data* md = nullptr;
        const MatchResult r = run_regex(*p.regex, s, offset, md);
        if (r == MatchResult::LIMIT) return r;
        if (r == MatchResult::NO) break;
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        out.append(s.substr(copied, ov[0] - copied));
        for (const auto& piece : sub.pieces) {
            if (!piece.is_group) {
                out += piece.text;
            } else if (ov[2 * piece.group] != PCRE2_UNSET) {
                out.append(
                    s.substr(ov[2 * piece.group],
                             ov[2 * piece.group + 1] - ov[2 * piece.group]));
            }
        }
        copied = ov[1];
        any = true;
        if (ov[1] > ov[0]) {
            offset = ov[1];
        } else if (ov[1] < s.size()) {
            offset = next_char(s, ov[1]);
            out.append(s.substr(ov[1], offset - ov[1]));
            copied = offset;
        } else {
            break;
        }
    }
    if (!any) {
        out.assign(s);
        return MatchResult::NO;
    }
    out.append(s.substr(copied));
    return MatchResult::YES;
}

const std::vector<std::string>& required_literals(const CompiledPattern& p) {
    return p.required;
}

}  // namespace dftracer::utils::duql
