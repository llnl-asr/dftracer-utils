// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <doctest/doctest.h>
#include <yaml/yaml_dump.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

using namespace dftracer::utils;
using dftu_yaml_test::dump;
using yaml::Document;
using yaml::Records;
using yaml::RecordsOptions;
using yaml::Value;

namespace {

// Every record of `text`, as dump text, cut into chunks of about `chunk`
// bytes.
dstd::Vector<dstd::String> records_of(dstd::StringView text, std::size_t chunk,
                                      std::size_t batch = 1) {
    RecordsOptions options;
    options.chunk_bytes = chunk;
    options.batch_chunks = batch;
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

// Gives `records` the next block of at most `block` bytes of `text` from `at`,
// or ends the input.
void feed(Records& records, dstd::StringView text, std::size_t& at,
          std::size_t block) {
    const std::size_t size = std::min(block, text.size() - at);
    if (size == 0) {
        records.end_input();
        return;
    }
    dstd::Span<char> space = records.input_space(block);
    REQUIRE(space.size() >= size);
    std::memcpy(space.data(), text.data() + at, size);
    records.commit_input(size);
    at += size;
}

// As records_of, with the text given in blocks of `block` bytes.
dstd::Vector<dstd::String> records_in_blocks(dstd::StringView text,
                                             std::size_t chunk,
                                             std::size_t block,
                                             std::size_t batch = 1) {
    RecordsOptions options;
    options.chunk_bytes = chunk;
    options.batch_chunks = batch;
    Records records = Records::from_blocks("r.yaml", options);
    dstd::Vector<dstd::String> out;
    std::size_t at = 0;
    for (;;) {
        auto next = records.next();
        REQUIRE(next.has_value());
        if (next->has_value()) {
            out.push_back(dump(**next));
            continue;
        }
        if (!records.needs_input()) break;
        feed(records, text, at, block);
    }
    return out;
}

// The elements of the top-level sequence, or the documents, of a whole parse.
// As records_in_blocks, read with next_chunks in batches of `batch` chunks.
// The chunks of a batch are read last to first, or each on its own thread,
// and their records are put back in text order. The first error, in text
// order, goes to `error`. The chunks of the first `skipped` batches are not
// read.
dstd::Vector<dstd::String> records_in_batches(
    dstd::StringView text, std::size_t chunk, std::size_t block,
    std::size_t batch, bool on_threads, dstd::String* error = nullptr,
    std::size_t skipped = 0) {
    RecordsOptions options;
    options.chunk_bytes = chunk;
    options.batch_chunks = batch;
    Records records = Records::from_blocks("r.yaml", options);
    dstd::Vector<dstd::String> out;
    std::size_t at = 0;
    for (;;) {
        auto chunks = records.next_chunks();
        REQUIRE(chunks.has_value());
        if (chunks->empty()) {
            if (!records.needs_input()) break;
            feed(records, text, at, block);
            continue;
        }
        CHECK(chunks->size() <= batch);
        if (skipped > 0) {
            --skipped;
            continue;
        }
        dstd::Vector<dstd::Vector<dstd::String>> parts(chunks->size());
        dstd::Vector<dstd::String> errors(chunks->size());
        const auto read = [&](std::size_t i) {
            for (;;) {
                auto next = (*chunks)[i].next();
                if (!next.has_value()) {
                    errors[i] = dstd::String(next.error().message());
                    return;
                }
                if (!next->has_value()) return;
                parts[i].push_back(dump(**next));
            }
        };
        if (on_threads) {
            dstd::Vector<dstd::Thread> threads;
            for (std::size_t i = 0; i < chunks->size(); ++i)
                threads.emplace_back([&read, i] { read(i); });
            for (auto& thread : threads) thread.join();
        } else {
            for (std::size_t i = chunks->size(); i-- > 0;) read(i);
        }
        for (std::size_t i = 0; i < chunks->size(); ++i) {
            for (auto& record : parts[i]) out.push_back(std::move(record));
            if (!errors[i].empty()) {
                REQUIRE(error != nullptr);
                *error = errors[i];
                return out;
            }
        }
    }
    return out;
}

dstd::Vector<dstd::String> whole_elements(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "");
    if (!doc.has_value()) FAIL(dstd::String(doc.error().message()));
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

TEST_CASE("yaml records: documents of a stream, with markers") {
    const dstd::String text =
        "---\n"
        "a: 1\n"
        "t: |\n"
        "  --- in a block scalar\n"
        "  ... too\n"
        "...\n"
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

TEST_CASE("yaml records: an anchor, an alias and a tag are refused") {
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{100000}}) {
        CAPTURE(chunk);
        CHECK(error_after_records("- &x 1\n- *x\n", chunk)
                  .find("anchors are not supported") != dstd::String::npos);
        CHECK(error_after_records("- 1\n- *x\n", chunk)
                  .find("aliases are not supported") != dstd::String::npos);
        CHECK(error_after_records("- 1\n- !!str x\n", chunk)
                  .find("tags are not supported") != dstd::String::npos);
        CHECK(
            error_after_records("---\na: 1\n---\n%YAML 1.2\n---\nb: 2\n", chunk)
                .find("directives are not supported") != dstd::String::npos);
    }
}

TEST_CASE("yaml records: a document marker inside a sequence is refused") {
    CHECK(error_after_records("- a\n- b\n---\n- c\n", 100000)
              .find("document marker") != dstd::String::npos);
}

TEST_CASE("yaml records: text given in blocks gives the records of the whole") {
    dstd::String seq;
    for (int i = 0; i < 3000; ++i)
        seq += dstd::fmt<"- id: {}\n  name: item number {}\n  tags: [a, b]\n">(
            i, i);
    dstd::String docs;
    for (int i = 0; i < 2000; ++i)
        docs += dstd::fmt<"---\nid: {}\nbody: |\n  line\n  more\n">(i);
    dstd::String map;
    for (int i = 0; i < 500; ++i) map += dstd::fmt<"k{}: {}\n">(i, i);
    const dstd::String texts[] = {
        seq,
        docs,
        map,
        "",
        "# only a comment\n",
        "# c\n---\na: 1\n...\n# d\n---\nb: 2\n...\nc: 3\n",
        "- a\r\n- b: [1,\r\n    2]\r\n- c",
        "--- 1\n--- 2\n---\n",
        "---\n----\n---\nx\n",
        "\xEF\xBB\xBF- a\n- b\n",
        "\xEF\xBB\xBF---\na: 1\n---\nb: 2\n",
    };
    for (const dstd::String& text : texts) {
        CAPTURE(text.substr(0, 40));
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{40000}}) {
            const auto expected = records_of(text, chunk);
            for (const std::size_t block :
                 {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
                CAPTURE(chunk);
                CAPTURE(block);
                CHECK(records_in_blocks(text, chunk, block) == expected);
                for (const std::size_t batch :
                     {std::size_t{1}, std::size_t{3}, std::size_t{8}}) {
                    CHECK(records_in_batches(text, chunk, block, batch,
                                             false) == expected);
                    CHECK(records_in_blocks(text, chunk, block, batch) ==
                          expected);
                    CHECK(records_of(text, chunk, batch) == expected);
                }
            }
        }
    }
}

