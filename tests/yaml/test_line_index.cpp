// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/records.h>
#include <doctest/doctest.h>
#include <yaml/yaml_dump.h>
#include <yaml/yaml_gen.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using dftu_yaml_test::dump;
using dftu_yaml_test::Gen;
using dftu_yaml_test::matches;
using dftu_yaml_test::Tree;
using yaml::Document;
using yaml::LineIndex;
using yaml::Records;
using yaml::RecordsOptions;
using yaml::Value;

namespace {

dstd::String error_of(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "n.yaml");
    REQUIRE_FALSE(doc.has_value());
    return dstd::String(doc.error().message());
}

Document must_parse(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "");
    REQUIRE(doc.has_value());
    return std::move(*doc);
}

void check_same_index(const LineIndex& a, const LineIndex& b) {
    REQUIRE(a.line_count == b.line_count);
    CHECK(std::memcmp(a.entries.data(), b.entries.data(),
                      (a.line_count + 1) * sizeof(yaml::LineEntry)) == 0);
    CHECK(std::memcmp(a.next.data(), b.next.data(),
                      (a.line_count + 1) * sizeof(std::uint32_t)) == 0);
    CHECK(a.escapes.size() == b.escapes.size());
    REQUIRE(a.arena.size() == b.arena.size());
    CHECK(std::memcmp(a.arena.data(), b.arena.data(), a.arena.size()) == 0);
    REQUIRE(a.tape.size() == b.tape.size());
    for (std::size_t i = 0; i < a.tape.size(); ++i) {
        const yaml::TapeEntry& x = a.tape[i];
        const yaml::TapeEntry& y = b.tape[i];
        CHECK((x.pos == y.pos && x.size == y.size && x.kind == y.kind &&
               x.flags == y.flags));
    }
}

}  // namespace

TEST_CASE("line index: every text builds, and a fault is an error") {
    LineIndex ix;
    for (const char* text :
         {"a: 1\n", "[a, b]\n", "{a: [1,\n 2]}\n", "- - a\n  - b\n",
          "--- |\n x\n", "", "\n\n", "# c\n", "a: \"x\n y\"\n"}) {
        INFO("text: ", dstd::String(text));
        CHECK(ix.build(text, std::strlen(text)) == yaml::BuildResult::Accepted);
    }
    for (const char* text :
         {"a: &x 1\n", "a: [1\n", "a: 1\na: 2\n", "- a\nb: 1\n"}) {
        INFO("text: ", dstd::String(text));
        CHECK(ix.build(text, std::strlen(text)) == yaml::BuildResult::Invalid);
    }
}

TEST_CASE("line index: each kind of error, with its message") {
    struct Case {
        const char* what;
        const char* text;
        const char* message;
    };
    const Case CASES[] = {
        {"duplicate key", "a: 1\nb: 2\na: 3\n",
         "n.yaml:3:1: duplicate key 'a'"},
        {"duplicate key, quoted", "\"a\": 1\n'a': 2\n",
         "n.yaml:2:1: duplicate key 'a'"},
        {"duplicate key in a compact map", "- a: 1\n  b: 2\n  a: 3\n",
         "n.yaml:3:3: duplicate key 'a'"},
        {"duplicate key in a flow map", "{a: 1, b: 2, a: 3}\n",
         "n.yaml:1:14: duplicate key 'a'"},
        {"indent matches no level", "a:\n    b: 1\n  c: 2\n",
         "n.yaml:3:3: indentation does not match any open block"},
        {"item after keys", "a: 1\n- b\n",
         "n.yaml:2:1: a sequence item cannot follow mapping keys at the same "
         "indentation"},
        {"key after items", "- a\nb: 1\n",
         "n.yaml:2:1: a mapping key cannot follow sequence items at the same "
         "indentation"},
        {"unterminated double quote", "a: \"abc\n",
         "n.yaml:1:4: unterminated quoted scalar"},
        {"text after a quoted value", "a: \"x\" y\n",
         "n.yaml:1:8: unexpected text after a quoted scalar"},
        {"unknown escape", "a: \"\\q\"\n",
         "n.yaml:1:5: unknown escape character 'q'"},
        {"deeper line after a quoted value", "a: \"x\"\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"key in a plain value that goes on", "a: b\n  c: d\n",
         "n.yaml:2:4: mapping values are not allowed in a multi-line plain "
         "scalar"},
        {"colon in a plain value", "a: b: c\n",
         "n.yaml:1:5: mapping values are not allowed in this context"},
        {"text after a flow value", "a: [1] x\n",
         "n.yaml:1:8: unexpected text after a flow collection"},
        {"unterminated flow", "a: [1, 2\n",
         "n.yaml:1:4: unterminated flow sequence"},
    };
    for (const Case& c : CASES) {
        INFO("kind: ", dstd::String(c.what));
        LineIndex probe;
        CHECK(probe.build(c.text, std::strlen(c.text)) ==
              yaml::BuildResult::Invalid);
        CHECK(error_of(c.text) == dstd::String(c.message));
    }
}

