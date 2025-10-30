#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils::utilities::io;

TEST_CASE("ManualStreamingCompressorUtility - Basic Operations") {
    SUBCASE("Compress single chunk") {
        ManualStreamingCompressorUtility compressor;

        std::string text = "Hello, World!";
        RawData chunk(text);

        auto compressed_chunks = compressor.process(chunk);
        auto final_chunks = compressor.finalize();

        // Should produce some compressed data
        CHECK((compressed_chunks.size() + final_chunks.size()) > 0);
        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("Compress empty chunk") {
        ManualStreamingCompressorUtility compressor;

        RawData empty(std::vector<unsigned char>{});
        auto compressed_chunks = compressor.process(empty);

        CHECK(compressed_chunks.empty());
    }

    SUBCASE("Multiple small chunks") {
        ManualStreamingCompressorUtility compressor;

        std::vector<std::string> texts = {"Hello, ", "World", "!"};
        std::size_t total_size = 0;

        for (const auto& text : texts) {
            RawData chunk(text);
            total_size += text.size();
            auto compressed_chunks = compressor.process(chunk);
        }

        auto final_chunks = compressor.finalize();

        CHECK(compressor.total_bytes_in() == total_size);
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Compression Levels") {
    SUBCASE("Default compression level") {
        ManualStreamingCompressorUtility compressor;

        std::string text(1000, 'a');
        RawData chunk(text);

        auto compressed_chunks = compressor.process(chunk);
        auto final_chunks = compressor.finalize();

        CHECK(compressor.total_bytes_in() == 1000);
        CHECK(compressor.compression_ratio() < 1.0);
    }

    SUBCASE("Level 0 - No compression") {
        ManualStreamingCompressorUtility compressor(0);

        std::string text(1000, 'a');
        RawData chunk(text);

        auto compressed_chunks = compressor.process(chunk);
        auto final_chunks = compressor.finalize();

        CHECK(compressor.total_bytes_in() == 1000);
    }

    SUBCASE("Level 9 - Maximum compression") {
        ManualStreamingCompressorUtility compressor(9);

        std::string text(1000, 'a');
        RawData chunk(text);

        auto compressed_chunks = compressor.process(chunk);
        auto final_chunks = compressor.finalize();

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
            RawData chunk(text);
            total_input += text.size();
            auto compressed = compressor.process(chunk);
            // Each chunk may or may not produce output
        }

        auto final_chunks = compressor.finalize();

        CHECK(compressor.total_bytes_in() == total_input);
        CHECK(compressor.total_bytes_out() > 0);
        CHECK(compressor.total_bytes_out() < total_input);
    }

    SUBCASE("Large chunks") {
        ManualStreamingCompressorUtility compressor;

        // Create large repetitive data
        std::string large_chunk(100000, 'x');
        RawData chunk(large_chunk);

        auto compressed = compressor.process(chunk);
        auto final_chunks = compressor.finalize();

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
            RawData chunk(text);
            compressor.process(chunk);
        }

        compressor.finalize();

        CHECK(compressor.total_bytes_in() == (10 + 1000 + 10000 + 100));
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Metadata") {
    SUBCASE("Compression ratio calculation") {
        ManualStreamingCompressorUtility compressor;

        std::string text(10000, 'z');
        RawData chunk(text);

        compressor.process(chunk);
        compressor.finalize();

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
            RawData chunk(text);
            expected_in += text.size();
            compressor.process(chunk);
        }

        compressor.finalize();

        CHECK(compressor.total_bytes_in() == expected_in);
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Edge Cases") {
    SUBCASE("Finalize without processing") {
        ManualStreamingCompressorUtility compressor;

        auto final_chunks = compressor.finalize();

        // Should handle empty stream
        CHECK(compressor.total_bytes_in() == 0);
    }

    SUBCASE("All zeros") {
        ManualStreamingCompressorUtility compressor;

        std::vector<unsigned char> zeros(10000, 0);
        RawData chunk(zeros);

        compressor.process(chunk);
        compressor.finalize();

        // Should compress extremely well
        CHECK(compressor.compression_ratio() < 0.01);
    }

    SUBCASE("Binary data with pattern") {
        ManualStreamingCompressorUtility compressor;

        std::vector<unsigned char> pattern;
        for (int i = 0; i < 1000; ++i) {
            pattern.push_back(static_cast<unsigned char>(i % 256));
        }
        RawData chunk(pattern);

        compressor.process(chunk);
        compressor.finalize();

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
            RawData chunk(log_entry);
            compressor.process(chunk);
        }

        compressor.finalize();

        // Log data should compress well due to repetition
        CHECK(compressor.compression_ratio() < 0.5);
    }

    SUBCASE("CSV data streaming") {
        ManualStreamingCompressorUtility compressor;

        // CSV header
        std::string header = "id,name,value\n";
        compressor.process(RawData(header));

        // CSV rows
        for (int i = 0; i < 1000; ++i) {
            std::string row = std::to_string(i) + ",item" + std::to_string(i) +
                              "," + std::to_string(i * 100) + "\n";
            compressor.process(RawData(row));
        }

        compressor.finalize();

        CHECK(compressor.total_bytes_in() > 1000);
        CHECK(compressor.compression_ratio() < 1.0);
    }

    SUBCASE("JSON streaming") {
        ManualStreamingCompressorUtility compressor;

        // Simulate JSON array streaming
        compressor.process(RawData("[\n"));

        for (int i = 0; i < 50; ++i) {
            std::string json_obj = R"(  {"id": )" + std::to_string(i) +
                                   R"(, "value": )" + std::to_string(i * 10) +
                                   "}";
            if (i < 49) json_obj += ",";
            json_obj += "\n";
            compressor.process(RawData(json_obj));
        }

        compressor.process(RawData("]\n"));
        compressor.finalize();

        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Consistency") {
    SUBCASE("Same data different chunking") {
        std::string full_text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

        // Compress as single chunk
        ManualStreamingCompressorUtility compressor1;
        compressor1.process(RawData(full_text));
        compressor1.finalize();

        // Compress as multiple chunks
        ManualStreamingCompressorUtility compressor2;
        for (char c : full_text) {
            compressor2.process(RawData(std::string(1, c)));
        }
        compressor2.finalize();

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
            compressor.process(RawData(chunk_data));
        }

        compressor.finalize();

        CHECK(compressor.total_bytes_in() == chunk_size * num_chunks);
        CHECK(compressor.total_bytes_out() > 0);
        CHECK(compressor.compression_ratio() < 0.1);  // Should compress well
    }
}

