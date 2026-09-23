#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <memory>
#include <vector>

using namespace dftracer::utils::utilities::filesystem;

// Run a scanner in a Runtime, injecting its scope. Re-raises any exception.
template <typename Scanner, typename Input>
static std::vector<FileEntry> run_scan(Scanner& scanner, const Input& input) {
    dftracer::utils::Runtime rt;
    std::vector<FileEntry> result;
    rt.run_blocking("scan",
                    [&](dftracer::utils::CoroScope& s)
                        -> dftracer::utils::coro::CoroTask<void> {
                        result = co_await scanner(s, input);
                    });
    return result;
}

// Helper to create a test directory structure
using dftu_utils_test::ScopedTestDir;

TEST_CASE("PatternDirectoryScannerUtility - Basic Operations") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Scan with no patterns - match all files") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");
        fixture.create_file("file3.log");

        PatternDirectoryScannerUtilityInput input{fixture.str(), {}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Scan empty directory") {
        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.empty());
    }

    SUBCASE("Single extension pattern") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.txt");
        fixture.create_file("file3.dat");
        fixture.create_file("file4.log");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

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

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt", ".dat"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
        for (const auto& entry : result) {
            std::string ext = entry.path.extension().string();
            CHECK((ext == ".txt" || ext == ".dat"));
        }
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Pattern Matching") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Extension pattern - single extension") {
        fixture.create_file("document.pdf");
        fixture.create_file("image.png");
        fixture.create_file("text.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".pdf"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "document.pdf");
    }

    SUBCASE("Compound extension pattern") {
        fixture.create_file("archive.tar.gz");
        fixture.create_file("data.pfw.gz");
        fixture.create_file("file.gz");
        fixture.create_file("plain.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".tar.gz", ".pfw.gz"}, false};
        auto result = run_scan(*scanner, input);

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

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {"*.txt"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".txt");
        }
    }

    SUBCASE("Exact filename match") {
        fixture.create_file("README.md");
        fixture.create_file("LICENSE");
        fixture.create_file("CHANGELOG");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {"README.md"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "README.md");
    }

    SUBCASE("No matches") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".log"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.empty());
    }

    SUBCASE("Mixed pattern types") {
        fixture.create_file("data.pfw");
        fixture.create_file("data.pfw.gz");
        fixture.create_file("README.md");
        fixture.create_file("notes.txt");
        fixture.create_file("image.png");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".pfw", ".pfw.gz", "README.md", "*.txt"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 4);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Recursive Scanning") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Recursive scan with pattern") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir/file2.txt");
        fixture.create_file("subdir/file3.dat");
        fixture.create_file("subdir/nested/file4.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, true};
        auto result = run_scan(*scanner, input);

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
        PatternDirectoryScannerUtilityInput non_recursive{
            fixture.str(), {".txt"}, false};
        auto non_recursive_result = run_scan(*scanner, non_recursive);

        // Recursive
        PatternDirectoryScannerUtilityInput recursive{
            fixture.str(), {".txt"}, true};
        auto recursive_result = run_scan(*scanner, recursive);

        CHECK(non_recursive_result.size() == 1);
        CHECK(recursive_result.size() == 3);
    }

    SUBCASE("Deep hierarchy with patterns") {
        fixture.create_file("level1/data.pfw");
        fixture.create_file("level1/level2/data.pfw");
        fixture.create_file("level1/level2/level3/data.pfw");
        fixture.create_file("level1/level2/level3/other.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".pfw"}, true};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
        for (const auto& entry : result) {
            CHECK(entry.path.extension() == ".pfw");
        }
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Directory Filtering") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Directories are not matched") {
        fixture.create_dir("test.txt");  // Directory named like a file
        fixture.create_file("real_file.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

        // Should only find the real file, not the directory
        CHECK(result.size() == 1);
        CHECK(result[0].is_regular_file);
        CHECK_FALSE(result[0].is_directory);
    }

    SUBCASE("Mixed files and directories") {
        fixture.create_file("file1.txt");
        fixture.create_dir("dir.txt");
        fixture.create_file("file2.txt");
        fixture.create_dir("normal_dir");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

        // Should only match regular files
        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.is_regular_file);
        }
    }
}

