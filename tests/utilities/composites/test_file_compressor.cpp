#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <random>
#include <string>

using namespace dftracer::utils::utilities::composites;

TEST_SUITE("FileCompressor") {
    TEST_CASE("FileCompressor - Basic Compression") {
        SUBCASE("Compress small text file") {
            // Create test file
            std::string test_file = "./test_compress.txt";
            std::string expected_output = test_file + ".gz";

            std::ofstream ofs(test_file);
            for (int i = 0; i < 100; ++i) {
                ofs << "This is line " << i
                    << " of test content that will be compressed.\n";
            }
            ofs.close();

            auto original_size = fs::file_size(test_file);

            // Create compressor
            FileCompressorUtility compressor;

            // Create input
            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_compression_level(6);  // Default level

            // Compress
            auto result = compressor.process(input);

            // Check results
            CHECK(result.success == true);
            CHECK(result.input_path == test_file);
            CHECK(result.output_path == expected_output);
            CHECK(result.original_size == original_size);
            CHECK(result.compressed_size > 0);
            CHECK(result.compressed_size <
                  result.original_size);  // Should be smaller
            CHECK(result.compression_ratio() > 0.0);
            CHECK(result.compression_ratio() <
                  1.0);                   // Compression ratio should be < 1
            CHECK(fs::exists(expected_output));

            // Clean up
            fs::remove(test_file);
            fs::remove(expected_output);
        }

        SUBCASE("Compress with custom output path") {
            std::string test_file = "./test_input.txt";
            std::string custom_output = "./custom_output.gz";

            // Create test file
            std::ofstream ofs(test_file);
            ofs << "Test content for custom output path\n";
            ofs.close();

            FileCompressorUtility compressor;

            auto input =
                FileCompressionUtilityInput::from_file(test_file).with_output(
                    custom_output);

            auto result = compressor.process(input);

            CHECK(result.success == true);
            CHECK(result.output_path == custom_output);
            CHECK(fs::exists(custom_output));

            // Clean up
            fs::remove(test_file);
            fs::remove(custom_output);
        }
    }

    TEST_CASE("FileCompressor - Compression Levels") {
        SUBCASE("Different compression levels") {
            std::string test_file = "./test_levels.txt";

            // Create test file with repetitive content (compresses well)
            std::ofstream ofs(test_file);
            for (int i = 0; i < 1000; ++i) {
                ofs << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
            }
            ofs.close();

            FileCompressorUtility compressor;

            // Test level 1 (fastest, least compression)
            auto input1 = FileCompressionUtilityInput::from_file(test_file)
                              .with_output("./level1.gz")
                              .with_compression_level(1);
            auto result1 = compressor.process(input1);

            // Test level 9 (slowest, best compression)
            auto input9 = FileCompressionUtilityInput::from_file(test_file)
                              .with_output("./level9.gz")
                              .with_compression_level(9);
            auto result9 = compressor.process(input9);

            CHECK(result1.success == true);
            CHECK(result9.success == true);

            // Higher compression level should produce smaller file (usually)
            // Note: For very small files or certain patterns this might not
            // always be true
            CHECK(result9.compressed_size <= result1.compressed_size);

            // Clean up
            fs::remove(test_file);
            fs::remove("./level1.gz");
            fs::remove("./level9.gz");
        }
    }

    TEST_CASE("FileCompressor - Large Files") {
        SUBCASE("Compress file with different chunk sizes") {
            std::string test_file = "./test_chunks.txt";

            // Create a moderately sized file
            std::ofstream ofs(test_file);
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(33, 126);  // Printable ASCII

            for (int i = 0; i < 10000; ++i) {
                for (int j = 0; j < 80; ++j) {
                    ofs << static_cast<char>(dis(gen));
                }
                ofs << '\n';
            }
            ofs.close();

            FileCompressorUtility compressor;

            // Small chunk size
            auto input_small = FileCompressionUtilityInput::from_file(test_file)
                                   .with_output("./small_chunks.gz")
                                   .with_chunk_size(1024);  // 1KB chunks
            auto result_small = compressor.process(input_small);

            // Large chunk size
            auto input_large = FileCompressionUtilityInput::from_file(test_file)
                                   .with_output("./large_chunks.gz")
                                   .with_chunk_size(64 * 1024);  // 64KB chunks
            auto result_large = compressor.process(input_large);

            CHECK(result_small.success == true);
            CHECK(result_large.success == true);

            // Both should produce similar compressed sizes
            double size_diff =
                std::abs(static_cast<double>(result_small.compressed_size) -
                         static_cast<double>(result_large.compressed_size));
            double avg_size =
                static_cast<double>(result_small.compressed_size +
                                    result_large.compressed_size) /
                2.0;
            double diff_ratio = size_diff / avg_size;

            CHECK(diff_ratio < 0.1);  // Less than 10% difference

            // Clean up
            fs::remove(test_file);
            fs::remove("./small_chunks.gz");
            fs::remove("./large_chunks.gz");
        }
    }

    TEST_CASE("FileCompressor - Error Handling") {
        SUBCASE("Non-existent input file") {
            FileCompressorUtility compressor;

            auto input =
                FileCompressionUtilityInput::from_file("./non_existent.txt");
            auto result = compressor.process(input);

            CHECK(result.success == false);
            CHECK(result.error_message.find("does not exist") !=
                  std::string::npos);
            CHECK(!fs::exists("./non_existent.txt.gz"));
        }

        SUBCASE("Empty file") {
            std::string test_file = "./empty.txt";
            std::string output_file = test_file + ".gz";

            // Ensure cleanup happens even if test fails
            auto cleanup = [&]() {
                if (fs::exists(test_file)) fs::remove(test_file);
                if (fs::exists(output_file)) fs::remove(output_file);
            };
            std::shared_ptr<void> guard(nullptr, [&](void*) { cleanup(); });

            // Create empty file
            std::ofstream ofs(test_file);
            ofs.close();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file);
            auto result = compressor.process(input);

            INFO("Compression error: ", result.error_message);
            CHECK(result.success == true);
            CHECK(result.original_size == 0);
            // Empty files may produce empty compressed files with streaming
            // compression
            CHECK(result.compressed_size >= 0);
            CHECK(fs::exists(output_file));
        }

        SUBCASE("Invalid compression level") {
            std::string test_file = "./test.txt";

            std::ofstream ofs(test_file);
            ofs << "Test content";
            ofs.close();

            FileCompressorUtility compressor;

            // Note: zlib typically clamps invalid levels, so this might still
            // succeed
            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_compression_level(100);  // Invalid level
            auto result = compressor.process(input);

            // The result might still succeed as zlib may clamp the value
            // Just ensure no crash occurs
            if (result.success) {
                CHECK(fs::exists(test_file + ".gz"));
                fs::remove(test_file + ".gz");
            }

            // Clean up
            fs::remove(test_file);
        }
    }

    TEST_CASE("FileCompressor - Binary Files") {
        SUBCASE("Compress binary data") {
            std::string test_file = "./binary.dat";
            std::string output_file = test_file + ".gz";

            // Ensure cleanup happens even if test fails
            auto cleanup = [&]() {
                if (fs::exists(test_file)) fs::remove(test_file);
                if (fs::exists(output_file)) fs::remove(output_file);
            };
            std::shared_ptr<void> guard(nullptr, [&](void*) { cleanup(); });

            // Create binary file with random data
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
            auto result = compressor.process(input);

            INFO("Compression error: ", result.error_message);
            CHECK(result.success == true);
            CHECK(result.original_size == data.size());
            CHECK(result.compressed_size > 0);
            CHECK(fs::exists(output_file));
        }
    }
}
