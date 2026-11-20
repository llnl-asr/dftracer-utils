// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/tz/footer.h>
#include <dftracer/utils/dstd/ascii.h>

namespace dftracer::utils::dataframe::tz {

namespace {

constexpr std::int32_t MAX_OFFSET_HOURS = 24;
constexpr std::int32_t MAX_RULE_HOURS = 167;

class Scanner {
   public:
    explicit Scanner(dstd::StringView text) : text_(text) {}

    bool done() const { return at_ >= text_.size(); }
    char peek() const { return done() ? '\0' : text_[at_]; }
    void skip() { ++at_; }
    bool eat(char c) {
        if (peek() != c) return false;
        ++at_;
        return true;
    }

    // An abbreviation: three or more letters, or any run in angle brackets.
    bool name(dstd::String& out) {
        if (peek() == '<') {
            const std::size_t close = text_.find('>', at_);
            if (close == dstd::StringView::npos || close == at_ + 1)
                return false;
            const dstd::StringView body =
                text_.substr(at_ + 1, close - at_ - 1);
            for (const char c : body)
                if (!dstd::is_ascii_alnum(c) && c != '+' && c != '-')
                    return false;
            out.assign(body);
            at_ = close + 1;
            return true;
        }
        const std::size_t from = at_;
        while (dstd::is_ascii_alpha(peek())) ++at_;
        if (at_ - from < 3) return false;
        out.assign(text_.substr(from, at_ - from));
        return true;
    }

    bool number(std::int32_t max, std::int32_t& out) {
        if (!dstd::is_ascii_digit(peek())) return false;
        std::int64_t v = 0;
        while (dstd::is_ascii_digit(peek())) {
            v = v * 10 + (text_[at_] - '0');
            if (v > max) return false;
            ++at_;
        }
        out = static_cast<std::int32_t>(v);
        return true;
    }

    // [+-]hh[:mm[:ss]] in seconds; hours up to `max_hours`.
    bool clock(std::int32_t max_hours, std::int32_t& out) {
        std::int32_t sign = 1;
        if (peek() == '+') {
            skip();
        } else if (peek() == '-') {
            sign = -1;
            skip();
        }
        std::int32_t h = 0, m = 0, s = 0;
        if (!number(max_hours, h)) return false;
        if (eat(':')) {
            if (!number(59, m)) return false;
            if (eat(':') && !number(59, s)) return false;
        }
        out = sign * (h * 3600 + m * 60 + s);
        return true;
    }

   private:
    dstd::StringView text_;
    std::size_t at_ = 0;
};

bool parse_rule(Scanner& in, PosixRule& rule) {
    if (in.eat('M')) {
        rule.kind = PosixRule::Kind::MonthWeekDay;
        if (!in.number(12, rule.month) || rule.month < 1 || !in.eat('.') ||
            !in.number(5, rule.week) || rule.week < 1 || !in.eat('.') ||
            !in.number(6, rule.day))
            return false;
    } else if (in.eat('J')) {
        rule.kind = PosixRule::Kind::Julian1;
        if (!in.number(365, rule.day) || rule.day < 1) return false;
    } else {
        rule.kind = PosixRule::Kind::Julian0;
        if (!in.number(365, rule.day)) return false;
    }
    rule.time = 7200;
    if (in.eat('/') && !in.clock(MAX_RULE_HOURS, rule.time)) return false;
    return true;
}

std::int64_t rule_day(const PosixRule& rule, std::int64_t year) {
    switch (rule.kind) {
        case PosixRule::Kind::Julian1: {
            const std::int64_t jan1 = common::days_from_civil(year, 1, 1);
            return jan1 + rule.day - 1 +
                   (common::is_leap_year(year) && rule.day >= 60 ? 1 : 0);
        }
        case PosixRule::Kind::Julian0:
            return common::days_from_civil(year, 1, 1) + rule.day;
        case PosixRule::Kind::MonthWeekDay:
            break;
    }
    const std::int64_t first = common::days_from_civil(year, rule.month, 1);
    const std::int64_t first_dow = common::floor_mod(first + 4, 7);
    std::int64_t day = first + common::floor_mod(rule.day - first_dow, 7) +
                       static_cast<std::int64_t>(rule.week - 1) * 7;
    const std::int64_t last = first + common::days_in_month(year, rule.month);
    while (day >= last) day -= 7;
    return day;
}

}  // namespace

dstd::Result<ParsedFooter> parse_posix_tz(dstd::StringView text) {
    const auto bad = [&](dstd::StringView why) {
        return common::make_error<"bad time zone rule '{}': {}">(
            common::ErrorCode::Parse, text, why);
    };
    Scanner in(text);
    ParsedFooter out;
    if (!in.name(out.std_name)) return bad("no standard time name");
    std::int32_t posix_std = 0;
    if (!in.clock(MAX_OFFSET_HOURS, posix_std))
        return bad("no standard offset");
    out.footer.std_offset = -posix_std;
    if (in.done()) return out;

    out.footer.has_dst = true;
    if (!in.name(out.dst_name)) return bad("bad daylight time name");
    out.footer.dst_offset = out.footer.std_offset + 3600;
    if (in.peek() != ',' && !in.done()) {
        std::int32_t posix_dst = 0;
        if (!in.clock(MAX_OFFSET_HOURS, posix_dst))
            return bad("bad daylight offset");
        out.footer.dst_offset = -posix_dst;
    }
    if (!in.eat(',')) return bad("no daylight rules");
    if (!parse_rule(in, out.footer.start)) return bad("bad start rule");
    if (!in.eat(',')) return bad("no end rule");
    if (!parse_rule(in, out.footer.end)) return bad("bad end rule");
    if (!in.done()) return bad("trailing text");
    return out;
}

void footer_events(const Footer& footer, std::int64_t year,
                   FooterEvent out[2]) noexcept {
    const FooterEvent start{rule_day(footer.start, year) * 86400 +
                                footer.start.time - footer.std_offset,
                            true};
    const FooterEvent end{rule_day(footer.end, year) * 86400 + footer.end.time -
                              footer.dst_offset,
                          false};
    if (start.at <= end.at) {
        out[0] = start;
        out[1] = end;
    } else {
        out[0] = end;
        out[1] = start;
    }
}

}  // namespace dftracer::utils::dataframe::tz
