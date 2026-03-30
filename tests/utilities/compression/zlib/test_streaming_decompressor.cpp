#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils;
using namespace dftracer::utils::coro;

// Helper to collect all ByteView chunks from an AsyncGenerator into a single
// owned vector. Each ByteView is only valid until the next iteration, so we
// must copy into owned storage.
static std::vector<unsigned char> drain_generator(
    AsyncGenerator<ByteView> gen) {
    std::vector<unsigned char> result;
    [&]() -> CoroTask<void> {
        while (auto chunk = co_await gen.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
    }()
                 .get();
    return result;
}

// Helper function to compress a string using ManualStreamingCompressorUtility,
// returning the compressed bytes as an owned vector.
static std::vector<unsigned char> compress_with_streaming(
    const std::string& text, int level = Z_DEFAULT_COMPRESSION,
    CompressionFormat format = CompressionFormat::ZLIB) {
    ManualStreamingCompressorUtility compressor(level, format);

    auto compressed = drain_generator(compressor.compress(ByteView(text)));
    auto final_bytes = drain_generator(compressor.finalize_stream());

    compressed.insert(compressed.end(), final_bytes.begin(), final_bytes.end());
    return compressed;
}

// Helper function to compress binary data.
static std::vector<unsigned char> compress_binary_with_streaming(
    const std::vector<unsigned char>& data, int level = Z_DEFAULT_COMPRESSION,
    CompressionFormat format = CompressionFormat::ZLIB) {
    ManualStreamingCompressorUtility compressor(level, format);

    auto compressed = drain_generator(compressor.compress(ByteView(data)));
    auto final_bytes = drain_generator(compressor.finalize_stream());

    compressed.insert(compressed.end(), final_bytes.begin(), final_bytes.end());
    return compressed;
}

TEST_CASE("StreamingDecompressorUtility - Basic Operations") {
    SUBCASE("Decompress compressed data") {
        std::string text = "Hello, World! This is a test message.";
        auto compressed = compress_with_streaming(text, Z_DEFAULT_COMPRESSION,
                                                  CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == text);
        CHECK(decompressor.total_bytes_out() == text.size());
    }

    SUBCASE("Decompress empty chunk") {
        StreamingDecompressorUtility decompressor;

        auto decompressed =
            drain_generator(decompressor.decompress(ByteView()));

        CHECK(decompressed.empty());
    }
}

TEST_CASE("StreamingDecompressorUtility - Round Trip") {
    SUBCASE("Simple text") {
        std::string original = "The quick brown fox jumps over the lazy dog.";

        auto compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == original);
    }

    SUBCASE("Repetitive data") {
        std::string original(10000, 'a');

        auto compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == original);
        CHECK(result.size() == 10000);
    }

    SUBCASE("Binary data") {
        std::vector<unsigned char> original = {0x00, 0x01, 0x02, 0xFF, 0xFE,
                                               0xFD, 0x00, 0x01, 0x02};

        auto compressed = compress_binary_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressed == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Different Data Sizes") {
    SUBCASE("Very small data") {
        std::string text = "x";
        auto compressed = compress_with_streaming(text, Z_DEFAULT_COMPRESSION,
                                                  CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == text);
    }

    SUBCASE("Medium data") {
        std::string text(1024, 'a');  // 1KB
        auto compressed = compress_with_streaming(text, Z_DEFAULT_COMPRESSION,
                                                  CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressed.size() == 1024);
    }

    SUBCASE("Large data") {
        std::string text(100000, 'b');  // 100KB
        auto compressed = compress_with_streaming(text, Z_DEFAULT_COMPRESSION,
                                                  CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressed.size() == 100000);
    }
}

TEST_CASE("StreamingDecompressorUtility - Different Compression Levels") {
    std::string original(1000, 'x');

    SUBCASE("Level 1") {
        auto compressed =
            compress_with_streaming(original, 1, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == original);
    }

    SUBCASE("Level 9") {
        auto compressed =
            compress_with_streaming(original, 9, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Error Handling") {
    StreamingDecompressorUtility decompressor;

    SUBCASE("Invalid compressed data") {
        std::vector<unsigned char> invalid = {0x00, 0x01, 0x02, 0x03};

        CHECK_THROWS_AS(
            drain_generator(decompressor.decompress(ByteView(invalid))),
            std::runtime_error);
    }
}

TEST_CASE("StreamingDecompressorUtility - Metadata") {
    SUBCASE("Byte counters") {
        std::string original = "Test data for byte counting";

        auto compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressor.total_bytes_in() == compressed.size());
        CHECK(decompressor.total_bytes_out() == original.size());
    }
}

TEST_CASE("StreamingDecompressorUtility - Real World Scenarios") {
    SUBCASE("Log file data") {
        std::string log_data;
        for (int i = 0; i < 100; ++i) {
            log_data += "[2024-01-01 12:00:00] INFO: Log entry " +
                        std::to_string(i) + "\n";
        }

        auto compressed = compress_with_streaming(
            log_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == log_data);
    }

    SUBCASE("CSV data") {
        std::string csv_data = "id,name,value\n";
        for (int i = 0; i < 1000; ++i) {
            csv_data += std::to_string(i) + ",item" + std::to_string(i) + "," +
                        std::to_string(i * 100) + "\n";
        }

        auto compressed = compress_with_streaming(
            csv_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == csv_data);
    }

    SUBCASE("JSON data") {
        std::string json_data = R"({
            "name": "test",
            "values": [1, 2, 3, 4, 5],
            "nested": {
                "key": "value"
            }
        })";

        auto compressed = compress_with_streaming(
            json_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == json_data);
    }
}

TEST_CASE("StreamingDecompressorUtility - Edge Cases") {
    SUBCASE("All zeros") {
        std::vector<unsigned char> zeros(10000, 0);

        auto compressed = compress_binary_with_streaming(
            zeros, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressed == zeros);
    }

    SUBCASE("Single byte") {
        std::string original = "x";

        auto compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Large Data") {
    SUBCASE("Large repetitive data") {
        // 1MB of repetitive data
        std::string original(1024 * 1024, 'z');

        auto compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        CHECK(decompressed.size() == original.size());
    }
}

TEST_CASE("StreamingDecompressorUtility - Multiple Formats") {
    std::string test_data =
        "Test data for multiple compression formats. This should work across "
        "ZLIB, GZIP, and DEFLATE_RAW!";

    SUBCASE("ZLIB format") {
        auto compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("GZIP format") {
        auto compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::GZIP);

        StreamingDecompressorUtility decompressor(DecompressionFormat::GZIP);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("DEFLATE_RAW format") {
        auto compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);

        StreamingDecompressorUtility decompressor(
            DecompressionFormat::DEFLATE_RAW);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("AUTO format detection with GZIP input") {
        auto compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::GZIP);

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("AUTO format detection with ZLIB input") {
        auto compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed =
            drain_generator(decompressor.decompress(ByteView(compressed)));

        std::string result(decompressed.begin(), decompressed.end());

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }
}
