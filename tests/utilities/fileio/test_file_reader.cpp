// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/utilities/fileio/file_reader_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

using namespace dftracer::utils::utilities::fileio;
using namespace dftracer::utils::utilities::text;
using namespace dftu_utils_test;

TEST_CASE("FileReaderUtility - Basic Operations") {
    FileReaderUtility reader;
    dstd::Path test_file = make_unique_test_path("test_file_reader.txt");

    SUBCASE("Read simple text file") {
        // Create test file
        {
            dstd::String ofs;
            ofs += "Hello, World!\n";
            ofs += "This is a test file.\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content == "Hello, World!\nThis is a test file.\n");
        CHECK(content.size() == 35);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Read empty file") {
        // Create empty file
        {
            dstd::String ofs;
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.empty());
        CHECK(content.size() == 0);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Read file with special characters") {
        {
            dstd::String ofs;
            ofs += "Special: !@#$%^&*()\n";
            ofs += "Unicode: こんにちは\n";
            ofs += "Tabs:\t\t\tSpaces:   \n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.find("Special: !@#$%^&*()") !=
              dstd::String::npos);
        CHECK(content.content.find("こんにちは") != dstd::String::npos);

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("FileReaderUtility - Error Handling") {
    FileReaderUtility reader;

    SUBCASE("Non-existent file") {
        const dstd::Path missing("non_existent_file_12345.txt");
        CHECK_THROWS_AS(reader(missing).get(),
                        dftracer::utils::common::DFTUtilsException);
    }

    SUBCASE("Directory instead of file") {
        dstd::Path test_dir = make_unique_test_path("test_dir_reader");
        REQUIRE(dstd::fs::create_directory(test_dir).has_value());

        CHECK_THROWS_AS(reader(test_dir).get(),
                        dftracer::utils::common::DFTUtilsException);

        dstd::fs::remove(test_dir).discard();
    }
}

TEST_CASE("FileReaderUtility - Different File Sizes") {
    FileReaderUtility reader;
    dstd::Path test_file = make_unique_test_path("test_file_sizes.txt");

    SUBCASE("Single byte") {
        {
            dstd::String ofs;
            ofs += "x";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.size() == 1);
        CHECK(content.content == "x");

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("1KB file") {
        {
            dstd::String ofs;
            for (int i = 0; i < 1024; ++i) {
                ofs += "a";
            }
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.size() == 1024);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Large file (100KB)") {
        {
            dstd::String ofs;
            for (int i = 0; i < 100 * 1024; ++i) {
                ofs += "b";
            }
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.size() == 100 * 1024);

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("FileReaderUtility - Different Content Types") {
    FileReaderUtility reader;
    dstd::Path test_file = make_unique_test_path("test_content_types.txt");

    SUBCASE("Newlines only") {
        {
            dstd::String ofs;
            ofs += "\n\n\n\n\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.size() == 5);
        CHECK(content.content == "\n\n\n\n\n");

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Mixed line endings") {
        {
            dstd::String ofs;
            ofs += "Line 1\n";
            ofs += "Line 2\r\n";
            ofs += "Line 3\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.find("Line 1") != dstd::String::npos);
        CHECK(content.content.find("Line 2") != dstd::String::npos);
        CHECK(content.content.find("Line 3") != dstd::String::npos);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("No trailing newline") {
        {
            dstd::fs::write_file(dstd::PathView(test_file), "No newline at end")
                .discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content == "No newline at end");

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("FileReaderUtility - Real World Scenarios") {
    FileReaderUtility reader;
    dstd::Path test_file =
        make_unique_test_path("test_real_world_file_reader.txt");

    SUBCASE("Log file") {
        {
            dstd::String ofs;
            ofs += "[2024-01-01 10:00:00] INFO: Application started\n";
            ofs += "[2024-01-01 10:00:01] DEBUG: Loading configuration\n";
            ofs += "[2024-01-01 10:00:02] INFO: Configuration loaded\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.find("INFO: Application started") !=
              dstd::String::npos);
        CHECK(content.content.find("DEBUG: Loading configuration") !=
              dstd::String::npos);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("CSV file") {
        {
            dstd::String ofs;
            ofs += "id,name,value\n";
            ofs += "1,Alice,100\n";
            ofs += "2,Bob,200\n";
            ofs += "3,Charlie,300\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.find("id,name,value") != dstd::String::npos);
        CHECK(content.content.find("Alice") != dstd::String::npos);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("JSON file") {
        {
            dstd::fs::write_file(dstd::PathView(test_file), R"({
  "name": "test",
  "value": 42,
  "items": [1, 2, 3]
})")
                .discard();
        }

        Text content = reader(test_file).get();

        CHECK(content.content.find("\"name\": \"test\"") != dstd::String::npos);
        CHECK(content.content.find("\"value\": 42") != dstd::String::npos);

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("FileReaderUtility - Composition with find_all") {
    FileReaderUtility reader;

    // Create test directory with files
    dstd::Path test_dir = make_unique_test_path("test_composition_dir");
    REQUIRE(dstd::fs::create_directory(test_dir).has_value());

    {
        dstd::String ofs;
        ofs += "Content 1";
        dstd::fs::write_file(dstd::PathView(test_dir / "file1.txt"), ofs)
            .discard();
    }
    {
        dstd::fs::write_file(dstd::PathView(test_dir / "file2.txt"),
                             "Content 2")
            .discard();
    }

    // Scan and read
    dstd::Vector<runtime::fs::FoundFile> files;
    coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto found = co_await runtime::fs::find_all(scope, test_dir.view());
        REQUIRE(found.has_value());
        files = std::move(*found);
    }).get();

    int files_read = 0;
    for (const auto& file : files) {
        Text content = reader(file.path).get();
        CHECK(content.size() > 0);
        files_read++;
    }

    CHECK(files_read == 2);

    // Cleanup
    dstd::fs::remove_all(test_dir).has_value();
}