TEST_CASE("line index: a refused feature, at every position, with its column") {
    struct Where {
        const char* head;
        const char* tail;
        std::size_t column;
    };
    struct Feature {
        const char* text;
        const char* reason;
    };
    const Feature FEATURES[] = {{"&x v", "anchors are not supported"},
                                {"*x", "aliases are not supported"},
                                {"!!str v", "tags are not supported"}};
    const Where WHERES[] = {
        {"k: ", "\n", 4},   {"- ", "\n", 3},       {"", "\n", 1},
        {"k: [", "]\n", 5}, {"k: {a: ", "}\n", 8}, {"- k: ", "\n", 6},
        {"{", ": 1}\n", 2},
    };
    for (const Feature& f : FEATURES) {
        for (const Where& w : WHERES) {
            const dstd::String text =
                dstd::String(w.head) + f.text + dstd::String(w.tail);
            INFO("text: ", text);
            const dstd::String message = error_of(text);
            const dstd::String expected =
                dstd::fmt<"n.yaml:1:{}: {}">(w.column, f.reason);
            CHECK(message == expected);
        }
    }
    CHECK(error_of("? a\n: b\n") ==
          "n.yaml:1:1: complex keys are not supported");
    CHECK(error_of("{? a: b}\n") ==
          "n.yaml:1:2: complex keys are not supported");
    CHECK(error_of("~: 1\n") == "n.yaml:1:1: a null key is not supported");
    CHECK(error_of("{null: 1}\n") == "n.yaml:1:2: a null key is not supported");
    CHECK(error_of("[a]: 1\n") == "n.yaml:1:4: a collection cannot be a key");
    CHECK(error_of("%YAML 1.2\n---\na: 1\n") ==
          "n.yaml:1:1: directives are not supported");
}

TEST_CASE("line index: a tab in indentation and a tab in a value") {
    const Document doc = must_parse("a:\t1\nb: x\ty\nc:\t[1,\t2]\n");
    CHECK(doc.root().find("a")->raw() == "1");
    CHECK(doc.root().find("b")->raw() == "x\ty");
    CHECK(doc.root().find("c")->is_array());
    CHECK(error_of("a:\n\tb: 1\n").find("n.yaml:2:") == 0);
    CHECK(error_of("a:\n  \tb: 1\n").find("n.yaml:2:") == 0);
    CHECK(error_of("- \t- a\n").find("n.yaml:1:") == 0);
    CHECK(must_parse("\t[a, b]\n").root().is_array());
}

TEST_CASE("line index: a nested sequence on the line of its dash") {
    const Document doc = must_parse("- - a\n  - b\n- - - c\n    - d\n- e\n");
    CHECK(dump(doc.root()) == "[['a','b',],[['c','d',],],'e',]");
    const Document in_map = must_parse("k:\n  - - a\n    - b\n  - c\n");
    CHECK(dump(in_map.root()) == "{k:[['a','b',],'c',],}");
    const Document mixed = must_parse("- - k: 1\n    m: 2\n  - x\n");
    CHECK(dump(mixed.root()) == "[[{k:'1',m:'2',},'x',],]");
}

TEST_CASE("line index: multi-line flow values and a flow value then text") {
    const Document doc = must_parse(
        "a: [1,\n  [2,\n   3],\n  {k: v,\n   l: w}]\nb: {x: 1,\n  y: [2,\n "
        "3]}\nc: end\n");
    CHECK(dump(doc.root()) ==
          "{a:['1',['2','3',],{k:'v',l:'w',},],b:{x:'1',y:['2','3',],},c:'end',"
          "}");
    CHECK(error_of("a: [1,\n  2] x\n").find("n.yaml:2:") == 0);
    CHECK(error_of("a: {k: 1}\n  more\n").find("n.yaml:2:") == 0);
    CHECK(error_of("a: [1,\n2]\n").find("n.yaml:") == 0);
}

TEST_CASE("line index: a JSON text of many megabytes on one line") {
    dstd::String json = "[";
    for (int i = 0; json.size() < (std::size_t{50} << 20); ++i) {
        if (i != 0) json += ',';
        json +=
            dstd::fmt<"{\"id\":{},\"tags\":[\"a\",\"b\"],\"v\":\"s{}\"}">(i, i);
    }
    json += "]\n";
    const Document doc = must_parse(json);
    REQUIRE(doc.root().is_array());
    std::size_t count = 0;
    doc.root().for_each_element([&](Value element) {
        CHECK(element.find("id")->as_uint() == count);
        ++count;
    });
    CHECK(count > 1000000);
}

TEST_CASE("line index: lines and quotes across 64-byte blocks") {
    for (std::size_t pad = 0; pad < 200; ++pad) {
        INFO("pad: ", pad);
        const dstd::String p(pad + 1, 'p');
        const dstd::String key = "k" + p;
        const Document block = must_parse(key + ": |\n  one\n  two\nz: 1\n");
        CHECK(block.root().find(key)->raw() == "one\ntwo\n");
        CHECK(block.root().find("z")->as_uint() == 1);
        const Document folded =
            must_parse("k: >\n  " + p + "\n  two\n\n  three\nz: 1\n");
        CHECK(folded.root().find("k")->raw() == p + " two\nthree\n");
        const Document dq =
            must_parse("k: \"" + p + "\n  two\n  \\x41 three\"\nz: 1\n");
        CHECK(dq.root().find("k")->raw() == p + " two A three");
        const Document sq =
            must_parse("k: '" + p + "\n\n  it''s\n  three'\nz: 1\n");
        CHECK(sq.root().find("k")->raw() == p + "\nit's three");
        const Document plain =
            must_parse("k: " + p + "\n  two\n  three\nz: 1\n");
        CHECK(plain.root().find("k")->raw() == p + " two three");
        const Document crlf =
            must_parse("a: 1\r\nb: \"" + p + "\"\r\nc: '" + p + "'\r\n");
        CHECK(crlf.root().find("b")->raw() == p);
        CHECK(crlf.root().find("c")->raw() == p);
        const Document comment = must_parse(dstd::String(pad, ' ') +
                                            "# c\na: 'q''q'\nb: \"x\\ny\"\n");
        CHECK(comment.root().find("a")->raw() == "q'q");
        CHECK(comment.root().find("b")->raw() == "x\ny");
        const Document lead =
            must_parse(dstd::String(pad, '\n') + "a: |\n  x\n  y\nb: 1\n");
        CHECK(lead.root().find("a")->raw() == "x\ny\n");
        const Document keep = must_parse("- |+\n  " + p + "\n\n\n- b\n");
        CHECK(dump(keep.root()) == "['" + p + "\n\n\n','b',]");
    }
}

