#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils::utilities::io;

// Helper function to compress data using ManualStreamingCompressorUtility
static CompressedData compress_with_streaming(
    const std::string& text, int level = Z_DEFAULT_COMPRESSION,
    CompressionFormat format = CompressionFormat::ZLIB) {
    ManualStreamingCompressorUtility compressor(level, format);
    RawData input(text);

    // Feed data in chunks
    auto chunks = compressor.process(input);
    auto final_chunks = compressor.finalize();

    // Combine all chunks into a single CompressedData
    std::vector<unsigned char> all_data;
    for (const auto& chunk : chunks) {
        all_data.insert(all_data.end(), chunk.data.begin(), chunk.data.end());
    }
    for (const auto& chunk : final_chunks) {
        all_data.insert(all_data.end(), chunk.data.begin(), chunk.data.end());
    }

    return CompressedData(std::move(all_data), text.size());
}

// Helper function to compress binary data
static CompressedData compress_binary_with_streaming(
    const std::vector<unsigned char>& data, int level = Z_DEFAULT_COMPRESSION,
    CompressionFormat format = CompressionFormat::ZLIB) {
    ManualStreamingCompressorUtility compressor(level, format);
    RawData input(data);

    auto chunks = compressor.process(input);
    auto final_chunks = compressor.finalize();

    std::vector<unsigned char> all_data;
    for (const auto& chunk : chunks) {
        all_data.insert(all_data.end(), chunk.data.begin(), chunk.data.end());
    }
    for (const auto& chunk : final_chunks) {
        all_data.insert(all_data.end(), chunk.data.begin(), chunk.data.end());
    }

    return CompressedData(std::move(all_data), data.size());
}

TEST_CASE("StreamingDecompressorUtility - Basic Operations") {
    SUBCASE("Decompress compressed data") {
        // Use ManualStreamingCompressorUtility with ZLIB format
        std::string text = "Hello, World! This is a test message.";
        CompressedData compressed = compress_with_streaming(
            text, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        // Now decompress using streaming decompressor with ZLIB format
        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed_chunks = decompressor.process(compressed);

        // Reconstruct original text
        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == text);
        CHECK(decompressor.total_bytes_out() == text.size());
    }

    SUBCASE("Decompress empty chunk") {
        StreamingDecompressorUtility decompressor;

        CompressedData empty({}, 0);
        auto decompressed_chunks = decompressor.process(empty);

        CHECK(decompressed_chunks.empty());
    }
}