TEST_CASE("yaml records: an error in text given in blocks") {
    const dstd::String text = "- a\n- b\n---\n- c\n";
    for (const std::size_t block : {std::size_t{1}, std::size_t{5}}) {
        RecordsOptions options;
        options.chunk_bytes = 100000;
        Records records = Records::from_blocks("r.yaml", options);
        std::size_t at = 0;
        dstd::String message;
        for (;;) {
            auto next = records.next();
            if (!next.has_value()) {
                message = dstd::String(next.error().message());
                break;
            }
            if (next->has_value()) continue;
            if (!records.needs_input()) break;
            feed(records, text, at, block);
        }
        CHECK(message == error_after_records(text, 100000));
        CHECK(message.find("document marker") != dstd::String::npos);
    }
}

TEST_CASE("yaml records: the chunks of a batch are read on their own threads") {
    dstd::String seq;
    for (int i = 0; i < 4000; ++i)
        seq += dstd::fmt<"- id: {}\n  text: \"a\\tb {}\"\n  more: |\n    x\n">(
            i, i);
    const auto expected = records_of(seq, 40000);
    for (const std::size_t batch : {std::size_t{2}, std::size_t{8}})
        CHECK(records_in_batches(seq, 3000, 4096, batch, true) == expected);
}

TEST_CASE("yaml records: an error in a later chunk of a batch names its line") {
    dstd::String seq;
    for (int i = 0; i < 200; ++i) {
        seq += dstd::fmt<"- id: {}\n  name: n\n">(i);
        if (i == 137) seq += "  id: again\n";
    }
    const dstd::String expected = error_after_records(seq, 50);
    CHECK(expected.find("duplicate key 'id'") != dstd::String::npos);
    for (const std::size_t batch : {std::size_t{1}, std::size_t{4}}) {
        for (const bool on_threads : {false, true}) {
            CAPTURE(batch);
            CAPTURE(on_threads);
            dstd::String message;
            records_in_batches(seq, 50, 7, batch, on_threads, &message);
            CHECK(message == expected);
            dstd::String after_skip;
            records_in_batches(seq, 50, 7, batch, on_threads, &after_skip, 3);
            CHECK(after_skip == expected);
        }
    }
}