TEST_CASE("line index: scalars across the 4096-byte batches of stage 1") {
    for (std::size_t base : {4096, 8192}) {
        for (std::size_t pad = base - 80; pad < base + 70; ++pad) {
            INFO("pad: ", pad);
            const dstd::String filler =
                "# " + dstd::String(pad > 2 ? pad - 2 : 0, 'p') + "\n";
            CHECK(must_parse(filler + "a: |\n  one\n  two\n\n  three\nb: 1\n")
                      .root()
                      .find("a")
                      ->raw() == "one\ntwo\n\nthree\n");
            CHECK(must_parse(filler +
                             "a: >-\n  one\n  two\n\n   more\n  end\nb: 1\n")
                      .root()
                      .find("a")
                      ->raw() == "one two\n\n more\nend");
            CHECK(must_parse(filler + "a: \"one\n  two\n\n  three\"\nb: 1\n")
                      .root()
                      .find("a")
                      ->raw() == "one two\nthree");
            CHECK(must_parse(filler + "a: 'one\n  two\n\n  three'\nb: 1\n")
                      .root()
                      .find("a")
                      ->raw() == "one two\nthree");
            CHECK(must_parse(filler + "a: one\n  two\n\n  three\nb: 1\n")
                      .root()
                      .find("a")
                      ->raw() == "one two\nthree");
            CHECK(must_parse(filler + "a: [1,\n  2]\nb: 1\n")
                      .root()
                      .find("a")
                      ->is_array());
            CHECK(must_parse(filler + "--- |\n x\n y\n...\n").root().raw() ==
                  "x\ny\n");
        }
    }
}

TEST_CASE("line index: the scalar Highway target builds the same index") {
    dstd::String text;
    for (std::size_t i = 0; i < 3000; ++i) {
        text += "- id: " + dstd::fmt<"{}">(i) + "\n  name: \"n\\t" +
                dstd::fmt<"{}">(i * 7) +
                "\"\n  list:\n    - a\n    - 'b''c'\n  f: [1,\n    {k: v}]\n  "
                "d: |\n    x\n";
        if (i % 7 == 0) text += "  # comment\n\n";
    }
    LineIndex best;
    REQUIRE(best.build(text.data(), text.size()) ==
            yaml::BuildResult::Accepted);
    yaml::detail::set_scalar_line_masks_for_test(true);
    LineIndex scalar;
    const bool ok =
        scalar.build(text.data(), text.size()) == yaml::BuildResult::Accepted;
    yaml::detail::set_scalar_line_masks_for_test(false);
    REQUIRE(ok);
    check_same_index(best, scalar);
}

// Lines of each length around the 57 bits that one short window holds and the
// 64 of a block, as keys, values, items and spaces, at each offset in a block.
TEST_CASE("line index: lines around the short window read the same way") {
    for (std::size_t lead = 0; lead < 64; lead += 7) {
        dstd::String text = "pad: " + dstd::String(lead, 'p') + "\n";
        for (std::size_t len = 50; len <= 70; ++len) {
            const std::size_t fill = len - 9;
            text += "k" + dstd::fmt<"{}">(len) + ": " +
                    dstd::String(fill - 2, 'v') + "  \n";
            text += "l" + dstd::fmt<"{}">(len) + ":\n  - " +
                    dstd::String(fill, 'x') +
                    "\n  - y: " + dstd::String(fill, 'z') + "\n";
            text += "q" + dstd::fmt<"{}">(len) + dstd::String(fill, 'w') +
                    ": 1\n" + dstd::String(len, ' ') + "\n";
            // A `: ` or a ` #` near the end changes what the line is.
            text += "s" + dstd::fmt<"{}">(len) + ":\n  - " +
                    dstd::String(len - 7, 'a') + ": v\n";
            text += "c" + dstd::fmt<"{}">(len) + ": " +
                    dstd::String(len - 9, 'b') + " #c\n";
        }
        LineIndex plain;
        REQUIRE(plain.build(text.data(), text.size()) ==
                yaml::BuildResult::Accepted);
        yaml::detail::set_slow_lines_for_test(true);
        LineIndex slow;
        const auto slow_ok = slow.build(text.data(), text.size());
        yaml::detail::set_slow_lines_for_test(false);
        REQUIRE(slow_ok == yaml::BuildResult::Accepted);
        check_same_index(plain, slow);
    }
}

TEST_CASE("line index: the plain-line path and the byte classifier agree") {
    std::size_t built = 0;
    for (std::size_t seed = 1; seed <= 20000; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        const dstd::String text = gen.text(tree);
        LineIndex plain;
        const auto plain_ok = plain.build(text.data(), text.size());
        yaml::detail::set_slow_lines_for_test(true);
        LineIndex slow;
        const auto slow_ok = slow.build(text.data(), text.size());
        yaml::detail::set_slow_lines_for_test(false);
        REQUIRE(plain_ok == slow_ok);
        if (plain_ok != yaml::BuildResult::Accepted) continue;
        ++built;
        check_same_index(plain, slow);
    }
    CHECK(built > 19000);
    // Lines of more than 65534 spaces, blank or not.
    for (const std::size_t spaces : {65533u, 65534u, 65535u, 65536u, 70000u}) {
        const dstd::String pad(spaces, ' ');
        const dstd::String texts[] = {
            "a: 1\n" + pad + "\nb: 2\n",
            "a: 1\n" + pad + "\n",
            pad + "\n",
            "- a: 1\n" + pad + "# c\n- b: 2\n",
        };
        for (const auto& text : texts) {
            LineIndex plain;
            const auto plain_ok = plain.build(text.data(), text.size());
            yaml::detail::set_slow_lines_for_test(true);
            LineIndex slow;
            const auto slow_ok = slow.build(text.data(), text.size());
            yaml::detail::set_slow_lines_for_test(false);
            CHECK(plain_ok == slow_ok);
            if (plain_ok == yaml::BuildResult::Accepted)
                check_same_index(plain, slow);
        }
    }
}

