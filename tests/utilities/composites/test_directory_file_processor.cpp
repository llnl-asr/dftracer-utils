#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/directory_file_processor_utility.h>
#include <doctest/doctest.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::behaviors;

namespace tags = dftracer::utils::utilities::tags;

struct FileInfo {
    std::string path;
    std::size_t size;
    std::size_t line_count;

    FileInfo() : size(0), line_count(0) {}
    FileInfo(const std::string& p, std::size_t s, std::size_t l)
        : path(p), size(s), line_count(l) {}
};

TEST_SUITE("DirectoryFileProcessor") {
    TEST_CASE("Basic File Processing - txt files only") {
        std::string test_dir = "./test_dir_processor";
        fs::create_directory(test_dir);

        std::ofstream(test_dir + "/file1.txt") << "Line 1\nLine 2\nLine 3\n";
        std::ofstream(test_dir + "/file2.txt") << "Single line";
        std::ofstream(test_dir + "/file3.log") << "Log line 1\nLog line 2\n";
        std::ofstream(test_dir + "/readme.md") << "# Title\n\nContent\n";

        auto processor = [](CoroScope&, const std::string& path) -> FileInfo {
            std::ifstream file(path);
            std::size_t lines = 0;
            std::size_t size = fs::file_size(path);
            std::string line;
            while (std::getline(file, line)) {
                lines++;
            }
            return FileInfo(path, size, lines);
        };

        Runtime rt(4);
        DirectoryProcessInput input{test_dir, {".txt"}, false};

        BatchFileProcessOutput<FileInfo> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util =
                    std::make_shared<DirectoryFileProcessorUtility<FileInfo>>(
                        processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<FileInfo>,
                                tags::NeedsContext>
                    exec(util,
                         BehaviorChain<DirectoryProcessInput,
                                       BatchFileProcessOutput<FileInfo>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-txt").wait();

        CHECK(output.results.size() == 2);

        auto file1_result = std::find_if(
            output.results.begin(), output.results.end(),
            [](const FileInfo& info) {
                return info.path.find("file1.txt") != std::string::npos;
            });
        CHECK(file1_result != output.results.end());
        CHECK(file1_result->line_count == 3);

        auto file2_result = std::find_if(
            output.results.begin(), output.results.end(),
            [](const FileInfo& info) {
                return info.path.find("file2.txt") != std::string::npos;
            });
        CHECK(file2_result != output.results.end());
        CHECK(file2_result->line_count == 1);

        rt.shutdown();
        fs::remove_all(test_dir);
    }

    TEST_CASE("Multiple file extensions") {
        std::string test_dir = "./test_multi_ext";
        fs::create_directory(test_dir);

        std::ofstream(test_dir + "/doc.txt") << "text";
        std::ofstream(test_dir + "/script.py") << "print('hello')";
        std::ofstream(test_dir + "/data.json") << "{}";
        std::ofstream(test_dir + "/config.yaml") << "key: value";

        auto processor = [](CoroScope&,
                            const std::string& path) -> std::string {
            return fs::path(path).extension().string();
        };

        Runtime rt(2);
        DirectoryProcessInput input{test_dir, {".txt", ".json"}, false};

        BatchFileProcessOutput<std::string> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util = std::make_shared<
                    DirectoryFileProcessorUtility<std::string>>(processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<std::string>,
                                tags::NeedsContext>
                    exec(util,
                         BehaviorChain<DirectoryProcessInput,
                                       BatchFileProcessOutput<std::string>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-multi").wait();

        CHECK(output.results.size() == 2);
        CHECK(std::find(output.results.begin(), output.results.end(), ".txt") !=
              output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(),
                        ".json") != output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(), ".py") ==
              output.results.end());

        rt.shutdown();
        fs::remove_all(test_dir);
    }

    TEST_CASE("Recursive Processing") {
        std::string test_dir = "./test_recursive";
        fs::create_directories(test_dir + "/sub1");
        fs::create_directories(test_dir + "/sub2/nested");

        std::ofstream(test_dir + "/root.txt") << "root";
        std::ofstream(test_dir + "/sub1/file1.txt") << "sub1";
        std::ofstream(test_dir + "/sub2/file2.txt") << "sub2";
        std::ofstream(test_dir + "/sub2/nested/deep.txt") << "deep";

        auto test_dir_copy = test_dir;
        auto processor = [test_dir_copy](
                             CoroScope&,
                             const std::string& path) -> std::string {
            return fs::relative(path, test_dir_copy).string();
        };

        Runtime rt(4);
        DirectoryProcessInput input{test_dir, {".txt"}, true};

        BatchFileProcessOutput<std::string> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util = std::make_shared<
                    DirectoryFileProcessorUtility<std::string>>(processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<std::string>,
                                tags::NeedsContext>
                    exec(util,
                         BehaviorChain<DirectoryProcessInput,
                                       BatchFileProcessOutput<std::string>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-recursive").wait();

        CHECK(output.results.size() == 4);
        CHECK(std::find(output.results.begin(), output.results.end(),
                        "root.txt") != output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(),
                        "sub1/file1.txt") != output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(),
                        "sub2/file2.txt") != output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(),
                        "sub2/nested/deep.txt") != output.results.end());

        rt.shutdown();
        fs::remove_all(test_dir);
    }

    TEST_CASE("Empty directory") {
        std::string test_dir = "./test_empty";
        fs::create_directory(test_dir);

        auto processor = [](CoroScope&, const std::string&) -> int {
            return 1;
        };

        Runtime rt(1);
        DirectoryProcessInput input{test_dir, {".txt"}, false};

        BatchFileProcessOutput<int> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util =
                    std::make_shared<DirectoryFileProcessorUtility<int>>(
                        processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<int>, tags::NeedsContext>
                    exec(util, BehaviorChain<DirectoryProcessInput,
                                             BatchFileProcessOutput<int>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-empty").wait();

        CHECK(output.results.empty());

        rt.shutdown();
        fs::remove_all(test_dir);
    }

    TEST_CASE("No matching extensions") {
        std::string test_dir = "./test_no_match";
        fs::create_directory(test_dir);

        std::ofstream(test_dir + "/file.cpp") << "code";
        std::ofstream(test_dir + "/file.h") << "header";

        auto processor = [](CoroScope&,
                            const std::string& path) -> std::string {
            return fs::path(path).filename().string();
        };

        Runtime rt(2);
        DirectoryProcessInput input{test_dir, {".txt", ".md"}, false};

        BatchFileProcessOutput<std::string> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util = std::make_shared<
                    DirectoryFileProcessorUtility<std::string>>(processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<std::string>,
                                tags::NeedsContext>
                    exec(util,
                         BehaviorChain<DirectoryProcessInput,
                                       BatchFileProcessOutput<std::string>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-nomatch").wait();

        CHECK(output.results.empty());

        rt.shutdown();
        fs::remove_all(test_dir);
    }

    TEST_CASE("All extensions (empty filter)") {
        std::string test_dir = "./test_all_ext";
        fs::create_directory(test_dir);

        std::ofstream(test_dir + "/file1.txt") << "text";
        std::ofstream(test_dir + "/file2.cpp") << "code";
        std::ofstream(test_dir + "/file3.md") << "markdown";

        auto processor = [](CoroScope&,
                            const std::string& path) -> std::string {
            return fs::path(path).extension().string();
        };

        Runtime rt(3);
        DirectoryProcessInput input{test_dir, {}, false};

        BatchFileProcessOutput<std::string> output;
        auto* out_ptr = &output;

        auto task = run_coro_scope(
            rt.executor(),
            [processor, input,
             out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                auto util = std::make_shared<
                    DirectoryFileProcessorUtility<std::string>>(processor);
                UtilityExecutor<DirectoryProcessInput,
                                BatchFileProcessOutput<std::string>,
                                tags::NeedsContext>
                    exec(util,
                         BehaviorChain<DirectoryProcessInput,
                                       BatchFileProcessOutput<std::string>>{});
                *out_ptr = co_await exec.execute_with_context(scope, input);
            });

        rt.submit(std::move(task), "dir-process-all").wait();

        CHECK(output.results.size() == 3);
        CHECK(std::find(output.results.begin(), output.results.end(), ".txt") !=
              output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(), ".cpp") !=
              output.results.end());
        CHECK(std::find(output.results.begin(), output.results.end(), ".md") !=
              output.results.end());

        rt.shutdown();
        fs::remove_all(test_dir);
    }
}
