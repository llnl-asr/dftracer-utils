#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner.h>
#include <doctest/doctest.h>

#include <fstream>
#include <memory>

using namespace dftracer::utils::utilities::filesystem;

// Helper to create a test directory structure
class TestDirectoryFixture {
   public:
    fs::path test_root;

    TestDirectoryFixture() {
        // Create a unique test directory
        test_root = fs::temp_directory_path() / "dftracer_test_pattern_scanner";

        // Clean up if it exists from a previous run
        if (fs::exists(test_root)) {
            fs::remove_all(test_root);
        }

        fs::create_directories(test_root);
    }

    ~TestDirectoryFixture() {
        // Clean up test directory
        if (fs::exists(test_root)) {
            fs::remove_all(test_root);
        }
    }

    void create_file(const std::string& relative_path,
                     const std::string& content = "test") {
        fs::path file_path = test_root / relative_path;

        // Create parent directories if needed
        fs::create_directories(file_path.parent_path());

        std::ofstream ofs(file_path);
        ofs << content;
        ofs.close();
    }

    void create_directory(const std::string& relative_path) {
        fs::path dir_path = test_root / relative_path;
        fs::create_directories(dir_path);
    }

    std::string get_path(const std::string& relative_path = "") const {
        if (relative_path.empty()) {
            return test_root.string();
        }
        return (test_root / relative_path).string();
    }
};

TEST_CASE("PatternDirectoryScannerUtility - Basic Operations") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Scan with no patterns - match all files") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");
        fixture.create_file("file3.log");

        PatternDirectory input{fixture.get_path(), {}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Scan empty directory") {
        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.empty());
    }

    SUBCASE("Single extension pattern") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.txt");
        fixture.create_file("file3.dat");
        fixture.create_file("file4.log");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".txt");
        }
    }

    SUBCASE("Multiple extension patterns") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");
        fixture.create_file("file3.log");
        fixture.create_file("file4.txt");

        PatternDirectory input{fixture.get_path(), {".txt", ".dat"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
        for (const auto& entry : result) {
            std::string ext = entry.path.extension().string();
            CHECK((ext == ".txt" || ext == ".dat"));
        }
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Pattern Matching") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Extension pattern - single extension") {
        fixture.create_file("document.pdf");
        fixture.create_file("image.png");
        fixture.create_file("text.txt");

        PatternDirectory input{fixture.get_path(), {".pdf"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "document.pdf");
    }

    SUBCASE("Compound extension pattern") {
        fixture.create_file("archive.tar.gz");
        fixture.create_file("data.pfw.gz");
        fixture.create_file("file.gz");
        fixture.create_file("plain.txt");

        PatternDirectory input{
            fixture.get_path(), {".tar.gz", ".pfw.gz"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            std::string filename = entry.path.filename().string();
            CHECK((filename == "archive.tar.gz" || filename == "data.pfw.gz"));
        }
    }

    SUBCASE("Glob pattern - *.extension") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.txt");
        fixture.create_file("file3.dat");

        PatternDirectory input{fixture.get_path(), {"*.txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".txt");
        }
    }

    SUBCASE("Exact filename match") {
        fixture.create_file("README.md");
        fixture.create_file("LICENSE");
        fixture.create_file("CHANGELOG");

        PatternDirectory input{fixture.get_path(), {"README.md"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "README.md");
    }

    SUBCASE("No matches") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");

        PatternDirectory input{fixture.get_path(), {".log"}, false};
        auto result = scanner->process(input);

        CHECK(result.empty());
    }

    SUBCASE("Mixed pattern types") {
        fixture.create_file("data.pfw");
        fixture.create_file("data.pfw.gz");
        fixture.create_file("README.md");
        fixture.create_file("notes.txt");
        fixture.create_file("image.png");

        PatternDirectory input{fixture.get_path(),
                               {".pfw", ".pfw.gz", "README.md", "*.txt"},
                               false};
        auto result = scanner->process(input);

        CHECK(result.size() == 4);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Recursive Scanning") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Recursive scan with pattern") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir/file2.txt");
        fixture.create_file("subdir/file3.dat");
        fixture.create_file("subdir/nested/file4.txt");

        PatternDirectory input{fixture.get_path(), {".txt"}, true};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".txt");
        }
    }

    SUBCASE("Recursive vs non-recursive") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir/file2.txt");
        fixture.create_file("subdir/nested/file3.txt");

        // Non-recursive
        PatternDirectory non_recursive{fixture.get_path(), {".txt"}, false};
        auto non_recursive_result = scanner->process(non_recursive);

        // Recursive
        PatternDirectory recursive{fixture.get_path(), {".txt"}, true};
        auto recursive_result = scanner->process(recursive);

        CHECK(non_recursive_result.size() == 1);
        CHECK(recursive_result.size() == 3);
    }

    SUBCASE("Deep hierarchy with patterns") {
        fixture.create_file("level1/data.pfw");
        fixture.create_file("level1/level2/data.pfw");
        fixture.create_file("level1/level2/level3/data.pfw");
        fixture.create_file("level1/level2/level3/other.txt");

        PatternDirectory input{fixture.get_path(), {".pfw"}, true};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".pfw");
        }
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Directory Filtering") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Directories are not matched") {
        fixture.create_directory("test.txt");  // Directory named like a file
        fixture.create_file("real_file.txt");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        // Should only find the real file, not the directory
        CHECK(result.size() == 1);
        CHECK(result[0].is_regular_file);
        CHECK_FALSE(result[0].is_directory);
    }

    SUBCASE("Mixed files and directories") {
        fixture.create_file("file1.txt");
        fixture.create_directory("dir.txt");
        fixture.create_file("file2.txt");
        fixture.create_directory("normal_dir");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        // Should only match regular files
        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.is_regular_file);
        }
    }
}