TEST_CASE("line index: a generated text reads as its tree on every target") {
    for (std::size_t seed = 1; seed <= 2000; ++seed) {
        Gen gen(seed);
        const Tree tree = gen.tree();
        const dstd::String text = gen.text(tree);
        yaml::detail::set_scalar_line_masks_for_test(true);
        yaml::detail::set_slow_lines_for_test(true);
        auto doc = Document::parse(text, "g");
        yaml::detail::set_slow_lines_for_test(false);
        yaml::detail::set_scalar_line_masks_for_test(false);
        REQUIRE(doc.has_value());
        dstd::String why;
        CHECK_MESSAGE(matches(tree, doc->root(), why), why);
    }
}

TEST_CASE("line index: an error in Records names the line of the whole text") {
    for (std::size_t chunk :
         {std::size_t{1}, std::size_t{64}, std::size_t{1} << 20}) {
        dstd::String text;
        for (std::size_t i = 0; i < 50; ++i)
            text += "- id: " + dstd::fmt<"{}">(i) + "\n  name: \"n\\t\"\n";
        text += "- id: 50\n  id: 51\n- id: 52\n";
        RecordsOptions options;
        options.chunk_bytes = chunk;
        Records records = Records::from_text(text, "r.yaml", options);
        dstd::String error;
        for (;;) {
            auto next = records.next();
            if (!next.has_value()) {
                error = dstd::String(next.error().message());
                break;
            }
            if (!next->has_value()) break;
        }
        CHECK(error == "r.yaml:102:3: duplicate key 'id'");
    }
}

TEST_CASE("line index: records from a sequence and from documents") {
    dstd::String seq;
    dstd::String docs;
    for (std::size_t i = 0; i < 400; ++i) {
        seq += "- id: " + dstd::fmt<"{}">(i) +
               "\n  tag: 'x'\n  n:\n    a: 1\n  f: [1,\n    2]\n";
        docs += "---\nid: " + dstd::fmt<"{}">(i) + "\nname: \"d\\t" +
                dstd::fmt<"{}">(i) + "\"\nf: {a: 1,\n  b: 2}\n";
    }
    const dstd::String seq_bom = dstd::String("\xEF\xBB\xBF") + seq;
    const dstd::String docs_bom = dstd::String("\xEF\xBB\xBF") + docs;
    const dstd::String* const all[] = {&seq, &docs, &seq_bom, &docs_bom};
    for (const dstd::String* text : all) {
        for (std::size_t chunk : {std::size_t{1}, std::size_t{64},
                                  std::size_t{1000}, std::size_t{1} << 20}) {
            RecordsOptions options;
            options.chunk_bytes = chunk;
            Records records = Records::from_text(*text, "r.yaml", options);
            std::size_t count = 0;
            for (;;) {
                auto next = records.next();
                REQUIRE(next.has_value());
                if (!next->has_value()) break;
                CHECK((*next)->find("id")->as_uint() == count);
                CHECK((*next)->find("f")->is_array() ==
                      (text == &seq || text == &seq_bom));
                ++count;
            }
            CHECK(count == 400);
        }
    }
}

TEST_CASE(
    "line index: a value on a later line, after blank and comment lines") {
    struct Value {
        const char* what;
        dstd::Vector<dstd::String> lines;
        const char* dump;
    };
    const Value VALUES[] = {
        {"plain", {"p"}, "'p'"},
        {"plain over lines", {"p", "q"}, "'p q'"},
        {"double quoted", {"\"q\""}, "'q'"},
        {"quoted over lines", {"\"a", "b\""}, "'a b'"},
        {"flow", {"[x, y]"}, "['x','y',]"},
        {"flow over lines", {"{a: [1,", " 2]}"}, "{a:['1','2',],}"},
        {"literal", {"|", "x"}, "'x\n'"},
        {"folded", {">", "x", "y"}, "'x y\n'"},
    };
    struct Context {
        const char* what;
        const char* head;
        std::size_t indent;
        const char* tail;
        const char* before;
        const char* after;
    };
    const Context CONTEXTS[] = {
        {"top key", "k:", 2, "z: 1\n", "{k:", ",z:'1',}"},
        {"map in a seq item", "- k:", 4, "  z: 1\n", "[{k:", ",z:'1',},]"},
        {"map in a nested item", "- - k:", 6, "    z: 1\n",
         "[[{k:", ",z:'1',},],]"},
        {"seq item", "-", 2, "- 1\n", "[", ",'1',]"},
        {"nested seq item", "- -", 4, "  - 1\n", "[[", ",'1',],]"},
        {"map in a map", "m:\n  k:", 4, "  z: 1\ny: 2\n",
         "{m:{k:", ",z:'1',},y:'2',}"},
        {"map in a seq in a map", "m:\n  - k:", 6, "    z: 1\n  - 2\ny: 3\n",
         "{m:[{k:", ",z:'1',},'2',],y:'3',}"},
    };
    const char* const GAPS[] = {"\n", "# c\n", "\n# c\n\n", "  # c\n\n"};
    for (const Context& c : CONTEXTS) {
        for (const Value& v : VALUES) {
            for (const char* gap : GAPS) {
                const dstd::String pad(c.indent, ' ');
                dstd::String text = dstd::String(c.head) + "\n" + gap;
                for (std::size_t i = 0; i < v.lines.size(); ++i) {
                    text += pad + v.lines[i];
                    text += "\n";
                }
                text += c.tail;
                INFO("context: ", dstd::String(c.what),
                     ", value: ", dstd::String(v.what), "\n", text);
                auto doc = Document::parse(text, "g");
                REQUIRE(doc.has_value());
                CHECK(dump(doc->root()) ==
                      dstd::String(c.before) + v.dump + c.after);
            }
        }
    }
}

