#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils::coro;
using dftracer::utils::ByteView;

// Helper to compress input and finalize, returning all compressed bytes.
static std::vector<unsigned char> compress_all(
    ManualStreamingCompressorUtility& comp, ByteView input) {
    std::vector<unsigned char> result;
    [&]() -> CoroTask<void> {
        auto gen = comp.compress(input);
        while (auto chunk = co_await gen.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
        auto fin = comp.finalize_stream();
        while (auto chunk = co_await fin.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
    }()
                 .get();
    return result;
}

// Helper to compress without finalizing (for incremental use).
static std::vector<unsigned char> compress_chunk(
    ManualStreamingCompressorUtility& comp, ByteView input) {
    std::vector<unsigned char> result;
    [&]() -> CoroTask<void> {
        auto gen = comp.compress(input);
        while (auto chunk = co_await gen.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
    }()
                 .get();
    return result;
}

// Helper to finalize a compressor, returning remaining bytes.
static std::vector<unsigned char> finalize_compressor(
    ManualStreamingCompressorUtility& comp) {
    std::vector<unsigned char> result;
    [&]() -> CoroTask<void> {
        auto fin = comp.finalize_stream();
        while (auto chunk = co_await fin.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
    }()
                 .get();
    return result;
}

// Helper to decompress compressed bytes, returning all decompressed bytes.
static std::vector<unsigned char> decompress_all(
    StreamingDecompressorUtility& decomp, ByteView input) {
    std::vector<unsigned char> result;
    [&]() -> CoroTask<void> {
        auto gen = decomp.decompress(input);
        while (auto chunk = co_await gen.next()) {
            result.insert(result.end(), chunk->as<unsigned char>(),
                          chunk->as<unsigned char>() + chunk->size());
        }
    }()
                 .get();
    return result;
}

TEST_CASE("ManualStreamingCompressorUtility - Basic Operations") {
    SUBCASE("Compress single chunk") {
        ManualStreamingCompressorUtility compressor;

        std::string text = "Hello, World!";
        auto compressed = compress_all(compressor, ByteView(text));

        // Should produce some compressed data
        CHECK(compressed.size() > 0);
        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("Compress empty chunk") {
        ManualStreamingCompressorUtility compressor;

        std::vector<unsigned char> empty{};
        auto compressed = compress_chunk(compressor, ByteView(empty));

        CHECK(compressed.empty());
    }

    SUBCASE("Multiple small chunks") {
        ManualStreamingCompressorUtility compressor;

        std::vector<std::string> texts = {"Hello, ", "World", "!"};
        std::size_t total_size = 0;

        for (const auto& text : texts) {
            total_size += text.size();
            compress_chunk(compressor, ByteView(text));
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() == total_size);
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Compression Levels") {
    SUBCASE("Default compression level") {
        ManualStreamingCompressorUtility compressor;

        std::string text(1000, 'a');
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == 1000);
        CHECK(compressor.compression_ratio() < 1.0);
    }

    SUBCASE("Level 0 - No compression") {
        ManualStreamingCompressorUtility compressor(0);

        std::string text(1000, 'a');
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == 1000);
    }

    SUBCASE("Level 9 - Maximum compression") {
        ManualStreamingCompressorUtility compressor(9);

        std::string text(1000, 'a');
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == 1000);
        // Level 9 should compress very well
        CHECK(compressor.compression_ratio() < 0.1);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Streaming Behavior") {
    SUBCASE("Process multiple chunks incrementally") {
        ManualStreamingCompressorUtility compressor;

        // Simulate streaming data
        std::vector<std::string> chunks_data = {
            "First chunk of data that will be compressed. ",
            "Second chunk of data that will be compressed. ",
            "Third chunk of data that will be compressed. ",
            "Fourth chunk of data that will be compressed. "};

        std::size_t total_input = 0;
        for (const auto& text : chunks_data) {
            total_input += text.size();
            compress_chunk(compressor, ByteView(text));
            // Each chunk may or may not produce output
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() == total_input);
        CHECK(compressor.total_bytes_out() > 0);
        CHECK(compressor.total_bytes_out() < total_input);
    }

    SUBCASE("Large chunks") {
        ManualStreamingCompressorUtility compressor;

        // Create large repetitive data
        std::string large_chunk(100000, 'x');
        compress_all(compressor, ByteView(large_chunk));

        // Should compress very well
        CHECK(compressor.compression_ratio() < 0.01);
    }

    SUBCASE("Mixed chunk sizes") {
        ManualStreamingCompressorUtility compressor;

        std::vector<std::string> chunks_data = {
            std::string(10, 'a'),     // Small
            std::string(1000, 'b'),   // Medium
            std::string(10000, 'c'),  // Large
            std::string(100, 'd')     // Small
        };

        for (const auto& text : chunks_data) {
            compress_chunk(compressor, ByteView(text));
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() == (10 + 1000 + 10000 + 100));
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Metadata") {
    SUBCASE("Compression ratio calculation") {
        ManualStreamingCompressorUtility compressor;

        std::string text(10000, 'z');
        compress_all(compressor, ByteView(text));

        double ratio = compressor.compression_ratio();
        double expected =
            static_cast<double>(compressor.total_bytes_out()) / 10000.0;

        CHECK(ratio == doctest::Approx(expected));
    }

    SUBCASE("Byte counters") {
        ManualStreamingCompressorUtility compressor;

        std::vector<std::string> texts = {"abc", "def", "ghi"};
        std::size_t expected_in = 0;

        for (const auto& text : texts) {
            expected_in += text.size();
            compress_chunk(compressor, ByteView(text));
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() == expected_in);
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Edge Cases") {
    SUBCASE("Finalize without processing") {
        ManualStreamingCompressorUtility compressor;

        finalize_compressor(compressor);

        // Should handle empty stream
        CHECK(compressor.total_bytes_in() == 0);
    }

    SUBCASE("All zeros") {
        ManualStreamingCompressorUtility compressor;

        std::vector<unsigned char> zeros(10000, 0);
        compress_all(compressor, ByteView(zeros));

        // Should compress extremely well
        CHECK(compressor.compression_ratio() < 0.01);
    }

    SUBCASE("Binary data with pattern") {
        ManualStreamingCompressorUtility compressor;

        std::vector<unsigned char> pattern;
        for (int i = 0; i < 1000; ++i) {
            pattern.push_back(static_cast<unsigned char>(i % 256));
        }
        compress_all(compressor, ByteView(pattern));

        CHECK(compressor.total_bytes_in() == 1000);
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Real World Scenarios") {
    SUBCASE("Log file streaming") {
        ManualStreamingCompressorUtility compressor;

        // Simulate log entries coming in
        for (int i = 0; i < 100; ++i) {
            std::string log_entry = "[2024-01-01 12:00:00] INFO: Log entry " +
                                    std::to_string(i) + "\n";
            compress_chunk(compressor, ByteView(log_entry));
        }

        finalize_compressor(compressor);

        // Log data should compress well due to repetition
        CHECK(compressor.compression_ratio() < 0.5);
    }

    SUBCASE("CSV data streaming") {
        ManualStreamingCompressorUtility compressor;

        // CSV header
        std::string header = "id,name,value\n";
        compress_chunk(compressor, ByteView(header));

        // CSV rows
        for (int i = 0; i < 1000; ++i) {
            std::string row = std::to_string(i) + ",item" + std::to_string(i) +
                              "," + std::to_string(i * 100) + "\n";
            compress_chunk(compressor, ByteView(row));
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() > 1000);
        CHECK(compressor.compression_ratio() < 1.0);
    }

    SUBCASE("JSON streaming") {
        ManualStreamingCompressorUtility compressor;

        // Simulate JSON array streaming
        std::string open_bracket = "[\n";
        compress_chunk(compressor, ByteView(open_bracket));

        for (int i = 0; i < 50; ++i) {
            std::string json_obj = R"(  {"id": )" + std::to_string(i) +
                                   R"(, "value": )" + std::to_string(i * 10) +
                                   "}";
            if (i < 49) json_obj += ",";
            json_obj += "\n";
            compress_chunk(compressor, ByteView(json_obj));
        }

        std::string close_bracket = "]\n";
        compress_chunk(compressor, ByteView(close_bracket));
        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Consistency") {
    SUBCASE("Same data different chunking") {
        std::string full_text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

        // Compress as single chunk
        ManualStreamingCompressorUtility compressor1;
        compress_all(compressor1, ByteView(full_text));

        // Compress as multiple chunks
        ManualStreamingCompressorUtility compressor2;
        for (char c : full_text) {
            std::string s(1, c);
            compress_chunk(compressor2, ByteView(s));
        }
        finalize_compressor(compressor2);

        // Both should produce same input size
        CHECK(compressor1.total_bytes_in() == compressor2.total_bytes_in());
        CHECK(compressor1.total_bytes_in() == full_text.size());
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Performance") {
    SUBCASE("Large streaming data") {
        ManualStreamingCompressorUtility compressor;

        // Stream 1MB in 1KB chunks
        const std::size_t chunk_size = 1024;
        const std::size_t num_chunks = 1024;

        for (std::size_t i = 0; i < num_chunks; ++i) {
            std::string chunk_data(chunk_size,
                                   static_cast<char>('a' + (i % 26)));
            compress_chunk(compressor, ByteView(chunk_data));
        }

        finalize_compressor(compressor);

        CHECK(compressor.total_bytes_in() == chunk_size * num_chunks);
        CHECK(compressor.total_bytes_out() > 0);
        CHECK(compressor.compression_ratio() < 0.1);  // Should compress well
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Compression Formats") {
    SUBCASE("GZIP format (default)") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        std::string text = "Test data for GZIP format";
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("ZLIB format") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::ZLIB);
        std::string text = "Test data for ZLIB format";
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("DEFLATE_RAW format") {
        ManualStreamingCompressorUtility compressor(
            Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);
        std::string text = "Test data for RAW DEFLATE format";
        compress_all(compressor, ByteView(text));

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE(
    "ManualStreamingCompressorUtility - Multiple Compression Formats Round "
    "Trip") {
    std::string original =
        "Test data for format testing with multiple compression formats.";

    SUBCASE("ZLIB format round trip") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::ZLIB);
        auto compressed_data = compress_all(compressor, ByteView(original));

        // Decompress with matching format
        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        auto decompressed =
            decompress_all(decompressor, ByteView(compressed_data));

        std::string result(decompressed.begin(), decompressed.end());
        CHECK(result == original);
    }

    SUBCASE("GZIP format round trip") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        auto compressed_data = compress_all(compressor, ByteView(original));

        StreamingDecompressorUtility decompressor(DecompressionFormat::GZIP);
        auto decompressed =
            decompress_all(decompressor, ByteView(compressed_data));

        std::string result(decompressed.begin(), decompressed.end());
        CHECK(result == original);
    }

    SUBCASE("DEFLATE_RAW format round trip") {
        ManualStreamingCompressorUtility compressor(
            Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);
        auto compressed_data = compress_all(compressor, ByteView(original));

        StreamingDecompressorUtility decompressor(
            DecompressionFormat::DEFLATE_RAW);
        auto decompressed =
            decompress_all(decompressor, ByteView(compressed_data));

        std::string result(decompressed.begin(), decompressed.end());
        CHECK(result == original);
    }

    SUBCASE("AUTO decompression with ZLIB input") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::ZLIB);
        auto compressed_data = compress_all(compressor, ByteView(original));

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed =
            decompress_all(decompressor, ByteView(compressed_data));

        std::string result(decompressed.begin(), decompressed.end());
        CHECK(result == original);
    }

    SUBCASE("AUTO decompression with GZIP input") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        auto compressed_data = compress_all(compressor, ByteView(original));

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        auto decompressed =
            decompress_all(decompressor, ByteView(compressed_data));

        std::string result(decompressed.begin(), decompressed.end());
        CHECK(result == original);
    }
}
