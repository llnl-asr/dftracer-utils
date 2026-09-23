#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/fileio/file_compressor_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <memory>
#include <random>
#include <string>

using namespace dftracer::utils::utilities::fileio;

static fs::path create_test_dir() {
    auto dir =
        dftu_utils_test::make_unique_test_path("dftracer_test_compressor");
    fs::create_directories(dir);
    return dir;
}

TEST_SUITE("FileCompressor") {
    TEST_CASE("FileCompressor - Basic Compression") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Compress small text file") {
            std::string test_file = (test_dir / "test_compress.txt").string();
            std::string expected_output = test_file + ".gz";

            std::ofstream ofs(test_file);
            for (int i = 0; i < 100; ++i) {
                ofs << "This is line " << i
                    << " of test content that will be compressed.\n";
            }
            ofs.close();

            auto original_size = fs::file_size(test_file);

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_compression_level(6);

            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->input_path == test_file);
            CHECK(result->output_path == expected_output);
            CHECK(result->original_size == original_size);
            CHECK(result->compressed_size > 0);
            CHECK(result->compressed_size < result->original_size);
            CHECK(result->compression_ratio() > 0.0);
            CHECK(result->compression_ratio() < 1.0);
            CHECK(fs::exists(expected_output));
        }

        SUBCASE("Compress with custom output path") {
            std::string test_file = (test_dir / "test_input.txt").string();
            std::string custom_output =
                (test_dir / "custom_output.gz").string();

            std::ofstream ofs(test_file);
            ofs << "Test content for custom output path\n";
            ofs.close();

            FileCompressorUtility compressor;

            auto input =
                FileCompressionUtilityInput::from_file(test_file).with_output(
                    custom_output);

            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->output_path == custom_output);
            CHECK(fs::exists(custom_output));
        }
    }

    TEST_CASE("FileCompressor - Compression Levels") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Different compression levels") {
            std::string test_file = (test_dir / "test_levels.txt").string();

            std::ofstream ofs(test_file);
            for (int i = 0; i < 1000; ++i) {
                ofs << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
            }
            ofs.close();

            FileCompressorUtility compressor;

            std::string level1_output = (test_dir / "level1.gz").string();
            std::string level9_output = (test_dir / "level9.gz").string();

            auto input1 = FileCompressionUtilityInput::from_file(test_file)
                              .with_output(level1_output)
                              .with_compression_level(1);
            auto result1 = compressor(input1).get();

            auto input9 = FileCompressionUtilityInput::from_file(test_file)
                              .with_output(level9_output)
                              .with_compression_level(9);
            auto result9 = compressor(input9).get();

            REQUIRE(result1.has_value());
            REQUIRE(result9.has_value());
            CHECK(result9->compressed_size <= result1->compressed_size);
        }
    }

    TEST_CASE("FileCompressor - Large Files") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Compress file with different chunk sizes") {
            std::string test_file = (test_dir / "test_chunks.txt").string();

            std::ofstream ofs(test_file);
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(33, 126);

            for (int i = 0; i < 10000; ++i) {
                for (int j = 0; j < 80; ++j) {
                    ofs << static_cast<char>(dis(gen));
                }
                ofs << '\n';
            }
            ofs.close();

            FileCompressorUtility compressor;

            std::string small_output = (test_dir / "small_chunks.gz").string();
            std::string large_output = (test_dir / "large_chunks.gz").string();

            auto input_small = FileCompressionUtilityInput::from_file(test_file)
                                   .with_output(small_output)
                                   .with_member_size(1024);
            auto result_small = compressor(input_small).get();

            auto input_large = FileCompressionUtilityInput::from_file(test_file)
                                   .with_output(large_output)
                                   .with_member_size(64 * 1024);
            auto result_large = compressor(input_large).get();

            REQUIRE(result_small.has_value());
            REQUIRE(result_large.has_value());

            double size_diff =
                std::abs(static_cast<double>(result_small->compressed_size) -
                         static_cast<double>(result_large->compressed_size));
            double avg_size =
                static_cast<double>(result_small->compressed_size +
                                    result_large->compressed_size) /
                2.0;
            double diff_ratio = size_diff / avg_size;

            CHECK(diff_ratio < 0.1);
        }
    }

    TEST_CASE("FileCompressor - Error Handling") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Non-existent input file") {
            FileCompressorUtility compressor;

            std::string non_existent = (test_dir / "non_existent.txt").string();
            auto input = FileCompressionUtilityInput::from_file(non_existent);
            auto result = compressor(input).get();

            REQUIRE(!result);
            CHECK(result.error().code == dftracer::utils::ErrorCode::NOT_FOUND);
            CHECK(result.error().message.find("does not exist") !=
                  std::string::npos);
            CHECK(!fs::exists(non_existent + ".gz"));
        }

        SUBCASE("Empty file") {
            std::string test_file = (test_dir / "empty.txt").string();
            std::string output_file = test_file + ".gz";

            std::ofstream ofs(test_file);
            ofs.close();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file);
            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->original_size == 0);
            CHECK(result->compressed_size >= 0);
            CHECK(fs::exists(output_file));
        }

        SUBCASE("Invalid compression level") {
            std::string test_file = (test_dir / "test.txt").string();

            std::ofstream ofs(test_file);
            ofs << "Test content";
            ofs.close();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_compression_level(100);
            auto result = compressor(input).get();

            if (result.has_value()) {
                CHECK(fs::exists(test_file + ".gz"));
            }
        }
    }

    TEST_CASE("FileCompressor - Binary Files") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Compress binary data") {
            std::string test_file = (test_dir / "binary.dat").string();
            std::string output_file = test_file + ".gz";

            std::ofstream ofs(test_file, std::ios::binary);
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);

            std::vector<unsigned char> data(10000);
            for (auto& byte : data) {
                byte = static_cast<unsigned char>(dis(gen));
            }
            ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
            ofs.close();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file);
            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->original_size == data.size());
            CHECK(result->compressed_size > 0);
            CHECK(fs::exists(output_file));
        }
    }
}
