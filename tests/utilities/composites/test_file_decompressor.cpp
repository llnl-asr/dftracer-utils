#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/file_decompressor_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils::utilities::composites;

// Helper function to read file content
static std::string read_file_content(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

TEST_SUITE("FileDecompressor") {
    TEST_CASE("FileDecompressor - Basic Decompression") {
        SUBCASE("Decompress gzipped text file") {
            // Step 1: Create original file
            std::string original_file = "./test_original.txt";
            std::string compressed_file = original_file + ".gz";
            std::string decompressed_file =
                "./test_original.txt";  // Will be auto-generated

            std::string original_content =
                "This is test content.\n"
                "It has multiple lines.\n"
                "We will compress and decompress it.\n";

            std::ofstream ofs(original_file);
            ofs << original_content;
            ofs.close();

            // Step 2: Compress the file
            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            auto compress_result = compressor.process(compress_input);
            REQUIRE(compress_result.success == true);

            // Remove original to test decompression
            fs::remove(original_file);

            // Step 3: Decompress the file
            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto decompress_result = decompressor.process(decompress_input);

            // Check results
            CHECK(decompress_result.success == true);
            CHECK(decompress_result.input_path == compressed_file);
            CHECK(decompress_result.output_path == decompressed_file);
            CHECK(decompress_result.compressed_size ==
                  compress_result.compressed_size);
            CHECK(decompress_result.decompressed_size ==
                  original_content.size());
            CHECK(fs::exists(decompressed_file));

            // Verify content matches
            std::string decompressed_content =
                read_file_content(decompressed_file);
            CHECK(decompressed_content == original_content);

            // Clean up
            fs::remove(compressed_file);
            fs::remove(decompressed_file);
        }

        SUBCASE("Decompress with custom output path") {
            // Create and compress a file
            std::string original_file = "./source.txt";
            std::string compressed_file = original_file + ".gz";
            std::string custom_output = "./custom_decompressed.txt";

            std::ofstream ofs(original_file);
            ofs << "Custom output path test";
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            compressor.process(compress_input);
            fs::remove(original_file);

            // Decompress with custom output
            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file)
                    .with_output(custom_output);
            auto result = decompressor.process(decompress_input);

            CHECK(result.success == true);
            CHECK(result.output_path == custom_output);
            CHECK(fs::exists(custom_output));

            // Clean up
            fs::remove(compressed_file);
            fs::remove(custom_output);
        }
    }

    TEST_CASE("FileDecompressor - Round-trip Testing") {
        SUBCASE("Compress and decompress preserves content") {
            std::string original_file = "./roundtrip.txt";
            std::string compressed_file = original_file + ".gz";

            // Create test content with various patterns
            std::stringstream content;
            content << "Line with spaces    and    tabs\t\there\n";
            content << "Numbers: 123456789 0.123456789\n";
            content << "Special chars: !@#$%^&*()_+-=[]{}|;:',.<>?/\n";
            content << "Unicode: Hello 世界 🌍\n";
            content << "\n\n";  // Empty lines
            content << "Final line without newline";

            std::string original_content = content.str();

            std::ofstream ofs(original_file);
            ofs << original_content;
            ofs.close();

            // Compress
            FileCompressorUtility compressor;
            auto compress_result = compressor.process(
                FileCompressionUtilityInput::from_file(original_file));
            REQUIRE(compress_result.success == true);

            // Remove original
            fs::remove(original_file);

            // Decompress
            FileDecompressorUtility decompressor;
            auto decompress_result = decompressor.process(
                FileDecompressionUtilityInput::from_file(compressed_file));
            REQUIRE(decompress_result.success == true);

            // Verify content matches exactly
            std::string decompressed_content = read_file_content(original_file);
            CHECK(decompressed_content == original_content);
            CHECK(decompressed_content.size() == original_content.size());

            // Clean up
            fs::remove(compressed_file);
            fs::remove(original_file);
        }

        SUBCASE("Large file round-trip") {
            std::string original_file = "./large_roundtrip.txt";
            std::string compressed_file = original_file + ".gz";

            // Create large file
            std::ofstream ofs(original_file);
            std::string line =
                "This is a line that will be repeated many times to create a "
                "large file.\n";
            for (int i = 0; i < 10000; ++i) {
                ofs << i << ": " << line;
            }
            ofs.close();

            auto original_size = fs::file_size(original_file);

            // Compress with small chunks
            FileCompressorUtility compressor;
            auto compress_result = compressor.process(
                FileCompressionUtilityInput::from_file(original_file)
                    .with_chunk_size(1024));  // 1KB chunks
            REQUIRE(compress_result.success == true);

            fs::remove(original_file);

            // Decompress with different chunk size
            FileDecompressorUtility decompressor;
            auto decompress_result = decompressor.process(
                FileDecompressionUtilityInput::from_file(compressed_file)
                    .with_chunk_size(4096));  // 4KB chunks
            REQUIRE(decompress_result.success == true);

            // Verify sizes match
            CHECK(fs::file_size(original_file) == original_size);
            CHECK(decompress_result.decompressed_size == original_size);

            // Clean up
            fs::remove(compressed_file);
            fs::remove(original_file);
        }
    }

    TEST_CASE("FileDecompressor - Error Handling") {
        SUBCASE("Non-existent input file") {
            FileDecompressorUtility decompressor;

            auto input =
                FileDecompressionUtilityInput::from_file("./non_existent.gz");
            auto result = decompressor.process(input);

            CHECK(result.success == false);
            CHECK(result.error_message.find("does not exist") !=
                  std::string::npos);
        }

        SUBCASE("Corrupt compressed file") {
            std::string corrupt_file = "./corrupt.gz";

            // Create a file with invalid gzip data
            std::ofstream ofs(corrupt_file, std::ios::binary);
            ofs << "This is not valid gzip data!";
            ofs.close();

            FileDecompressorUtility decompressor;
            auto input = FileDecompressionUtilityInput::from_file(corrupt_file);
            auto result = decompressor.process(input);

            CHECK(result.success == false);
            bool has_decompression_error =
                result.error_message.find("Decompression failed") !=
                std::string::npos;
            bool has_header_error =
                result.error_message.find("incorrect header") !=
                std::string::npos;
            CHECK((has_decompression_error || has_header_error));

            // Clean up
            fs::remove(corrupt_file);
            if (fs::exists("./corrupt")) {
                fs::remove("./corrupt");
            }
        }

        SUBCASE("Empty compressed file") {
            // First create an empty file and compress it
            std::string empty_file = "./empty.txt";
            std::string compressed_file = empty_file + ".gz";

            std::ofstream ofs(empty_file);
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_result = compressor.process(
                FileCompressionUtilityInput::from_file(empty_file));
            REQUIRE(compress_result.success == true);

            fs::remove(empty_file);

            // Now decompress it
            FileDecompressorUtility decompressor;
            auto decompress_result = decompressor.process(
                FileDecompressionUtilityInput::from_file(compressed_file));

            CHECK(decompress_result.success == true);
            CHECK(decompress_result.decompressed_size == 0);
            CHECK(fs::exists(empty_file));
            CHECK(fs::file_size(empty_file) == 0);

            // Clean up
            fs::remove(compressed_file);
            fs::remove(empty_file);
        }
    }

    TEST_CASE("FileDecompressor - Binary Files") {
        SUBCASE("Decompress binary data") {
            std::string binary_file = "./binary.dat";
            std::string compressed_file = binary_file + ".gz";

            // Ensure cleanup happens even if test fails
            auto cleanup = [&]() {
                if (fs::exists(binary_file)) fs::remove(binary_file);
                if (fs::exists(compressed_file)) fs::remove(compressed_file);
            };
            std::shared_ptr<void> guard(nullptr, [&](void*) { cleanup(); });

            // Create binary file with specific pattern
            std::ofstream ofs(binary_file, std::ios::binary);
            std::vector<unsigned char> original_data;
            for (int i = 0; i < 256; ++i) {
                for (int j = 0; j < 10; ++j) {
                    original_data.push_back(static_cast<unsigned char>(i));
                }
            }
            ofs.write(reinterpret_cast<const char*>(original_data.data()),
                      original_data.size());
            ofs.close();

            // Compress
            FileCompressorUtility compressor;
            auto compress_result = compressor.process(
                FileCompressionUtilityInput::from_file(binary_file));
            INFO("Compression error: ", compress_result.error_message);
            REQUIRE(compress_result.success == true);

            fs::remove(binary_file);

            // Decompress
            FileDecompressorUtility decompressor;
            auto decompress_result = decompressor.process(
                FileDecompressionUtilityInput::from_file(compressed_file));
            INFO("Decompression error: ", decompress_result.error_message);
            REQUIRE(decompress_result.success == true);

            // Verify binary content
            std::ifstream ifs(binary_file, std::ios::binary);
            std::vector<unsigned char> decompressed_data(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());

            CHECK(decompressed_data.size() == original_data.size());
            CHECK(decompressed_data == original_data);
        }
    }
}
