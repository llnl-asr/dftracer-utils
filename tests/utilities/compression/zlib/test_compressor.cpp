#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/compression/zlib/compressor_utility.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace dftracer::utils::utilities::compression::zlib;
using namespace dftracer::utils::utilities::io;

TEST_CASE("CompressorUtility - Basic Operations") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("Compress simple string") {
        RawData input("Hello, World!");
        CompressedData output = compressor->process(input);

        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == input.size());
        // Note: Very small strings may expand due to compression overhead
    }

    SUBCASE("Compress empty data") {
        RawData input(std::vector<unsigned char>{});
        CompressedData output = compressor->process(input);

        CHECK(output.data.empty());
        CHECK(output.original_size == 0);
    }

    SUBCASE("Default compression level") {
        CHECK(compressor->get_compression_level() == Z_DEFAULT_COMPRESSION);
    }
}

TEST_CASE("CompressorUtility - Compression Levels") {
    SUBCASE("Level 0 - No compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(0);

        std::string test_data(1000, 'a');  // Highly repetitive
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(compressor->get_compression_level() == 0);
        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == input.size());
    }

    SUBCASE("Level 1 - Fast compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(1);

        std::string test_data(1000, 'a');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(compressor->get_compression_level() == 1);
        CHECK(output.size() < input.size());
    }

    SUBCASE("Level 9 - Maximum compression") {
        auto compressor = std::make_shared<CompressorUtility>();
        compressor->set_compression_level(9);

        std::string test_data(1000, 'a');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(compressor->get_compression_level() == 9);
        CHECK(output.size() < input.size());
    }

    SUBCASE("Invalid compression level") {
        auto compressor = std::make_shared<CompressorUtility>();

        CHECK_THROWS_AS(compressor->set_compression_level(10),
                        std::invalid_argument);
        CHECK_THROWS_AS(compressor->set_compression_level(-2),
                        std::invalid_argument);
    }

    SUBCASE("Z_DEFAULT_COMPRESSION is valid") {
        auto compressor = std::make_shared<CompressorUtility>();
        CHECK_NOTHROW(compressor->set_compression_level(Z_DEFAULT_COMPRESSION));
    }
}

TEST_CASE("CompressorUtility - Compression Effectiveness") {
    auto compressor = std::make_shared<CompressorUtility>();
    compressor->set_compression_level(6);

    SUBCASE("Highly repetitive data") {
        std::string test_data(10000, 'a');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        // Should compress very well
        CHECK(output.compression_ratio() < 0.1);  // Less than 10% of original
        CHECK(output.space_savings() > 90.0);     // More than 90% savings
    }

    SUBCASE("Random-like data") {
        std::string test_data =
            "aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        // Random data compresses poorly or not at all
        CHECK(output.size() > 0);
        CHECK(output.original_size == input.size());
    }

    SUBCASE("Text data") {
        std::string test_data = "The quick brown fox jumps over the lazy dog. ";
        // Repeat to make it more compressible
        for (int i = 0; i < 10; ++i) {
            test_data += test_data;
        }
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        // Text should compress reasonably well
        CHECK(output.size() < input.size());
        CHECK(output.compression_ratio() < 1.0);
    }
}

TEST_CASE("CompressorUtility - Data Sizes") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("Very small data") {
        RawData input("x");
        CompressedData output = compressor->process(input);

        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == 1);
        // Note: Very small data may expand when compressed
    }

    SUBCASE("Medium data") {
        std::string test_data(1024, 'a');  // 1KB
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(output.original_size == 1024);
        CHECK(output.size() < 1024);
    }

    SUBCASE("Large data") {
        std::string test_data(1024 * 1024, 'b');  // 1MB
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(output.original_size == 1024 * 1024);
        CHECK(output.size() < 1024 * 1024);
        CHECK(output.compression_ratio() < 0.01);  // Should compress very well
    }
}

TEST_CASE("CompressorUtility - Metadata") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("Original size is preserved") {
        std::string test_data(500, 'x');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK(output.original_size == 500);
        CHECK(output.original_size == input.size());
    }

    SUBCASE("Compression ratio calculation") {
        std::string test_data(1000, 'y');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        double ratio = output.compression_ratio();
        double expected = static_cast<double>(output.size()) / 1000.0;

        CHECK(ratio == doctest::Approx(expected));
    }

    SUBCASE("Space savings calculation") {
        std::string test_data(1000, 'z');
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        double savings = output.space_savings();
        double expected = (1.0 - output.compression_ratio()) * 100.0;

        CHECK(savings == doctest::Approx(expected));
    }
}

TEST_CASE("CompressorUtility - Different Data Types") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("Binary data") {
        std::vector<unsigned char> binary_data = {0x00, 0x01, 0x02, 0xFF, 0xFE,
                                                  0xFD, 0x00, 0x01, 0x02};
        RawData input(binary_data);
        CompressedData output = compressor->process(input);

        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == binary_data.size());
    }

    SUBCASE("String with special characters") {
        std::string test_data = "Hello\n\tWorld!\r\n\0Special";
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == test_data.size());
    }

    SUBCASE("Unicode data") {
        std::string test_data = "Hello, 世界! 🌍";
        RawData input(test_data);
        CompressedData output = compressor->process(input);

        CHECK_FALSE(output.data.empty());
        CHECK(output.original_size == test_data.size());
    }
}

TEST_CASE("CompressorUtility - Consistency") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("Same input produces same output") {
        std::string test_data = "Test data for consistency check";
        RawData input1(test_data);
        RawData input2(test_data);

        CompressedData output1 = compressor->process(input1);
        CompressedData output2 = compressor->process(input2);

        CHECK(output1 == output2);
        CHECK(output1.data == output2.data);
        CHECK(output1.original_size == output2.original_size);
    }

    SUBCASE("Different compression levels produce different results") {
        std::string test_data(1000, 'a');
        RawData input(test_data);

        auto compressor1 = std::make_shared<CompressorUtility>();
        compressor1->set_compression_level(1);
        CompressedData output1 = compressor1->process(input);

        auto compressor9 = std::make_shared<CompressorUtility>();
        compressor9->set_compression_level(9);
        CompressedData output9 = compressor9->process(input);

        // Different levels should produce different compressed sizes
        // (Level 9 should generally be smaller or equal)
        CHECK(output9.size() <= output1.size());
    }
}

TEST_CASE("CompressorUtility - Edge Cases") {
    auto compressor = std::make_shared<CompressorUtility>();

    SUBCASE("All zeros") {
        std::vector<unsigned char> zeros(1000, 0);
        RawData input(zeros);
        CompressedData output = compressor->process(input);

        // Should compress extremely well
        CHECK(output.compression_ratio() < 0.05);
    }

    SUBCASE("All ones") {
        std::vector<unsigned char> ones(1000, 0xFF);
        RawData input(ones);
        CompressedData output = compressor->process(input);

        // Should compress extremely well
        CHECK(output.compression_ratio() < 0.05);
    }

    SUBCASE("Alternating pattern") {
        std::vector<unsigned char> pattern;
        for (int i = 0; i < 1000; ++i) {
            pattern.push_back(i % 2 ? 0xFF : 0x00);
        }
        RawData input(pattern);
        CompressedData output = compressor->process(input);

        CHECK(output.size() < input.size());
    }
}
