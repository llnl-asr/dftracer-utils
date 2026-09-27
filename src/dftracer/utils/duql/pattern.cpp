#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>
#include <pcre2.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::duql {

using namespace detail;

namespace detail {

struct RegexCode {
    pcre2_code* code = nullptr;
    std::size_t groups = 0;
    ~RegexCode() { pcre2_code_free(code); }
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
    CompiledPattern p;
    p.kind = Kind::REGEX;
    p.icase = icase;
    p.regex = std::move(code);
    if (!icase) p.required = scan.literals();
    return make(std::move(p));
}

MatchResult detail::match_regex(const CompiledPattern& p, std::string_view s) {
    if (!has_required(p, s)) return MatchResult::NO;
    pcre2_match_data* md = nullptr;
    return run_regex(*p.regex, s, 0, md);
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

const std::vector<std::string>& required_literals(const CompiledPattern& p) {
    return p.required;
}

}  // namespace dftracer::utils::duql
