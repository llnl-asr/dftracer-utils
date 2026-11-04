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
}