TEST_CASE("StreamingCompressorUtility - Set Compression Level") {
    auto streaming_compressor = std::make_shared<StreamingCompressorUtility>();

    SUBCASE("Default level") {
        CHECK(streaming_compressor->get_compression_level() ==
              Z_DEFAULT_COMPRESSION);
    }

    SUBCASE("Set valid levels") {
        for (int level = 0; level <= 9; ++level) {
            CHECK_NOTHROW(streaming_compressor->set_compression_level(level));
            CHECK(streaming_compressor->get_compression_level() == level);
        }
    }

    SUBCASE("Set Z_DEFAULT_COMPRESSION") {
        CHECK_NOTHROW(
            streaming_compressor->set_compression_level(Z_DEFAULT_COMPRESSION));
        CHECK(streaming_compressor->get_compression_level() ==
              Z_DEFAULT_COMPRESSION);
    }

    SUBCASE("Invalid levels") {
        CHECK_THROWS_AS(streaming_compressor->set_compression_level(10),
                        std::invalid_argument);
        CHECK_THROWS_AS(streaming_compressor->set_compression_level(-2),
                        std::invalid_argument);
    }
}

TEST_CASE("ManualStreamingCompressorUtility - Compression Formats") {
    SUBCASE("GZIP format (default)") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        std::string text = "Test data for GZIP format";
        RawData chunk(text);

        auto compressed = compressor.process(chunk);
        auto final = compressor.finalize();

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("ZLIB format") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::ZLIB);
        std::string text = "Test data for ZLIB format";
        RawData chunk(text);

        auto compressed = compressor.process(chunk);
        auto final = compressor.finalize();

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }

    SUBCASE("DEFLATE_RAW format") {
        ManualStreamingCompressorUtility compressor(
            Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);
        std::string text = "Test data for RAW DEFLATE format";
        RawData chunk(text);

        auto compressed = compressor.process(chunk);
        auto final = compressor.finalize();

        CHECK(compressor.total_bytes_in() == text.size());
        CHECK(compressor.total_bytes_out() > 0);
    }
}

