// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <doctest/doctest.h>
#include <duql/filter_corpus.h>
#include <trace/views/fold_shapes.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace fold_shapes;

namespace {

using dftracer::utils::trace::views::detail::extract_fold_event_into;
using json::detail::ValueAccess;

String describe_reset() {
    StringIntern intern;
    return describe(FoldEvent{}, intern);
}

struct OnDemandRead {
    String text;
    bool parsed = false;
    bool broken = false;
};

OnDemandRead read_on_demand(json::Document& doc, StringView line,
                            bool needs_args, bool with_cache = false) {
    OnDemandRead out;
    auto value = doc.parse(line);
    if (!value) return out;
    out.parsed = true;
    StringIntern intern;
    dstd::InternCache cache(intern);
    FoldEvent ev;
    extract_fold_event_into(ev, *value, intern, needs_args,
                            with_cache ? &cache : nullptr);
    out.broken = ValueAccess::broken(*value);
    out.text = describe(ev, intern);
    return out;
}

// The two readers agree on `line`. A line the DOM parser rejects is one the
// on-demand reader drops as broken when it reaches the bad part, which the
// walk with args always does; without args, a bad part inside args is not
// reached.
void check_line(json::Document& doc, StringView line, const char* expected) {
    CAPTURE(line);
    for (const bool needs_args : {true, false}) {
        for (const bool with_cache : {false, true}) {
            CAPTURE(needs_args);
            CAPTURE(with_cache);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            const bool dom_ok = read_dom(line, dom_ev, dom_intern, needs_args);
            const OnDemandRead got =
                read_on_demand(doc, line, needs_args, with_cache);
            if (!got.parsed) {
                CHECK_FALSE(dom_ok);
                continue;
            }
            if (dom_ok) {
                CHECK_FALSE(got.broken);
                CHECK(got.text == describe(dom_ev, dom_intern));
                if (needs_args && expected) CHECK(got.text == expected);
            } else {
                if (needs_args) CHECK(got.broken);
                if (got.broken) CHECK(got.text == describe_reset());
            }
        }
    }
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Lines shaped like the ones dftracer_gen_fake_trace writes, with the extra
// args a real trace carries.
dstd::Vector<String> generated_lines(std::size_t count) {
    static const char* const NAMES[] = {"read",  "write",    "open64",
                                        "fsync", "H5Dwrite", "MPI_Send"};
    static const char* const CATS[] = {"POSIX", "STDIO", "MPI", "HDF5"};
    static const char* const EXTRAS[] = {
        R"("fname":"/p/lustre/data/a.h5","size":4096)",
        R"("ret":-1,"errno":2)",
        R"("offset":9223372036854775807,"flags":[1,2,3])",
        R"("io":{"off":1.5,"len":[1,{"k":"v"}]},"e":{},"f":[],"n":null)",
        R"("msg":"a\"bé","ok":true)",
        R"("big":18446744073709551615,"huge":123456789012345678901234567890)",
        R"("a.b":1,"a":{"b":2})",
    };
    dstd::Vector<String> lines;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t r = next_random(state);
        String line = common::str_cat(
            R"({"id":)", i, R"(,"name":")", NAMES[r % 6], R"(","cat":")",
            CATS[(r >> 8) % 4], R"(","pid":)", 1000 + (r >> 16) % 8,
            R"(,"tid":)", 10000 + (r >> 20) % 64, R"(,"ts":)",
            1000000000 + i * 100, R"(,"dur":)", (r >> 24) % 5000, R"(,"ph":)",
            1 + (r >> 40) % 3, R"(,"type":)", 1 + (r >> 44) % 9,
            R"(,"args":{"hhash":"h)", (r >> 48) % 4, R"(","level":)",
            (r >> 52) % 4);
        if ((r >> 56) & 1)
            line += common::str_cat(R"(,"fhash":"f)", r % 16, "\"");
        if ((r >> 57) & 1) line += common::str_cat(",", EXTRAS[(r >> 58) % 7]);
        if ((r >> 61) & 1) line += common::str_cat(",", EXTRAS[(r >> 3) % 7]);
        line += "}}";
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace

