#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner.h>
#include <dftracer/utils/utilities/io/file_reader.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>

using namespace dftracer::utils::utilities::io;
using namespace dftracer::utils::utilities::filesystem;
using namespace dftracer::utils::utilities::text;

TEST_CASE("FileReaderUtility - Basic Operations") {
    FileReaderUtility reader;
    fs::path test_file = "test_file_reader.txt";

    SUBCASE("Read simple text file") {
        // Create test file
        {
            std::ofstream ofs(test_file);
            ofs << "Hello, World!\n";
            ofs << "This is a test file.\n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content == "Hello, World!\nThis is a test file.\n");
        CHECK(content.size() == 35);

        fs::remove(test_file);
    }

    SUBCASE("Read empty file") {
        // Create empty file
        {
            std::ofstream ofs(test_file);
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.empty());
        CHECK(content.size() == 0);

        fs::remove(test_file);
    }

    SUBCASE("Read file with special characters") {
        {
            std::ofstream ofs(test_file);
            ofs << "Special: !@#$%^&*()\n";
            ofs << "Unicode: こんにちは\n";
            ofs << "Tabs:\t\t\tSpaces:   \n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.find("Special: !@#$%^&*()") != std::string::npos);
        CHECK(content.content.find("こんにちは") != std::string::npos);

        fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Error Handling") {
    FileReaderUtility reader;

    SUBCASE("Non-existent file") {
        FileEntry entry{"non_existent_file_12345.txt"};
        CHECK_THROWS_AS(reader.process(entry), std::runtime_error);
    }

    SUBCASE("Directory instead of file") {
        fs::path test_dir = "test_dir_reader";
        fs::create_directory(test_dir);

        FileEntry entry{test_dir};
        CHECK_THROWS_AS(reader.process(entry), std::runtime_error);

        fs::remove(test_dir);
    }
}

TEST_CASE("FileReaderUtility - Different File Sizes") {
    FileReaderUtility reader;
    fs::path test_file = "test_file_sizes.txt";

    SUBCASE("Single byte") {
        {
            std::ofstream ofs(test_file);
            ofs << "x";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.size() == 1);
        CHECK(content.content == "x");

        fs::remove(test_file);
    }

    SUBCASE("1KB file") {
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 1024; ++i) {
                ofs << "a";
            }
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.size() == 1024);

        fs::remove(test_file);
    }

    SUBCASE("Large file (100KB)") {
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 100 * 1024; ++i) {
                ofs << "b";
            }
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.size() == 100 * 1024);

        fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Different Content Types") {
    FileReaderUtility reader;
    fs::path test_file = "test_content_types.txt";

    SUBCASE("Newlines only") {
        {
            std::ofstream ofs(test_file);
            ofs << "\n\n\n\n\n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.size() == 5);
        CHECK(content.content == "\n\n\n\n\n");

        fs::remove(test_file);
    }

    SUBCASE("Mixed line endings") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs << "Line 1\n";
            ofs << "Line 2\r\n";
            ofs << "Line 3\n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.find("Line 1") != std::string::npos);
        CHECK(content.content.find("Line 2") != std::string::npos);
        CHECK(content.content.find("Line 3") != std::string::npos);

        fs::remove(test_file);
    }

    SUBCASE("No trailing newline") {
        {
            std::ofstream ofs(test_file);
            ofs << "No newline at end";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content == "No newline at end");

        fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Real World Scenarios") {
    FileReaderUtility reader;
    fs::path test_file = "test_real_world.txt";

    SUBCASE("Log file") {
        {
            std::ofstream ofs(test_file);
            ofs << "[2024-01-01 10:00:00] INFO: Application started\n";
            ofs << "[2024-01-01 10:00:01] DEBUG: Loading configuration\n";
            ofs << "[2024-01-01 10:00:02] INFO: Configuration loaded\n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.find("INFO: Application started") !=
              std::string::npos);
        CHECK(content.content.find("DEBUG: Loading configuration") !=
              std::string::npos);

        fs::remove(test_file);
    }

    SUBCASE("CSV file") {
        {
            std::ofstream ofs(test_file);
            ofs << "id,name,value\n";
            ofs << "1,Alice,100\n";
            ofs << "2,Bob,200\n";
            ofs << "3,Charlie,300\n";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.find("id,name,value") != std::string::npos);
        CHECK(content.content.find("Alice") != std::string::npos);

        fs::remove(test_file);
    }

    SUBCASE("JSON file") {
        {
            std::ofstream ofs(test_file);
            ofs << R"({
  "name": "test",
  "value": 42,
  "items": [1, 2, 3]
})";
        }

        FileEntry entry{test_file};
        Text content = reader.process(entry);

        CHECK(content.content.find("\"name\": \"test\"") != std::string::npos);
        CHECK(content.content.find("\"value\": 42") != std::string::npos);

        fs::remove(test_file);
    }
}

TEST_CASE("FileReaderUtility - Composition with DirectoryScanner") {
    FileReaderUtility reader;
    DirectoryScannerUtility scanner;

    // Create test directory with files
    fs::path test_dir = "test_composition_dir";
    fs::create_directory(test_dir);

    {
        std::ofstream ofs(test_dir / "file1.txt");
        ofs << "Content 1";
    }
    {
        std::ofstream ofs(test_dir / "file2.txt");
        ofs << "Content 2";
    }

    // Scan and read
    Directory dir{test_dir};
    auto files = scanner.process(dir);

    int files_read = 0;
    for (const auto& file : files) {
        if (file.is_regular_file) {
            Text content = reader.process(file);
            CHECK(content.size() > 0);
            files_read++;
        }
    }

    CHECK(files_read == 2);

    // Cleanup
    fs::remove_all(test_dir);
}
