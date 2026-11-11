// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <yaml/yaml_dump.h>

#include <cstddef>

using namespace dftracer::utils;
using dftu_yaml_test::dump;
using yaml::Document;
using yaml::Records;
using yaml::RecordsOptions;
using yaml::Value;

namespace {

// Every record of `text`, as dump text, cut into chunks of about `chunk`
// bytes.
dstd::Vector<dstd::String> records_of(dstd::StringView text,
                                      std::size_t chunk) {
    RecordsOptions options;
    options.chunk_bytes = chunk;
    Records records = Records::from_text(dstd::String(text), "r.yaml", options);
    dstd::Vector<dstd::String> out;
    for (;;) {
        auto next = records.next();
        REQUIRE(next.has_value());
        if (!next->has_value()) break;
        out.push_back(dump(**next));
    }
    return out;
}

// The elements of the top-level sequence, or the documents, of a whole parse.
dstd::Vector<dstd::String> whole_elements(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "");
    REQUIRE(doc.has_value());
    dstd::Vector<dstd::String> out;
    doc->root().for_each_element(
        [&](Value element) { out.push_back(dump(element)); });
    return out;
}

dstd::String error_after_records(dstd::StringView text, std::size_t chunk) {
    RecordsOptions options;
    options.chunk_bytes = chunk;
    Records records = Records::from_text(dstd::String(text), "r.yaml", options);
    for (;;) {
        auto next = records.next();
        if (!next.has_value()) return dstd::String(next.error().message());
        if (!next->has_value()) return {};
    }
}

constexpr dstd::StringView HARD_ITEMS =
    "# a comment at column 0\n"
    "- name: first\n"
    "  text: |\n"
    "    literal line one\n"
    "    - not an item, a block scalar line\n"
    "\n"
    "    after a blank\n"
    "  folded: >\n"
    "    folded one\n"
    "    folded two\n"
    "# a comment between items\n"
    "- plain: a multi line\n"
    "    plain scalar that goes on\n"
    "    and on\n"
    "  quoted: \"a double\n"
    "    quoted scalar with - dash\n"
    "    lines\"\n"
    "  single: 'a single\n"
    "    quoted ''one'''\n"
    "-\n"
    "  flow: [a, b,\n"
    "    c, {d: 1,\n"
    "    e: 2}]\n"
    "  more: x\n"
    "- - nested\n"
    "  - seq\n"
    "- |\n"
    "  top block scalar item\n"
    "  - looks like an item\n"
    "- last: 1\n"
    "# trailing comment\n";

}  // namespace

TEST_CASE(
    "yaml records: every cut of a sequence of hard items matches the whole") {
    const dstd::Vector<dstd::String> expected = whole_elements(HARD_ITEMS);
    REQUIRE(expected.size() == 6);
    for (std::size_t chunk : {1u, 7u, 50u, 120u, 200u, 100000u}) {
        CAPTURE(chunk);
        CHECK(records_of(HARD_ITEMS, chunk) == expected);
    }
}

TEST_CASE("yaml records: the same with CRLF line ends") {
    dstd::String crlf;
    for (char c : HARD_ITEMS) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    const dstd::Vector<dstd::String> expected = whole_elements(HARD_ITEMS);
    for (std::size_t chunk : {1u, 33u, 100000u}) {
        CAPTURE(chunk);
        CHECK(records_of(crlf, chunk) == expected);
    }
}

TEST_CASE("yaml records: a block scalar line that starts with a dash stays") {
    const dstd::Vector<dstd::String> got =
        records_of("- |\n  a\n  - b\n  -\n  - c\n- x\n", 1);
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "'a\n- b\n-\n- c\n'");
    CHECK(got[1] == "'x'");
}

TEST_CASE("yaml records: documents of a stream, with directives and markers") {
    const dstd::String text =
        "%YAML 1.2\n"
        "---\n"
        "a: 1\n"
        "t: |\n"
        "  --- in a block scalar\n"
        "  ... too\n"
        "...\n"
        "%YAML 1.2\n"
        "---\n"
        "b: 2\n"
        "---\n"
        "---\n"
        "- c\n"
        "- d\n"
        "...\n"
        "---\n"
        "e: 5\n";
    dstd::Vector<dstd::String> whole;
    for (std::size_t chunk : {1u, 20u, 100000u}) {
        CAPTURE(chunk);
        const dstd::Vector<dstd::String> got = records_of(text, chunk);
        REQUIRE(got.size() == 5);
        CHECK(got[0] == "{a:'1',t:'--- in a block scalar\n... too\n',}");
        CHECK(got[1] == "{b:'2',}");
        CHECK(got[2] == "~");
        CHECK(got[3] == "['c','d',]");
        CHECK(got[4] == "{e:'5',}");
    }
}

TEST_CASE("yaml records: one record, no record and a single document") {
    CHECK(records_of("- only\n", 1) == dstd::Vector<dstd::String>{"'only'"});
    CHECK(records_of("- only", 1) == dstd::Vector<dstd::String>{"'only'"});
    CHECK(records_of("---\na: 1\n", 1) ==
          dstd::Vector<dstd::String>{"{a:'1',}"});
    CHECK(records_of("a: 1\nb: 2\n", 1) ==
          dstd::Vector<dstd::String>{"{a:'1',b:'2',}"});
    CHECK(records_of("just a scalar\n", 1) ==
          dstd::Vector<dstd::String>{"'just a scalar'"});
    CHECK(records_of("", 1).empty());
    CHECK(records_of("\n\n", 1).empty());
    CHECK(records_of("# only a comment\n\n  # another\n", 1).empty());
    CHECK(records_of("[a, b]\n", 1) ==
          dstd::Vector<dstd::String>{"['a','b',]"});
}

