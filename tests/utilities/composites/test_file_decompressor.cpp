#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/file_decompressor_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils::utilities::composites;

static std::string read_file_content(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static fs::path create_test_dir() {
    auto dir = fs::temp_directory_path() / "dftracer_test_decompressor";
    fs::create_directories(dir);
    return dir;
}

TEST_SUITE("FileDecompressor") {
    TEST_CASE("FileDecompressor - Basic Decompression") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Decompress gzipped text file") {
            std::string original_file =
                (test_dir / "test_original.txt").string();
            std::string compressed_file = original_file + ".gz";
            std::string decompressed_file = original_file;

            std::string original_content =
                "This is test content.\n"
                "It has multiple lines.\n"
                "We will compress and decompress it.\n";

            std::ofstream ofs(original_file);
            ofs << original_content;
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            auto compress_result = compressor.process(compress_input).get();
            REQUIRE(compress_result.success == true);

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto decompress_result =
                decompressor.process(decompress_input).get();

            CHECK(decompress_result.success == true);
            CHECK(decompress_result.input_path == compressed_file);
            CHECK(decompress_result.output_path == decompressed_file);
            CHECK(decompress_result.compressed_size ==
                  compress_result.compressed_size);
            CHECK(decompress_result.decompressed_size ==
                  original_content.size());
            CHECK(fs::exists(decompressed_file));

            std::string decompressed_content =
                read_file_content(decompressed_file);
            CHECK(decompressed_content == original_content);
        }

        SUBCASE("Decompress with custom output path") {
            std::string original_file = (test_dir / "source.txt").string();
            std::string compressed_file = original_file + ".gz";
            std::string custom_output =
                (test_dir / "custom_decompressed.txt").string();

            std::ofstream ofs(original_file);
            ofs << "Custom output path test";
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            compressor.process(compress_input).get();
            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file)
                    .with_output(custom_output);
            auto result = decompressor.process(decompress_input).get();

            CHECK(result.success == true);
            CHECK(result.output_path == custom_output);
            CHECK(fs::exists(custom_output));
        }
    }

    TEST_CASE("FileDecompressor - Round-trip Testing") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Compress and decompress preserves content") {
            std::string original_file = (test_dir / "roundtrip.txt").string();
            std::string compressed_file = original_file + ".gz";

            std::stringstream content;
            content << "Line with spaces    and    tabs\t\there\n";
            content << "Numbers: 123456789 0.123456789\n";
            content << "Special chars: !@#$%^&*()_+-=[]{}|;:',.<>?/\n";
            content << "Unicode: Hello 世界 🌍\n";
            content << "\n\n";
            content << "Final line without newline";

            std::string original_content = content.str();

            std::ofstream ofs(original_file);
            ofs << original_content;
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_result =
                compressor
                    .process(
                        FileCompressionUtilityInput::from_file(original_file))
                    .get();
            REQUIRE(compress_result.success == true);

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();
            REQUIRE(decompress_result.success == true);

            std::string decompressed_content = read_file_content(original_file);
            CHECK(decompressed_content == original_content);
            CHECK(decompressed_content.size() == original_content.size());
        }

        SUBCASE("Large file round-trip") {
            std::string original_file =
                (test_dir / "large_roundtrip.txt").string();
            std::string compressed_file = original_file + ".gz";

            std::ofstream ofs(original_file);
            std::string line =
                "This is a line that will be repeated many times to create a "
                "large file.\n";
            for (int i = 0; i < 10000; ++i) {
                ofs << i << ": " << line;
            }
            ofs.close();

            auto original_size = fs::file_size(original_file);

            FileCompressorUtility compressor;
            auto compress_result =
                compressor
                    .process(
                        FileCompressionUtilityInput::from_file(original_file)
                            .with_chunk_size(1024))
                    .get();
            REQUIRE(compress_result.success == true);

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                                 compressed_file)
                                 .with_chunk_size(4096))
                    .get();
            REQUIRE(decompress_result.success == true);

            CHECK(fs::file_size(original_file) == original_size);
            CHECK(decompress_result.decompressed_size == original_size);
        }
    }

    TEST_CASE("FileDecompressor - Error Handling") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Non-existent input file") {
            FileDecompressorUtility decompressor;

            std::string non_existent = (test_dir / "non_existent.gz").string();
            auto input = FileDecompressionUtilityInput::from_file(non_existent);
            auto result = decompressor.process(input).get();

            CHECK(result.success == false);
            CHECK(result.error_message.find("does not exist") !=
                  std::string::npos);
        }

        SUBCASE("Corrupt compressed file") {
            std::string corrupt_file = (test_dir / "corrupt.gz").string();

            std::ofstream ofs(corrupt_file, std::ios::binary);
            ofs << "This is not valid gzip data!";
            ofs.close();

            FileDecompressorUtility decompressor;
            auto input = FileDecompressionUtilityInput::from_file(corrupt_file);
            auto result = decompressor.process(input).get();

            CHECK(result.success == false);
            bool has_decompression_error =
                result.error_message.find("Decompression failed") !=
                std::string::npos;
            bool has_header_error =
                result.error_message.find("incorrect header") !=
                std::string::npos;
            CHECK((has_decompression_error || has_header_error));
        }

        SUBCASE("Empty compressed file") {
            std::string empty_file = (test_dir / "empty.txt").string();
            std::string compressed_file = empty_file + ".gz";

            std::ofstream ofs(empty_file);
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_result =
                compressor
                    .process(FileCompressionUtilityInput::from_file(empty_file))
                    .get();
            REQUIRE(compress_result.success == true);

            fs::remove(empty_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();

            CHECK(decompress_result.success == true);
            CHECK(decompress_result.decompressed_size == 0);
            CHECK(fs::exists(empty_file));
            CHECK(fs::file_size(empty_file) == 0);
        }
    }

    TEST_CASE("FileDecompressor - Binary Files") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("Decompress binary data") {
            std::string binary_file = (test_dir / "binary.dat").string();
            std::string compressed_file = binary_file + ".gz";

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

            FileCompressorUtility compressor;
            auto compress_result =
                compressor
                    .process(
                        FileCompressionUtilityInput::from_file(binary_file))
                    .get();
            INFO("Compression error: ", compress_result.error_message);
            REQUIRE(compress_result.success == true);

            fs::remove(binary_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();
            INFO("Decompression error: ", decompress_result.error_message);
            REQUIRE(decompress_result.success == true);

            std::ifstream ifs(binary_file, std::ios::binary);
            std::vector<unsigned char> decompressed_data(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());

            CHECK(decompressed_data.size() == original_data.size());
            CHECK(decompressed_data == original_data);
        }
    }
}
