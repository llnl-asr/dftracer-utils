// The string kernels against a per-row reference: every length 0..130 (a
// string ending the buffer, one crossing every vector block), random bytes
// 0..255, nulls, an empty and an all-null column, a column cut from a larger
// one, a dictionary column; once with the vector kernels and once with the
// byte-at-a-time reference forced, both equal to the reference.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/string_simd.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;
namespace simd = dftracer::utils::dataframe::strsimd;

namespace {

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;

bool alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool lower(unsigned char c) { return c >= 'a' && c <= 'z'; }
bool upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool space(unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); }

// ---- references: Python str semantics over ASCII, a byte outside is plain --

std::string r_capitalize(const std::string& s) {
    std::string o = s;
    for (std::size_t i = 0; i < o.size(); ++i) {
        const auto c = static_cast<unsigned char>(o[i]);
        if (i == 0) {
            if (lower(c)) o[i] = static_cast<char>(c - 32);
        } else if (upper(c)) {
            o[i] = static_cast<char>(c + 32);
        }
    }
    return o;
}
std::string r_title(const std::string& s) {
    std::string o = s;
    bool start = true;
    for (char& ch : o) {
        const auto c = static_cast<unsigned char>(ch);
        if (alpha(c)) {
            if (start && lower(c)) ch = static_cast<char>(c - 32);
            if (!start && upper(c)) ch = static_cast<char>(c + 32);
            start = false;
        } else {
            start = true;
        }
    }
    return o;
}
std::string r_swapcase(const std::string& s) {
    std::string o = s;
    for (char& ch : o) {
        const auto c = static_cast<unsigned char>(ch);
        if (lower(c))
            ch = static_cast<char>(c - 32);
        else if (upper(c))
            ch = static_cast<char>(c + 32);
    }
    return o;
}
template <class P>
bool all_nonempty(const std::string& s, P p) {
    if (s.empty()) return false;
    for (char c : s)
        if (!p(static_cast<unsigned char>(c))) return false;
    return true;
}
bool r_is(const std::string& s, int cls) {
    switch (cls) {
        case DFTU_STR_ALNUM:
            return all_nonempty(
                s, [](unsigned char c) { return alpha(c) || digit(c); });
        case DFTU_STR_ALPHA:
            return all_nonempty(s, alpha);
        case DFTU_STR_DIGIT:
        case DFTU_STR_DECIMAL:
        case DFTU_STR_NUMERIC:
            return all_nonempty(s, digit);
        case DFTU_STR_SPACE:
            return all_nonempty(s, space);
        case DFTU_STR_LOWER: {
            bool cased = false;
            for (char ch : s) {
                const auto c = static_cast<unsigned char>(ch);
                if (upper(c)) return false;
                if (lower(c)) cased = true;
            }
            return cased;
        }
        case DFTU_STR_UPPER: {
            bool cased = false;
            for (char ch : s) {
                const auto c = static_cast<unsigned char>(ch);
                if (lower(c)) return false;
                if (upper(c)) cased = true;
            }
            return cased;
        }
        default: {  // title
            bool cased = false, prev = false;
            for (char ch : s) {
                const auto c = static_cast<unsigned char>(ch);
                if (upper(c)) {
                    if (prev) return false;
                    prev = cased = true;
                } else if (lower(c)) {
                    if (!prev) return false;
                    prev = cased = true;
                } else {
                    prev = false;
                }
            }
            return cased;
        }
    }
}
std::string r_pad(const std::string& s, std::int64_t w, char f, int mode) {
    const auto len = static_cast<std::int64_t>(s.size());
    if (len >= w) return s;
    const std::int64_t gap = w - len;
    const std::int64_t left = mode == 0   ? gap
                              : mode == 1 ? 0
                                          : gap / 2 + (gap & w & 1);
    return std::string(static_cast<std::size_t>(left), f) + s +
           std::string(static_cast<std::size_t>(gap - left), f);
}
std::string r_zfill(const std::string& s, std::int64_t w) {
    const auto len = static_cast<std::int64_t>(s.size());
    if (len >= w) return s;
    const auto n = static_cast<std::size_t>(w - len);
    if (!s.empty() && (s[0] == '+' || s[0] == '-'))
        return s.substr(0, 1) + std::string(n, '0') + s.substr(1);
    return std::string(n, '0') + s;
}
std::string r_slice(const std::string& s, std::int64_t start,
                    std::int64_t length) {
    const auto len = static_cast<std::int64_t>(s.size());
    std::int64_t b = start < 0 ? start + len : start;
    b = std::clamp<std::int64_t>(b, 0, len);
    std::int64_t e = length < 0 ? len : b + length;
    e = std::clamp<std::int64_t>(e, b, len);
    return s.substr(static_cast<std::size_t>(b),
                    static_cast<std::size_t>(e - b));
}
std::vector<std::string> r_split(const std::string& s, const std::string& sep) {
    if (sep.empty()) return {s};
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
}
std::vector<std::string> r_partition(const std::string& s,
                                     const std::string& sep, bool right) {
    const std::size_t hit = right ? s.rfind(sep) : s.find(sep);
    if (hit == std::string::npos)
        return right ? std::vector<std::string>{"", "", s}
                     : std::vector<std::string>{s, "", ""};
    return {s.substr(0, hit), sep, s.substr(hit + sep.size())};
}

