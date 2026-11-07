// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <doctest/doctest.h>
#include <trace/views/fold_shapes.h>

using namespace dftracer::utils;
using namespace fold_shapes;
using dftracer::utils::trace::views::detail::DecodeHints;

TEST_SUITE("FoldShapes") {
    TEST_CASE("the DOM reader gives the pinned event for each shape") {
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            StringIntern intern;
            FoldEvent ev;
            const String got =
                read_dom(shape.line, ev, intern, /*needs_args=*/true)
                    ? describe(ev, intern)
                    : String("rejected");
            CHECK(got == shape.expected);
        }
    }

    TEST_CASE("without args the DOM reader keeps the fixed fields and hashes") {
        for (const Shape& shape : SHAPES) {
            CAPTURE(shape.name);
            StringIntern intern;
            FoldEvent with_args;
            FoldEvent without_args;
            const bool read =
                read_dom(shape.line, with_args, intern, /*needs_args=*/true);
            REQUIRE(read_dom(shape.line, without_args, intern,
                             /*needs_args=*/false) == read);
            if (!read) continue;
            with_args.args.clear();
            with_args.specials.clear();
            CHECK(describe(without_args, intern) ==
                  describe(with_args, intern));
        }
    }

    TEST_CASE("the DOM path reader gives the pinned event for each line") {
        struct Pinned {
            const char* line;
            const char* expected;
        };
        const Pinned lines[] = {
            {R"j({"k":1,"k":2,"s":"x"})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- args{\"k\"=i1 "
             "\"k\"=i2 "
             "\"s\"=s\"x\"} top{} sp{} lv{k:2 k:2 s:5} unc=0 bp=1"},
            {R"j({"o":{"a":{"b":1,"c":[1,2]},"z":"q"},"e":[],"n":null})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"o.a.b\"=i1 "
             "\"o.a.c.0\"=i1 \"o.a.c.1\"=i2 \"o.z\"=s\"q\"} top{} sp{\"e\":3 "
             "\"n\":0} lv{o.a.b:2 o.a.c.0:2 o.a.c.1:2 o.z:5 e:8 n:0} unc=0 "
             "bp=1"},
            {R"j([{"k":1},[2],"s"])j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"0.k\"=i1 "
             "\"1.0\"=i2 \"2\"=s\"s\"} top{} sp{} lv{0.k:2 1.0:2 2:5} unc=0 "
             "bp=1"},
            {R"j({"b":123456789012345678901234567890,"u":18446744073709551615,"f":2.5,"t":true})j",
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"b\"=s\"123456789012345678901234567890\" "
             "\"u\"=u18446744073709551615 \"f\"=d2.5 \"t\"=i1} top{} "
             "sp{\"t\":2} "
             "lv{b:0 u:3 f:4 t:1} unc=0 bp=1"},
        };
        const PathOptions plain("plain");
        for (const Pinned& pinned : lines) {
            CAPTURE(pinned.line);
            StringIntern intern;
            FoldEvent ev;
            DecodeHints hints;
            REQUIRE(read_dom_path(pinned.line, ev, intern, plain, hints));
            CHECK(describe_path(ev, intern) == pinned.expected);
        }
    }

    TEST_CASE("the DOM captures give the pinned event for each line") {
        struct Pinned {
            const char* line;
            const char* expected;
        };
        const Pinned lines[] = {
            {CAPTURE_LINES[0],
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"io.off\"=d1.5 "
             "\"io.len.0\"=i1 \"io.len.1.k\"=s\"v\" \"cqe.raw_ns\"=i7 "
             "\"size\"=i4096 \"tags.0.name\"=s\"a\" \"tags.1.name\"=s\"b\" "
             "\"l[1]\"=i2 \"a.b.c\"=i5} top{\"type\"=i3 \"id\"=i5} sp{}"},
            {CAPTURE_LINES[2],
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=\"n\" fh=- hh=- "
             "args{\"a\"=i2 "
             "\"size\"=i3} top{\"type\"=s\"t\"} sp{}"},
            {CAPTURE_LINES[3],
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"hosts.0\"=s\"h0\" \"hosts.1\"=s\"h1\" \"k.q\"=i2} top{} "
             "sp{\"type\":4t \"id\":3t}"},
            {CAPTURE_LINES[8],
             "ph=1 pid=0 tid=0 ts=0 dur=- cat=- name=- fh=- hh=- "
             "args{\"big\"=s\"123456789012345678901234567890\" "
             "\"u\"=u18446744073709551615 \"s\"=s\"a\\\"b\" \"d\"=d2.5 "
             "\"t\"=i1 "
             "\"x\"=d1e+05} top{} sp{\"t\":2}"},
        };
        const dstd::Vector<String> names{"io.off",     "type", "l[1]", "a.b",
                                         "cqe.raw_ns", "id",   "x"};
        for (const Pinned& pinned : lines) {
            CAPTURE(pinned.line);
            StringIntern intern;
            FoldEvent ev;
            REQUIRE(read_dom_captures(pinned.line, ev, intern, true, names));
            CHECK(describe(ev, intern) == pinned.expected);
        }
    }
}
