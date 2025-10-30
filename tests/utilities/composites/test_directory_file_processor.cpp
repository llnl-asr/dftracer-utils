#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/utilities/composites/directory_file_processor_utility.h>
#include <doctest/doctest.h>

#include <any>
#include <chrono>
#include <fstream>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;

// Test data structure for file processing results
struct FileInfo {
    std::string path;
    std::size_t size;
    std::size_t line_count;

    FileInfo() : size(0), line_count(0) {}
    FileInfo(const std::string& p, std::size_t s, std::size_t l)
        : path(p), size(s), line_count(l) {}
};

TEST_SUITE("DirectoryFileProcessor") {
    TEST_CASE("DirectoryFileProcessor - Basic File Processing") {
        SUBCASE("Process all text files in directory") {
            // Create test directory with files
            std::string test_dir = "./test_dir_processor";
            fs::create_directory(test_dir);

            // Create test files
            std::ofstream f1(test_dir + "/file1.txt");
            f1 << "Line 1\nLine 2\nLine 3\n";
            f1.close();

            std::ofstream f2(test_dir + "/file2.txt");
            f2 << "Single line";
            f2.close();

            std::ofstream f3(test_dir + "/file3.log");
            f3 << "Log line 1\nLog line 2\n";
            f3.close();

            std::ofstream f4(test_dir + "/readme.md");
            f4 << "# Title\n\nContent\n";
            f4.close();

            // Create processor that counts lines in files
            auto processor = [](TaskContext& ctx,
                                const std::string& path) -> FileInfo {
                (void)ctx;  // Not used in this test

                std::ifstream file(path);
                std::size_t lines = 0;
                std::size_t size = fs::file_size(path);
                std::string line;

                while (std::getline(file, line)) {
                    lines++;
                }

                return FileInfo(path, size, lines);
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<FileInfo>>(
                    processor);

            // Set up executor and scheduler
            Executor executor(4);
            Scheduler scheduler(&executor);

            // Create input for .txt files only
            DirectoryProcessInput input{
                test_dir,
                {".txt"},  // Only .txt files
                false      // Not recursive
            };

            // Use adapter to convert to task
            auto process_task = use(dir_processor).as_task();

            // Schedule the task
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)
            auto output = process_task->get<BatchFileProcessOutput<FileInfo>>();

            CHECK(output.results.size() == 2);  // Only .txt files

            // Check file1.txt
            auto file1_result = std::find_if(
                output.results.begin(), output.results.end(),
                [](const FileInfo& info) {
                    return info.path.find("file1.txt") != std::string::npos;
                });
            CHECK(file1_result != output.results.end());
            CHECK(file1_result->line_count == 3);

            // Check file2.txt
            auto file2_result = std::find_if(
                output.results.begin(), output.results.end(),
                [](const FileInfo& info) {
                    return info.path.find("file2.txt") != std::string::npos;
                });
            CHECK(file2_result != output.results.end());
            CHECK(file2_result->line_count == 1);

            // Explicit shutdown before scope exit to ensure clean destruction
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }

        SUBCASE("Process multiple file extensions") {
            std::string test_dir = "./test_multi_ext";
            fs::create_directory(test_dir);

            // Create various files
            std::ofstream(test_dir + "/doc.txt") << "text";
            std::ofstream(test_dir + "/script.py") << "print('hello')";
            std::ofstream(test_dir + "/data.json") << "{}";
            std::ofstream(test_dir + "/config.yaml") << "key: value";

            // Processor that just returns file extension
            auto processor = [](TaskContext& ctx,
                                const std::string& path) -> std::string {
                (void)ctx;
                return fs::path(path).extension().string();
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<std::string>>(
                    processor);

            Executor executor(2);
            Scheduler scheduler(&executor);

            DirectoryProcessInput input{
                test_dir,
                {".txt", ".json"},  // Only these extensions
                false};

            auto process_task = use(dir_processor).as_task();
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)

            auto output =
                process_task->get<BatchFileProcessOutput<std::string>>();

            CHECK(output.results.size() == 2);
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".txt") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".json") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".py") == output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".yaml") == output.results.end());

            // Explicit shutdown before scope exit
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }
    }

    TEST_CASE("DirectoryFileProcessor - Recursive Processing") {
        SUBCASE("Process files recursively in subdirectories") {
            std::string test_dir = "./test_recursive";
            fs::create_directories(test_dir + "/sub1");
            fs::create_directories(test_dir + "/sub2/nested");

            // Create files at different levels
            std::ofstream(test_dir + "/root.txt") << "root";
            std::ofstream(test_dir + "/sub1/file1.txt") << "sub1";
            std::ofstream(test_dir + "/sub2/file2.txt") << "sub2";
            std::ofstream(test_dir + "/sub2/nested/deep.txt") << "deep";

            // Processor that returns relative path from test_dir
            auto processor = [test_dir](
                                 TaskContext& ctx,
                                 const std::string& path) -> std::string {
                (void)ctx;
                return fs::relative(path, test_dir).string();
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<std::string>>(
                    processor);

            Executor executor(4);
            Scheduler scheduler(&executor);

            DirectoryProcessInput input{
                test_dir,
                {".txt"},
                true  // Recursive
            };

            auto process_task = use(dir_processor).as_task();
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)

            auto output =
                process_task->get<BatchFileProcessOutput<std::string>>();

            CHECK(output.results.size() == 4);
            CHECK(std::find(output.results.begin(), output.results.end(),
                            "root.txt") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            "sub1/file1.txt") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            "sub2/file2.txt") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            "sub2/nested/deep.txt") != output.results.end());

            // Explicit shutdown before scope exit
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }
    }

    TEST_CASE("DirectoryFileProcessor - Empty and Edge Cases") {
        SUBCASE("Empty directory") {
            std::string test_dir = "./test_empty";
            fs::create_directory(test_dir);

            auto processor = [](TaskContext& ctx,
                                const std::string& path) -> int {
                (void)ctx;
                (void)path;
                return 1;
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<int>>(processor);

            Executor executor(1);
            Scheduler scheduler(&executor);

            DirectoryProcessInput input{test_dir, {".txt"}, false};

            auto process_task = use(dir_processor).as_task();
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)
            auto output = process_task->get<BatchFileProcessOutput<int>>();

            CHECK(output.results.empty());

            // Explicit shutdown before scope exit
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }

        SUBCASE("No matching extensions") {
            std::string test_dir = "./test_no_match";
            fs::create_directory(test_dir);

            std::ofstream(test_dir + "/file.cpp") << "code";
            std::ofstream(test_dir + "/file.h") << "header";

            auto processor = [](TaskContext& ctx,
                                const std::string& path) -> std::string {
                (void)ctx;
                return fs::path(path).filename().string();
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<std::string>>(
                    processor);

            Executor executor(2);
            Scheduler scheduler(&executor);

            DirectoryProcessInput input{
                test_dir,
                {".txt", ".md"},  // No files with these extensions
                false};

            auto process_task = use(dir_processor).as_task();
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)
            auto output =
                process_task->get<BatchFileProcessOutput<std::string>>();

            CHECK(output.results.empty());

            // Explicit shutdown before scope exit
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }

        SUBCASE("All extensions (empty filter)") {
            std::string test_dir = "./test_all_ext";
            fs::create_directory(test_dir);

            std::ofstream(test_dir + "/file1.txt") << "text";
            std::ofstream(test_dir + "/file2.cpp") << "code";
            std::ofstream(test_dir + "/file3.md") << "markdown";

            auto processor = [](TaskContext& ctx,
                                const std::string& path) -> std::string {
                (void)ctx;
                return fs::path(path).extension().string();
            };

            auto dir_processor =
                std::make_shared<DirectoryFileProcessorUtility<std::string>>(
                    processor);

            Executor executor(3);
            Scheduler scheduler(&executor);

            DirectoryProcessInput input{test_dir,
                                        {},  // Empty = all files
                                        false};

            auto process_task = use(dir_processor).as_task();
            scheduler.schedule(process_task, input);

            // Get results (blocks until task completes)

            auto output =
                process_task->get<BatchFileProcessOutput<std::string>>();

            CHECK(output.results.size() == 3);
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".txt") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".cpp") != output.results.end());
            CHECK(std::find(output.results.begin(), output.results.end(),
                            ".md") != output.results.end());

            // Explicit shutdown before scope exit
            executor.shutdown();

            // Clean up
            fs::remove_all(test_dir);
        }
    }
}
