// SPDX-License-Identifier: MIT
// The golden-test format of `dftracer_plugin check`: the parser and the record
// writer, without running a query.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/cli/plugin_golden.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

namespace {

dstd::StringView text_of(const dstd::String& value) {
    return dstd::StringView(value);
}

}  // namespace

TEST_SUITE("PluginGolden") {
    TEST_CASE("a file with two blocks parses into two blocks") {
        const dstd::String text =
            "# a comment\n"
            "input data/sample.pfw.gz\n"
            "query rowsort\n"
            "select name, dur\n"
            "from trace\n"
            "----\n"
            "name\tdur\n"
            "\n"
            "query error unknown column\n"
            "select nope\n"
            "----\n";
        auto parsed =
            cli::parse_golden_file(dstd::StringView(text), "basic.test");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->blocks.size() == 2);
        CHECK(text_of(parsed->path) == "basic.test");

        const cli::GoldenBlock& first = parsed->blocks[0];
        CHECK(text_of(first.input) == "data/sample.pfw.gz");
        CHECK(first.rowsort);
        CHECK(first.error_text.empty());
        CHECK(text_of(first.query) == "select name, dur\nfrom trace");
        CHECK(text_of(first.expected) == "name\tdur\n");
        CHECK(first.line == 3);
        // The comment and the input line belong to the first block, so a
        // rewrite keeps them.
        CHECK(text_of(first.lead) == "# a comment\ninput data/sample.pfw.gz\n");

        const cli::GoldenBlock& second = parsed->blocks[1];
        CHECK(text_of(second.input) == "data/sample.pfw.gz");
        CHECK(!second.rowsort);
        CHECK(text_of(second.error_text) == "unknown column");
        CHECK(text_of(second.query) == "select nope");
        CHECK(second.expected.empty());
    }

    TEST_CASE("a malformed file names the line it failed on") {
        SUBCASE("no ---- line") {
            auto parsed =
                cli::parse_golden_file("query\nselect 1\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("no '----' line") !=
                  dstd::String::npos);
            CHECK(parsed.error().message().find("bad.test:1:") !=
                  dstd::String::npos);
        }
        SUBCASE("an empty query block") {
            auto parsed = cli::parse_golden_file("query\n----\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("query block is empty") !=
                  dstd::String::npos);
        }
        SUBCASE("a block with no input line") {
            auto parsed =
                cli::parse_golden_file("query\nselect 1\n----\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("no input") !=
                  dstd::String::npos);
            CHECK(parsed.error().message().find("bad.test:1:") !=
                  dstd::String::npos);
        }
        SUBCASE("a line that is neither input, query nor a comment") {
            auto parsed =
                cli::parse_golden_file("qury\nselect 1\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("expected `input`, `query`") !=
                  dstd::String::npos);
        }
        SUBCASE("input with no path") {
            auto parsed = cli::parse_golden_file("input\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("names no path") !=
                  dstd::String::npos);
        }
        SUBCASE("query error with no text") {
            auto parsed = cli::parse_golden_file(
                "query error\nselect 1\n----\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("names no message text") !=
                  dstd::String::npos);
        }
        SUBCASE("an unknown query modifier") {
            auto parsed = cli::parse_golden_file(
                "query sorted\nselect 1\n----\n", "bad.test");
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().find("unknown query modifier") !=
                  dstd::String::npos);
        }
    }

    TEST_CASE("the record writer replaces the output and keeps the rest") {
        const dstd::String text =
            "# the comment\n"
            "input data/sample.pfw.gz\n"
            "query\nselect name\n----\n"
            "name\n"
            "wrong\n"
            "\n"
            "query error boom\nselect nope\n----\n";
        auto parsed =
            cli::parse_golden_file(dstd::StringView(text), "basic.test");
        REQUIRE(parsed.has_value());

        dstd::Vector<dstd::String> actuals;
        actuals.push_back(dstd::String("name\nopen\nclose\n"));
        actuals.push_back(dstd::String());
        const dstd::String rewritten =
            cli::render_golden_file(*parsed, actuals);
        CHECK(text_of(rewritten) ==
              "# the comment\n"
              "input data/sample.pfw.gz\n"
              "query\nselect name\n----\n"
              "name\nopen\nclose\n"
              "\n"
              "query error boom\nselect nope\n----\n");

        // The written form parses again into the same blocks, with the new
        // expected output, so a second run compares against it.
        auto again =
            cli::parse_golden_file(dstd::StringView(rewritten), "basic.test");
        REQUIRE(again.has_value());
        REQUIRE(again->blocks.size() == 2);
        CHECK(text_of(again->blocks[0].expected) == "name\nopen\nclose\n");
        CHECK(text_of(again->blocks[1].error_text) == "boom");
    }

    TEST_CASE("a row keeps its spaces and an empty last cell") {
        const dstd::String text =
            "# a value may start or end with a space, and a last cell may be\n"
            "# empty; both read back as written\n"
            "input data/sample.pfw.gz\n"
            "query\n"
            "select name, \"\" as empty\n"
            "----\n"
            "name\tempty\n"
            "  open  \t\n";
        auto parsed =
            cli::parse_golden_file(dstd::StringView(text), "spaces.test");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->blocks.size() == 1);
        CHECK(text_of(parsed->blocks[0].expected) ==
              "name\tempty\n  open  \t\n");

        // A record run writes the rows the query gave back, and the next parse
        // reads them as they were written.
        dstd::Vector<dstd::String> actuals;
        actuals.push_back(dstd::String("name\tempty\n  open  \t\n"));
        const dstd::String rewritten =
            cli::render_golden_file(*parsed, actuals);
        auto again =
            cli::parse_golden_file(dstd::StringView(rewritten), "spaces.test");
        REQUIRE(again.has_value());
        REQUIRE(again->blocks.size() == 1);
        CHECK(text_of(again->blocks[0].expected) ==
              text_of(parsed->blocks[0].expected));
    }

    TEST_CASE("a comment and a blank line after the last block survive") {
        const dstd::String text =
            "input data/sample.pfw.gz\n"
            "query\n"
            "select name\n"
            "----\n"
            "name\n"
            "wrong\n"
            "\n"
            "# the note after the last block\n";
        auto parsed =
            cli::parse_golden_file(dstd::StringView(text), "tail.test");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->blocks.size() == 1);
        CHECK(text_of(parsed->trailing) ==
              "\n# the note after the last block\n");

        dstd::Vector<dstd::String> actuals;
        actuals.push_back(dstd::String("name\nopen\n"));
        const dstd::String rewritten =
            cli::render_golden_file(*parsed, actuals);
        CHECK(text_of(rewritten) ==
              "input data/sample.pfw.gz\n"
              "query\n"
              "select name\n"
              "----\n"
              "name\nopen\n"
              "\n"
              "# the note after the last block\n");

        auto again =
            cli::parse_golden_file(dstd::StringView(rewritten), "tail.test");
        REQUIRE(again.has_value());
        CHECK(text_of(again->trailing) ==
              "\n# the note after the last block\n");
    }

    TEST_CASE("a record run writes the file and the next parse sees it") {
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        const dstd::String path = env.dir() + "/recorded.test";
        const dstd::String text =
            "input data/sample.pfw.gz\n"
            "query error not run\nselect nope\n----\n";
        REQUIRE(dstd::fs::write_file(dstd::PathView(path), text).has_value());

        auto parsed = cli::parse_golden_file(dstd::StringView(text), path);
        REQUIRE(parsed.has_value());
        dstd::Vector<dstd::String> actuals;
        actuals.push_back(dstd::String());
        const dstd::String rewritten =
            cli::render_golden_file(*parsed, actuals);
        REQUIRE(
            dstd::fs::write_file(dstd::PathView(path), rewritten).has_value());

        auto read = dstd::fs::read_file(dstd::PathView(path));
        REQUIRE(read.has_value());
        auto again = cli::parse_golden_file(dstd::StringView(*read), path);
        REQUIRE(again.has_value());
        REQUIRE(again->blocks.size() == 1);
        CHECK(text_of(again->blocks[0].query) == "select nope");
    }

    TEST_CASE("golden_test_files lists only .test files, sorted") {
        dftu_utils_test::TestEnvironment env(4);
        REQUIRE(env.is_valid());
        for (const char* name : {"a.test", "b.test", "notes.txt"})
            REQUIRE(
                dstd::fs::write_file(dstd::PathView(env.dir() + "/" + name), "")
                    .has_value());
        auto files = cli::golden_test_files(dstd::StringView(env.dir()));
        REQUIRE(files.has_value());
        REQUIRE(files->size() == 2);
        CHECK(files->at(0).ends_with("a.test"));
        CHECK(files->at(1).ends_with("b.test"));

        auto missing =
            cli::golden_test_files(dstd::StringView(env.dir() + "/nope"));
        CHECK(!missing.has_value());
    }
}
