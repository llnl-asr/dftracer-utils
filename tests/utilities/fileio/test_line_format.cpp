// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/fileio/json_line_format.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;

namespace {

template <line_format::FixedString Fmt, class E = line_format::PlainEscape,
          class... A>
dstd::String fmt_ct(const A&... a) {
    const auto v = line_format::view<Fmt>();
    dstd::String out(line_format::max_line_bytes<E>(v, a...), '\0');
    out.resize(line_format::format_line<E>(v, out.data(), a...));
    return out;
}

template <class E = line_format::PlainEscape, class... A>
dstd::String fmt_rt(dstd::StringView f, const A&... a) {
    auto lf = line_format::BasicLineFormat<E>::parse(f);
    REQUIRE(lf.has_value());
    const auto v = lf->view();
    REQUIRE(v.holes == sizeof...(A));
    dstd::String out(line_format::max_line_bytes<E>(v, a...), '\0');
    out.resize(line_format::format_line<E>(v, out.data(), a...));
    return out;
}

dstd::String gunzip_all(const dstd::String& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    dstd::String out;
    char b[4096];
    int n;
    while ((n = gzread(g, b, sizeof(b))) > 0) out.append(b, n);
    gzclose(g);
    return out;
}

template <class T>
concept CtOne = requires(GzipLineWriter& w, T v) { w.append_fmt<"{}">(v); };
template <class T>
concept RtOne = requires(GzipLineWriter& w, const line_format::LineFormat& f,
                         T v) { w.append_fmt(f, v); };
enum class Color { Red };

static_assert(CtOne<int> && CtOne<std::uint64_t> && CtOne<bool> &&
              CtOne<double> && CtOne<dstd::StringView> && CtOne<dstd::String> &&
              CtOne<const char*> && CtOne<line_format::Raw>);
static_assert(RtOne<int> && RtOne<dstd::String> && RtOne<line_format::Raw>);
template <class T>
concept JsonCtOne =
    requires(GzipLineWriter& w, T v) { w.append_json<"{}">(v); };
template <class T>
concept JsonRtOne =
    requires(GzipLineWriter& w, const line_format::JsonLineFormat& f, T v) {
        w.append_json(f, v);
    };
static_assert(JsonCtOne<int> && JsonCtOne<dstd::String> && JsonRtOne<bool> &&
              JsonRtOne<line_format::Raw>);
static_assert(!JsonCtOne<char> && !JsonCtOne<int*> && !JsonCtOne<Color> &&
              !JsonCtOne<std::nullptr_t> && !JsonRtOne<char> &&
              !JsonRtOne<Color> && !JsonRtOne<std::nullptr_t>);
static_assert(!CtOne<char> && !CtOne<int*> && !CtOne<std::nullptr_t> &&
              !CtOne<Color> && !CtOne<wchar_t>);
static_assert(!RtOne<char> && !RtOne<int*> && !RtOne<std::nullptr_t> &&
              !RtOne<Color> && !RtOne<wchar_t>);
template <class... A>
concept CtTwoHoles =
    requires(GzipLineWriter& w, A... a) { w.append_fmt<"{} {}">(a...); };
template <class... A>
concept CtNoHoles =
    requires(GzipLineWriter& w, A... a) { w.append_fmt<"x">(a...); };
template <class... A>
concept ProducerOneHole =
    requires(GzipLineWriter::Producer& p, A... a) { p.append_fmt<"{}">(a...); };
template <class... A>
concept PlainRtOnJson =
    requires(GzipLineWriter& w, const line_format::JsonLineFormat& f, A... a) {
        w.append_fmt(f, a...);
    };
template <class... A>
concept JsonRtOnPlain =
    requires(GzipLineWriter& w, const line_format::LineFormat& f, A... a) {
        w.append_json(f, a...);
    };
template <class... A>
concept CtNewline =
    requires(GzipLineWriter& w, A... a) { w.append_fmt<"x\n">(a...); };
template <class... A>
concept CtCarriageReturn =
    requires(GzipLineWriter& w, A... a) { w.append_json<"{}\r">(a...); };
static_assert(!CtNewline<> && !CtCarriageReturn<int>);
static_assert(CtTwoHoles<int, int> && !CtTwoHoles<int> &&
              !CtTwoHoles<int, int, int>);
static_assert(CtNoHoles<> && !CtNoHoles<int>);
static_assert(ProducerOneHole<int> && !ProducerOneHole<int, int>);
static_assert(!PlainRtOnJson<int> && !JsonRtOnPlain<int>);

}  // namespace

TEST_CASE("line_format - values and escapes") {
    CHECK(fmt_ct<"plain">() == "plain\n");
    CHECK(fmt_ct<"{{}}">() == "{}\n");
    CHECK(fmt_ct<"{ {}}">(7) == "{ 7}\n");
    CHECK(fmt_ct<"{} {} {} {}">(-5, std::uint64_t{18446744073709551615ull},
                                std::int8_t{-8}, std::uint16_t{9}) ==
          "-5 18446744073709551615 -8 9\n");
    CHECK(fmt_ct<"{} {}">(true, false) == "true false\n");
    CHECK(fmt_ct<"{} {} {}">(2.5, 0.1f, 1e300) == "2.5 0.1 1e+300\n");
    CHECK(fmt_ct<"\"{}\"">(dstd::StringView("a\"b\\c")) == "\"a\"b\\c\"\n");
    CHECK(fmt_ct<"\"{}\"", line_format::JsonEscape>(dstd::StringView(
              "a\"b\\c\n\t\x01")) == "\"a\\\"b\\\\c\\n\\t\\u0001\"\n");
    CHECK(fmt_ct<"{}", line_format::JsonEscape>(line_format::raw("a\"b")) ==
          "a\"b\n");
    CHECK(fmt_rt<line_format::JsonEscape>("{}", "q\"") == "q\\\"\n");
    CHECK(fmt_rt("{}", "q\"") == "q\"\n");
    CHECK(fmt_ct<"{}{}{}">(dstd::String("s"), "lit", (const char*)"p") ==
          "slitp\n");
    CHECK(fmt_ct<"{}">(line_format::raw("a\"b")) == "a\"b\n");
    CHECK(fmt_ct<"{}">(std::numeric_limits<std::int64_t>::min()) ==
          "-9223372036854775808\n");
}