TEST_CASE("line index: scalars of several lines, accepted or refused") {
    // 'A' is read, 'E' is an error.
    struct Case {
        const char* text;
        char want;
    };
    const Case CASES[] = {
        {"a: |\n  x\n", 'A'},
        {"a: |-\n  x\n", 'A'},
        {"a: |+\n  x\n\n\n", 'A'},
        {"a: |\n  x\n\n\n", 'A'},
        {"a: |+\n\n\n", 'A'},
        {"a: |\n\n", 'A'},
        {"a: |-\n\n", 'A'},
        {"a: >\n  a\n  b\n\n  c\n", 'A'},
        {"a: >\n  a\n   b\n  c\n", 'A'},
        {"a: >\n  a\n\n   b\n\n  c\n", 'A'},
        {"a: >\n\n  a\n", 'A'},
        {"a: |2\n   x\n", 'A'},
        {"a: |1-\n x\n", 'A'},
        {"a: |-1\n x\n", 'A'},
        {"a: | # c\n  x\n", 'A'},
        {"a: |#c\n  x\n", 'E'},
        {"a: |x\n  y\n", 'E'},
        {"a: |0\n x\n", 'E'},
        {"a: |10\n x\n", 'E'},
        {"a: |\n  x\n # c\n", 'A'},
        {"a: |\n  x\n # c\n  y\n", 'E'},
        {"a: |\n  x\n  # not comment\n", 'A'},
        {"a: |\n    x\n  y\n", 'E'},
        {"a: |\n    \n  x\n", 'E'},
        {"a: |\n\tx\n", 'E'},
        {"a: |\n  x\ty\n", 'A'},
        {"a: |\n  x\n  \ty\n", 'A'},
        {"a: >\n  a\n  \tb\n  c\n", 'A'},
        {"a: |\n  a\n    \n  b\n", 'A'},
        {"a: >\n  a\n \n  b\n", 'A'},
        {"a: >\n  a\n   \n  b\n", 'A'},
        {"a: |+\n  x\n    \n\n", 'A'},
        {"- |\n  x\n- y\n", 'A'},
        {"- |\n- b\n", 'A'},
        {"a:\n- |\n x\n", 'A'},
        {"- k: |\n    x\n  j: 1\n", 'A'},
        {"- k: |\n   x\n", 'A'},
        {"- k: |\n  x\n", 'E'},
        {"a:\n  b: |\n  x\n", 'E'},
        {"a:\n b: |\n  x\n", 'A'},
        {"|\n x\n", 'A'},
        {"|\nx\n", 'A'},
        {"--- |\nx\n", 'A'},
        {"a: |\r\n  x\r\n\r\n  y\r\n", 'A'},
        {"a: |\n  x\n  ...\n", 'A'},
        {"a: |\n  x\n...\n", 'A'},
        {"- |\n x\n - y\n", 'A'},
        {"a: b\n  c\n", 'A'},
        {"a: b\n\n  c\n", 'A'},
        {"a: b\n\n\n  c\n", 'A'},
        {"a: b  \n  c\n", 'A'},
        {"a: b\n   \nc: d\n", 'A'},
        {"- a\n  b\n- c\n", 'A'},
        {"- a\nb\n", 'E'},
        {"- a: b\n  c\n", 'E'},
        {"- a: b\n   c\n", 'A'},
        {"a: b\n  # c\n  d\n", 'E'},
        {"a: b # c\n  d\n", 'E'},
        {"a: b\n  #c\n", 'A'},
        {"a: b\n  c: d\n", 'E'},
        {"a: b\n  c:\n", 'E'},
        {"a: b\n  : c\n", 'E'},
        {"a: b:c\n  d:e\n", 'A'},
        {"a: b\n  - c\n", 'A'},
        {"a: b\n  [c]\n", 'A'},
        {"a: b\n  ---\n", 'A'},
        {"a: b#c\n  d\n", 'A'},
        {"top\n\nlevel\n", 'A'},
        {"top\nlevel\n", 'A'},
        {"a:\n  b\n  c\n", 'A'},
        {"a:\n- b\n c\n", 'A'},
        {"a:\n  - b\n  c\n", 'E'},
        {"a: b\r\n  c\r\n", 'A'},
        {"a: \"x\n  y\"\n", 'A'},
        {"a: \"x\n\n  y\"\n", 'A'},
        {"a: \"x  \n  y\"\n", 'A'},
        {"a: \"x\\\n  y\"\n", 'A'},
        {"a: \"x \\\n  y\"\n", 'A'},
        {"a: \"x\n y\"\n", 'A'},
        {"a: \"x\ny\"\n", 'E'},
        {"- \"x\ny\"\n", 'E'},
        {"- \"x\n y\"\n", 'A'},
        {"\"x\ny\"\n", 'A'},
        {"a: 'it''s\n  x'\n", 'A'},
        {"a: 'x\n  \n  y'\n", 'A'},
        {"a: 'x\n  y\n\n\n  z'\n", 'A'},
        {"a: \"x\n  # c\n  y\"\n", 'A'},
        {"a: \"x\n  y\" # c\n", 'A'},
        {"a: \"x\n  y\" z\n", 'E'},
        {"a: \"x\n---\n  y\"\n", 'E'},
        {"a: \"x\n  ---\n  y\"\n", 'A'},
        {"--- \"a\n...x\nb\"\n", 'A'},
        {"\"a\n b\": 1\n", 'E'},
        {"a: \" x\n y \"\n", 'A'},
        {"a: \"\n  x\"\n", 'A'},
        {"a: \"x\n  \"\n", 'A'},
        {"a: \"x\\n\n  y\"\n", 'A'},
        {"a: \"x\r\n  y\"\r\n", 'A'},
        {"a: \"x\n  y\nb: 1\n", 'E'},
        {"a: 'x\n  y'\nb: 1\n", 'A'},
        {"--- plain\n  scalar\n", 'A'},
        {"--- \"q\"\n", 'A'},
        {"--- >\n a\n\n b\n", 'A'},
        {"a\nb\n---\nc\n", 'A'},
    };
    for (const Case& c : CASES) {
        INFO("text: ", dstd::String(c.text));
        auto doc = Document::parse(dstd::String(c.text), "n.yaml");
        CHECK(doc.has_value() == (c.want == 'A'));
    }
}

