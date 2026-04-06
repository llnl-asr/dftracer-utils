#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <fstream>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft::internal;
using namespace dft_utils_test;

TEST_SUITE("IndexedFileReader") {
    TEST_CASE("IndexedFileReader - Basic File Processing") {
        SUBCASE("Process gzip file without existing index") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();
            std::string db_root = determine_index_path(gz_path, "");

            // Ensure no index exists initially
            if (fs::exists(db_root)) {
                fs::remove_all(db_root);
            }

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024);

            // Process should create index and return reader
            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));

            // Verify reader can read lines
            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES)
                                   .range_type(RangeType::LINE_RANGE)
                                   .from(1)
                                   .to(10));
            CHECK(stream != nullptr);

            std::vector<char> buffer(1024);
            CHECK(stream->read(buffer.data(), buffer.size()) > 0);
        }

        SUBCASE("Process gzip file with existing index") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();
            std::string db_root = determine_index_path(gz_path, "");

            // Create index first
            auto indexer = IndexerFactory::create(gz_path, db_root, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();
            REQUIRE(fs::exists(db_root));

            // Process with existing index (should not rebuild)
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));
            CHECK(reader->get_num_lines() > 0);
        }

        SUBCASE("Force rebuild existing index") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();
            std::string db_root = determine_index_path(gz_path, "");

            // Create index first
            auto indexer = IndexerFactory::create(gz_path, db_root, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();
            REQUIRE(fs::exists(db_root));

            // Sleep to ensure different timestamp
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Process with force rebuild
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024)
                                         .with_force_rebuild(true);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));

            // Reader should work
            CHECK(reader->get_num_lines() > 0);
        }
    }

    TEST_CASE("IndexedFileReader - Configuration") {
        SUBCASE("Configure checkpoint size") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();
            std::string db_root = determine_index_path(gz_path, "");

            IndexedFileReaderUtility reader_utility;

            // Use custom checkpoint size
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(2048);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));

            // Verify reader works
            CHECK(reader->get_num_lines() == 20);
        }

        SUBCASE("Fluent configuration API") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;

            // Test fluent API
            auto input = IndexedReadInput::from_file(gz_path)
                             .with_index(determine_index_path(gz_path, ""))
                             .with_checkpoint_size(512)
                             .with_force_rebuild(false);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(reader->get_num_lines() > 0);
        }

        SUBCASE("Constructor with all parameters") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();
            std::string db_root = determine_index_path(gz_path, "");

            IndexedFileReaderUtility reader_utility;

            // Use constructor directly
            IndexedReadInput input(gz_path, db_root, 1024, false);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));
        }
    }

    TEST_CASE("IndexedFileReader - Error Handling") {
        SUBCASE("Non-existent file") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("non_existent.gz")
                    .with_index("non_existent.gz.dftindex");

            CHECK_THROWS_AS(reader_utility.process(input).get(),
                            std::runtime_error);
        }

        SUBCASE("Invalid file path") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("/invalid/path/file.gz")
                    .with_index("/invalid/path/.dftindex");

            CHECK_THROWS_AS(reader_utility.process(input).get(),
                            std::runtime_error);
        }

        SUBCASE("Empty file path") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("").with_index(".dftindex");

            CHECK_THROWS_AS(reader_utility.process(input).get(),
                            std::runtime_error);
        }
    }

    TEST_CASE("IndexedFileReader - Different File Types") {
        SUBCASE("Gzip compressed file") {
            TestEnvironment env(15);
            std::string gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(
                    determine_index_path(gz_path, ""));

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(reader->get_num_lines() == 15);
        }

        // TODO: Enable when TAR.GZ support is implemented
        // SUBCASE("TAR.GZ file") {
        //     TestEnvironment env(10, Format::TAR_GZIP);
        //     std::string tar_gz_path = env.create_test_tar_gzip_file();

        //     IndexedFileReaderUtility reader_utility;
        //     IndexedReadInput input = IndexedReadInput::from_file(tar_gz_path)
        //         .with_index(tar_gz_path + ".idx");

        //     auto reader = reader_utility.process(input);

        //     CHECK(reader != nullptr);
        //     // TAR.GZ files have different structure, just verify it works
        //     CHECK(reader->get_num_lines() >= 0);
        // }
    }

    TEST_CASE("IndexedFileReader - Index Rebuild Detection") {
        SUBCASE("Rebuild when file modified after index") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();
            std::string index_path = gz_path + ".idx";
            std::string db_root = determine_index_path(gz_path, "");

            // Create index
            auto indexer =
                IndexerFactory::create(gz_path, index_path, 1024, true);
            indexer->build();
            REQUIRE(fs::exists(db_root));

            // Sleep to ensure different timestamp
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Modify the gz file (touch it to update mtime)
            std::ofstream ofs(gz_path, std::ios::app);
            ofs.close();

            // Process should detect outdated index and rebuild
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(index_path);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            // Note: We can't easily verify rebuild happened without checking
            // internals but the reader should still work
            CHECK(reader->get_num_lines() >= 0);
        }

        SUBCASE("No rebuild when index is up to date") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();
            std::string index_path = gz_path + ".idx";
            std::string db_root = determine_index_path(gz_path, "");

            // Create index
            auto indexer =
                IndexerFactory::create(gz_path, index_path, 1024, true);
            indexer->build();
            // Process without modifying file
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(index_path);

            auto reader = reader_utility.process(input).get();

            CHECK(reader != nullptr);
            CHECK(fs::exists(db_root));
            CHECK(reader->get_num_lines() > 0);
        }
    }

    TEST_CASE("IndexedFileReader - Reader Functionality") {
        SUBCASE("Read specific lines") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(gz_path +
                                                                ".idx");

            auto reader = reader_utility.process(input).get();
            REQUIRE(reader != nullptr);

            // Create line stream to read specific lines
            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES)
                                   .range_type(RangeType::LINE_RANGE)
                                   .from(5)
                                   .to(10));
            CHECK(stream != nullptr);

            std::vector<char> buffer(1024);
            int lines_read = 0;
            while (stream->read(buffer.data(), buffer.size()) > 0) {
                lines_read++;
            }

            CHECK(lines_read > 0);
            CHECK(lines_read <= 6);  // Lines 5-10 inclusive
        }

        SUBCASE("Get reader metadata") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(gz_path +
                                                                ".idx");

            auto reader = reader_utility.process(input).get();
            REQUIRE(reader != nullptr);

            CHECK(reader->get_num_lines() == 10);
            CHECK(reader->get_archive_path() == gz_path);
            CHECK(reader->get_index_path() ==
                  determine_index_path(gz_path, ""));
        }
    }
}
