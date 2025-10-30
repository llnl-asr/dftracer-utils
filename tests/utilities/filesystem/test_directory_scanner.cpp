#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
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
        test_root =
            fs::temp_directory_path() / "dftracer_test_directory_scanner";

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

    fs::path get_path(const std::string& relative_path = "") const {
        if (relative_path.empty()) {
            return test_root;
        }
        return test_root / relative_path;
    }
};

TEST_CASE("DirectoryScannerUtility - Basic Operations") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Scan empty directory") {
        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);
        CHECK(result.empty());
    }

    SUBCASE("Scan directory with files") {
        fixture.create_file("file1.txt", "content1");
        fixture.create_file("file2.txt", "content2");
        fixture.create_file("file3.dat", "content3");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

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
        fixture.create_directory("subdir1");
        fixture.create_directory("subdir2");
        fixture.create_file("subdir1/file2.txt", "content");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

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
        fs::path nonexistent = fixture.test_root / "nonexistent";
        DirectoryScannerUtilityInput input{nonexistent, false};

        CHECK_THROWS_AS(scanner->process(input), fs::filesystem_error);
    }

    SUBCASE("Error - path is not a directory") {
        fixture.create_file("regular_file.txt");
        fs::path file_path = fixture.get_path("regular_file.txt");
        DirectoryScannerUtilityInput input{file_path, false};

        CHECK_THROWS_AS(scanner->process(input), fs::filesystem_error);
    }
}

TEST_CASE("DirectoryScannerUtility - Recursive Scanning") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Recursive scan - simple hierarchy") {
        fixture.create_file("file1.txt");
        fixture.create_file("subdir1/file2.txt");
        fixture.create_file("subdir1/file3.txt");
        fixture.create_directory("subdir2");
        fixture.create_file("subdir2/file4.txt");

        DirectoryScannerUtilityInput input{fixture.test_root, true};
        auto result = scanner->process(input);

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

        DirectoryScannerUtilityInput input{fixture.test_root, true};
        auto result = scanner->process(input);

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
        DirectoryScannerUtilityInput non_recursive{fixture.test_root, false};
        auto non_recursive_result = scanner->process(non_recursive);

        // Recursive scan
        DirectoryScannerUtilityInput recursive{fixture.test_root, true};
        auto recursive_result = scanner->process(recursive);

        // Non-recursive should find less than recursive
        CHECK(non_recursive_result.size() < recursive_result.size());

        // Non-recursive: file1.txt, subdir/
        CHECK(non_recursive_result.size() == 2);

        // Recursive: file1.txt, subdir/, subdir/file2.txt, subdir/nested/,
        // subdir/nested/file3.txt
        CHECK(recursive_result.size() == 5);
    }
}

TEST_CASE("DirectoryScannerUtility - FileEntry Metadata") {
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("FileEntry contains correct metadata") {
        std::string content = "This is test content with some length";
        fixture.create_file("test_file.txt", content);

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

        REQUIRE(result.size() == 1);
        const auto& entry = result[0];

        CHECK(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size == content.size());
        CHECK(entry.path.filename().string() == "test_file.txt");
    }

    SUBCASE("Directory entries have zero size") {
        fixture.create_directory("test_dir");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

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

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

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
    TestDirectoryFixture fixture;

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
    TestDirectoryFixture fixture;
    auto scanner = std::make_shared<DirectoryScannerUtility>();

    SUBCASE("Empty files") {
        fixture.create_file("empty1.txt", "");
        fixture.create_file("empty2.txt", "");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 2);
        for (const auto& entry : result) {
            CHECK(entry.size == 0);
            CHECK(entry.is_regular_file);
        }
    }

    SUBCASE("Hidden files (Unix-style)") {
        fixture.create_file(".hidden_file");
        fixture.create_file("visible_file");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

        // Should find both files
        CHECK(result.size() == 2);
    }

    SUBCASE("Files with special characters in names") {
        fixture.create_file("file with spaces.txt");
        fixture.create_file("file-with-dashes.txt");
        fixture.create_file("file_with_underscores.txt");

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

        CHECK(result.size() == 3);
    }

    SUBCASE("Many files") {
        const int num_files = 100;
        for (int i = 0; i < num_files; ++i) {
            fixture.create_file("file" + std::to_string(i) + ".txt");
        }

        DirectoryScannerUtilityInput input{fixture.test_root, false};
        auto result = scanner->process(input);

        CHECK(result.size() == num_files);
    }
}

TEST_CASE("DirectoryScannerUtility - FileEntry Construction") {
    TestDirectoryFixture fixture;

    SUBCASE("FileEntry default constructor") {
        FileEntry entry;
        CHECK(entry.path.empty());
        CHECK(entry.size == 0);
        CHECK_FALSE(entry.is_directory);
        CHECK_FALSE(entry.is_regular_file);
    }

    SUBCASE("FileEntry with existing file") {
        fixture.create_file("test.txt", "content");
        fs::path file_path = fixture.get_path("test.txt");

        FileEntry entry{file_path};

        CHECK(entry.path == file_path);
        CHECK(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size > 0);
    }

    SUBCASE("FileEntry with existing directory") {
        fixture.create_directory("test_dir");
        fs::path dir_path = fixture.get_path("test_dir");

        FileEntry entry{dir_path};

        CHECK(entry.path == dir_path);
        CHECK_FALSE(entry.is_regular_file);
        CHECK(entry.is_directory);
        CHECK(entry.size == 0);
    }

    SUBCASE("FileEntry with nonexistent path") {
        fs::path nonexistent = fixture.get_path("nonexistent");

        FileEntry entry{nonexistent};

        CHECK(entry.path == nonexistent);
        CHECK_FALSE(entry.is_regular_file);
        CHECK_FALSE(entry.is_directory);
        CHECK(entry.size == 0);
    }
}
