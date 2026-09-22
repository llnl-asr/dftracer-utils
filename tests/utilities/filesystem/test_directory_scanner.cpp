#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
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

using dftu_utils_test::ScopedTestDir;

TEST_CASE("DirectoryScannerUtility - Basic Operations") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Scan empty directory") {
        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);
        CHECK(result.empty());
    }

    SUBCASE("Scan directory with files") {
        fixture.create_file("file1.txt", "content1");
        fixture.create_file("file2.txt", "content2");
        fixture.create_file("file3.dat", "content3");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);

        // Check all entries are regular files
        for (const auto& entry : result) {
            CHECK(entry.is_regular_file);
            CHECK_FALSE(entry.is_directory);
            CHECK(entry.size > 0);
        }
    }

    SUBCASE("Scan directory with subdirectories (non-recursive)") {
        fixture.create_file("file1.txt", "content");
        fixture.create_dir("subdir1");
        fixture.create_dir("subdir2");
        fixture.create_file("subdir1/file2.txt", "content");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        // Should find: file1.txt, subdir1, subdir2
        // Should NOT find: subdir1/file2.txt
        CHECK(result.size() == 3);

        int file_count = 0;
        int dir_count = 0;
        for (const auto& entry : result) {
            if (entry.is_regular_file) file_count++;
            if (entry.is_directory) dir_count++;
        }

        CHECK(file_count == 1);
        CHECK(dir_count == 2);
    }

    SUBCASE("Error - directory does not exist") {
        fs::path nonexistent = fixture.path() / "nonexistent";
        DirectoryScannerUtilityInput input{nonexistent, false};

        CHECK_THROWS_AS(run_scan(*scanner, input), fs::filesystem_error);
    }

    SUBCASE("Error - path is not a directory") {
        fixture.create_file("regular_file.txt");
        fs::path file_path = (fixture.path() / "regular_file.txt");
        DirectoryScannerUtilityInput input{file_path, false};

        CHECK_THROWS_AS(run_scan(*scanner, input), fs::filesystem_error);
    }
}

TEST_CASE("DirectoryScannerUtility - Recursive Scanning") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Recursive scan - simple hierarchy") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir1/file2.txt");
        fixture.create_file("subdir1/file3.txt");
        fixture.create_dir("subdir2");
        fixture.create_file("subdir2/file4.txt");

        DirectoryScannerUtilityInput input{fixture.path(), true};
        auto result = run_scan(*scanner, input);

        // Should find:
        // - file1.txt
        // - subdir1/
        // - subdir1/file2.txt
        // - subdir1/file3.txt
        // - subdir2/
        // - subdir2/file4.txt
        CHECK(result.size() == 6);

        int file_count = 0;
        int dir_count = 0;
        for (const auto& entry : result) {
            if (entry.is_regular_file) file_count++;
            if (entry.is_directory) dir_count++;
        }

        CHECK(file_count == 4);
        CHECK(dir_count == 2);
    }

    SUBCASE("Recursive scan - deep hierarchy") {
        fixture.create_file("level1/level2/level3/deep_file.txt");
        fixture.create_file("level1/level2/mid_file.txt");
        fixture.create_file("level1/shallow_file.txt");

        DirectoryScannerUtilityInput input{fixture.path(), true};
        auto result = run_scan(*scanner, input);

        // Should find all files and directories
        int file_count = 0;
        for (const auto& entry : result) {
            if (entry.is_regular_file) {
                file_count++;
            }
        }

        CHECK(file_count == 3);
    }

    SUBCASE("Recursive vs non-recursive comparison") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir/file2.txt");
        fixture.create_file("subdir/nested/file3.txt");

        // Non-recursive scan
        DirectoryScannerUtilityInput non_recursive{fixture.path(), false};
        auto non_recursive_result = run_scan(*scanner, non_recursive);

        // Recursive scan
        DirectoryScannerUtilityInput recursive{fixture.path(), true};
        auto recursive_result = run_scan(*scanner, recursive);

        // Non-recursive should find less than recursive
        CHECK(non_recursive_result.size() < recursive_result.size());

        // Non-recursive: file1.txt, subdir/
        CHECK(non_recursive_result.size() == 2);

        // Recursive: file1.txt, subdir/, subdir/file2.txt, subdir/nested/,
        // subdir/nested/file3.txt
        CHECK(recursive_result.size() == 5);
    }

    SUBCASE("Recursive scan skips generated output dirs (split/, .dftindex)") {
        fixture.create_file("trace.pfw.gz");
        // Derived split copy: must NOT be picked up by a recursive scan or it
        // would double-count events.
        fixture.create_file("split/trace.pfw.gz");
        fixture.create_file(".dftindex/view.pfw.gz");

        DirectoryScannerUtilityInput input{fixture.path(), true};
        auto result = run_scan(*scanner, input);

        for (const auto& entry : result) {
            const std::string path = entry.path.string();
            CHECK(path.find("/split/") == std::string::npos);
            CHECK(path.find("/split") ==
                  std::string::npos);  // no split dir entry either
            CHECK(path.find(".dftindex") == std::string::npos);
        }

        // Only the original trace remains.
        int file_count = 0;
        for (const auto& entry : result) {
            if (entry.is_regular_file) file_count++;
        }
        CHECK(file_count == 1);
    }
}