TEST_SUITE("FoldOnDemand") {
    TEST_CASE("the on-demand reader equals the DOM reader on every shape") {
        json::Document doc;
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            check_line(doc, shape.line, shape.expected);
        }
    }

    TEST_CASE("the on-demand reader equals the DOM reader on the corpus") {
        json::Document doc;
        for (const char* line : EVENTS) check_line(doc, line, nullptr);
    }

    TEST_CASE("whitespace after a value matches the DOM reader") {
        json::Document doc;
        const char* const lines[] = {
            "{\"ph\":\"X\"  ,\"name\":\"a\" ,\"cat\" : \"b\"\t,\"ts\":1 "
            ",\"dur\":2\n}",
            "{ \"ph\" : 1 , \"args\" : { \"s\" : \"v\" , \"fhash\" : "
            "\"f\"\r\n, \"n\" : 3 } }",
            "{\"ph\":\"X\"   }",
            "{\"ph\":1,\"args\":{\"esc\":\"a\\\"b\"  ,\"tail\":\"c\\\\\" }}",
        };
        for (const char* line : lines) check_line(doc, line, nullptr);
    }

    TEST_CASE("the on-demand reader equals the DOM reader on trace lines") {
        json::Document doc;
        for (const String& line : generated_lines(3000))
            check_line(doc, line, nullptr);
    }

    TEST_CASE("a part that is not reached is not checked") {
        json::Document doc;
        // The DOM parser rejects these lines. The reader does not read `id`,
        // an atom in a field it only type-checks, or args when it wants none.
        const OnDemandRead id =
            read_on_demand(doc, R"({"ph":1,"id":tru,"ts":5})", true);
        CHECK(id.parsed);
        CHECK_FALSE(id.broken);
        CHECK(id.text.find("ts=5") != String::npos);
        const OnDemandRead args =
            read_on_demand(doc, R"({"ph":1,"args":{"a":01},"ts":5})", false);
        CHECK_FALSE(args.broken);
        CHECK(args.text.find("ts=5") != String::npos);
        const OnDemandRead atom =
            read_on_demand(doc, R"({"ph":1,"cat":nul,"ts":5})", true);
        CHECK_FALSE(atom.broken);
        CHECK(atom.text.find("ts=5") != String::npos);
        StringIntern intern;
        FoldEvent ev;
        CHECK_FALSE(read_dom(R"({"ph":1,"id":tru,"ts":5})", ev, intern, true));
        CHECK_FALSE(read_dom(R"({"ph":1,"cat":nul,"ts":5})", ev, intern, true));
        CHECK_FALSE(
            read_dom(R"({"ph":1,"args":{"a":01},"ts":5})", ev, intern, false));
    }

    TEST_CASE("a broken line leaves the event reset and the next line clean") {
        json::Document doc;
        auto bad = doc.parse(R"({"ph":1,"ts":5,"args":{"a":01}})");
        REQUIRE(bad.has_value());
        StringIntern intern;
        FoldEvent ev;
        extract_fold_event_into(ev, *bad, intern, true);
        CHECK(ValueAccess::broken(*bad));
        CHECK(describe(ev, intern) == describe_reset());

        auto good = doc.parse(R"({"ph":1,"ts":6,"args":{"a":2}})");
        REQUIRE(good.has_value());
        extract_fold_event_into(ev, *good, intern, true);
        CHECK_FALSE(ValueAccess::broken(*good));
        CHECK(describe(ev, intern) ==
              "ph=1 pid=0 tid=0 ts=6 dur=- cat=- name=- fh=- hh=- "
              "args{\"a\"=i2} top{} sp{}");
    }

    TEST_CASE("a value that is not an object reads as a reset event") {
        json::Document doc;
        auto list = doc.parse(R"([{"ph":1}])");
        REQUIRE(list.has_value());
        StringIntern intern;
        FoldEvent ev;
        extract_fold_event_into(ev, *list, intern, true);
        CHECK_FALSE(ValueAccess::broken(*list));
        CHECK(describe(ev, intern) == describe_reset());
    }

    TEST_CASE("the reader leaves the line readable for the next consumer") {
        json::Document doc;
        const char* line =
            R"({"name":"read","ph":1,"ts":7,"args":{"fhash":"f","a":2}})";
        auto value = doc.parse(line);
        REQUIRE(value.has_value());
        const duql::Query query =
            *duql::Query::from_string(R"(name == "read")");
        StringIntern intern;
        FoldEvent first;
        FoldEvent second;
        FoldEvent third;
        // As the branch driver runs a line: a predicate, then each consumer
        // from the start of the event.
        CHECK(query.matches(*value));
        ValueAccess::rewind(*value);
        extract_fold_event_into(first, *value, intern, true);
        ValueAccess::rewind(*value);
        extract_fold_event_into(second, *value, intern, true);
        CHECK(query.matches(*value));
        ValueAccess::rewind(*value);
        extract_fold_event_into(third, *value, intern, false);
        CHECK_FALSE(ValueAccess::broken(*value));
        CHECK(describe(first, intern) == describe(second, intern));
        CHECK(describe(first, intern) ==
              "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=\"read\" fh=\"f\" hh=- "
              "args{\"a\"=i2} top{} sp{}");
        CHECK(describe(third, intern) ==
              "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=\"read\" fh=\"f\" hh=- "
              "args{} top{} sp{}");
    }

    TEST_CASE("a reused event slot carries nothing from the last line") {
        json::Document doc;
        StringIntern intern;
        FoldEvent ev;
        auto first = doc.parse(
            R"({"ph":1,"name":"n","ts":9,"args":{"fhash":"f","a":1,"b":[]}})");
        REQUIRE(first.has_value());
        extract_fold_event_into(ev, *first, intern, true);
        auto second = doc.parse(R"({"ph":2,"args":{"c":3}})");
        REQUIRE(second.has_value());
        extract_fold_event_into(ev, *second, intern, true);
        CHECK(describe(ev, intern) ==
              "ph=2 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
              "args{\"c\"=i3} top{} sp{}");
    }

    TEST_CASE(
        "a number too large for a double and a string with an invalid "
        "escape keep the line") {
        // The DOM parser rejects each of these lines, so the expected event is
        // written out here.
        struct OddShape {
            const char* name;
            const char* line;
            const char* expected;
        };
        const OddShape shapes[] = {
            {"args_plus_overflow", R"j({"ph":1,"args":{"a":1e400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=dinf \"x\"=i1} top{} sp{}"},
            {"args_minus_overflow", R"j({"ph":1,"args":{"a":-1e400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=d-inf \"x\"=i1} top{} sp{}"},
            {"args_big_exponent", R"j({"ph":1,"args":{"a":1.5E+999,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=dinf \"x\"=i1} top{} sp{}"},
            {"args_underflow_still_reads",
             R"j({"ph":1,"args":{"a":1e-400,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=d0 \"x\"=i1} top{} sp{}"},
            {"args_overflow_nested",
             R"j({"ph":1,"args":{"l":[1,1e400,-1e400],"o":{"k":1e400}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"l.0\"=i1 \"l.1\"=dinf \"l.2\"=d-inf \"o.k\"=dinf} top{} "
             "sp{}"},
            {"ts_overflow", R"j({"ph":1,"ts":1e400,"name":"n"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} "
             "top{} sp{}"},
            {"dur_overflow", R"j({"ph":1,"dur":-1e400,"ts":5})j",
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"ph_overflow", R"j({"ph":1e400,"ts":5})j",
             "ph=0 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"bad_escape_name", R"j({"ph":1,"name":"a\qb"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"a\\\\qb\" fh=- hh=- "
             "args{} top{} sp{}"},
            {"bad_escape_cat", R"j({"ph":1,"cat":"\q"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=\"\\\\q\" name=- fh=- hh=- "
             "args{} top{} sp{}"},
            {"bad_escape_arg", R"j({"ph":1,"args":{"a":"x\qy","b":2}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"x\\\\qy\" \"b\"=i2} top{} sp{}"},
            {"lone_high_surrogate", R"j({"ph":1,"args":{"a":"\ud800"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\ud800\"} top{} sp{}"},
            {"lone_low_surrogate", R"j({"ph":1,"args":{"a":"\udc00"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\udc00\"} top{} sp{}"},
            {"short_unicode_escape", R"j({"ph":1,"args":{"a":"\u12"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"\\\\u12\"} top{} sp{}"},
            {"bad_escape_key", R"j({"ph":1,"args":{"k\q":1,"b":2}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"k\\\\q\"=i1 \"b\"=i2} top{} sp{}"},
            {"bad_escape_nested_key", R"j({"ph":1,"args":{"o":{"k\q":1}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"o.k\\\\q\"=i1} top{} sp{}"},
            {"bad_escape_top_key", R"j({"k\q":1,"ph":1,"ts":5})j",
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"bad_escape_fhash", R"j({"ph":1,"args":{"fhash":"a\qb"}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"a\\\\qb\" hh=- "
             "args{} top{} sp{}"},
            {"ts_above_uint64", R"j({"ph":1,"ts":18446744073709551616})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
            {"ts_20_nines",
             R"j({"ph":1,"ts":99999999999999999999,"name":"n"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- args{} "
             "top{} sp{}"},
            {"args_bigint_20_over",
             R"j({"ph":1,"args":{"a":18446744073709551616,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"18446744073709551616\" \"x\"=i1} top{} sp{}"},
            {"args_bigint_20_nines",
             R"j({"ph":1,"args":{"a":99999999999999999999,"x":1}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"99999999999999999999\" \"x\"=i1} top{} sp{}"},
            {"args_bigint_20_nested",
             R"j({"ph":1,"args":{"l":[18446744073709551616,1],"o":{"k":99999999999999999999}}})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"l.0\"=s\"18446744073709551616\" \"l.1\"=i1 "
             "\"o.k\"=s\"99999999999999999999\"} top{} sp{}"},
            {"bad_escape_ph", R"j({"ph":"\q"})j",
             "ph=0 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
        };
        json::Document doc;
        for (const OddShape& shape : shapes) {
            INFO(String(shape.name) << " " << shape.line);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            const bool dom_ok = read_dom(shape.line, dom_ev, dom_intern, true);
            if (StringView(shape.name) != "args_underflow_still_reads")
                CHECK_FALSE(dom_ok);
            for (const bool needs_args : {true, false}) {
                CAPTURE(needs_args);
                const OnDemandRead got =
                    read_on_demand(doc, shape.line, needs_args);
                REQUIRE(got.parsed);
                CHECK_FALSE(got.broken);
                if (needs_args ||
                    StringView(shape.line).find("\"args\"") == StringView::npos)
                    CHECK(got.text == shape.expected);
                const OnDemandRead next = read_on_demand(
                    doc, R"j({"ph":1,"ts":9,"name":"z"})j", needs_args);
                CHECK_FALSE(next.broken);
                CHECK(next.text ==
                      "ph=1 pid=0 tid=0 ts=9 dur=- cat=- name=\"z\" fh=- "
                      "hh=- args{} top{} sp{}");
            }
        }
    }

    TEST_CASE(
        "text that is not a number or a closed string is still an error") {
        json::Document doc;
        const char* const bad_args[] = {"tru", "01",  "1x",  "-",
                                        "1.",  "+1",  ".5",  "NaN",
                                        "1e",  "1e+", "0x1", "1e400x"};
        for (const char* token : bad_args) {
            INFO(String("token ") << token);
            const String line = common::str_cat(
                R"({"ph":1,"ts":5,"args":{"a":)", token, R"(,"b":2}})");
            const OnDemandRead got = read_on_demand(doc, line, true);
            CHECK_FALSE((got.parsed && !got.broken));
            // A field the reader only type-checks never looks at an atom that
            // starts like no number.
            if (token[0] < '0' || token[0] > '9') continue;
            const String top =
                common::str_cat(R"({"ph":1,"ts":)", token, R"(,"name":"n"})");
            const OnDemandRead top_got = read_on_demand(doc, top, true);
            CHECK_FALSE((top_got.parsed && !top_got.broken));
        }
        const char* const cropped[] = {
            R"j({"ph":1,"args":{"a":"x)j", R"j({"ph":1,"args":{"a":"x\q)j",
            R"j({"ph":1,"args":{"a":"x\)j", R"j({"ph":1,"args":{"a":)j",
            R"j({"ph":1,"args":{"k\q":)j"};
        for (const char* line : cropped) {
            INFO(String("line ") << line);
            const OnDemandRead got = read_on_demand(doc, line, true);
            CHECK_FALSE((got.parsed && !got.broken));
        }
    }

    TEST_CASE("bad UTF-8 and raw control bytes in a string keep the line") {
        struct Repaired {
            String line;
            const char* expected;
        };
        const Repaired shapes[] = {
            {String("{\"ph\":1,\"name\":\"a\xff"
                    "b\"}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"a?b\" fh=- hh=- args{} "
             "top{} sp{}"},
            {String("{\"ph\":1,\"cat\":\"\xe2\x82\",\"ts\":5}"),
             "ph=1 pid=0 tid=0 ts=5 dur=- cat=\"??\" name=- fh=- hh=- args{} "
             "top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"a\":\"x\ty\",\"b\":2}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"a\"=s\"x?y\" \"b\"=i2} top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"k\tq\":1,\"b\":2}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"k?q\"=i1 \"b\"=i2} top{} sp{}"},
            {String("{\"ph\":1,\"args\":{\"fhash\":\"f\x01\"}}"),
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=\"f?\" hh=- args{} "
             "top{} sp{}"},
            {String("{\"\xff\":1,\"ph\":1,\"ts\":7}"),
             "ph=1 pid=0 tid=0 ts=7 dur=- cat=- name=- fh=- hh=- args{} top{} "
             "sp{}"},
        };
        json::Document doc;
        for (const Repaired& shape : shapes) {
            INFO(shape.line);
            StringIntern dom_intern;
            FoldEvent dom_ev;
            CHECK_FALSE(read_dom(shape.line, dom_ev, dom_intern, true));
            for (const bool needs_args : {true, false}) {
                const OnDemandRead got =
                    read_on_demand(doc, shape.line, needs_args);
                REQUIRE(got.parsed);
                CHECK_FALSE(got.broken);
                if (needs_args ||
                    StringView(shape.line).find("\"args\"") == StringView::npos)
                    CHECK(got.text == shape.expected);
                const OnDemandRead next = read_on_demand(
                    doc, R"j({"ph":1,"ts":9,"name":"z"})j", needs_args);
                CHECK_FALSE(next.broken);
            }
        }
        for (const String& broken : {String("{\"ph\":1,\x01\"ts\":5}"),
                                     String("{\"ph\":1,\xff\"ts\":5}"),
                                     String("{\"ph\":1,\"name\":\"a\tb"),
                                     String("{\"ph\":1,\"ts\":\x01 5}")}) {
            INFO(broken);
            const OnDemandRead got = read_on_demand(doc, broken, true);
            CHECK_FALSE((got.parsed && !got.broken));
        }
    }
}
