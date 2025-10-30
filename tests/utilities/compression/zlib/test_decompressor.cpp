#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/compression/zlib/compressor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/decompressor_utility.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils::utilities::io;

TEST_CASE("DecompressorUtility - Basic Operations") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Decompress simple compressed data") {
        std::string original_text = "Hello, World!";
        RawData input(original_text);
        CompressedData compressed = compressor->process(input);
        RawData decompressed = decompressor->process(compressed);

        CHECK(input == decompressed);
        CHECK(input.to_string() == decompressed.to_string());
    }

    SUBCASE("Decompress empty data") {
        CompressedData empty({}, 0);
        RawData decompressed = decompressor->process(empty);

        CHECK(decompressed.empty());
        CHECK(decompressed.size() == 0);
    }

    SUBCASE("Round-trip compression/decompression") {
        std::string original_text =
            "The quick brown fox jumps over the lazy dog.";
        RawData original(original_text);

        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(original.to_string() == restored.to_string());
    }
}

TEST_CASE("Decompressor Utility - Different Data Sizes") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Very small data") {
        std::string text = "x";
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
    }

    SUBCASE("Medium data") {
        std::string text(1024, 'a');  // 1KB
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.size() == 1024);
    }

    SUBCASE("Large data") {
        std::string text(100000, 'b');  // 100KB
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.size() == 100000);
    }
}

TEST_CASE("DecompressorUtility - Different Compression Levels") {
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Level 0 - No compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(0);

        std::string text(1000, 'a');
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
    }

    SUBCASE("Level 1 - Fast compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(1);

        std::string text(1000, 'a');
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
    }

    SUBCASE("Level 9 - Maximum compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(9);

        std::string text(1000, 'a');
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
    }
}

TEST_CASE("DecompressorUtility - Different Data Types") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Binary data") {
        std::vector<unsigned char> binary_data = {0x00, 0x01, 0x02, 0xFF, 0xFE,
                                                  0xFD, 0x00, 0x01, 0x02};
        RawData original(binary_data);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(original.data == restored.data);
    }

    SUBCASE("Text with newlines") {
        std::string text = "Line 1\nLine 2\nLine 3\n";
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(text == restored.to_string());
    }

    SUBCASE("Text with special characters") {
        std::string text = "Hello\n\tWorld!\r\n\0Special";
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
    }

    SUBCASE("Unicode data") {
        std::string text = "Hello, 世界! 🌍";
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(text == restored.to_string());
    }

    SUBCASE("Repetitive data") {
        std::string text(10000, 'x');
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.size() == 10000);
    }

    SUBCASE("Mixed data") {
        std::string text =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        RawData original(text);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(text == restored.to_string());
    }
}

TEST_CASE("DecompressorUtility - Error Handling") {
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Corrupted data") {
        // Create fake compressed data that's not valid gzip
        std::vector<unsigned char> corrupted = {0x00, 0x01, 0x02, 0x03};
        CompressedData invalid(corrupted, 100);

        CHECK_THROWS_AS(decompressor->process(invalid), std::runtime_error);
    }

    SUBCASE("Truncated compressed data") {
        auto compressor = std::make_shared<CompressorUtility>();
        std::string text = "This is a test string";
        RawData original(text);
        CompressedData compressed = compressor->process(original);

        // Truncate the compressed data
        std::vector<unsigned char> truncated(compressed.data.begin(),
                                             compressed.data.begin() + 5);
        CompressedData invalid(truncated, text.size());

        CHECK_THROWS_AS(decompressor->process(invalid), std::runtime_error);
    }
}