// ---- data ----

Series make(const Rows& rows) {
    std::vector<std::string_view> views;
    std::vector<std::uint8_t> validity((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        views.push_back(rows[i] ? std::string_view(*rows[i])
                                : std::string_view());
        if (rows[i])
            validity[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    return Series::strings(views, any_null ? validity.data() : nullptr);
}

// The rows after `junk` leading bytes of the data buffer: offsets[0] == junk.
Series make_offset(const Rows& rows, std::size_t junk) {
    std::string data(junk, '#');
    std::vector<std::int32_t> offsets{static_cast<std::int32_t>(junk)};
    std::vector<std::uint8_t> validity((rows.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i]) {
            data += *rows[i];
            validity[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        offsets.push_back(static_cast<std::int32_t>(data.size()));
    }
    return Series{dftu_series_new_string(
        static_cast<dftu_dtype>(TypeId::String), offsets.data(), data.data(),
        static_cast<std::int64_t>(rows.size()), validity.data())};
}

Rows corpus(unsigned seed, bool bytes) {
    std::mt19937_64 rng(seed);
    static const std::string kAlphabet = "abcxyzABCXYZ0189 \t\n+-_,.|ab--,,";
    Rows rows;
    auto add = [&](std::size_t len) {
        std::string s;
        for (std::size_t k = 0; k < len; ++k)
            s.push_back(bytes ? static_cast<char>(rng() & 0xFF)
                              : kAlphabet[rng() % kAlphabet.size()]);
        rows.emplace_back(rng() % 7 == 0 ? Row{} : Row{std::move(s)});
    };
    for (std::size_t len = 0; len <= 130; ++len) add(len);  // 0..130 in a row
    for (int k = 0; k < 400; ++k) add(rng() % 300);
    add(129);  // a long string ends the buffer
    rows.back() = std::string(129, 'q');
    return rows;
}

std::vector<Rows> corpora() {
    return {corpus(1, false), corpus(2, true),        Rows{},
            Rows(37, Row{}),  Rows{Row{""}, Row{""}}, Rows{Row{"x"}}};
}

void expect_strings(const Series& got, const Rows& want, const char* what) {
    INFO(what);
    INFO(want.size());
    REQUIRE(got.valid());
    REQUIRE(got.length() == static_cast<std::int64_t>(want.size()));
    for (std::size_t i = 0; i < want.size(); ++i) {
        const auto ii = static_cast<std::int64_t>(i);
        REQUIRE(got.is_null(ii) == !want[i].has_value());
        if (want[i]) REQUIRE(std::string(got.string_at(ii)) == *want[i]);
    }
}

void expect_bools(const Series& got, const Rows& in,
                  const std::vector<bool>& want, const char* what) {
    INFO(what);
    REQUIRE(got.valid());
    REQUIRE(got.type() == TypeId::Bool);
    const std::uint8_t* b = got.data<std::uint8_t>();
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto ii = static_cast<std::int64_t>(i);
        REQUIRE(got.is_null(ii) == !in[i].has_value());
        if (in[i]) REQUIRE((((b[i >> 3] >> (i & 7)) & 1) != 0) == want[i]);
    }
}

Rows map_rows(const Rows& in,
              const std::function<std::string(const std::string&)>& f) {
    Rows out;
    for (const Row& r : in) out.push_back(r ? Row{f(*r)} : Row{});
    return out;
}

// Run `body` with the vector kernels, then with the byte reference forced.
template <class Body>
void both_modes(Body body) {
    simd::set_force_scalar(false);
    body("simd");
    simd::set_force_scalar(true);
    body("scalar");
    simd::set_force_scalar(false);
}

std::vector<std::string> list_row(const Series& list, std::int64_t i) {
    const std::int32_t* off = dftu_series_offsets(list.handle());
    Series child{dftu_series_child(list.handle(), 0)};
    std::vector<std::string> out;
    for (std::int32_t k = off[i]; k < off[i + 1]; ++k)
        out.emplace_back(child.string_at(k));
    return out;
}

}  // namespace

TEST_SUITE("string simd") {
    TEST_CASE("reports the dispatched target") {
        std::printf("string kernels: Highway target %s\n", simd::target_name());
        CHECK(std::string(simd::target_name()).size() > 0);
    }

    TEST_CASE("bit helpers") {
        std::vector<std::uint64_t> b(4, 0);
        for (std::size_t i : {0u, 63u, 64u, 130u, 255u})
            b[i >> 6] |= std::uint64_t{1} << (i & 63);
        CHECK(simd::bits_any(b.data(), 0, 1));
        CHECK(!simd::bits_any(b.data(), 1, 63));
        CHECK(simd::bits_any(b.data(), 1, 64));
        CHECK(simd::bits_next(b.data(), 1, 256) == 63);
        CHECK(simd::bits_next(b.data(), 131, 255) == 255);  // none: e
        CHECK(simd::bits_next(b.data(), 64, 65) == 64);
        CHECK(simd::bits_prev(b.data(), 0, 256) == 255);
        CHECK(simd::bits_prev(b.data(), 1, 63) == simd::NPOS);
        CHECK(simd::bits_prev(b.data(), 0, 130) == 64);
        CHECK(simd::bits_prev(b.data(), 65, 131) == 130);
    }

    TEST_CASE("case maps") {
        for (const Rows& in : corpora()) {
            Series s = make(in);
            both_modes([&](const char* mode) {
                expect_strings(Series{dftu_series_str_case(
                                   s.handle(), DFTU_STR_CAPITALIZE)},
                               map_rows(in, r_capitalize), mode);
                expect_strings(Series{dftu_series_str_case(
                                   s.handle(), DFTU_STR_TITLE_CASE)},
                               map_rows(in, r_title), mode);
                expect_strings(
                    Series{dftu_series_str_case(s.handle(), DFTU_STR_SWAPCASE)},
                    map_rows(in, r_swapcase), mode);
            });
        }
    }

    TEST_CASE("character classes") {
        for (const Rows& in : corpora()) {
            Series s = make(in);
            for (int cls = DFTU_STR_ALNUM; cls <= DFTU_STR_TITLE; ++cls) {
                std::vector<bool> want;
                for (const Row& r : in)
                    want.push_back(r ? r_is(*r, cls) : false);
                both_modes([&](const char* mode) {
                    expect_bools(Series{dftu_series_str_is(s.handle(), cls)},
                                 in, want, mode);
                });
            }
        }
    }

    TEST_CASE("character classes by row and by whole-buffer bit mask") {
        // Long strings take the early-exit per-row path; every length 0..300,
        // a bad byte first / last / nowhere / random, equal to the reference
        // under both strategies, the vector kernels and the forced byte loop.
        std::mt19937 rng(7);
        auto build = [&](int kind, std::size_t len, int cls) {
            std::string s(len, 'a');  // good for alnum/alpha/lower
            if (cls == DFTU_STR_DIGIT || cls == DFTU_STR_DECIMAL ||
                cls == DFTU_STR_NUMERIC)
                s.assign(len, '5');
            else if (cls == DFTU_STR_SPACE)
                s.assign(len, ' ');
            else if (cls == DFTU_STR_UPPER)
                s.assign(len, 'A');
            else if (cls == DFTU_STR_TITLE)
                for (std::size_t i = 0; i < len; ++i) s[i] = i % 5 ? 'a' : 'A';
            if (kind == 1 && len) s[0] = '\x01';
            if (kind == 2 && len) s[len - 1] = '\x01';
            if (kind == 3)
                for (char& c : s) c = static_cast<char>(rng() & 0xff);
            return s;
        };
        for (int cls = DFTU_STR_ALNUM; cls <= DFTU_STR_TITLE; ++cls) {
            Rows in;
            for (std::size_t len = 0; len <= 300; ++len)
                for (int kind = 0; kind < 4; ++kind)
                    in.push_back(build(kind, len, cls));
            in.push_back(Row{});
            std::vector<bool> want;
            for (const Row& r : in) want.push_back(r ? r_is(*r, cls) : false);
            Series s = make(in);
            for (simd::PredStrategy st :
                 {simd::PredStrategy::Bits, simd::PredStrategy::Rows,
                  simd::PredStrategy::Auto}) {
                simd::set_pred_strategy(st);
                both_modes([&](const char* mode) {
                    expect_bools(Series{dftu_series_str_is(s.handle(), cls)},
                                 in, want, mode);
                });
            }
            simd::set_pred_strategy(simd::PredStrategy::Auto);
        }
    }

    TEST_CASE("pad, zfill, repeat, remove, slice") {
        for (const Rows& in : corpora()) {
            Series s = make(in);
            both_modes([&](const char* mode) {
                for (std::int64_t w : {0, 1, 5, 64, 131}) {
                    expect_strings(
                        Series{dftu_series_str_pad_start(s.handle(), w, '*')},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     return r_pad(x, w, '*', 0);
                                 }),
                        mode);
                    expect_strings(
                        Series{dftu_series_str_pad_end(s.handle(), w, '*')},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     return r_pad(x, w, '*', 1);
                                 }),
                        mode);
                    expect_strings(
                        Series{dftu_series_str_center(s.handle(), w, '*')},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     return r_pad(x, w, '*', 2);
                                 }),
                        mode);
                    expect_strings(Series{dftu_series_str_zfill(s.handle(), w)},
                                   map_rows(in,
                                            [&](const std::string& x) {
                                                return r_zfill(x, w);
                                            }),
                                   mode);
                }
                for (std::int64_t n : {0, 1, 3}) {
                    expect_strings(
                        Series{dftu_series_str_repeat(s.handle(), n)},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     std::string o;
                                     for (std::int64_t k = 0; k < n; ++k)
                                         o += x;
                                     return o;
                                 }),
                        mode);
                }
                for (const std::string p : {"a", "ab", "", "--"}) {
                    expect_strings(
                        Series{dftu_series_str_remove_prefix(
                            s.handle(), p.data(),
                            static_cast<std::int32_t>(p.size()))},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     return !p.empty() && x.compare(0, p.size(),
                                                                    p) == 0
                                                ? x.substr(p.size())
                                                : x;
                                 }),
                        mode);
                    expect_strings(
                        Series{dftu_series_str_remove_suffix(
                            s.handle(), p.data(),
                            static_cast<std::int32_t>(p.size()))},
                        map_rows(
                            in,
                            [&](const std::string& x) {
                                return !p.empty() && x.size() >= p.size() &&
                                               x.compare(x.size() - p.size(),
                                                         p.size(), p) == 0
                                           ? x.substr(0, x.size() - p.size())
                                           : x;
                            }),
                        mode);
                }
                for (auto [st, ln] :
                     std::vector<std::pair<std::int64_t, std::int64_t>>{
                         {0, 3}, {2, -1}, {-3, 2}, {100, 5}, {0, 0}}) {
                    expect_strings(
                        Series{dftu_series_str_slice(s.handle(), st, ln)},
                        map_rows(in,
                                 [&](const std::string& x) {
                                     return r_slice(x, st, ln);
                                 }),
                        mode);
                }
            });
        }
    }

    TEST_CASE("split, partition, join, rfind") {
        for (const Rows& in : corpora()) {
            Series s = make(in);
            both_modes([&](const char* mode) {
                INFO(mode);
                for (const std::string sep : {",", "--", "ab", "", "abc"}) {
                    Series parts{dftu_series_str_split(
                        s.handle(), sep.data(),
                        static_cast<std::int32_t>(sep.size()))};
                    REQUIRE(parts.valid());
                    for (std::size_t i = 0; i < in.size(); ++i) {
                        const auto ii = static_cast<std::int64_t>(i);
                        REQUIRE(parts.is_null(ii) == !in[i].has_value());
                        if (in[i])
                            REQUIRE(list_row(parts, ii) ==
                                    r_split(*in[i], sep));
                    }
                    Series joined{
                        dftu_series_list_join(parts.handle(), "|", 1)};
                    REQUIRE(joined.valid());
                    for (std::size_t i = 0; i < in.size(); ++i) {
                        if (!in[i]) continue;
                        std::string want;
                        const auto p = r_split(*in[i], sep);
                        for (std::size_t k = 0; k < p.size(); ++k)
                            want += (k ? "|" : "") + p[k];
                        REQUIRE(std::string(joined.string_at(
                                    static_cast<std::int64_t>(i))) == want);
                    }
                    Series glued{dftu_series_list_join(parts.handle(), "", 0)};
                    REQUIRE(glued.valid());
                }
                for (const std::string sep : {",", "--", "ab", "abc"}) {
                    for (int right : {0, 1}) {
                        Series parts{dftu_series_str_partition(
                            s.handle(), sep.data(),
                            static_cast<std::int32_t>(sep.size()), right)};
                        REQUIRE(parts.valid());
                        for (std::size_t i = 0; i < in.size(); ++i) {
                            const auto ii = static_cast<std::int64_t>(i);
                            REQUIRE(parts.is_null(ii) == !in[i].has_value());
                            if (in[i])
                                REQUIRE(list_row(parts, ii) ==
                                        r_partition(*in[i], sep, right != 0));
                        }
                    }
                }
                for (const std::string n : {",", "--", "ab", "", "abc", "b-"}) {
                    Series r{dftu_series_str_rfind(
                        s.handle(), n.data(),
                        static_cast<std::int32_t>(n.size()))};
                    REQUIRE(r.valid());
                    for (std::size_t i = 0; i < in.size(); ++i) {
                        const auto ii = static_cast<std::int64_t>(i);
                        REQUIRE(r.is_null(ii) == !in[i].has_value());
                        if (!in[i]) continue;
                        const std::size_t hit = in[i]->rfind(n);
                        const std::int64_t want =
                            hit == std::string::npos
                                ? -1
                                : static_cast<std::int64_t>(hit);
                        REQUIRE(r.data<std::int64_t>()[i] == want);
                    }
                }
            });
        }
    }

    TEST_CASE("cat keeps the nulls of both sides") {
        const Rows a = corpus(3, false);
        Rows b = corpus(4, false);
        b.resize(a.size());
        Series sa = make(a), sb = make(b);
        Rows want;
        for (std::size_t i = 0; i < a.size(); ++i)
            want.push_back(a[i] && b[i] ? Row{*a[i] + *b[i]} : Row{});
        expect_strings(Series{dftu_series_str_cat(sa.handle(), sb.handle())},
                       want, "cat");
    }

    TEST_CASE("a column cut from a larger one and a dictionary column") {
        const Rows in = corpus(5, false);
        Series s = make(in);
        // A column whose offsets start past 0 (a window of a larger buffer):
        // 77 bytes of junk, then the rows.
        const Rows cut_rows(in.begin() + 40, in.begin() + 160);
        Series cut = make_offset(cut_rows, 77);
        both_modes([&](const char* mode) {
            expect_strings(
                Series{dftu_series_str_case(cut.handle(), DFTU_STR_TITLE_CASE)},
                map_rows(cut_rows, r_title), mode);
            expect_strings(
                Series{dftu_series_str_pad_start(cut.handle(), 20, '.')},
                map_rows(
                    cut_rows,
                    [](const std::string& x) { return r_pad(x, 20, '.', 0); }),
                mode);
            Series parts{dftu_series_str_split(cut.handle(), ",", 1)};
            REQUIRE(parts.valid());
            for (std::size_t i = 0; i < cut_rows.size(); ++i)
                if (cut_rows[i])
                    REQUIRE(list_row(parts, static_cast<std::int64_t>(i)) ==
                            r_split(*cut_rows[i], ","));
        });
        Series dict = s.dictionary_encode();
        if (dict.valid()) {
            expect_strings(
                Series{dftu_series_str_case(dict.handle(), DFTU_STR_SWAPCASE)},
                map_rows(in, r_swapcase), "dictionary");
        }
    }

    TEST_CASE("output past the int32 offsets is refused, not wrapped") {
        const Rows in{Row{std::string(1 << 20, 'a')},
                      Row{std::string(1 << 20, 'b')}};
        Series s = make(in);
        CHECK(dftu_series_str_repeat(s.handle(), 1 << 12) == nullptr);
    }
}