TEST_CASE("DirectoryScannerUtility - FileEntry Metadata") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("FileEntry contains correct metadata") {
        std::string content = "This is test content with some length";
        fixture.create_file("test_file.txt", content);

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        REQUIRE(result.size() == 1);
        const auto& entry = result[0];

        CHECK(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size == content.size());
        CHECK(entry.path.filename().string() == "test_file.txt");
    }

    SUBCASE("Directory entries have zero size") {
        fixture.create_dir("test_dir");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        REQUIRE(result.size() == 1);
        const auto& entry = result[0];

        CHECK_FALSE(entry.is_regular_file);
        CHECK(entry.is_directory);
        CHECK(entry.size == 0);
    }

    SUBCASE("Multiple files with different sizes") {
        fixture.create_file("small.txt", "x");
        fixture.create_file("medium.txt", std::string(100, 'y'));
        fixture.create_file("large.txt", std::string(1000, 'z'));

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);

        // Find and verify each file
        bool found_small = false, found_medium = false, found_large = false;
        for (const auto& entry : result) {
            if (entry.path.filename() == "small.txt") {
                CHECK(entry.size == 1);
                found_small = true;
            } else if (entry.path.filename() == "medium.txt") {
                CHECK(entry.size == 100);
                found_medium = true;
            } else if (entry.path.filename() == "large.txt") {
                CHECK(entry.size == 1000);
                found_large = true;
            }
        }

        CHECK(found_small);
        CHECK(found_medium);
        CHECK(found_large);
    }
}

TEST_CASE("DirectoryScannerUtility - Directory Struct") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");

    SUBCASE("Directory equality operator") {
        DirectoryScannerUtilityInput dir1{"/path/to/dir", false};
        DirectoryScannerUtilityInput dir2{"/path/to/dir", false};
        DirectoryScannerUtilityInput dir3{"/path/to/dir", true};
        DirectoryScannerUtilityInput dir4{"/different/path", false};

        CHECK(dir1 == dir2);
        CHECK(dir1 != dir3);  // Different recursive flag
        CHECK(dir1 != dir4);  // Different path
    }

    SUBCASE("Directory construction") {
        fs::path test_path = "/test/path";
        DirectoryScannerUtilityInput dir{test_path, true};

        CHECK(dir.path == test_path);
        CHECK(dir.recursive == true);
    }
}

TEST_CASE("DirectoryScannerUtility - Edge Cases") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Empty files") {
        fixture.create_file("empty1.txt", "");
        fixture.create_file("empty2.txt", "");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.size == 0);
            CHECK(entry.is_regular_file);
        }
    }

    SUBCASE("Hidden files (Unix-style)") {
        fixture.create_file(".hidden_file");
        fixture.create_file("visible_file");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        // Should find both files
        CHECK(result.size() == 2);
    }

    SUBCASE("Files with special characters in names") {
        fixture.create_file("file with spaces.txt");
        fixture.create_file("file-with-dashes.txt");
        fixture.create_file("file_with_underscores.txt");

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Many files") {
        const int num_files = 100;
        for (int i = 0; i < num_files; ++i) {
            fixture.create_file("file" + std::to_string(i) + ".txt");
        }

        DirectoryScannerUtilityInput input{fixture.path(), false};
        auto result = run_scan(*scanner, input);

        CHECK(result.size() == num_files);
    }
}

TEST_CASE("DirectoryScannerUtility - FileEntry Construction") {
    ScopedTestDir fixture("dftracer_test_directory_scanner");

    SUBCASE("FileEntry default constructor") {
        FileEntry entry;
        CHECK(entry.path.empty());
        CHECK(entry.size == 0);
        CHECK_FALSE(entry.is_directory);
        CHECK_FALSE(entry.is_regular_file);
    }

    SUBCASE("FileEntry with existing file") {
        fixture.create_file("test.txt", "content");
        fs::path file_path = (fixture.path() / "test.txt");

        FileEntry entry{file_path};

        CHECK(entry.path == file_path);
        CHECK(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size > 0);
    }

    SUBCASE("FileEntry with existing directory") {
        fixture.create_dir("test_dir");
        fs::path dir_path = (fixture.path() / "test_dir");

        FileEntry entry{dir_path};

        CHECK(entry.path == dir_path);
        CHECK_FALSE(entry.is_regular_file);
        CHECK(entry.is_directory);
        CHECK(entry.size == 0);
    }

    SUBCASE("FileEntry with nonexistent path") {
        fs::path nonexistent = (fixture.path() / "nonexistent");

        FileEntry entry{nonexistent};

        CHECK(entry.path == nonexistent);
        CHECK_FALSE(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size == 0);
    }
}