TEST_CASE("yaml records: next after the end and after an error is empty") {
    RecordsOptions options;
    Records records = Records::from_text("- a: [\n", "r.yaml", options);
    CHECK_FALSE(records.next().has_value());
    auto again = records.next();
    REQUIRE(again.has_value());
    CHECK_FALSE(again->has_value());
}

TEST_CASE(
    "yaml records: block, multi-line and quoted scalars read as a whole") {
    const dstd::String items =
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
        "  more: x\n"
        "- |\n"
        "  top block scalar item\n"
        "  - looks like an item\n"
        "- last: 1\n"
        "# trailing comment\n";
    const dstd::Vector<dstd::String> expected = whole_elements(items);
    REQUIRE(expected.size() == 5);
    for (std::size_t chunk : {1u, 7u, 100000u}) {
        CAPTURE(chunk);
        CHECK(records_of(items, chunk) == expected);
    }
}

TEST_CASE("yaml records: documents that are top-level scalars") {
    const dstd::String text =
        "--- |\n"
        "  literal\n"
        "   text\n"
        "--- plain\n"
        "  scalar\n"
        "--- \"quoted\n"
        "  scalar\"\n"
        "--- >-\n"
        "  folded\n"
        "  text\n"
        "---\n"
        "bare\n"
        "---\n"
        "a: 1\n"
        "--- [x, {y: 1}]\n"
        "--- {z: [1,\n  2]}\n";
    const dstd::Vector<dstd::String> expected = records_of(text, 100000);
    REQUIRE(expected.size() == 8);
    CHECK(expected[0] == "'literal\n text\n'");
    CHECK(expected[1] == "'plain scalar'");
    CHECK(expected[2] == "'quoted scalar'");
    CHECK(expected[3] == "'folded text'");
    CHECK(expected[4] == "'bare'");
    CHECK(expected[5] == "{a:'1',}");
    CHECK(expected[6] == "['x',{y:'1',},]");
    CHECK(expected[7] == "{z:['1','2',],}");
    for (std::size_t chunk : {1u, 7u, 100000u}) {
        CAPTURE(chunk);
        CHECK(records_of(text, chunk) == expected);
    }
    // A text that is one scalar is one record.
    for (const char* single : {"|\n x\n y\n", "a\nb\n", "\"a\n b\"\n"}) {
        for (std::size_t chunk : {1u, 7u, 100000u})
            CHECK(records_of(single, chunk).size() == 1);
    }
}

TEST_CASE("yaml records: a cut never lands inside a multi-line flow value") {
    // A line inside a flow collection can start with a dash or three, where a
    // cut could take it for an item or a document.
    dstd::String items;
    for (int i = 0; i < 30; ++i)
        items += dstd::fmt<
            "- id: {}\n  v: [a,\n    -1,\n    {k: {{}},\n     "
            "---x: {}}]\n">(i, i);
    const dstd::Vector<dstd::String> expected = whole_elements(items);
    REQUIRE(expected.size() == 30);
    for (std::size_t chunk : {1u, 3u, 40u, 90u, 100000u}) {
        CAPTURE(chunk);
        CHECK(records_of(items, chunk) == expected);
    }
    dstd::String docs;
    for (int i = 0; i < 30; ++i)
        docs +=
            dstd::fmt<"---\nid: {}\nk: {a: {{}},\n   b: [1,\n     2]}\n">(i);
    const dstd::Vector<dstd::String> docs_expected = records_of(docs, 100000);
    REQUIRE(docs_expected.size() == 30);
    for (std::size_t chunk : {1u, 3u, 40u, 90u})
        CHECK(records_of(docs, chunk) == docs_expected);
}

TEST_CASE("yaml records: a JSON text is one record, however long") {
    dstd::String json = "{\"items\": [";
    for (int i = 0; i < 5000; ++i) {
        if (i != 0) json += ",\n";
        json += dstd::fmt<"{\"id\": {}, \"tags\": [\"a\", \"b\"]}">(i);
    }
    json += "]}\n";
    for (std::size_t chunk : {1u, 100u, 100000000u}) {
        RecordsOptions options;
        options.chunk_bytes = chunk;
        Records records = Records::from_text(json, "j.json", options);
        auto first = records.next();
        INFO("chunk ", chunk, ": ",
             first.has_value() ? dstd::String()
                               : dstd::String(first.error().message()));
        REQUIRE(first.has_value());
        REQUIRE(first->has_value());
        CHECK((*first)->find("items")->is_array());
        auto second = records.next();
        REQUIRE(second.has_value());
        CHECK_FALSE(second->has_value());
    }
}
