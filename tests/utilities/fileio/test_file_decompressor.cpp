// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/file_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/file_decompressor_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstddef>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;

static dstd::String read_file_content(const dstd::String& path) {
    return dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
}

static dstd::Path create_test_dir() {
    auto dir =
        dftu_utils_test::make_unique_test_path("dftracer_test_decompressor");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    return dir;
}

// Exercises DFTRACER_UTILS_CO_TRY: binds the decompression payload on success,
// or propagates the error to the caller. Proves the macro compiles and is
// frame-safe inside a coroutine that returns dstd::Result<...>.
static dftracer::utils::coro::CoroTask<
    dftracer::utils::dstd::Result<std::size_t>>
decompressed_size_via_dft_try(FileDecompressorUtility& decompressor,
                              const FileDecompressionUtilityInput& input) {
    DFTRACER_UTILS_CO_TRY(auto out, co_await decompressor(input));
    co_return out.decompressed_size;
}

TEST_SUITE("FileDecompressor") {
    TEST_CASE("FileDecompressor - Basic Decompression") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Decompress gzipped text file") {
            dstd::String original_file =
                dstd::String((test_dir / "test_original.txt").str());
            dstd::String compressed_file = original_file + ".gz";
            dstd::String decompressed_file = original_file;

            dstd::String original_content =
                "This is test content.\n"
                "It has multiple lines.\n"
                "We will compress and decompress it.\n";

            dstd::String ofs;
            ofs += common::str_cat(original_content);
            (void)dstd::fs::write_file(dstd::PathView(original_file), ofs);

            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            auto compress_result = compressor(compress_input).get();
            REQUIRE(compress_result.has_value());

            (void)dstd::fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto decompress_result = decompressor(decompress_input).get();

            REQUIRE(decompress_result.has_value());
            CHECK(decompress_result->input_path == compressed_file);
            CHECK(decompress_result->output_path == decompressed_file);
            CHECK(decompress_result->compressed_size ==
                  compress_result->compressed_size);
            CHECK(decompress_result->decompressed_size ==
                  original_content.size());
            CHECK(dstd::fs::exists(decompressed_file).value());

            dstd::String decompressed_content =
                read_file_content(decompressed_file);
            CHECK(decompressed_content == original_content);
        }

        SUBCASE("Decompress with custom output path") {
            dstd::String original_file =
                dstd::String((test_dir / "source.txt").str());
            dstd::String compressed_file = original_file + ".gz";
            dstd::String custom_output =
                dstd::String((test_dir / "custom_decompressed.txt").str());

            dstd::String ofs;
            ofs += "Custom output path test";
            (void)dstd::fs::write_file(dstd::PathView(original_file), ofs);

            FileCompressorUtility compressor;
            auto compress_input =
                FileCompressionUtilityInput::from_file(original_file);
            compressor(compress_input).get();
            (void)dstd::fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_input =
                FileDecompressionUtilityInput::from_file(compressed_file)
                    .with_output(custom_output);
            auto result = decompressor(decompress_input).get();

            REQUIRE(result.has_value());
            CHECK(result->output_path == custom_output);
            CHECK(dstd::fs::exists(custom_output).value());
        }
    }

    TEST_CASE("FileDecompressor - Round-trip Testing") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Compress and decompress preserves content") {
            dstd::String original_file =
                dstd::String((test_dir / "roundtrip.txt").str());
            dstd::String compressed_file = original_file + ".gz";

            dstd::String original_content;
            original_content += "Line with spaces    and    tabs\t\there\n";
            original_content += "Numbers: 123456789 0.123456789\n";
            original_content += "Special chars: !@#$%^&*()_+-=[]{}|;:',.<>?/\n";
            original_content += "Unicode: Hello 世界 🌍\n";
            original_content += "\n\n";
            original_content += "Final line without newline";

            dstd::String ofs;
            ofs += common::str_cat(original_content);
            (void)dstd::fs::write_file(dstd::PathView(original_file), ofs);

            FileCompressorUtility compressor;
            auto compress_result =
                compressor(
                    FileCompressionUtilityInput::from_file(original_file))
                    .get();
            REQUIRE(compress_result.has_value());

            (void)dstd::fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor(
                    FileDecompressionUtilityInput::from_file(compressed_file))
                    .get();
            REQUIRE(decompress_result.has_value());

            dstd::String decompressed_content =
                read_file_content(original_file);
            CHECK(decompressed_content == original_content);
            CHECK(decompressed_content.size() == original_content.size());
        }

        SUBCASE("Large file round-trip") {
            dstd::String original_file =
                dstd::String((test_dir / "large_roundtrip.txt").str());
            dstd::String compressed_file = original_file + ".gz";

            dstd::String ofs;
            dstd::String line =
                "This is a line that will be repeated many times to create a "
                "large file.\n";
            for (int i = 0; i < 10000; ++i) {
                ofs += common::str_cat(i, ": ", line);
            }
            (void)dstd::fs::write_file(dstd::PathView(original_file), ofs);

            auto original_size = dstd::fs::file_size(original_file).value();

            FileCompressorUtility compressor;
            auto compress_result =
                compressor(FileCompressionUtilityInput::from_file(original_file)
                               .with_member_size(1024))
                    .get();
            REQUIRE(compress_result.has_value());

            (void)dstd::fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor(
                    FileDecompressionUtilityInput::from_file(compressed_file))
                    .get();
            REQUIRE(decompress_result.has_value());

            CHECK(dstd::fs::file_size(original_file).value() == original_size);
            CHECK(decompress_result->decompressed_size == original_size);
        }
    }

    TEST_CASE("FileDecompressor - Error Handling") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Non-existent input file") {
            FileDecompressorUtility decompressor;

            dstd::String non_existent =
                dstd::String((test_dir / "non_existent.gz").str());
            auto input = FileDecompressionUtilityInput::from_file(non_existent);
            auto result = decompressor(input).get();

            REQUIRE(!result);
            CHECK(result.error().is_code(
                dftracer::utils::common::ErrorCode::NotFound));
            CHECK(result.error().message().find("does not exist") !=
                  dstd::String::npos);
        }

        SUBCASE("Corrupt compressed file") {
            dstd::String corrupt_file =
                dstd::String((test_dir / "corrupt.gz").str());

            dstd::String ofs;
            ofs += "This is not valid gzip data!";
            (void)dstd::fs::write_file(dstd::PathView(corrupt_file), ofs);

            FileDecompressorUtility decompressor;
            auto input = FileDecompressionUtilityInput::from_file(corrupt_file);
            auto result = decompressor(input).get();

            REQUIRE(!result);
            CHECK(result.error().is_code(
                dftracer::utils::common::ErrorCode::Compression));
            bool has_decompression_error =
                result.error().message().find("Decompression failed") !=
                dstd::String::npos;
            bool has_header_error =
                result.error().message().find("incorrect header") !=
                dstd::String::npos;
            CHECK((has_decompression_error || has_header_error));
        }

        SUBCASE("Empty compressed file") {
            dstd::String empty_file =
                dstd::String((test_dir / "empty.txt").str());
            dstd::String compressed_file = empty_file + ".gz";

            dstd::String ofs;
            (void)dstd::fs::write_file(dstd::PathView(empty_file), ofs);

            FileCompressorUtility compressor;
            auto compress_result =
                compressor(FileCompressionUtilityInput::from_file(empty_file))
                    .get();
            REQUIRE(compress_result.has_value());

            (void)dstd::fs::remove(empty_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor(
                    FileDecompressionUtilityInput::from_file(compressed_file))
                    .get();

            REQUIRE(decompress_result.has_value());
            CHECK(decompress_result->decompressed_size == 0);
            CHECK(dstd::fs::exists(empty_file).value());
            CHECK(dstd::fs::file_size(empty_file).value() == 0);
        }
    }

    TEST_CASE("FileDecompressor - Binary Files") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Decompress binary data") {
            dstd::String binary_file =
                dstd::String((test_dir / "binary.dat").str());
            dstd::String compressed_file = binary_file + ".gz";

            dstd::String ofs;
            dstd::Vector<unsigned char> original_data;
            for (int i = 0; i < 256; ++i) {
                for (int j = 0; j < 10; ++j) {
                    original_data.push_back(static_cast<unsigned char>(i));
                }
            }
            ofs.append(reinterpret_cast<const char*>(original_data.data()),
                       original_data.size());
            (void)dstd::fs::write_file(dstd::PathView(binary_file), ofs);

            FileCompressorUtility compressor;
            auto compress_result =
                compressor(FileCompressionUtilityInput::from_file(binary_file))
                    .get();
            REQUIRE(compress_result.has_value());

            (void)dstd::fs::remove(binary_file);

            FileDecompressorUtility decompressor;
            auto decompress_result =
                decompressor(
                    FileDecompressionUtilityInput::from_file(compressed_file))
                    .get();
            REQUIRE(decompress_result.has_value());

            const dstd::String decompressed_data_raw =
                dstd::fs::read_file(dstd::PathView(binary_file))
                    .value_or(dstd::String());
            dstd::Vector<unsigned char> decompressed_data(
                decompressed_data_raw.begin(), decompressed_data_raw.end());

            CHECK(decompressed_data.size() == original_data.size());
            CHECK(decompressed_data == original_data);
        }
    }

    TEST_CASE("FileDecompressor - DFTRACER_UTILS_CO_TRY propagation") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("DFTRACER_UTILS_CO_TRY binds value on success") {
            dstd::String original_file =
                dstd::String((test_dir / "dftu_try.txt").str());
            dstd::String compressed_file = original_file + ".gz";
            dstd::String original_content =
                "DFTRACER_UTILS_CO_TRY happy path content\n";

            (void)dstd::fs::write_file(dstd::PathView(original_file),
                                       original_content);

            FileCompressorUtility compressor;
            auto compress_result =
                compressor(
                    FileCompressionUtilityInput::from_file(original_file))
                    .get();
            REQUIRE(compress_result.has_value());
            (void)dstd::fs::remove(original_file);

            FileDecompressorUtility decompressor;
            auto input =
                FileDecompressionUtilityInput::from_file(compressed_file);
            auto r = decompressed_size_via_dft_try(decompressor, input).get();

            REQUIRE(r.has_value());
            CHECK(*r == original_content.size());
        }

        SUBCASE("DFTRACER_UTILS_CO_TRY propagates error on bad input") {
            dstd::String non_existent =
                dstd::String((test_dir / "missing.gz").str());
            FileDecompressorUtility decompressor;
            auto input = FileDecompressionUtilityInput::from_file(non_existent);
            auto r = decompressed_size_via_dft_try(decompressor, input).get();

            REQUIRE(!r);
            CHECK(r.error().is_code(
                dftracer::utils::common::ErrorCode::NotFound));
        }
    }
}
