#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/fileio/json_line_format.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;
using dftu_utils_test::run_coro;
using line_format::JsonEscape;
using line_format::JsonLineFormat;
using line_format::LineFormat;
using line_format::PlainEscape;
using line_format::raw;

namespace {

template <line_format::FixedString Fmt, class E = PlainEscape, class... A>
std::string fmt_ct(const A&... a) {
    const auto v = line_format::view<Fmt>();
    std::string out(line_format::max_line_bytes<E>(v, a...), '\0');
    out.resize(line_format::format_line<E>(v, out.data(), a...));
    return out;
}

template <class E = PlainEscape, class... A>
std::string fmt_rt(std::string_view f, const A&... a) {
    auto lf = line_format::BasicLineFormat<E>::parse(f);
    REQUIRE(lf.has_value());
    const auto v = lf->view();
    REQUIRE(v.holes == sizeof...(A));
    std::string out(line_format::max_line_bytes<E>(v, a...), '\0');
    out.resize(line_format::format_line<E>(v, out.data(), a...));
    return out;
}

std::string gunzip_all(const std::string& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    std::string out;
    char b[4096];
    int n;
    while ((n = gzread(g, b, sizeof(b))) > 0) out.append(b, n);
    gzclose(g);
    return out;
}

template <class T>
concept CtOne = requires(GzipLineWriter& w, T v) { w.append_fmt<"{}">(v); };
template <class T>
concept RtOne = requires(GzipLineWriter& w, const LineFormat& f, T v) {
    w.append_fmt(f, v);
};
enum class Color { Red };

static_assert(CtOne<int> && CtOne<std::uint64_t> && CtOne<bool> &&
              CtOne<double> && CtOne<std::string_view> && CtOne<std::string> &&
              CtOne<const char*> && CtOne<line_format::Raw>);
static_assert(RtOne<int> && RtOne<std::string> && RtOne<line_format::Raw>);
template <class T>
concept JsonCtOne =
    requires(GzipLineWriter& w, T v) { w.append_json<"{}">(v); };
template <class T>
concept JsonRtOne = requires(GzipLineWriter& w, const JsonLineFormat& f, T v) {
    w.append_json(f, v);
};
static_assert(JsonCtOne<int> && JsonCtOne<std::string> && JsonRtOne<bool> &&
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
concept PlainRtOnJson = requires(GzipLineWriter& w, const JsonLineFormat& f,
                                 A... a) { w.append_fmt(f, a...); };
template <class... A>
concept JsonRtOnPlain = requires(GzipLineWriter& w, const LineFormat& f,
                                 A... a) { w.append_json(f, a...); };
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
    CHECK(fmt_ct<"\"{}\"">(std::string_view("a\"b\\c")) == "\"a\"b\\c\"\n");
    CHECK(fmt_ct<"\"{}\"", JsonEscape>(std::string_view("a\"b\\c\n\t\x01")) ==
          "\"a\\\"b\\\\c\\n\\t\\u0001\"\n");
    CHECK(fmt_ct<"{}", JsonEscape>(raw("a\"b")) == "a\"b\n");
    CHECK(fmt_rt<JsonEscape>("{}", "q\"") == "q\\\"\n");
    CHECK(fmt_rt("{}", "q\"") == "q\"\n");
    CHECK(fmt_ct<"{}{}{}">(std::string("s"), "lit", (const char*)"p") ==
          "slitp\n");
    CHECK(fmt_ct<"{}">(raw("a\"b")) == "a\"b\n");
    CHECK(fmt_ct<"{}">(std::numeric_limits<std::int64_t>::min()) ==
          "-9223372036854775808\n");
}

TEST_CASE("line_format - raw literal JSON with object braces") {
    CHECK(fmt_ct<R"({"name":"{}","ts":{},"dur":{}})", JsonEscape>(
              "a\"b", 5, 7) == "{\"name\":\"a\\\"b\",\"ts\":5,\"dur\":7}\n");
    CHECK(fmt_ct<R"({"args":{{}},"v":{}})", JsonEscape>(raw("1")) ==
          "{\"args\":{},\"v\":1}\n");
    CHECK(fmt_ct<"{{}} {} {{}}">(1) == "{} 1 {}\n");
}

TEST_CASE("line_format - run-time form equals compile-time form") {
#define F R"({"a":{{}},"n":"{}","k":{},"x":{},"b":{}})"
    CHECK(fmt_rt<JsonEscape>(F, "q\"", 7, 2.5, true) ==
          fmt_ct<F, JsonEscape>("q\"", 7, 2.5, true));
#undef F
    CHECK(fmt_rt("{{}}{}", raw("r")) == fmt_ct<"{{}}{}">(raw("r")));
    CHECK(fmt_rt("x") == "x\n");
    CHECK(fmt_rt("") == "\n");
}

TEST_CASE("line_format - run-time parse errors") {
    for (const char* bad : {"a\n", "\n", "a\r", "{}\n{}"}) {
        auto r = LineFormat::parse(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::INVALID_ARGUMENT);
        auto j = JsonLineFormat::parse(bad);
        REQUIRE_FALSE(j.has_value());
        CHECK(j.error().code == ErrorCode::INVALID_ARGUMENT);
    }
    CHECK(LineFormat::parse("a {\n}").has_value() == false);
    CHECK(LineFormat::parse("a { } {x}").has_value());
}

TEST_CASE("line_format - writer output equals hand-built lines") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "dftu_line_format_test";
    fs::create_directories(dir);
    const std::string p1 = (dir / "ordered.gz").string();
    const std::string p2 = (dir / "producer.gz").string();
    auto lf = JsonLineFormat::parse(R"({"id":{},"n":"{}","v":{}})");
    REQUIRE(lf.has_value());
    std::string want;
    for (int i = 0; i < 500; ++i)
        want += "{\"id\":" + std::to_string(i) + ",\"n\":\"n\\\"" +
                std::to_string(i) + "\",\"v\":" + (i % 2 ? "true" : "false") +
                "}\n";

