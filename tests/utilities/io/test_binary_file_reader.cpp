#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/io/binary_file_reader_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <vector>

using namespace dftracer::utils::utilities::io;
using namespace dftracer::utils::utilities::filesystem;

TEST_CASE("BinaryFileReaderUtility - Basic Operations") {
    BinaryFileReaderUtility reader;
    fs::path test_file = "test_binary_file_reader.bin";

    SUBCASE("Read simple binary file") {
        // Create test file with binary data
        {
            std::ofstream ofs(test_file, std::ios::binary);
            unsigned char data[] = {0x01, 0x02, 0x03, 0x04, 0xFF, 0xFE};
            ofs.write(reinterpret_cast<char*>(data), sizeof(data));
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 6);
        CHECK(content.data[0] == 0x01);
        CHECK(content.data[4] == 0xFF);
        CHECK(content.data[5] == 0xFE);

        fs::remove(test_file);
    }

    SUBCASE("Read empty file") {
        // Create empty file
        {
            std::ofstream ofs(test_file, std::ios::binary);
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.data.empty());
        CHECK(content.size() == 0);

        fs::remove(test_file);
    }

    SUBCASE("Read file with null bytes") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            unsigned char data[] = {0x00, 0x00, 0x00, 0x01, 0x00};
            ofs.write(reinterpret_cast<char*>(data), sizeof(data));
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 5);
        CHECK(content.data[0] == 0x00);
        CHECK(content.data[3] == 0x01);

        fs::remove(test_file);
    }
}

TEST_CASE("BinaryFileReaderUtility - Error Handling") {
    BinaryFileReaderUtility reader;

    SUBCASE("Non-existent file") {
        FileEntry entry{"non_existent_binary_file_12345.bin"};
        CHECK_THROWS_AS(reader.process(entry), std::runtime_error);
    }

    SUBCASE("Directory instead of file") {
        fs::path test_dir = "test_dir_binary_reader";
        fs::create_directory(test_dir);

        FileEntry entry{test_dir};
        CHECK_THROWS_AS(reader.process(entry), std::runtime_error);

        fs::remove(test_dir);
    }
}

TEST_CASE("BinaryFileReaderUtility - Different File Sizes") {
    BinaryFileReaderUtility reader;
    fs::path test_file = "test_binary_sizes.bin";

    SUBCASE("Single byte") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            unsigned char byte = 0xAB;
            ofs.write(reinterpret_cast<char*>(&byte), 1);
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 1);
        CHECK(content.data[0] == 0xAB);

        fs::remove(test_file);
    }

    SUBCASE("1KB file") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            std::vector<unsigned char> data(1024, 0xCC);
            ofs.write(reinterpret_cast<char*>(data.data()), data.size());
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 1024);
        CHECK(content.data[0] == 0xCC);
        CHECK(content.data[1023] == 0xCC);

        fs::remove(test_file);
    }

    SUBCASE("Large file (100KB)") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            std::vector<unsigned char> data(100 * 1024, 0xDD);
            ofs.write(reinterpret_cast<char*>(data.data()), data.size());
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 100 * 1024);
        CHECK(content.data[0] == 0xDD);
        CHECK(content.data[100 * 1024 - 1] == 0xDD);

        fs::remove(test_file);
    }
}