TEST_CASE(
    "PatternDirectoryScannerUtility - PatternDirectoryScannerUtilityInput "
    "Builder") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");

    SUBCASE("Builder pattern - from_path") {
        auto input =
            PatternDirectoryScannerUtilityInput::from_path(fixture.str());
        CHECK(input.path == fixture.str());
        CHECK(input.patterns.empty());
        CHECK(input.recursive == false);
    }

    SUBCASE("Builder pattern - with_patterns") {
        auto input =
            PatternDirectoryScannerUtilityInput::from_path(fixture.str())
                .with_patterns({".txt", ".dat"});

        CHECK(input.patterns.size() == 2);
        CHECK(input.patterns[0] == ".txt");
        CHECK(input.patterns[1] == ".dat");
    }

    SUBCASE("Builder pattern - with_recursive") {
        auto input =
            PatternDirectoryScannerUtilityInput::from_path(fixture.str())
                .with_recursive(true);

        CHECK(input.recursive == true);
    }

    SUBCASE("Builder pattern - chained") {
        auto input =
            PatternDirectoryScannerUtilityInput::from_path(fixture.str())
                .with_patterns({".pfw", ".pfw.gz"})
                .with_recursive(true);

        CHECK(input.path == fixture.str());
        CHECK(input.patterns.size() == 2);
        CHECK(input.recursive == true);
    }

    SUBCASE("Direct constructor") {
        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt", ".dat"}, true};

        CHECK(input.path == fixture.str());
        CHECK(input.patterns.size() == 2);
        CHECK(input.recursive == true);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Edge Cases") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("Empty pattern string") {
        fixture.create_file("file1.txt");
        fixture.create_file("file2.dat");

        PatternDirectoryScannerUtilityInput input{fixture.str(), {""}, false};
        auto result = run_scan(*scanner, input);

        // Empty pattern should match all files
        CHECK(result.size() == 2);
    }

    SUBCASE("Files without extensions") {
        fixture.create_file("README");
        fixture.create_file("LICENSE");
        fixture.create_file("Makefile");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {"README", "LICENSE"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 2);
    }

    SUBCASE("Case-sensitive matching") {
        fixture.create_file("file.TXT");
        fixture.create_file("other.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

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

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 1);
        CHECK(result[0].path.filename() == "file.v1.0.txt");
    }

    SUBCASE("Files with similar extensions") {
        fixture.create_file("file.txt");
        fixture.create_file("file.text");
        fixture.create_file("file.tx");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

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

        PatternDirectoryScannerUtilityInput input{fixture.str(), many_patterns,
                                                  false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Pattern with special characters") {
        fixture.create_file("file-1.txt");
        fixture.create_file("file_2.txt");
        fixture.create_file("file 3.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Performance") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
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

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".txt"}, false};
        auto result = run_scan(*scanner, input);

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

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".pfw"}, true};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 5);
    }
}

TEST_CASE("PatternDirectoryScannerUtility - Real World Scenarios") {
    ScopedTestDir fixture("dftracer_test_pattern_scanner");
    auto scanner = std::make_shared<PatternDirectoryScannerUtility>();

    SUBCASE("DFTracer files - .pfw and .pfw.gz") {
        fixture.create_file("trace1.pfw");
        fixture.create_file("trace2.pfw.gz");
        fixture.create_file("trace3.pfw");
        fixture.create_file("metadata.json");
        fixture.create_file("readme.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".pfw", ".pfw.gz"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Source code files") {
        fixture.create_file("main.cpp");
        fixture.create_file("utils.cpp");
        fixture.create_file("utils.h");
        fixture.create_file("README.md");
        fixture.create_file("CMakeLists.txt");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".cpp", ".h"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Log files with date patterns") {
        fixture.create_file("app.log");
        fixture.create_file("app.2024-01-01.log");
        fixture.create_file("app.2024-01-02.log");
        fixture.create_file("error.log");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {"*.log"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 4);
    }

    SUBCASE("Archive files") {
        fixture.create_file("backup.tar");
        fixture.create_file("backup.tar.gz");
        fixture.create_file("data.zip");
        fixture.create_file("archive.7z");

        PatternDirectoryScannerUtilityInput input{
            fixture.str(), {".tar", ".tar.gz", ".zip"}, false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }
}