    bool bad_count = false, null_arg = false;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        o.member_size = 1024;
        auto w = co_await GzipLineWriter::open(p1, o);
        REQUIRE(w.has_value());
        for (int i = 0; i < 500; ++i) {
            const std::string n = "n\"" + std::to_string(i);
            if (i % 2) {
                REQUIRE(co_await w->append_json<R"({"id":{},"n":"{}","v":{}})">(
                    i, n, true));
            } else {
                REQUIRE(co_await w->append_json(*lf, i, n, false));
            }
        }
        auto r = co_await w->append_json(*lf, 1, 2);
        bad_count =
            !r.has_value() && r.error().code == ErrorCode::INVALID_ARGUMENT;
        auto r2 = co_await w->append_json<"{}">((const char*)nullptr);
        null_arg =
            !r2.has_value() && r2.error().code == ErrorCode::INVALID_ARGUMENT;
        REQUIRE(co_await w->close());

        GzipWriterOptions u;
        u.ordered = false;
        u.member_size = 1024;
        auto wu = co_await GzipLineWriter::open(p2, u);
        REQUIRE(wu.has_value());
        auto pr = wu->producer();
        REQUIRE(pr.has_value());
        for (int i = 0; i < 500; ++i) {
            const std::string n = "n\"" + std::to_string(i);
            REQUIRE(co_await pr->append_json(*lf, i, n, i % 2 != 0));
        }
        REQUIRE(co_await pr->flush());
        REQUIRE(co_await wu->close());
    });
    CHECK(bad_count);
    CHECK(null_arg);
    CHECK(gunzip_all(p1) == want);
    auto lines = [](const std::string& t) {
        std::multiset<std::string> out;
        for (std::size_t b = 0; b < t.size();) {
            const auto e = t.find('\n', b);
            out.insert(t.substr(b, e - b + 1));
            b = e + 1;
        }
        return out;
    };
    CHECK(lines(gunzip_all(p2)) == lines(want));
    fs::remove_all(dir);
}