TEST_CASE("StreamingDecompressorUtility - Round Trip") {
    SUBCASE("Simple text") {
        std::string original = "The quick brown fox jumps over the lazy dog.";

        CompressedData compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("Repetitive data") {
        std::string original(10000, 'a');

        CompressedData compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == original);
        CHECK(result.size() == 10000);
    }

    SUBCASE("Binary data") {
        std::vector<unsigned char> original = {0x00, 0x01, 0x02, 0xFF, 0xFE,
                                               0xFD, 0x00, 0x01, 0x02};

        CompressedData compressed = compress_binary_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed_chunks = decompressor.process(compressed);

        std::vector<unsigned char> result;
        for (const auto& chunk : decompressed_chunks) {
            result.insert(result.end(), chunk.data.begin(), chunk.data.end());
        }

        CHECK(result == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Different Data Sizes") {
    SUBCASE("Very small data") {
        std::string text = "x";
        CompressedData compressed = compress_with_streaming(
            text, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == text);
    }

    SUBCASE("Medium data") {
        std::string text(1024, 'a');  // 1KB
        CompressedData compressed = compress_with_streaming(
            text, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::size_t total_size = 0;
        for (const auto& chunk : decompressed) {
            total_size += chunk.size();
        }

        CHECK(total_size == 1024);
    }

    SUBCASE("Large data") {
        std::string text(100000, 'b');  // 100KB
        CompressedData compressed = compress_with_streaming(
            text, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::size_t total_size = 0;
        for (const auto& chunk : decompressed) {
            total_size += chunk.size();
        }

        CHECK(total_size == 100000);
    }
}

TEST_CASE("StreamingDecompressorUtility - Different Compression Levels") {
    std::string original(1000, 'x');

    SUBCASE("Level 1") {
        CompressedData compressed =
            compress_with_streaming(original, 1, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("Level 9") {
        CompressedData compressed =
            compress_with_streaming(original, 9, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Error Handling") {
    StreamingDecompressorUtility decompressor;

    SUBCASE("Invalid compressed data") {
        // Create fake invalid compressed data
        std::vector<unsigned char> invalid = {0x00, 0x01, 0x02, 0x03};
        CompressedData bad_data(invalid, 100);

        CHECK_THROWS_AS(decompressor.process(bad_data), std::runtime_error);
    }
}

TEST_CASE("StreamingDecompressorUtility - Metadata") {
    SUBCASE("Byte counters") {
        std::string original = "Test data for byte counting";

        CompressedData compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        decompressor.process(compressed);

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

        CompressedData compressed = compress_with_streaming(
            log_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == log_data);
    }

    SUBCASE("CSV data") {
        std::string csv_data = "id,name,value\n";
        for (int i = 0; i < 1000; ++i) {
            csv_data += std::to_string(i) + ",item" + std::to_string(i) + "," +
                        std::to_string(i * 100) + "\n";
        }

        CompressedData compressed = compress_with_streaming(
            csv_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

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

        CompressedData compressed = compress_with_streaming(
            json_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == json_data);
    }
}

TEST_CASE("StreamingDecompressorUtility - Edge Cases") {
    SUBCASE("All zeros") {
        std::vector<unsigned char> zeros(10000, 0);

        CompressedData compressed = compress_binary_with_streaming(
            zeros, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::vector<unsigned char> result;
        for (const auto& chunk : decompressed) {
            result.insert(result.end(), chunk.data.begin(), chunk.data.end());
        }

        CHECK(result == zeros);
    }

    SUBCASE("Single byte") {
        std::string original = "x";

        CompressedData compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }
}

TEST_CASE("StreamingDecompressorUtility - Large Data") {
    SUBCASE("Large repetitive data") {
        // 1MB of repetitive data
        std::string original(1024 * 1024, 'z');

        CompressedData compressed = compress_with_streaming(
            original, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed = decompressor.process(compressed);

        std::size_t total_decompressed = 0;
        for (const auto& chunk : decompressed) {
            total_decompressed += chunk.size();
        }

        CHECK(total_decompressed == original.size());
    }
}

TEST_CASE("StreamingDecompressorUtility - Multiple Formats") {
    std::string test_data =
        "Test data for multiple compression formats. This should work across "
        "ZLIB, GZIP, and DEFLATE_RAW!";

    SUBCASE("ZLIB format") {
        // Compress with ZLIB format
        CompressedData compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        // Decompress with ZLIB format
        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("GZIP format") {
        // Compress with GZIP format
        CompressedData compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::GZIP);

        // Decompress with GZIP format
        StreamingDecompressorUtility decompressor(DecompressionFormat::GZIP);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("DEFLATE_RAW format") {
        // Compress with DEFLATE_RAW format
        CompressedData compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);

        // Decompress with DEFLATE_RAW format
        StreamingDecompressorUtility decompressor(
            DecompressionFormat::DEFLATE_RAW);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("AUTO format detection with GZIP input") {
        // Compress with GZIP format
        CompressedData compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::GZIP);

        // Decompress with AUTO format detection
        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }

    SUBCASE("AUTO format detection with ZLIB input") {
        // Compress with ZLIB format
        CompressedData compressed = compress_with_streaming(
            test_data, Z_DEFAULT_COMPRESSION, CompressionFormat::ZLIB);

        // Decompress with AUTO format detection
        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed_chunks = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed_chunks) {
            result += chunk.to_string();
        }

        CHECK(result == test_data);
        CHECK(decompressor.total_bytes_out() == test_data.size());
    }
}
