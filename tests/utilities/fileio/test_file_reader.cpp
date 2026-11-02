// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/utilities/fileio/file_reader_utility.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils::utilities::fileio;
using namespace dftracer::utils::utilities::filesystem;
using namespace dftracer::utils::utilities::text;
using namespace dftu_utils_test;

TEST_CASE("FileReaderUtility - Basic Operations") {
    FileReaderUtility reader;
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_file_reader.txt");

    SUBCASE("Read simple text file") {
        // Create test file
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "Hello, World!\n";
            ofs << "This is a test file.\n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content == "Hello, World!\nThis is a test file.\n");
        CHECK(content.size() == 35);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Read empty file") {
        // Create empty file
        {
            std::ofstream ofs(test_file.c_str());
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.empty());
        CHECK(content.size() == 0);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Read file with special characters") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "Special: !@#$%^&*()\n";
            ofs << "Unicode: こんにちは\n";
            ofs << "Tabs:\t\t\tSpaces:   \n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.find("Special: !@#$%^&*()") != std::string::npos);
        CHECK(content.content.find("こんにちは") != std::string::npos);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Error Handling") {
    FileReaderUtility reader;

    SUBCASE("Non-existent file") {
        FileEntry entry{"non_existent_file_12345.txt"};
        CHECK_THROWS_AS(reader(entry).get(),
                        dftracer::utils::common::DFTUtilsException);
    }

    SUBCASE("Directory instead of file") {
        dftracer::utils::dstd::Path test_dir =
            make_unique_test_path("test_dir_reader");
        REQUIRE(
            dftracer::utils::dstd::fs::create_directory(test_dir).has_value());

        FileEntry entry{test_dir.str()};
        CHECK_THROWS_AS(reader(entry).get(),
                        dftracer::utils::common::DFTUtilsException);

        (void)dftracer::utils::dstd::fs::remove(test_dir);
    }
}

TEST_CASE("FileReaderUtility - Different File Sizes") {
    FileReaderUtility reader;
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_file_sizes.txt");

    SUBCASE("Single byte") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "x";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.size() == 1);
        CHECK(content.content == "x");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("1KB file") {
        {
            std::ofstream ofs(test_file.c_str());
            for (int i = 0; i < 1024; ++i) {
                ofs << "a";
            }
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.size() == 1024);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Large file (100KB)") {
        {
            std::ofstream ofs(test_file.c_str());
            for (int i = 0; i < 100 * 1024; ++i) {
                ofs << "b";
            }
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.size() == 100 * 1024);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Different Content Types") {
    FileReaderUtility reader;
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_content_types.txt");

    SUBCASE("Newlines only") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "\n\n\n\n\n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.size() == 5);
        CHECK(content.content == "\n\n\n\n\n");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Mixed line endings") {
        {
            std::ofstream ofs(test_file.c_str(), std::ios::binary);
            ofs << "Line 1\n";
            ofs << "Line 2\r\n";
            ofs << "Line 3\n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.find("Line 1") != std::string::npos);
        CHECK(content.content.find("Line 2") != std::string::npos);
        CHECK(content.content.find("Line 3") != std::string::npos);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("No trailing newline") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "No newline at end";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content == "No newline at end");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Real World Scenarios") {
    FileReaderUtility reader;
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_real_world_file_reader.txt");

    SUBCASE("Log file") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "[2024-01-01 10:00:00] INFO: Application started\n";
            ofs << "[2024-01-01 10:00:01] DEBUG: Loading configuration\n";
            ofs << "[2024-01-01 10:00:02] INFO: Configuration loaded\n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.find("INFO: Application started") !=
              std::string::npos);
        CHECK(content.content.find("DEBUG: Loading configuration") !=
              std::string::npos);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("CSV file") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << "id,name,value\n";
            ofs << "1,Alice,100\n";
            ofs << "2,Bob,200\n";
            ofs << "3,Charlie,300\n";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.find("id,name,value") != std::string::npos);
        CHECK(content.content.find("Alice") != std::string::npos);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("JSON file") {
        {
            std::ofstream ofs(test_file.c_str());
            ofs << R"({
  "name": "test",
  "value": 42,
  "items": [1, 2, 3]
})";
        }

        FileEntry entry{test_file.str()};
        Text content = reader(entry).get();

        CHECK(content.content.find("\"name\": \"test\"") != std::string::npos);
        CHECK(content.content.find("\"value\": 42") != std::string::npos);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Composition with DirectoryScanner") {
    FileReaderUtility reader;
    DirectoryScannerUtility scanner;

    // Create test directory with files
    dftracer::utils::dstd::Path test_dir =
        make_unique_test_path("test_composition_dir");
    REQUIRE(dftracer::utils::dstd::fs::create_directory(test_dir).has_value());

    {
        std::ofstream ofs((test_dir / "file1.txt").c_str());
        ofs << "Content 1";
    }
    {
        std::ofstream ofs((test_dir / "file2.txt").c_str());
        ofs << "Content 2";
    }

    // Scan and read
    DirectoryScannerUtilityInput dir{test_dir.str()};
    auto files = scanner(dir).get();

    int files_read = 0;
    for (const auto& file : files) {
        if (file.is_regular_file) {
            Text content = reader(file).get();
            CHECK(content.size() > 0);
            files_read++;
        }
    }

    CHECK(files_read == 2);

    // Cleanup
    dftracer::utils::dstd::fs::remove_all(test_dir).has_value();
}