TEST_CASE("yaml records: a byte order mark and an indented sequence") {
    CHECK(records_of("\xEF\xBB\xBF- a\n- b\n", 1).size() == 2);
    const dstd::Vector<dstd::String> got = records_of("  - a\n  - b\n", 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "['a','b',]");
}

TEST_CASE("yaml records: a record that is a long run is one chunk") {
    dstd::String text = "- ";
    for (int i = 0; i < 3000; ++i) text += "word ";
    text += "\n- b\n";
    const dstd::Vector<dstd::String> got = records_of(text, 16);
    REQUIRE(got.size() == 2);
    CHECK(got[1] == "'b'");
}

TEST_CASE("yaml records: a syntax error names the line of the whole text") {
    dstd::String text;
    for (int i = 0; i < 50; ++i) text += dstd::fmt<"- a: {}\n  b: 2\n">(i);
    text += "- c: [1\n";
    auto whole = Document::parse(text, "r.yaml");
    REQUIRE_FALSE(whole.has_value());
    const dstd::String expected(whole.error().message());
    CHECK(expected.find("r.yaml:10") == 0);
    CHECK(error_after_records(text, 64) == expected);
    CHECK(error_after_records(text, 1) == expected);
}

TEST_CASE("yaml records: a duplicate key names the line of the whole text") {
    dstd::String text;
    for (int i = 0; i < 20; ++i) text += dstd::fmt<"- a: {}\n  b: 2\n">(i);
    text += "- k: 1\n  k: 2\n";
    CHECK(error_after_records(text, 32) == "r.yaml:42:3: duplicate key 'k'");
}

TEST_CASE("yaml records: an alias is read inside its chunk only") {
    CHECK(error_after_records("- &x 1\n- *x\n", 100000).empty());
    CHECK(error_after_records("- &x 1\n- *x\n", 1).find("undefined alias") !=
          dstd::String::npos);
}

TEST_CASE("yaml records: a document marker inside a sequence is refused") {
    CHECK(error_after_records("- a\n- b\n---\n- c\n", 100000)
              .find("document marker") != dstd::String::npos);
}

TEST_CASE("yaml records: a file is read in blocks and cut by size") {
    const dstd::Path path =
        dftu_utils_test::make_unique_test_path("yaml_records_file");
    dstd::String text;
    constexpr int COUNT = 60000;
    for (int i = 0; i < COUNT; ++i)
        text += dstd::fmt<"- id: {}\n  name: item number {}\n  tags: [a, b]\n">(
            i, i);
    REQUIRE(dstd::fs::write_file(path, text).has_value());
    RecordsOptions options;
    options.chunk_bytes = 100000;
    auto opened = Records::open(path, options);
    REQUIRE(opened.has_value());
    int count = 0;
    for (;;) {
        auto next = opened->next();
        REQUIRE(next.has_value());
        if (!next->has_value()) break;
        CHECK((*next)->find("id")->as_int() == count);
        ++count;
    }
    CHECK(count == COUNT);
    REQUIRE(dstd::fs::remove(path).has_value());
    CHECK_FALSE(Records::open(path, options).has_value());
}

TEST_CASE("yaml records: a file of documents, and a single large map") {
    const dstd::Path path =
        dftu_utils_test::make_unique_test_path("yaml_records_docs");
    dstd::String text;
    for (int i = 0; i < 20000; ++i)
        text += dstd::fmt<"---\nid: {}\nbody: |\n  line\n  more\n">(i);
    REQUIRE(dstd::fs::write_file(path, text).has_value());
    RecordsOptions options;
    options.chunk_bytes = 50000;
    auto opened = Records::open(path, options);
    REQUIRE(opened.has_value());
    int count = 0;
    for (;;) {
        auto next = opened->next();
        REQUIRE(next.has_value());
        if (!next->has_value()) break;
        CHECK((*next)->find("id")->as_int() == count);
        ++count;
    }
    CHECK(count == 20000);

    dstd::String map;
    for (int i = 0; i < 5000; ++i) map += dstd::fmt<"k{}: {}\n">(i, i);
    REQUIRE(dstd::fs::write_file(path, map).has_value());
    auto single = Records::open(path, options);
    REQUIRE(single.has_value());
    auto first = single->next();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->find("k4999")->as_int() == 4999);
    auto second = single->next();
    REQUIRE(second.has_value());
    CHECK_FALSE(second->has_value());
    REQUIRE(dstd::fs::remove(path).has_value());
}

TEST_CASE("yaml records: next after the end and after an error is empty") {
    RecordsOptions options;
    Records records = Records::from_text("- a: [\n", "r.yaml", options);
    CHECK_FALSE(records.next().has_value());
    auto again = records.next();
    REQUIRE(again.has_value());
    CHECK_FALSE(again->has_value());
}

TEST_CASE("yaml records: an anchor does not cross a document at any chunk") {
    const dstd::StringView text = "---\na: &x 1\n---\nb: *x\n";
    CHECK_FALSE(Document::parse(dstd::String(text), "").has_value());
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{100000}}) {
        RecordsOptions options;
        options.chunk_bytes = chunk;
        Records records =
            Records::from_text(dstd::String(text), "r.yaml", options);
        bool failed = false;
        for (int i = 0; i < 3 && !failed; ++i) {
            auto next = records.next();
            if (!next.has_value())
                failed = true;
            else if (!next->has_value())
                break;
        }
        CHECK(failed);
    }
}
