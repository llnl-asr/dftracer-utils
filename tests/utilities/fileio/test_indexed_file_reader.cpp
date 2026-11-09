// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/fileio/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dftracer::utils::utilities::fileio;
using namespace dftracer::utils::trace::internal;
using namespace dftu_utils_test;

TEST_SUITE("IndexedFileReader") {
    TEST_CASE("IndexedFileReader - Basic File Processing") {
        SUBCASE("Process gzip file without existing index") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            // Ensure no index exists initially
            if (dstd::fs::exists(db_root).value()) {
                dstd::fs::remove_all(db_root).discard();
            }

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024);

            // Process should create index and return reader
            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());

            // Verify reader can read lines
            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MultiLines)
                                   .range_type(RangeType::LineRange)
                                   .from(1)
                                   .to(10));
            CHECK(stream != nullptr);

            dstd::Vector<char> buffer(1024);
            CHECK(stream->read(buffer.data(), buffer.size()) > 0);
        }

        SUBCASE("Process gzip file with existing index") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            // Create index first
            auto indexer =
                CheckpointIndexerFactory::create(gz_path, db_root, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();
            REQUIRE(dstd::fs::exists(db_root).value());

            // Process with existing index (should not rebuild)
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());
            CHECK(reader->line_count() > 0);
        }

        SUBCASE("Force rebuild existing index") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            // Create index first
            auto indexer =
                CheckpointIndexerFactory::create(gz_path, db_root, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();
            REQUIRE(dstd::fs::exists(db_root).value());

            // Sleep to ensure different timestamp
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Process with force rebuild
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(1024)
                                         .with_force_rebuild(true);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());

            // Reader should work
            CHECK(reader->line_count() > 0);
        }
    }

    TEST_CASE("IndexedFileReader - Configuration") {
        SUBCASE("Configure checkpoint size") {
            TestEnvironment env(20);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            IndexedFileReaderUtility reader_utility;

            // Use custom checkpoint size
            IndexedReadInput input = IndexedReadInput::from_file(gz_path)
                                         .with_index(db_root)
                                         .with_checkpoint_size(2048);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());

            // Verify reader works
            CHECK(reader->line_count() == 20);
        }

        SUBCASE("Fluent configuration API") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;

            // Test fluent API
            auto input =
                IndexedReadInput::from_file(gz_path)
                    .with_index(dstd::String(determine_index_path(gz_path, "")))
                    .with_checkpoint_size(512)
                    .with_force_rebuild(false);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(reader->line_count() > 0);
        }

        SUBCASE("Constructor with all parameters") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            IndexedFileReaderUtility reader_utility;

            // Use constructor directly
            IndexedReadInput input(gz_path, db_root, 1024, false);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());
        }
    }

    TEST_CASE("IndexedFileReader - Error Handling") {
        SUBCASE("Non-existent file") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("non_existent.gz")
                    .with_index("non_existent.gz.dftindex");

            CHECK_THROWS_AS(reader_utility(input).get(),
                            dftracer::utils::common::DFTUtilsException);
        }

        SUBCASE("Invalid file path") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("/invalid/path/file.gz")
                    .with_index("/invalid/path/.dftindex");

            CHECK_THROWS_AS(reader_utility(input).get(),
                            dftracer::utils::common::DFTUtilsException);
        }

        SUBCASE("Empty file path") {
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file("").with_index(".dftindex");

            CHECK_THROWS_AS(reader_utility(input).get(),
                            dftracer::utils::common::DFTUtilsException);
        }
    }

    TEST_CASE("IndexedFileReader - Different File Types") {
        SUBCASE("Gzip compressed file") {
            TestEnvironment env(15);
            dstd::String gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(
                    dstd::String(determine_index_path(gz_path, "")));

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(reader->line_count() == 15);
        }

        // TODO: Enable when TAR.GZ support is implemented
        // SUBCASE("TAR.GZ file") {
        //     TestEnvironment env(10, Format::TAR_GZIP);
        //     std::string tar_gz_path = env.create_test_tar_gzip_file();

        //     IndexedFileReaderUtility reader_utility;
        //     IndexedReadInput input = IndexedReadInput::from_file(tar_gz_path)
        //         .with_index(tar_gz_path + ".idx");

        //     auto reader = reader_utility(input);

        //     CHECK(reader != nullptr);
        //     // TAR.GZ files have different structure, just verify it works
        //     CHECK(reader->line_count() >= 0);
        // }
    }

    TEST_CASE("IndexedFileReader - Index Rebuild Detection") {
        SUBCASE("Rebuild when file modified after index") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String index_path = gz_path + ".idx";
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            // Create index
            auto indexer = CheckpointIndexerFactory::create(gz_path, index_path,
                                                            1024, true);
            indexer->build();
            REQUIRE(dstd::fs::exists(db_root).value());

            // Sleep to ensure different timestamp
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Modify the gz file (touch it to update mtime)
            dstd::String ofs;
            dstd::fs::append_file(dstd::PathView(gz_path), ofs).discard();

            // Process should detect outdated index and rebuild
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(index_path);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            // Note: We can't easily verify rebuild happened without checking
            // internals but the reader should still work
            CHECK(reader->line_count() >= 0);
        }

        SUBCASE("No rebuild when index is up to date") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();
            dstd::String index_path = gz_path + ".idx";
            dstd::String db_root =
                dstd::String(determine_index_path(gz_path, ""));

            // Create index
            auto indexer = CheckpointIndexerFactory::create(gz_path, index_path,
                                                            1024, true);
            indexer->build();
            // Process without modifying file
            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(index_path);

            auto reader = reader_utility(input).get();

            CHECK(reader != nullptr);
            CHECK(dstd::fs::exists(db_root).value());
            CHECK(reader->line_count() > 0);
        }
    }

    TEST_CASE("IndexedFileReader - Reader Functionality") {
        SUBCASE("Read specific lines") {
            TestEnvironment env(20);
            dstd::String gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(gz_path +
                                                                ".idx");

            auto reader = reader_utility(input).get();
            REQUIRE(reader != nullptr);

            // Create line stream to read specific lines
            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MultiLines)
                                   .range_type(RangeType::LineRange)
                                   .from(5)
                                   .to(10));
            CHECK(stream != nullptr);

            dstd::Vector<char> buffer(1024);
            int lines_read = 0;
            while (stream->read(buffer.data(), buffer.size()) > 0) {
                lines_read++;
            }

            CHECK(lines_read > 0);
            CHECK(lines_read <= 6);  // Lines 5-10 inclusive
        }

        SUBCASE("Get reader metadata") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            IndexedFileReaderUtility reader_utility;
            IndexedReadInput input =
                IndexedReadInput::from_file(gz_path).with_index(gz_path +
                                                                ".idx");

            auto reader = reader_utility(input).get();
            REQUIRE(reader != nullptr);

            CHECK(reader->line_count() == 10);
            CHECK(dstd::StringView(reader->archive_path()) == gz_path);
            CHECK(dstd::StringView(reader->index_path()) ==
                  dstd::String(determine_index_path(gz_path, "")));
        }
    }
}