TEST_CASE("DecompressorUtility - Consistency") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Multiple decompression of same data") {
        std::string text = "Test data for consistency";
        RawData original(text);
        CompressedData compressed = compressor->process(original);

        RawData restored1 = decompressor->process(compressed);
        RawData restored2 = decompressor->process(compressed);

        CHECK(restored1 == restored2);
        CHECK(restored1 == original);
        CHECK(restored2 == original);
    }

    SUBCASE("Sequential compress/decompress operations") {
        std::vector<std::string> test_strings = {
            "First string", "Second string", "Third string", "Fourth string"};

        for (const auto& text : test_strings) {
            RawData original(text);
            CompressedData compressed = compressor->process(original);
            RawData restored = decompressor->process(compressed);

            CHECK(original == restored);
            CHECK(text == restored.to_string());
        }
    }
}

TEST_CASE("DecompressorUtility - Metadata Handling") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Original size is used correctly") {
        std::string text(1000, 'a');
        RawData original(text);
        CompressedData compressed = compressor->process(original);

        // Verify original_size is set correctly
        CHECK(compressed.original_size == 1000);

        RawData restored = decompressor->process(compressed);
        CHECK(restored.size() == compressed.original_size);
    }

    SUBCASE("Decompression without original_size hint") {
        std::string text = "Test without size hint";
        RawData original(text);
        CompressedData compressed = compressor->process(original);

        // Clear the original_size hint
        compressed.original_size = 0;

        // Should still decompress correctly (with automatic resize)
        RawData restored = decompressor->process(compressed);
        CHECK(original == restored);
    }
}

TEST_CASE("DecompressorUtility - Edge Cases") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("All zeros") {
        std::vector<unsigned char> zeros(1000, 0);
        RawData original(zeros);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.data == zeros);
    }

    SUBCASE("All ones") {
        std::vector<unsigned char> ones(1000, 0xFF);
        RawData original(ones);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.data == ones);
    }

    SUBCASE("Alternating pattern") {
        std::vector<unsigned char> pattern;
        for (int i = 0; i < 1000; ++i) {
            pattern.push_back(i % 2 ? 0xFF : 0x00);
        }
        RawData original(pattern);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.data == pattern);
    }

    SUBCASE("Single byte") {
        std::vector<unsigned char> single = {0x42};
        RawData original(single);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(restored.data == single);
    }
}

TEST_CASE("DecompressorUtility - Performance Characteristics") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("Large highly compressible data") {
        // Create 1MB of repetitive data
        std::string text(1024 * 1024, 'a');
        RawData original(text);

        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        // Should compress to < 1% of original
        CHECK(compressed.compression_ratio() < 0.01);
    }

    SUBCASE("Multiple small decompressions") {
        for (int i = 0; i < 100; ++i) {
            std::string text = "Test " + std::to_string(i);
            RawData original(text);
            CompressedData compressed = compressor->process(original);
            RawData restored = decompressor->process(compressed);

            CHECK(original == restored);
        }
    }
}

TEST_CASE("DecompressorUtility - Real World Scenarios") {
    auto compressor = std::make_shared<CompressorUtility>();
    auto decompressor = std::make_shared<DecompressorUtility>();

    SUBCASE("JSON data") {
        std::string json = R"({
            "name": "test",
            "value": 123,
            "nested": {
                "array": [1, 2, 3, 4, 5]
            }
        })";
        RawData original(json);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(json == restored.to_string());
    }

    SUBCASE("Log file data") {
        std::string log;
        for (int i = 0; i < 100; ++i) {
            log += "[2024-01-01 12:00:00] INFO: Log entry " +
                   std::to_string(i) + "\n";
        }
        RawData original(log);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(log == restored.to_string());
    }

    SUBCASE("CSV data") {
        std::string csv = "id,name,value\n";
        for (int i = 0; i < 1000; ++i) {
            csv += std::to_string(i) + ",item" + std::to_string(i) + "," +
                   std::to_string(i * 100) + "\n";
        }
        RawData original(csv);
        CompressedData compressed = compressor->process(original);
        RawData restored = decompressor->process(compressed);

        CHECK(original == restored);
        CHECK(csv == restored.to_string());
    }
}
