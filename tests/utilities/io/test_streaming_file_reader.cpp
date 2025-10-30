#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/streaming_file_reader_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>

using namespace dftracer::utils::utilities::io;

TEST_CASE("StreamingFileReaderUtility - Basic Operations") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_streaming_reader.txt";

    SUBCASE("Read file in chunks") {
        // Create test file
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 1000; ++i) {
                ofs << "Line " << i << "\n";
            }
        }

        StreamReadInput input{test_file, 1024};  // 1KB chunks
        ChunkRange chunks = reader.process(input);

        int chunk_count = 0;
        std::size_t total_bytes = 0;

        for (const auto& chunk : chunks) {
            CHECK(chunk.size() > 0);
            CHECK(chunk.size() <= 1024);
            total_bytes += chunk.size();
            chunk_count++;
        }

        CHECK(chunk_count > 0);
        CHECK(total_bytes > 0);

        fs::remove(test_file);
    }

    SUBCASE("Read empty file") {
        // Create empty file
        {
            std::ofstream ofs(test_file);
        }

        StreamReadInput input{test_file, 1024};
        ChunkRange chunks = reader.process(input);

        int chunk_count = 0;
        for ([[maybe_unused]] const auto& chunk : chunks) {
            chunk_count++;
        }

        CHECK(chunk_count == 0);

        fs::remove(test_file);
    }

    SUBCASE("Read with different chunk sizes") {
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 100; ++i) {
                ofs << "0123456789";  // 10 bytes per line
            }
        }

        // Small chunks
        {
            StreamReadInput input{test_file, 50};
            ChunkRange chunks = reader.process(input);

            int chunk_count = 0;
            for (const auto& chunk : chunks) {
                CHECK(chunk.size() <= 50);
                chunk_count++;
            }
            CHECK(chunk_count > 10);  // 1000 bytes / 50 = 20 chunks
        }

        // Large chunks
        {
            StreamReadInput input{test_file, 2000};
            ChunkRange chunks = reader.process(input);

            int chunk_count = 0;
            for ([[maybe_unused]] const auto& chunk : chunks) {
                chunk_count++;
            }
            CHECK(chunk_count == 1);  // All data fits in one chunk
        }

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileReaderUtility - Error Handling") {
    StreamingFileReaderUtility reader;

    SUBCASE("Non-existent file") {
        StreamReadInput input{"non_existent_streaming_file_12345.txt", 1024};
        CHECK_THROWS_AS(reader.process(input), std::runtime_error);
    }

    SUBCASE("Directory instead of file") {
        fs::path test_dir = "test_streaming_dir";
        fs::create_directory(test_dir);

        StreamReadInput input{test_dir, 1024};
        CHECK_THROWS_AS(reader.process(input), std::runtime_error);

        fs::remove(test_dir);
    }
}

