// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_compressor_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::fileio;

static dstd::Path create_test_dir() {
    auto dir =
        dftu_utils_test::make_unique_test_path("dftracer_test_compressor");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    return dir;
}

TEST_SUITE("FileCompressor") {
    TEST_CASE("FileCompressor - Basic Compression") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Compress small text file") {
            dstd::String test_file =
                dstd::String((test_dir / "test_compress.txt").str());
            dstd::String expected_output = dstd::fmt<"{}.gz">(test_file);

            dstd::String ofs;
            for (int i = 0; i < 100; ++i) {
                ofs += dstd::fmt<
                    "This is line {} of test content that will be "
                    "compressed.\n">(i);
            }
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            auto original_size = dstd::fs::file_size(test_file).value();

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
            CHECK(dstd::fs::exists(expected_output).value());
        }

        SUBCASE("File without a final newline") {
            dstd::String test_file =
                dstd::String((test_dir / "no_newline.txt").str());
            dstd::String content;
            for (int i = 0; i < 500; ++i) {
                content += dstd::fmt<"line {} of the file\n">(i);
            }
            content += "last line without newline";
            {
                dstd::String ofs;
                ofs += dstd::fmt<"{}">(content);
                dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();
            }

            FileCompressorUtility compressor;
            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_member_size(1000);
            auto result = compressor(input).get();
            REQUIRE(result.has_value());

            gzFile g = gzopen(result->output_path.c_str(), "rb");
            REQUIRE(g != nullptr);
            dstd::String decoded;
            char buf[4096];
            int n;
            while ((n = gzread(g, buf, sizeof(buf))) > 0)
                decoded.append(buf, n);
            gzclose(g);
            CHECK(decoded == content);
        }

        SUBCASE("Compress with custom output path") {
            dstd::String test_file =
                dstd::String((test_dir / "test_input.txt").str());
            dstd::String custom_output =
                dstd::String((test_dir / "custom_output.gz").str());

            dstd::String ofs;
            ofs += "Test content for custom output path\n";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            auto input =
                FileCompressionUtilityInput::from_file(test_file).with_output(
                    custom_output);

            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->output_path == custom_output);
            CHECK(dstd::fs::exists(custom_output).value());
        }
    }

    TEST_CASE("FileCompressor - Compression Levels") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Different compression levels") {
            dstd::String test_file =
                dstd::String((test_dir / "test_levels.txt").str());

            dstd::String ofs;
            for (int i = 0; i < 1000; ++i) {
                ofs += "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
            }
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            dstd::String level1_output =
                dstd::String((test_dir / "level1.gz").str());
            dstd::String level9_output =
                dstd::String((test_dir / "level9.gz").str());

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
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Compress file with different chunk sizes") {
            dstd::String test_file =
                dstd::String((test_dir / "test_chunks.txt").str());

            dstd::String ofs;
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(33, 126);

            for (int i = 0; i < 10000; ++i) {
                for (int j = 0; j < 80; ++j) {
                    ofs += dstd::fmt<"{}">(static_cast<char>(dis(gen)));
                }
                ofs += dstd::fmt<"\n">();
            }
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            dstd::String small_output =
                dstd::String((test_dir / "small_chunks.gz").str());
            dstd::String large_output =
                dstd::String((test_dir / "large_chunks.gz").str());

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
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Non-existent input file") {
            FileCompressorUtility compressor;

            dstd::String non_existent =
                dstd::String((test_dir / "non_existent.txt").str());
            auto input = FileCompressionUtilityInput::from_file(non_existent);
            auto result = compressor(input).get();

            REQUIRE(!result);
            CHECK(result.error().is_code(
                dftracer::utils::common::ErrorCode::NotFound));
            CHECK(result.error().message().find("does not exist") !=
                  dstd::String::npos);
            CHECK(!dstd::fs::exists(dstd::fmt<"{}.gz">(non_existent)).value());
        }

        SUBCASE("Empty file") {
            dstd::String test_file =
                dstd::String((test_dir / "empty.txt").str());
            dstd::String output_file = dstd::fmt<"{}.gz">(test_file);

            dstd::String ofs;
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file);
            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->original_size == 0);
            CHECK(result->compressed_size >= 0);
            CHECK(dstd::fs::exists(output_file).value());
        }

        SUBCASE("Invalid compression level") {
            dstd::String test_file =
                dstd::String((test_dir / "test.txt").str());

            dstd::String ofs;
            ofs += "Test content";
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file)
                             .with_compression_level(100);
            auto result = compressor(input).get();

            if (result.has_value()) {
                CHECK(dstd::fs::exists(dstd::fmt<"{}.gz">(test_file)).value());
            }
        }
    }

    TEST_CASE("FileCompressor - Binary Files") {
        auto test_dir = create_test_dir();
        auto cleanup_dir = dstd::SharedPtr<void>(nullptr, [&](void*) {
            dstd::fs::remove_all(test_dir).has_value();
        });

        SUBCASE("Compress binary data") {
            dstd::String test_file =
                dstd::String((test_dir / "binary.dat").str());
            dstd::String output_file = dstd::fmt<"{}.gz">(test_file);

            dstd::String ofs;
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);

            dstd::Vector<unsigned char> data(10000);
            for (auto& byte : data) {
                byte = static_cast<unsigned char>(dis(gen));
            }
            ofs.append(reinterpret_cast<const char*>(data.data()), data.size());
            dstd::fs::write_file(dstd::PathView(test_file), ofs).discard();

            FileCompressorUtility compressor;

            auto input = FileCompressionUtilityInput::from_file(test_file);
            auto result = compressor(input).get();

            REQUIRE(result.has_value());
            CHECK(result->original_size == data.size());
            CHECK(result->compressed_size > 0);
            CHECK(dstd::fs::exists(output_file).value());
        }
    }
}