TEST_CASE("line_format - raw literal JSON with object braces") {
    CHECK(fmt_ct<R"({"name":"{}","ts":{},"dur":{}})", line_format::JsonEscape>(
              "a\"b", 5, 7) == "{\"name\":\"a\\\"b\",\"ts\":5,\"dur\":7}\n");
    CHECK(fmt_ct<R"({"args":{{}},"v":{}})", line_format::JsonEscape>(
              line_format::raw("1")) == "{\"args\":{},\"v\":1}\n");
    CHECK(fmt_ct<"{{}} {} {{}}">(1) == "{} 1 {}\n");
}

TEST_CASE("line_format - run-time form equals compile-time form") {
#define F R"({"a":{{}},"n":"{}","k":{},"x":{},"b":{}})"
    CHECK(fmt_rt<line_format::JsonEscape>(F, "q\"", 7, 2.5, true) ==
          fmt_ct<F, line_format::JsonEscape>("q\"", 7, 2.5, true));
#undef F
    CHECK(fmt_rt("{{}}{}", line_format::raw("r")) ==
          fmt_ct<"{{}}{}">(line_format::raw("r")));
    CHECK(fmt_rt("x") == "x\n");
    CHECK(fmt_rt("") == "\n");
}

TEST_CASE("line_format - run-time parse errors") {
    for (const char* bad : {"a\n", "\n", "a\r", "{}\n{}"}) {
        auto r = line_format::LineFormat::parse(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().is_code(common::ErrorCode::InvalidArgument));
        auto j = line_format::JsonLineFormat::parse(bad);
        REQUIRE_FALSE(j.has_value());
        CHECK(j.error().is_code(common::ErrorCode::InvalidArgument));
    }
    CHECK(line_format::LineFormat::parse("a {\n}").has_value() == false);
    CHECK(line_format::LineFormat::parse("a { } {x}").has_value());
}

TEST_CASE("line_format - writer output equals hand-built lines") {
    // A unique folder, as two test processes must not share and remove it.
    dftu_utils_test::ScopedTestDir scoped("dftu_line_format_test");
    const dstd::Path& dir = scoped.path();
    const dstd::String p1 = dstd::String((dir / "ordered.gz").str());
    const dstd::String p2 = dstd::String((dir / "producer.gz").str());
    auto lf =
        line_format::JsonLineFormat::parse(R"({"id":{},"n":"{}","v":{}})");
    REQUIRE(lf.has_value());
    dstd::String want;
    for (int i = 0; i < 500; ++i)
        want += "{\"id\":" + dstd::to_string(i) + ",\"n\":\"n\\\"" +
                dstd::to_string(i) + "\",\"v\":" + (i % 2 ? "true" : "false") +
                "}\n";

    bool bad_count = false, null_arg = false;
    dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        o.member_size = 1024;
        auto w = co_await GzipLineWriter::open(p1, std::move(o));
        REQUIRE(w.has_value());
        for (int i = 0; i < 500; ++i) {
            const dstd::String n = "n\"" + dstd::to_string(i);
            if (i % 2) {
                REQUIRE(co_await w->append_json<R"({"id":{},"n":"{}","v":{}})">(
                    i, n, true));
            } else {
                REQUIRE(co_await w->append_json(*lf, i, n, false));
            }
        }
        auto r = co_await w->append_json(*lf, 1, 2);
        bad_count = !r.has_value() &&
                    r.error().is_code(common::ErrorCode::InvalidArgument);
        auto r2 = co_await w->append_json<"{}">((const char*)nullptr);
        null_arg = !r2.has_value() &&
                   r2.error().is_code(common::ErrorCode::InvalidArgument);
        REQUIRE(co_await w->close());

        GzipWriterOptions u;
        u.ordered = false;
        u.member_size = 1024;
        auto wu = co_await GzipLineWriter::open(p2, std::move(u));
        REQUIRE(wu.has_value());
        auto pr = wu->producer();
        REQUIRE(pr.has_value());
        for (int i = 0; i < 500; ++i) {
            const dstd::String n = "n\"" + dstd::to_string(i);
            REQUIRE(co_await pr->append_json(*lf, i, n, i % 2 != 0));
        }
        REQUIRE(co_await pr->flush());
        REQUIRE(co_await wu->close());
    });
    CHECK(bad_count);
    CHECK(null_arg);
    CHECK(gunzip_all(p1) == want);
    auto lines = [](const dstd::String& t) {
        dstd::MultiSet<dstd::String> out;
        for (std::size_t b = 0; b < t.size();) {
            const auto e = t.find('\n', b);
            out.insert(t.substr(b, e - b + 1));
            b = e + 1;
        }
        return out;
    };
    CHECK(lines(gunzip_all(p2)) == lines(want));
}