TEST_CASE("line index: the value or the reason of a scalar of several lines") {
    struct Case {
        const char* text;
        const char* result;
    };
    const Case CASES[] = {
        {"a: |x\n  y\n",
         "n.yaml:1:5: a block scalar header holds text that is not an "
         "indicator or a comment"},
        {"a: |0\n x\n",
         "n.yaml:1:5: a block scalar header holds text that is not an "
         "indicator or a comment"},
        {"a: |10\n x\n",
         "n.yaml:1:6: a block scalar header holds text that is not an "
         "indicator or a comment"},
        {"a: |#c\n  x\n",
         "n.yaml:1:5: a block scalar header holds text that is not an "
         "indicator or a comment"},
        {"a: b # c\n  d\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"a: |\n    \n  x\n",
         "n.yaml:2:3: an empty line of a block scalar is more indented than "
         "its first line of text"},
        {"a: \"x\ny\"\n", "n.yaml:2:1: a quoted scalar is not indented enough"},
        {"- \"x\ny\"\n", "n.yaml:2:1: a quoted scalar is not indented enough"},
        {"a: |\n    x\n  y\n",
         "n.yaml:3:3: a complete value cannot go on in a deeper line"},
        {"a: |\n  x\n # c\n  y\n",
         "n.yaml:4:3: a complete value cannot go on in a deeper line"},
        {"\"a\n b\": 1\n", "n.yaml:2:4: unexpected text after a quoted scalar"},
        {"a: |\n  x", "{a:'x',}"},
        {"a: >\n  x", "{a:'x',}"},
        {"a: |+\n  x", "{a:'x',}"},
        {"a: |-\n  x", "{a:'x',}"},
        {"a: |\n  x\n\n", "{a:'x\n',}"},
        {"a: |+\n  x\n\n", "{a:'x\n\n',}"},
        {"a: \"x\\\n\n  y\"\n", "{a:'x\ny',}"},
        {"a: \"x\\\n\n\n  y\"\n", "{a:'x\n\ny',}"},
        {"--- |\n--- key\n", "''"},
        {"--- >+\n...\n", "''"},
    };
    for (const Case& c : CASES) {
        INFO("text: ", dstd::String(c.text));
        auto doc = Document::parse(dstd::String(c.text), "n.yaml");
        if (doc.has_value())
            CHECK(dump(doc->root()) == dstd::String(c.result));
        else
            CHECK(dstd::String(doc.error().message()) ==
                  dstd::String(c.result));
    }
}

TEST_CASE("line index: a block scalar at the end of a text with no line feed") {
    for (const char* header : {"|", ">", "|+", "|-"}) {
        INFO("header: ", dstd::String(header));
        const dstd::String h(header);
        CHECK(dump(must_parse("a: " + h + "\n  x").root()) == "{a:'x',}");
        CHECK(dump(must_parse("- " + h + "\n  x").root()) == "['x',]");
        CHECK(dump(must_parse(h + "\n  x").root()) == "'x'");
        CHECK(dump(must_parse("- " + h + "\n  x\n- " + h + "\n  y").root()) ==
              "['x" + dstd::String(h == "|-" ? "" : "\n") + "','y',]");
    }
    // A last line that is only spaces is a line, so its line feed is kept.
    CHECK(dump(must_parse("a: |\n x\n   ").root()) == "{a:'x\n  \n',}");
    CHECK(dump(must_parse("- |+\n   ").root()) == "['\n',]");
    // In Records, the chunk that ends the text is the same.
    for (std::size_t chunk : {std::size_t{1}, std::size_t{100000}}) {
        RecordsOptions options;
        options.chunk_bytes = chunk;
        Records seq = Records::from_text("- |\n  x\n- |\n  y", "r", options);
        dstd::Vector<dstd::String> got;
        for (;;) {
            auto next = seq.next();
            REQUIRE(next.has_value());
            if (!next->has_value()) break;
            got.push_back(dump(**next));
        }
        REQUIRE(got.size() == 2);
        CHECK(got[0] == "'x\n'");
        CHECK(got[1] == "'y'");
        Records docs =
            Records::from_text("--- |\n  x\n--- >\n  y", "r", options);
        got.clear();
        for (;;) {
            auto next = docs.next();
            REQUIRE(next.has_value());
            if (!next->has_value()) break;
            got.push_back(dump(**next));
        }
        REQUIRE(got.size() == 2);
        CHECK(got[0] == "'x\n'");
        CHECK(got[1] == "'y'");
    }
}

