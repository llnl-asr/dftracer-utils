#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/file_merger_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <yyjson.h>

#include <fstream>
#include <sstream>

using namespace dftracer::utils::utilities::composites;
using namespace dft_utils_test;

TEST_SUITE("FileMerger") {
    TEST_CASE("FileMergerUtility - Basic file merging") {
        TestEnvironment env(200);

        SUBCASE("Process single plain DFTracer file") {
            // Create test file with 10 valid events
            std::string test_file = env.create_dft_test_file(10);
            std::string temp_output = env.get_dir() + "/output_temp.json";

            // Create input
            auto input = FileMergeValidatorUtilityInput::from_file(test_file)
                             .with_output(temp_output);

            // Process
            FileMergeValidatorUtility merger;
            auto output = merger.process(input);

            // Verify
            CHECK(output.success == true);
            CHECK(output.file_path == test_file);
            CHECK(output.output_path == temp_output);
            CHECK(output.valid_events == 10);
            CHECK(output.lines_processed == 10);

            // Check output file exists
            CHECK(fs::exists(temp_output));

            // Verify JSON content
            std::ifstream ifs(temp_output);
            CHECK(ifs.good());
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
#pragma GCC diagnostic pop

            // Should have validated JSON events separated by commas
            CHECK(content.find("\"id\":") != std::string::npos);
            CHECK(content.find("\"pid\":") != std::string::npos);
            CHECK(content.find("\"args\":{\"ret\":") != std::string::npos);
        }

        SUBCASE("Process compressed DFTracer file") {
            // Create compressed test file
            std::string gz_file = env.create_dft_test_gzip_file(20);
            std::string temp_output = env.get_dir() + "/output_gz_temp.json";
            std::string index_path = env.get_dir() + "/test.idx";

            // Create input
            auto input = FileMergeValidatorUtilityInput::from_file(gz_file)
                             .with_output(temp_output)
                             .with_index(index_path)
                             .with_checkpoint_size(1024)
                             .with_force_rebuild(true);

            // Process
            FileMergeValidatorUtility merger;
            auto output = merger.process(input);

            // Verify
            CHECK(output.success == true);
            CHECK(output.valid_events == 20);
            CHECK(fs::exists(temp_output));
        }
    }

    TEST_CASE("FileCombinerUtility - Combine multiple files") {
        TestEnvironment env(300);

        // Create multiple test files and process them
        std::vector<FileMergeValidatorUtilityOutput> merge_results;

        for (int i = 0; i < 3; ++i) {
            std::string test_file = env.create_dft_test_file(5 + i * 2);
            std::string temp_output =
                env.get_dir() + "/temp_" + std::to_string(i) + ".json";

            auto input = FileMergeValidatorUtilityInput::from_file(test_file)
                             .with_output(temp_output);

            FileMergeValidatorUtility merger;
            auto output = merger.process(input);

            if (output.success) {
                merge_results.push_back(output);
            }
        }

        SUBCASE("Combine without compression") {
            std::string final_output = env.get_dir() + "/combined.pfw";

            // Create merger input
            FileMergerUtilityInput merger_input;
            merger_input.file_results = merge_results;
            merger_input.output_file = final_output;
            merger_input.compress = false;

            // Process
            FileMergerUtility merger;
            auto output = merger.process(merger_input);

            // Verify
            CHECK(output.success == true);
            CHECK(output.output_path == final_output);
            CHECK(output.files_combined == 3);
            CHECK(output.total_events == (5 + 7 + 9));  // 5, 7, 9 events

            // Check output file exists and has valid JSON array
            CHECK(fs::exists(final_output));

            // Read and validate Perfetto format ([ + NDJSON + ])
            std::ifstream ifs(final_output);
            std::string line;
            size_t event_count = 0;
            bool seen_open_bracket = false;
            bool seen_close_bracket = false;

            while (std::getline(ifs, line)) {
                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\n\r"));
                line.erase(line.find_last_not_of(" \t\n\r") + 1);

                // Skip empty lines
                if (line.empty()) {
                    continue;
                }

                // Check for opening bracket
                if (line == "[") {
                    CHECK(!seen_open_bracket);  // Should only see one opening
                                                // bracket
                    seen_open_bracket = true;
                    continue;
                }

                // Check for closing bracket
                if (line == "]") {
                    CHECK(!seen_close_bracket);  // Should only see one closing
                                                 // bracket
                    seen_close_bracket = true;
                    continue;
                }

                // Parse each JSON line
                yyjson_doc *doc = yyjson_read(line.c_str(), line.size(), 0);
                if (doc != nullptr) {
                    yyjson_val *root = yyjson_doc_get_root(doc);
                    if (yyjson_is_obj(root)) {
                        event_count++;
                    }
                    yyjson_doc_free(doc);
                }
            }
            ifs.close();

            CHECK(seen_open_bracket);   // Should have seen opening bracket
            CHECK(seen_close_bracket);  // Should have seen closing bracket
            // Use collected_events size as ground truth since it's what we
            // actually wrote
            CHECK(event_count == output.collected_events.size());
        }

        SUBCASE("Combine with compression") {
            std::string final_output =
                env.get_dir() + "/combined_compressed.pfw";

            // Create merger input
            FileMergerUtilityInput merger_input;
            merger_input.file_results = merge_results;
            merger_input.output_file = final_output;
            merger_input.compress = true;

            // Process
            FileMergerUtility merger;
            auto output = merger.process(merger_input);

            // Verify
            CHECK(output.success == true);
            CHECK(output.output_path == (final_output + ".gz"));
            CHECK(output.files_combined == 3);
            CHECK(fs::exists(output.output_path));
            CHECK(!fs::exists(final_output));  // Original should be removed
        }
    }

    TEST_CASE("FileMergerUtility - Error handling") {
        TestEnvironment env(400);

        SUBCASE("Non-existent file") {
            auto input = FileMergeValidatorUtilityInput::from_file(
                             "/non/existent/file.pfw")
                             .with_output(env.get_dir() + "/output.json");

            FileMergeValidatorUtility merger;
            auto output = merger.process(input);

            CHECK(output.success == false);
            CHECK(output.valid_events == 0);
        }

        SUBCASE("Invalid JSON in file") {
            std::string bad_file = env.get_dir() + "/bad.pfw";
            std::ofstream ofs(bad_file);
            ofs << "not valid json\n";
            ofs << "{broken json\n";
            ofs << R"({"valid":"json","id":1})" << "\n";
            ofs.close();

            auto input =
                FileMergeValidatorUtilityInput::from_file(bad_file).with_output(
                    env.get_dir() + "/output.json");

            FileMergeValidatorUtility merger;
            auto output = merger.process(input);

            // json_trim_and_validate is designed to be fast, not comprehensive
            // It only filters out obviously invalid cases (empty, single
            // brackets) All three lines pass the basic validation since they
            // have content > 8 chars
            CHECK(output.success == true);
            CHECK(output.valid_events ==
                  3);  // All 3 lines pass basic validation
            CHECK(output.lines_processed == 3);
        }
    }
}