TEST_CASE("StreamingCompressorUtility - With ChunkRange") {
    // Create a test file
    fs::path test_file =
        fs::temp_directory_path() / "test_streaming_compress.txt";

    SUBCASE("Compress file using ChunkRange") {
        // Write test data to file
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 1000; ++i) {
                ofs << "Line " << i
                    << ": This is a test line with some data.\n";
            }
        }

        // Create streaming compressor
        auto compressor = std::make_shared<StreamingCompressorUtility>();
        compressor->set_compression_level(6);

        // Create ChunkRange
        ChunkRange chunks(test_file, 1024);  // 1KB chunks

        // Compress using the StreamingCompressorUtility::process
        auto compressed_range = compressor->process(chunks);

        // Iterate through compressed chunks
        std::size_t total_compressed = 0;
        int chunk_count = 0;
        for (const auto& compressed_chunk : compressed_range) {
            total_compressed += compressed_chunk.size();
            chunk_count++;
        }

        CHECK(chunk_count > 0);
        CHECK(total_compressed > 0);

        // Clean up
        fs::remove(test_file);
    }

    SUBCASE("Different compression levels with ChunkRange") {
        // Write test data
        {
            std::ofstream ofs(test_file);
            std::string line(100, 'a');
            for (int i = 0; i < 1000; ++i) {
                ofs << line << "\n";
            }
        }

        // Test level 1
        {
            auto compressor1 = std::make_shared<StreamingCompressorUtility>();
            compressor1->set_compression_level(1);
            ChunkRange chunks(test_file, 1024);
            auto compressed = compressor1->process(chunks);

            std::size_t size1 = 0;
            for (const auto& chunk : compressed) {
                size1 += chunk.size();
            }

            CHECK(size1 > 0);
        }

        // Test level 9
        {
            auto compressor9 = std::make_shared<StreamingCompressorUtility>();
            compressor9->set_compression_level(9);
            ChunkRange chunks(test_file, 1024);
            auto compressed = compressor9->process(chunks);

            std::size_t size9 = 0;
            for (const auto& chunk : compressed) {
                size9 += chunk.size();
            }

            CHECK(size9 > 0);
            // Level 9 should generally be smaller or equal
        }

        fs::remove(test_file);
    }

    SUBCASE("Large file streaming compression") {
        // Create larger file
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 10000; ++i) {
                ofs << "This is test line " << i << " with repeated data. ";
                ofs << "More data here for compression testing.\n";
            }
        }

        auto compressor = std::make_shared<StreamingCompressorUtility>();
        compressor->set_compression_level(6);

        // Use smaller chunks to test streaming
        ChunkRange chunks(test_file, 512);
        auto compressed_range = compressor->process(chunks);

        std::size_t total_compressed = 0;
        int chunk_count = 0;
        for (const auto& compressed_chunk : compressed_range) {
            total_compressed += compressed_chunk.size();
            chunk_count++;
        }

        // Should have multiple chunks
        CHECK(chunk_count > 1);
        CHECK(total_compressed > 0);

        // Should compress well due to repetitive data
        std::size_t file_size = fs::file_size(test_file);
        double compression_ratio = static_cast<double>(total_compressed) /
                                   static_cast<double>(file_size);
        CHECK(compression_ratio < 1.0);

        fs::remove(test_file);
    }

    SUBCASE("Small file single chunk") {
        // Write small file
        {
            std::ofstream ofs(test_file);
            ofs << "Small test file";
        }

        auto compressor = std::make_shared<StreamingCompressorUtility>();
        ChunkRange chunks(test_file, 1024);
        auto compressed_range = compressor->process(chunks);

        int chunk_count = 0;
        for (const auto& compressed_chunk : compressed_range) {
            chunk_count++;
            CHECK_FALSE(compressed_chunk.data.empty());
        }

        // Small file should result in 1 compressed chunk
        CHECK(chunk_count >= 1);

        fs::remove(test_file);
    }

    SUBCASE("Binary file compression") {
        // Write binary data
        {
            std::ofstream ofs(test_file, std::ios::binary);
            for (int i = 0; i < 10000; ++i) {
                unsigned char byte = static_cast<unsigned char>(i % 256);
                ofs.write(reinterpret_cast<const char*>(&byte), 1);
            }
        }

        auto compressor = std::make_shared<StreamingCompressorUtility>();
        ChunkRange chunks(test_file, 1024);
        auto compressed_range = compressor->process(chunks);

        std::size_t total_compressed = 0;
        for (const auto& compressed_chunk : compressed_range) {
            total_compressed += compressed_chunk.size();
        }

        CHECK(total_compressed > 0);

        fs::remove(test_file);
    }

    SUBCASE("Empty file") {
        // Create empty file
        {
            std::ofstream ofs(test_file);
        }

        auto compressor = std::make_shared<StreamingCompressorUtility>();
        ChunkRange chunks(test_file, 1024);
        auto compressed_range = compressor->process(chunks);

        int chunk_count = 0;
        for ([[maybe_unused]] const auto& compressed_chunk : compressed_range) {
            chunk_count++;
        }

        // Empty file might produce header/trailer
        CHECK(chunk_count >= 0);

        fs::remove(test_file);
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
        RawData input(original);

        auto chunks = compressor.process(input);
        auto final_chunks = compressor.finalize();

        // Combine compressed data
        std::vector<unsigned char> compressed_data;
        for (const auto& chunk : chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }
        for (const auto& chunk : final_chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }

        // Decompress with matching format
        StreamingDecompressorUtility decompressor(DecompressionFormat::ZLIB);
        CompressedData compressed(std::move(compressed_data), original.size());
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("GZIP format round trip") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        RawData input(original);

        auto chunks = compressor.process(input);
        auto final_chunks = compressor.finalize();

        std::vector<unsigned char> compressed_data;
        for (const auto& chunk : chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }
        for (const auto& chunk : final_chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }

        StreamingDecompressorUtility decompressor(DecompressionFormat::GZIP);
        CompressedData compressed(std::move(compressed_data), original.size());
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("DEFLATE_RAW format round trip") {
        ManualStreamingCompressorUtility compressor(
            Z_DEFAULT_COMPRESSION, CompressionFormat::DEFLATE_RAW);
        RawData input(original);

        auto chunks = compressor.process(input);
        auto final_chunks = compressor.finalize();

        std::vector<unsigned char> compressed_data;
        for (const auto& chunk : chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }
        for (const auto& chunk : final_chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }

        StreamingDecompressorUtility decompressor(
            DecompressionFormat::DEFLATE_RAW);
        CompressedData compressed(std::move(compressed_data), original.size());
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("AUTO decompression with ZLIB input") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::ZLIB);
        RawData input(original);

        auto chunks = compressor.process(input);
        auto final_chunks = compressor.finalize();

        std::vector<unsigned char> compressed_data;
        for (const auto& chunk : chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }
        for (const auto& chunk : final_chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        CompressedData compressed(std::move(compressed_data), original.size());
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }

    SUBCASE("AUTO decompression with GZIP input") {
        ManualStreamingCompressorUtility compressor(Z_DEFAULT_COMPRESSION,
                                                    CompressionFormat::GZIP);
        RawData input(original);

        auto chunks = compressor.process(input);
        auto final_chunks = compressor.finalize();

        std::vector<unsigned char> compressed_data;
        for (const auto& chunk : chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }
        for (const auto& chunk : final_chunks) {
            compressed_data.insert(compressed_data.end(), chunk.data.begin(),
                                   chunk.data.end());
        }

        StreamingDecompressorUtility decompressor(DecompressionFormat::AUTO);
        CompressedData compressed(std::move(compressed_data), original.size());
        auto decompressed = decompressor.process(compressed);

        std::string result;
        for (const auto& chunk : decompressed) {
            result += chunk.to_string();
        }

        CHECK(result == original);
    }
}