TEST_CASE(
    "line index: plain scalars that start like an indicator or hold one") {
    const Document doc = must_parse(
        "a: -5\nb: a:b\nc: a#b\nd: 1,2\ne: it's\nf: q\"q\ng: a]b\nh: ~x\n"
        "i: -x\nj: ?x\nk: :x\nl: <<\nm: x{y}\nn: caf\xC3\xA9\no: a\tb\n");
    const char* const EXPECTED[][2] = {
        {"a", "-5"},   {"b", "a:b"},         {"c", "a#b"}, {"d", "1,2"},
        {"e", "it's"}, {"f", "q\"q"},        {"g", "a]b"}, {"h", "~x"},
        {"i", "-x"},   {"j", "?x"},          {"k", ":x"},  {"l", "<<"},
        {"m", "x{y}"}, {"n", "caf\xC3\xA9"}, {"o", "a\tb"}};
    for (const auto& e : EXPECTED) {
        INFO("key: ", dstd::String(e[0]));
        auto v = doc.root().find(e[0]);
        REQUIRE(v.has_value());
        CHECK(v->is_string());
        CHECK(v->raw() == dstd::StringView(e[1]));
    }
    // The same as items of a sequence and in a flow collection.
    const Document seq = must_parse("- -5\n- a:b\n- 1,2\n- it's\n- q\"q\n");
    CHECK(dump(seq.root()) == "['-5','a:b','1,2','it's','q\"q',]");
    const Document flow =
        must_parse("[-5, a:b, it's, q\"q, a#b, ~x, ?x, <<]\n");
    CHECK(dump(flow.root()) ==
          "['-5','a:b','it's','q\"q','a#b','~x','?x','<<',]");
    // `<<` is a plain key: there is no merge.
    const Document merge = must_parse("<<: 1\nb: 2\n");
    CHECK(dump(merge.root()) == "{<<:'1',b:'2',}");
    CHECK(merge.root().find("<<")->as_uint() == 1);
    Records records = Records::from_text("<<: 1\nb: 2\n", "r", {});
    auto first = records.next();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK(dump(**first) == "{<<:'1',b:'2',}");
    Records items = Records::from_text("- <<: 1\n  b: 2\n- c\n", "r", {});
    auto item = items.next();
    REQUIRE(item.has_value());
    REQUIRE(item->has_value());
    CHECK(dump(**item) == "{<<:'1',b:'2',}");
}

