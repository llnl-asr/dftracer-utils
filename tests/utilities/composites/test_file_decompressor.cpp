#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/file_decompressor_utility.h>
#include <doctest/doctest.h>

#include <cstddef>
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

// Exercises DFT_TRY: binds the decompression payload on success, or propagates
// the error to the caller. Proves the macro compiles and is frame-safe inside a
// coroutine that returns Result<...>.
static dftracer::utils::coro::CoroTask<dftracer::utils::Result<std::size_t>>
decompressed_size_via_dft_try(FileDecompressorUtility& decompressor,
                              const FileDecompressionUtilityInput& input) {
    DFT_TRY(auto out, co_await decompressor.process(input));
    co_return out.decompressed_size;
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
            REQUIRE(compress_result.has_value());

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto decompress_result =
                decompressor.process(decompress_input).get();

            REQUIRE(decompress_result.has_value());
            CHECK(decompress_result->input_path == compressed_file);
            CHECK(decompress_result->output_path == decompressed_file);
            CHECK(decompress_result->compressed_size ==
                  compress_result->compressed_size);
            CHECK(decompress_result->decompressed_size ==
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

            REQUIRE(result.has_value());
            CHECK(result->output_path == custom_output);
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
            REQUIRE(compress_result.has_value());

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();
            REQUIRE(decompress_result.has_value());

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
            REQUIRE(compress_result.has_value());

            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                                 compressed_file)
                                 .with_chunk_size(4096))
                    .get();
            REQUIRE(decompress_result.has_value());

            CHECK(fs::file_size(original_file) == original_size);
            CHECK(decompress_result->decompressed_size == original_size);
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

            REQUIRE(!result);
            CHECK(result.error().code == dftracer::utils::ErrorCode::NOT_FOUND);
            CHECK(result.error().message.find("does not exist") !=
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

            REQUIRE(!result);
            CHECK(result.error().code ==
                  dftracer::utils::ErrorCode::COMPRESSION);
            bool has_decompression_error =
                result.error().message.find("Decompression failed") !=
                std::string::npos;
            bool has_header_error =
                result.error().message.find("incorrect header") !=
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
            REQUIRE(compress_result.has_value());

            fs::remove(empty_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();

            REQUIRE(decompress_result.has_value());
            CHECK(decompress_result->decompressed_size == 0);
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
            REQUIRE(compress_result.has_value());

            fs::remove(binary_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor
                    .process(FileDecompressionUtilityInput::from_file(
                        compressed_file))
                    .get();
            REQUIRE(decompress_result.has_value());

            std::ifstream ifs(binary_file, std::ios::binary);
            std::vector<unsigned char> decompressed_data(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());

            CHECK(decompressed_data.size() == original_data.size());
            CHECK(decompressed_data == original_data);
        }
    }

    TEST_CASE("FileDecompressor - DFT_TRY propagation") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = std::shared_ptr<void>(
            nullptr, [&](void*) { fs::remove_all(test_dir); });

        SUBCASE("DFT_TRY binds value on success") {
            std::string original_file = (test_dir / "dft_try.txt").string();
            std::string compressed_file = original_file + ".gz";
            std::string original_content = "DFT_TRY happy path content\n";

            std::ofstream ofs(original_file);
            ofs << original_content;
            ofs.close();

            FileCompressorUtility compressor;
            auto compress_result =
                compressor
                    .process(
                        FileCompressionUtilityInput::from_file(original_file))
                    .get();
            REQUIRE(compress_result.has_value());
            fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto r = decompressed_size_via_dft_try(decompressor, input).get();

            REQUIRE(r.has_value());
            CHECK(*r == original_content.size());
        }

        SUBCASE("DFT_TRY propagates error on bad input") {
            std::string non_existent = (test_dir / "missing.gz").string();
            FileDecompressorUtility decompressor;
            auto input = FileDecompressionUtilityInput::from_file(non_existent);
            auto r = decompressed_size_via_dft_try(decompressor, input).get();

            REQUIRE(!r);
            CHECK(r.error().code == dftracer::utils::ErrorCode::NOT_FOUND);
        }
    }
}