TEST_CASE("BinaryFileReaderUtility - Different Data Patterns") {
    BinaryFileReaderUtility reader;
    fs::path test_file = "test_binary_patterns.bin";

    SUBCASE("Sequential bytes") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            for (unsigned char i = 0; i < 255; ++i) {
                ofs.write(reinterpret_cast<char*>(&i), 1);
            }
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 255);
        for (size_t i = 0; i < 255; ++i) {
            CHECK(content.data[i] == static_cast<unsigned char>(i));
        }

        fs::remove(test_file);
    }

    SUBCASE("Alternating pattern") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            for (int i = 0; i < 100; ++i) {
                unsigned char byte = (i % 2 == 0) ? 0xAA : 0x55;
                ofs.write(reinterpret_cast<char*>(&byte), 1);
            }
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 100);
        CHECK(content.data[0] == 0xAA);
        CHECK(content.data[1] == 0x55);
        CHECK(content.data[98] == 0xAA);
        CHECK(content.data[99] == 0x55);

        fs::remove(test_file);
    }

    SUBCASE("All zeros") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            std::vector<unsigned char> zeros(1000, 0x00);
            ofs.write(reinterpret_cast<char*>(zeros.data()), zeros.size());
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 1000);
        for (size_t i = 0; i < 1000; ++i) {
            CHECK(content.data[i] == 0x00);
        }

        fs::remove(test_file);
    }

    SUBCASE("All ones") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            std::vector<unsigned char> ones(1000, 0xFF);
            ofs.write(reinterpret_cast<char*>(ones.data()), ones.size());
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 1000);
        for (size_t i = 0; i < 1000; ++i) {
            CHECK(content.data[i] == 0xFF);
        }

        fs::remove(test_file);
    }
}

TEST_CASE("BinaryFileReaderUtility - Real World Scenarios") {
    BinaryFileReaderUtility reader;
    fs::path test_file = "test_binary_real_world.bin";

    SUBCASE("Image header (PNG-like)") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            // PNG header: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A
            unsigned char png_header[] = {0x89, 0x50, 0x4E, 0x47,
                                          0x0D, 0x0A, 0x1A, 0x0A};
            ofs.write(reinterpret_cast<char*>(png_header), sizeof(png_header));
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 8);
        CHECK(content.data[0] == 0x89);
        CHECK(content.data[1] == 0x50);  // 'P'
        CHECK(content.data[2] == 0x4E);  // 'N'
        CHECK(content.data[3] == 0x47);  // 'G'

        fs::remove(test_file);
    }

    SUBCASE("Multi-byte integers") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            // Write a 32-bit integer in big-endian
            uint32_t value = 0x12345678;
            unsigned char bytes[] = {
                static_cast<unsigned char>((value >> 24) & 0xFF),
                static_cast<unsigned char>((value >> 16) & 0xFF),
                static_cast<unsigned char>((value >> 8) & 0xFF),
                static_cast<unsigned char>(value & 0xFF)};
            ofs.write(reinterpret_cast<char*>(bytes), sizeof(bytes));
        }

        FileEntry entry{test_file};
        RawData content = reader.process(entry);

        CHECK(content.size() == 4);
        CHECK(content.data[0] == 0x12);
        CHECK(content.data[1] == 0x34);
        CHECK(content.data[2] == 0x56);
        CHECK(content.data[3] == 0x78);

        fs::remove(test_file);
    }
}

TEST_CASE("BinaryFileReaderUtility - Composition with DirectoryScanner") {
    BinaryFileReaderUtility reader;
    DirectoryScannerUtility scanner;

    // Create test directory with binary files
    fs::path test_dir = "test_binary_composition_dir";
    fs::create_directory(test_dir);

    {
        std::ofstream ofs(test_dir / "file1.bin", std::ios::binary);
        unsigned char data[] = {0x01, 0x02, 0x03};
        ofs.write(reinterpret_cast<char*>(data), sizeof(data));
    }
    {
        std::ofstream ofs(test_dir / "file2.bin", std::ios::binary);
        unsigned char data[] = {0xAA, 0xBB, 0xCC, 0xDD};
        ofs.write(reinterpret_cast<char*>(data), sizeof(data));
    }

    // Scan and read
    DirectoryScannerUtilityInput dir{test_dir};
    auto files = scanner.process(dir);

    int files_read = 0;
    for (const auto& file : files) {
        if (file.is_regular_file) {
            RawData content = reader.process(file);
            CHECK(content.size() > 0);
            files_read++;
        }
    }

    CHECK(files_read == 2);

    // Cleanup
    fs::remove_all(test_dir);
}