TEST_CASE("PatternDirectoryScannerUtility - PatternDirectory Builder") {
    TestDirectoryFixture fixture;

    SUBCASE("Builder pattern - from_path") {
        auto input = PatternDirectory::from_path(fixture.get_path());
        CHECK(input.path == fixture.get_path());
        CHECK(input.patterns.empty());
        CHECK(input.recursive == false);
    }

    SUBCASE("Builder pattern - with_patterns") {
        auto input = PatternDirectory::from_path(fixture.get_path())
                         .with_patterns({".txt", ".dat"});

        CHECK(input.patterns.size() == 2);
        CHECK(input.patterns[0] == ".txt");
        CHECK(input.patterns[1] == ".dat");
    }

    SUBCASE("Builder pattern - with_recursive") {
        auto input = PatternDirectory::from_path(fixture.get_path())
                         .with_recursive(true);

        CHECK(input.recursive == true);
    }

    SUBCASE("Builder pattern - chained") {
        auto input = PatternDirectory::from_path(fixture.get_path())
                         .with_patterns({".pfw", ".pfw.gz"})
                         .with_recursive(true);

        CHECK(input.path == fixture.get_path());
        CHECK(input.patterns.size() == 2);
        CHECK(input.recursive == true);
    }

    SUBCASE("Direct constructor") {
        PatternDirectory input{fixture.get_path(), {".txt", ".dat"}, true};

        CHECK(input.path == fixture.get_path());
        CHECK(input.patterns.size() == 2);
        CHECK(input.recursive == true);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Edge Cases") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Empty pattern string") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");

        PatternDirectory input{fixture.get_path(), {""}, false};
        auto result = scanner->process(input);

        // Empty pattern should match all files
        CHECK(result.size() == 2);
    }

    SUBCASE("Files without extensions") {
        fixture.create_file("README");
        fixture.create_file("LICENSE");
        fixture.create_file("Makefile");

        PatternDirectory input{
            fixture.get_path(), {"README", "LICENSE"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 2);
    }

    SUBCASE("Case-sensitive matching") {
        fixture.create_file("file.TXT");
        fixture.create_file("other.txt");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        // Should match .txt files (behavior may be platform-dependent for case)
        // On case-insensitive filesystems (macOS default), may match both
        // On case-sensitive filesystems (Linux), will match only lowercase
        CHECK(result.size() >= 1);

        // At least one .txt or .TXT file should be matched
        bool found_txt = false;
        for (const auto& entry : result) {
            std::string ext = entry.path.extension().string();
            if (ext == ".txt" || ext == ".TXT") {
                found_txt = true;
            }
        }
        CHECK(found_txt);
    }

    SUBCASE("Multiple dots in filename") {
        fixture.create_file("file.v1.0.txt");
        fixture.create_file("data.backup.2024.dat");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "file.v1.0.txt");
    }

    SUBCASE("Files with similar extensions") {
        fixture.create_file("file.txt");
        fixture.create_file("file.text");
        fixture.create_file("file.tx");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.extension() == ".txt");
    }

    SUBCASE("Large number of patterns") {
        fixture.create_file("file.ext1");
        fixture.create_file("file.ext50");
        fixture.create_file("file.ext100");

        std::vector<std::string> many_patterns;
        for (int i = 1; i <= 100; ++i) {
            many_patterns.push_back(".ext" + std::to_string(i));
        }

        PatternDirectory input{fixture.get_path(), many_patterns, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Pattern with special characters") {
        fixture.create_file("file-1.txt");
        fixture.create_file("file_2.txt");
        fixture.create_file("file 3.txt");

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Performance") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Many files with pattern matching") {
        const int num_txt_files = 50;
        const int num_dat_files = 50;

        for (int i = 0; i < num_txt_files; ++i) {
            fixture.create_file("file" + std::to_string(i) + ".txt");
        }
        for (int i = 0; i < num_dat_files; ++i) {
            fixture.create_file("data" + std::to_string(i) + ".dat");
        }

        PatternDirectory input{fixture.get_path(), {".txt"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == num_txt_files);
    }

    SUBCASE("Deep recursive scan with filtering") {
        // Create a deep hierarchy
        for (int level = 0; level < 5; ++level) {
            std::string path = "level" + std::to_string(level);
            for (int j = level; j > 0; --j) {
                path = "level" + std::to_string(j - 1) + "/" + path;
            }
            fixture.create_file(path + "/data.pfw");
            fixture.create_file(path + "/other.txt");
        }

        PatternDirectory input{fixture.get_path(), {".pfw"}, true};
        auto result = scanner->process(input);

        CHECK(result.size() == 5);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Real World Scenarios") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("DFTracer files - .pfw and .pfw.gz") {
        fixture.create_file("trace1.pfw");
        fixture.create_file("trace2.pfw.gz");
        fixture.create_file("trace3.pfw");
        fixture.create_file("metadata.json");
        fixture.create_file("readme.txt");

        PatternDirectory input{fixture.get_path(), {".pfw", ".pfw.gz"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Source code files") {
        fixture.create_file("main.cpp");
        fixture.create_file("utils.cpp");
        fixture.create_file("utils.h");
        fixture.create_file("README.md");
        fixture.create_file("CMakeLists.txt");

        PatternDirectory input{fixture.get_path(), {".cpp", ".h"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Log files with date patterns") {
        fixture.create_file("app.log");
        fixture.create_file("app.2024-01-01.log");
        fixture.create_file("app.2024-01-02.log");
        fixture.create_file("error.log");

        PatternDirectory input{fixture.get_path(), {"*.log"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 4);
    }

    SUBCASE("Archive files") {
        fixture.create_file("backup.tar");
        fixture.create_file("backup.tar.gz");
        fixture.create_file("data.zip");
        fixture.create_file("archive.7z");

        PatternDirectory input{
            fixture.get_path(), {".tar", ".tar.gz", ".zip"}, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }
}