TEST_CASE("StreamingFileReaderUtility - Chunk Boundaries") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_chunk_boundaries.txt";

    SUBCASE("Exact chunk size boundary") {
        {
            std::ofstream ofs(test_file);
            // Write exactly 100 bytes
            for (int i = 0; i < 10; ++i) {
                ofs << "0123456789";
            }
        }

        StreamReadInput input{test_file, 50};  // Should give exactly 2 chunks
        ChunkRange chunks = reader.process(input);

        int chunk_count = 0;
        std::size_t total_size = 0;
        for (const auto& chunk : chunks) {
            total_size += chunk.size();
            chunk_count++;
        }

        CHECK(chunk_count == 2);
        CHECK(total_size == 100);

        fs::remove(test_file);
    }

    SUBCASE("Uneven chunk distribution") {
        {
            std::ofstream ofs(test_file);
            // Write 95 bytes
            for (int i = 0; i < 95; ++i) {
                ofs << "x";
            }
        }

        StreamReadInput input{test_file, 50};
        ChunkRange chunks = reader.process(input);

        std::vector<std::size_t> chunk_sizes;
        for (const auto& chunk : chunks) {
            chunk_sizes.push_back(chunk.size());
        }

        CHECK(chunk_sizes.size() == 2);
        CHECK(chunk_sizes[0] == 50);
        CHECK(chunk_sizes[1] == 45);

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileReaderUtility - Data Integrity") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_data_integrity.txt";

    SUBCASE("Verify complete data") {
        std::string original_data;
        {
            std::ofstream ofs(test_file);
            for (int i = 0; i < 100; ++i) {
                std::string line = "Line " + std::to_string(i) + "\n";
                ofs << line;
                original_data += line;
            }
        }

        StreamReadInput input{test_file, 256};
        ChunkRange chunks = reader.process(input);

        std::string reconstructed;
        for (const auto& chunk : chunks) {
            reconstructed += chunk.to_string();
        }

        CHECK(reconstructed == original_data);
        CHECK(reconstructed.size() == original_data.size());

        fs::remove(test_file);
    }

    SUBCASE("Binary data preservation") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            for (unsigned char i = 0; i < 255; ++i) {
                ofs.write(reinterpret_cast<char*>(&i), 1);
            }
        }

        StreamReadInput input{test_file, 100};
        ChunkRange chunks = reader.process(input);

        std::vector<unsigned char> reconstructed;
        for (const auto& chunk : chunks) {
            reconstructed.insert(reconstructed.end(), chunk.data.begin(),
                                 chunk.data.end());
        }

        CHECK(reconstructed.size() == 255);
        for (size_t i = 0; i < 255; ++i) {
            CHECK(reconstructed[i] == static_cast<unsigned char>(i));
        }

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileReaderUtility - Large Files") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_large_streaming.txt";

    SUBCASE("1MB file") {
        {
            std::ofstream ofs(test_file);
            // Write ~1MB of data (1024000 bytes)
            for (int i = 0; i < 1024 * 100; ++i) {
                ofs << "0123456789";  // 10 bytes
            }
        }

        StreamReadInput input{test_file, 64 * 1024};  // 64KB chunks
        ChunkRange chunks = reader.process(input);

        int chunk_count = 0;
        std::size_t total_bytes = 0;
        std::size_t max_chunk_size = 0;

        for (const auto& chunk : chunks) {
            chunk_count++;
            total_bytes += chunk.size();
            max_chunk_size = std::max(max_chunk_size, chunk.size());
        }

        CHECK(chunk_count > 10);             // Should have multiple chunks
        CHECK(total_bytes == 1024000);       // 1024 * 100 * 10 = 1024000 bytes
        CHECK(max_chunk_size <= 64 * 1024);  // No chunk exceeds limit

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileReaderUtility - Special Characters") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_special_chars.txt";

    SUBCASE("Null bytes in data") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs << "Before";
            char null_byte = '\0';
            ofs.write(&null_byte, 1);
            ofs << "After";
        }

        StreamReadInput input{test_file, 1024};
        ChunkRange chunks = reader.process(input);

        std::size_t total_size = 0;
        for (const auto& chunk : chunks) {
            total_size += chunk.size();
        }

        CHECK(total_size == 12);  // "Before" (6) + '\0' (1) + "After" (5)

        fs::remove(test_file);
    }

    SUBCASE("Unicode characters") {
        {
            std::ofstream ofs(test_file);
            ofs << "Hello こんにちは 你好 مرحبا\n";
        }

        StreamReadInput input{test_file, 1024};
        ChunkRange chunks = reader.process(input);

        std::string reconstructed;
        for (const auto& chunk : chunks) {
            reconstructed += chunk.to_string();
        }

        CHECK(reconstructed.find("Hello") != std::string::npos);
        CHECK(reconstructed.find("こんにちは") != std::string::npos);

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileReaderUtility - Multiple Iterations") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_multi_iter.txt";

    {
        std::ofstream ofs(test_file);
        for (int i = 0; i < 50; ++i) {
            ofs << "Line " << i << "\n";
        }
    }

    StreamReadInput input{test_file, 512};

    SUBCASE("Iterate multiple times") {
        // First iteration
        ChunkRange chunks1 = reader.process(input);
        int count1 = 0;
        for ([[maybe_unused]] const auto& chunk : chunks1) {
            count1++;
        }

        // Second iteration
        ChunkRange chunks2 = reader.process(input);
        int count2 = 0;
        for ([[maybe_unused]] const auto& chunk : chunks2) {
            count2++;
        }

        CHECK(count1 == count2);
        CHECK(count1 > 0);
    }

    fs::remove(test_file);
}

TEST_CASE("StreamingFileReaderUtility - Default Chunk Size") {
    StreamingFileReaderUtility reader;
    fs::path test_file = "test_default_chunk.txt";

    {
        std::ofstream ofs(test_file);
        // Write more than 64KB
        for (int i = 0; i < 10000; ++i) {
            ofs << "This is a test line with some content.\n";
        }
    }

    // Use default chunk size (64KB)
    StreamReadInput input{test_file};
    ChunkRange chunks = reader.process(input);

    int chunk_count = 0;
    bool has_large_chunk = false;
    for (const auto& chunk : chunks) {
        if (chunk.size() > 32 * 1024) {  // Larger than 32KB
            has_large_chunk = true;
        }
        chunk_count++;
    }

    CHECK(chunk_count > 1);  // Should have multiple chunks
    CHECK(has_large_chunk);  // At least one chunk should be large

    fs::remove(test_file);
}