TEST_CASE("line index: more errors, each with its message") {
    struct Case {
        const char* text;
        const char* message;
    };
    const Case CASES[] = {
        {"a: 'abc\nb: 1\n", "n.yaml:1:4: unterminated quoted scalar"},
        {"a: 'abc\n", "n.yaml:1:4: unterminated quoted scalar"},
        {"a: \"\\x4\"\n", "n.yaml:1:5: escape '\\x' needs 2 hex digits"},
        {"a: \"\\u00\"\n", "n.yaml:1:5: escape '\\u' needs 4 hex digits"},
        {"a: \"\\xZZ\"\n",
         "n.yaml:1:5: escape '\\x' has a character that is not a hex digit"},
        {"a: \"\\U00110000\"\n", "n.yaml:1:5: escape is not a code point"},
        {"a: \"\\uD83D\"\n", "n.yaml:1:5: escape is not a code point"},
        {"a: \"\\uDE00\"\n", "n.yaml:1:5: escape is not a code point"},
        {"a: {}\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"a: []\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"a: \"x\"\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"a: 1\x01\n", "n.yaml:1:5: a control character is not allowed"},
        {"a: \x01\n", "n.yaml:1:4: a control character is not allowed"},
        {"a: 1\x7F\n", "n.yaml:1:5: a control character is not allowed"},
        {"a: \xFF\n",
         "n.yaml:1:4: invalid UTF-8 or a character that YAML does not print"},
        {"a: \xC3\n",
         "n.yaml:1:4: invalid UTF-8 or a character that YAML does not print"},
        {"a: \xC3(\n",
         "n.yaml:1:4: invalid UTF-8 or a character that YAML does not print"},
        {"b: 1\na: \xE2\x82\n",
         "n.yaml:2:4: invalid UTF-8 or a character that YAML does not print"},
    };
    for (const Case& c : CASES) {
        INFO("text: ", dstd::String(c.text));
        CHECK(error_of(c.text) == dstd::String(c.message));
    }
}

// Each control byte and each lone byte of 0x80 and above, at each offset of a
// 64-byte block, is an error at its column.
TEST_CASE("line index: every byte that is not allowed, at every offset") {
    for (unsigned c = 0; c < 256; ++c) {
        const bool control = (c < 0x20 && c != '\t' && c != '\n') || c == 0x7F;
        if (!control && c < 0x80) continue;
        for (std::size_t pad = 0; pad < 70; ++pad) {
            const dstd::String text =
                "a: " + dstd::String(pad, 'x') + static_cast<char>(c) + "y\n";
            INFO("byte: ", c, " pad: ", pad);
            const dstd::String expected =
                control ? dstd::fmt<
                              "n.yaml:1:{}: a control character is not "
                              "allowed">(pad + 4)
                        : dstd::fmt<
                              "n.yaml:1:{}: invalid UTF-8 or a character "
                              "that YAML does not print">(pad + 4);
            REQUIRE(error_of(text) == expected);
        }
    }
}

TEST_CASE("line index: the limits of indentation and nesting") {
    const dstd::String over =
        "n.yaml:2:65536: indentation deeper than 65534 columns is not "
        "supported";
    for (const std::size_t n : {std::size_t{65533}, std::size_t{65534}}) {
        const dstd::String pad(n, ' ');
        CHECK(
            must_parse("a:\n" + pad + "b: 1\n").root().find("a")->is_object());
        CHECK(must_parse("a:\n" + pad + "- 1\n").root().find("a")->is_array());
        CHECK(must_parse(pad + "a: 1\n").root().find("a")->as_uint() == 1);
    }
    const dstd::String pad(65535, ' ');
    CHECK(error_of("a:\n" + pad + "b: 1\n") == over);
    CHECK(error_of("a:\n" + pad + "- 1\n") == over);
    CHECK(error_of("a: 1\n" + pad + "# c\n") == over);
    CHECK(error_of(pad + "a: 1\n") ==
          "n.yaml:1:65536: indentation deeper than 65534 columns is not "
          "supported");
    const auto nested_map = [](int levels) {
        dstd::String s;
        for (int i = 0; i < levels - 1; ++i)
            s += dstd::String(static_cast<std::size_t>(i), ' ') + "k:\n";
        s += dstd::String(static_cast<std::size_t>(levels - 1), ' ') + "v: 1\n";
        return s;
    };
    const auto nested_seq = [](int levels) {
        dstd::String s;
        for (int i = 0; i < levels - 1; ++i)
            s += dstd::String(static_cast<std::size_t>(i), ' ') + "-\n";
        s += dstd::String(static_cast<std::size_t>(levels - 1), ' ') + "- 1\n";
        return s;
    };
    CHECK(Document::parse(nested_map(128), "n.yaml").has_value());
    CHECK(Document::parse(nested_seq(128), "n.yaml").has_value());
    const dstd::String deep =
        "n.yaml:129:129: blocks nested deeper than 128 levels are not "
        "supported";
    CHECK(error_of(nested_map(129)) == deep);
    CHECK(error_of(nested_seq(129)) == deep);
    const dstd::String ok_flow =
        dstd::String(128, '[') + "x" + dstd::String(128, ']') + "\n";
    CHECK(Document::parse(ok_flow, "n.yaml").has_value());
    const dstd::String deep_flow =
        dstd::String(129, '[') + "x" + dstd::String(129, ']') + "\n";
    CHECK(error_of(deep_flow) ==
          "n.yaml:1:129: flow collections nested deeper than 128 levels are "
          "not supported");
}

TEST_CASE("line index: a byte order mark") {
    const dstd::String bom = "\xEF\xBB\xBF";
    const Document doc = must_parse(bom + "a: 1\nb: 2\n");
    CHECK(doc.root().find("a")->as_uint() == 1);
    CHECK_FALSE(doc.root().find(bom + "a").has_value());
    CHECK(error_of(bom + "a: 1\na: 2\n") == "n.yaml:2:1: duplicate key 'a'");
    const Document seq = must_parse(bom + "- a\n- b\n");
    CHECK(dump(seq.root()) == "['a','b',]");
    // Inside the text it is a character like any other.
    const Document inner = must_parse("a" + bom + "b: 1\nc: x" + bom + "y\n");
    CHECK(inner.root().find("a" + bom + "b")->as_uint() == 1);
    CHECK(inner.root().find("c")->raw() == "x" + bom + "y");
    CHECK(must_parse("a: " + bom + "b\n").root().find("a")->raw() == bom + "b");
}

TEST_CASE("line index: every escape of a double quoted scalar") {
    struct Case {
        const char* escape;
        const char* text;
        std::size_t size;
    };
    const Case CASES[] = {
        {"\\0", "\0", 1},
        {"\\a", "\x07", 1},
        {"\\b", "\x08", 1},
        {"\\t", "\t", 1},
        {"\\\t", "\t", 1},
        {"\\n", "\n", 1},
        {"\\v", "\x0B", 1},
        {"\\f", "\x0C", 1},
        {"\\r", "\r", 1},
        {"\\e", "\x1B", 1},
        {"\\ ", " ", 1},
        {"\\\"", "\"", 1},
        {"\\/", "/", 1},
        {"\\\\", "\\", 1},
        {"\\N", "\xC2\x85", 2},
        {"\\_", "\xC2\xA0", 2},
        {"\\L", "\xE2\x80\xA8", 3},
        {"\\P", "\xE2\x80\xA9", 3},
        {"\\x41", "A", 1},
        {"\\u00e9", "\xC3\xA9", 2},
        {"\\u65E5", "\xE6\x97\xA5", 3},
        {"\\U0001F600", "\xF0\x9F\x98\x80", 4},
        {"\\uD83D\\uDE00", "\xF0\x9F\x98\x80", 4},
        {"\\U0010FFFF", "\xF4\x8F\xBF\xBF", 4},
    };
    for (const Case& c : CASES) {
        INFO("escape: ", dstd::String(c.escape));
        const Document doc =
            must_parse("a: \"<" + dstd::String(c.escape) + ">\"\n");
        const dstd::String want = "<" + dstd::String(c.text, c.size) + ">";
        const dstd::StringView got = doc.root().find("a")->raw();
        CHECK(dstd::String(got) == want);
    }
    CHECK(Document::parse("a: \"\xF0\x9F\x98\x80\"\n", "n.yaml").has_value());
}
