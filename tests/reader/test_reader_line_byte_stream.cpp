// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <checkpoint_indexer_helpers.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/reader/internal/reader.h>
#include <dftracer/utils/trace/reader/internal/reader_factory.h>
#include <dftracer/utils/trace/reader/internal/stream.h>
#include <dftracer/utils/trace/reader/internal/stream_type.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index::gzip;
using namespace dftracer::utils::trace::reader::internal;
using namespace dftu_utils_test;

// Helper function to create a test file with variable line lengths
static dstd::String create_variable_line_test_file(
    const dstd::String& test_dir, std::size_t line_count,
    std::size_t min_line_size = 50, std::size_t max_line_size = 500) {
    dstd::String txt_file = test_dir + "/variable_lines.txt";
    dstd::String gz_file = test_dir + "/variable_lines.gz";

    // Create text file with variable line lengths
    dftu_utils_test::FileOut f(txt_file);
    if (!f.is_open()) {
        return "";
    }

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<std::size_t> dist(min_line_size,
                                                    max_line_size);

    for (std::size_t i = 1; i <= line_count; ++i) {
        std::size_t line_size = dist(rng);

        // Create JSON line with variable padding to reach desired length
        dftu_utils_test::TextOut line;
        line << "{\"id\":" << i << ",\"name\":\"event_" << i
             << "\",\"data\":\"";

        // Fill with padding to reach target length
        std::size_t current_size = line.str().length();
        std::size_t padding_needed = (line_size > current_size + 10)
                                         ? (line_size - current_size - 10)
                                         : 0;

        for (std::size_t j = 0; j < padding_needed; ++j) {
            line << static_cast<char>('a' + (j % 26));
        }

        line << "\",\"ts\":" << (1000000 + i * 100) << "}";

        f << line.str() << "\n";
    }
    f.close();

    // Compress to gzip
    if (!compress_file_to_gzip(txt_file, gz_file)) {
        return "";
    }

    dstd::fs::remove(txt_file).discard();
    return gz_file;
}

TEST_CASE("LINE_BYTES Stream - Sequential Read Tests") {
    SUBCASE("Complete file read - single chunk") {
        TestEnvironment env(valgrind_scale(1000, 10));
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), valgrind_scale(1000, 10));
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        // Build index
        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(0.5));
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);
        REQUIRE(reader->is_valid());

        std::size_t max_bytes = reader->max_bytes();
        auto stream = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(max_bytes)));
        REQUIRE(stream != nullptr);

        // Read entire file
        dstd::Vector<char> buffer(1024 * 1024);  // 1MB buffer
        dstd::String result;

        while (!stream->done()) {
            std::size_t bytes_read =
                dftu_utils_test::ok(stream->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                result.append(buffer.data(), bytes_read);
            }
        }

        // Verify we got complete lines
        CHECK(!result.empty());

        // Count lines (should end with newline)
        std::size_t line_count = std::count(result.begin(), result.end(), '\n');
        CHECK(line_count > 0);
        CHECK(line_count <= 1000);
    }

    SUBCASE("Boundary alignment - 4MB chunks") {
        TestEnvironment env(valgrind_scale(10000, 100));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(10000, 100), 100, 300);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(1.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        std::size_t chunk_size = max_bytes / 2;  // Split file in half

        // Read first chunk
        auto stream1 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(chunk_size)));
        REQUIRE(stream1 != nullptr);

        dstd::Vector<char> buffer(1024 * 1024);
        dstd::String chunk1;
        while (!stream1->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream1->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk1.append(buffer.data(), bytes_read);
            }
        }

        // Read second chunk
        auto stream2 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(chunk_size)
                               .to(max_bytes)));
        REQUIRE(stream2 != nullptr);

        dstd::String chunk2;
        while (!stream2->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream2->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk2.append(buffer.data(), bytes_read);
            }
        }

        // Verify both chunks have content
        CHECK(!chunk1.empty());
        CHECK(!chunk2.empty());

        // Verify no overlap - last line of chunk1 should be different from
        // first line of chunk2
        auto chunk1_last_newline = chunk1.rfind('\n');
        auto chunk2_first_newline = chunk2.find('\n');

        if (chunk1_last_newline != dstd::String::npos &&
            chunk2_first_newline != dstd::String::npos) {
            dstd::String last_line1 = chunk1.substr(chunk1_last_newline + 1);
            dstd::String first_line2 = chunk2.substr(0, chunk2_first_newline);

            // They should be different (no overlap)
            CHECK(last_line1 != first_line2);
        }
    }

    SUBCASE("Adjacent chunks concatenation equals full read") {
        TestEnvironment env(valgrind_scale(5000, 50));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(5000, 50), 80, 250);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                dftu_utils_test::create_indexer(gz_file, idx_file, mb_to_b(1));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        std::size_t mid_point = max_bytes / 2;

        dstd::Vector<char> buffer(512 * 1024);

        // Read first half
        auto stream1 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(mid_point)));
        dstd::String chunk1;
        while (!stream1->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream1->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk1.append(buffer.data(), bytes_read);
            }
        }

        // Read second half
        auto stream2 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(mid_point)
                               .to(max_bytes)));
        dstd::String chunk2;
        while (!stream2->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream2->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk2.append(buffer.data(), bytes_read);
            }
        }

        // Read full file
        auto stream_full = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(max_bytes)));
        dstd::String full;
        while (!stream_full->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream_full->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                full.append(buffer.data(), bytes_read);
            }
        }

        // Concatenate chunks
        dstd::String concatenated = chunk1 + chunk2;

        // Sort both for comparison (to handle potential ordering differences)
        dstd::Vector<dstd::String> concat_lines, full_lines;

        dftu_utils_test::LineStream concat_stream(concatenated);
        dstd::String line;
        while (concat_stream.next(line)) {
            concat_lines.push_back(line);
        }

        dftu_utils_test::LineStream full_stream(full);
        while (full_stream.next(line)) {
            full_lines.push_back(line);
        }

        std::sort(concat_lines.begin(), concat_lines.end());
        std::sort(full_lines.begin(), full_lines.end());

        // Verify same line count
        CHECK(concat_lines.size() == full_lines.size());

        // Verify all lines match (allowing for ordering differences)
        if (concat_lines.size() == full_lines.size()) {
            CHECK(concat_lines == full_lines);
        }
    }

    SUBCASE("Multiple small chunks - no gaps or overlaps") {
        TestEnvironment env(valgrind_scale(3000, 30));
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), valgrind_scale(3000, 30));
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(0.5));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        std::size_t chunk_count = 8;
        std::size_t chunk_size = max_bytes / chunk_count;

        dstd::Vector<dstd::String> chunks;
        dstd::Vector<char> buffer(256 * 1024);

        // Read all chunks
        for (std::size_t i = 0; i < chunk_count; ++i) {
            std::size_t start = i * chunk_size;
            std::size_t end =
                (i == chunk_count - 1) ? max_bytes : (i + 1) * chunk_size;

            auto stream = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(start)
                                   .to(end)));
            REQUIRE(stream != nullptr);

            dstd::String chunk;
            while (!stream->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) {
                    chunk.append(buffer.data(), bytes_read);
                }
            }

            chunks.push_back(chunk);
        }

        // Extract all lines from all chunks
        dstd::Vector<dstd::String> all_lines;
        for (const auto& chunk : chunks) {
            dftu_utils_test::LineStream stream(chunk);
            dstd::String line;
            while (stream.next(line)) {
                if (!line.empty()) {
                    all_lines.push_back(line);
                }
            }
        }

        // Check for duplicates (sort and check unique)
        dstd::Vector<dstd::String> sorted_lines = all_lines;
        std::sort(sorted_lines.begin(), sorted_lines.end());

        auto last = std::unique(sorted_lines.begin(), sorted_lines.end());
        std::size_t unique_count = std::distance(sorted_lines.begin(), last);

        // All lines should be unique (no overlaps)
        CHECK(unique_count == all_lines.size());

        // Should have reasonable number of lines
        CHECK(all_lines.size() > 0);
        CHECK(all_lines.size() <= 3000);
    }
}

TEST_CASE("LINE_BYTES Stream - Parallel/Threaded Read Tests" *
          doctest::test_suite("vg")) {
    SUBCASE("Parallel read with 4 threads") {
        TestEnvironment env(valgrind_scale(10000, 100));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(10000, 100), 100, 400);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(2.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        std::size_t max_bytes;
        {
            auto reader =
                dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
            REQUIRE(reader != nullptr);
            max_bytes = reader->max_bytes();
        }

        const int thread_count = valgrind_threads(4);
        std::size_t chunk_size = max_bytes / thread_count;

        dstd::Vector<dstd::String> chunks(thread_count);
        dstd::Vector<dstd::Thread> threads;
        std::atomic<bool> error_occurred{false};

        // Launch threads to read different chunks in parallel
        // Each thread creates its own reader to avoid sharing indexer
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    // Create separate reader per thread
                    auto reader = dftu_utils_test::ok(
                        ReaderFactory::create(gz_file, idx_file));
                    if (!reader) {
                        error_occurred = true;
                        return;
                    }

                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == thread_count - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = dftu_utils_test::ok(
                        reader->stream(StreamConfig()
                                           .stream_type(StreamType::LineBytes)
                                           .range_type(RangeType::ByteRange)
                                           .from(start)
                                           .to(end)));
                    if (!stream) {
                        error_occurred = true;
                        return;
                    }

                    dstd::Vector<char> buffer(512 * 1024);
                    dstd::String& chunk = chunks[i];

                    while (!stream->done()) {
                        std::size_t bytes_read = dftu_utils_test::ok(
                            stream->read(buffer.data(), buffer.size()));
                        if (bytes_read > 0) {
                            chunk.append(buffer.data(), bytes_read);
                        }
                    }
                } catch (...) {
                    error_occurred = true;
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }

        CHECK(!error_occurred);

        // Verify all chunks have content
        for (const auto& chunk : chunks) {
            CHECK(!chunk.empty());
        }

        // Collect all lines from all chunks
        dstd::Vector<dstd::String> all_lines;
        for (const auto& chunk : chunks) {
            dftu_utils_test::LineStream stream(chunk);
            dstd::String line;
            while (stream.next(line)) {
                if (!line.empty()) {
                    all_lines.push_back(line);
                }
            }
        }

        // Sort and check for duplicates
        dstd::Vector<dstd::String> sorted_lines = all_lines;
        std::sort(sorted_lines.begin(), sorted_lines.end());

        auto last = std::unique(sorted_lines.begin(), sorted_lines.end());
        std::size_t unique_count = std::distance(sorted_lines.begin(), last);

        INFO("Total lines: ", all_lines.size());
        INFO("Unique lines: ", unique_count);

        // All lines should be unique (no overlaps between threads)
        CHECK(unique_count == all_lines.size());
    }

    SUBCASE("Parallel read with 16 threads - stress test") {
        TestEnvironment env(valgrind_scale(20000, 200));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(20000, 200), 80, 350);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(4.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        std::size_t max_bytes;
        {
            auto reader =
                dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
            REQUIRE(reader != nullptr);
            max_bytes = reader->max_bytes();
        }

        const int thread_count = valgrind_threads(16);
        std::size_t chunk_size = max_bytes / thread_count;

        dstd::Vector<dstd::String> chunks(thread_count);
        dstd::Vector<dstd::Thread> threads;
        std::atomic<int> errors{0};
        std::atomic<int> completed{0};

        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    // Create separate reader per thread
                    auto reader = dftu_utils_test::ok(
                        ReaderFactory::create(gz_file, idx_file));
                    if (!reader) {
                        errors++;
                        return;
                    }

                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == thread_count - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = dftu_utils_test::ok(
                        reader->stream(StreamConfig()
                                           .stream_type(StreamType::LineBytes)
                                           .range_type(RangeType::ByteRange)
                                           .from(start)
                                           .to(end)));
                    if (!stream) {
                        errors++;
                        return;
                    }

                    dstd::Vector<char> buffer(256 * 1024);
                    dstd::String& chunk = chunks[i];

                    while (!stream->done()) {
                        std::size_t bytes_read = dftu_utils_test::ok(
                            stream->read(buffer.data(), buffer.size()));
                        if (bytes_read > 0) {
                            chunk.append(buffer.data(), bytes_read);
                        }
                    }

                    completed++;
                } catch (...) {
                    errors++;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        INFO("Completed threads: ", completed.load());
        INFO("Errors: ", errors.load());

        CHECK(errors == 0);
        CHECK(completed == thread_count);

        // Verify completeness
        dstd::Vector<dstd::String> all_lines;
        for (const auto& chunk : chunks) {
            dftu_utils_test::LineStream stream(chunk);
            dstd::String line;
            while (stream.next(line)) {
                if (!line.empty()) {
                    all_lines.push_back(line);
                }
            }
        }

        std::sort(all_lines.begin(), all_lines.end());
        auto last = std::unique(all_lines.begin(), all_lines.end());
        std::size_t unique_count = std::distance(all_lines.begin(), last);

        CHECK(unique_count == all_lines.size());
        CHECK(all_lines.size() > 0);
    }

    SUBCASE("Concurrent reads of same range - thread safety") {
        TestEnvironment env(valgrind_scale(5000, 50));
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), valgrind_scale(5000, 50));
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(1.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        std::size_t max_bytes;
        {
            auto reader =
                dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
            REQUIRE(reader != nullptr);
            max_bytes = reader->max_bytes();
        }

        const int thread_count = valgrind_threads(8);

        dstd::Vector<dstd::String> results(thread_count);
        dstd::Vector<dstd::Thread> threads;
        std::atomic<bool> error_occurred{false};

        // All threads read the SAME range (but with separate reader instances)
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    // Create separate reader per thread
                    auto reader = dftu_utils_test::ok(
                        ReaderFactory::create(gz_file, idx_file));
                    if (!reader) {
                        error_occurred = true;
                        return;
                    }

                    auto stream = dftu_utils_test::ok(
                        reader->stream(StreamConfig()
                                           .stream_type(StreamType::LineBytes)
                                           .range_type(RangeType::ByteRange)
                                           .from(0)
                                           .to(max_bytes)));
                    if (!stream) {
                        error_occurred = true;
                        return;
                    }

                    dstd::Vector<char> buffer(512 * 1024);
                    dstd::String& result = results[i];

                    while (!stream->done()) {
                        std::size_t bytes_read = dftu_utils_test::ok(
                            stream->read(buffer.data(), buffer.size()));
                        if (bytes_read > 0) {
                            result.append(buffer.data(), bytes_read);
                        }
                    }
                } catch (...) {
                    error_occurred = true;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        CHECK(!error_occurred);

        // All results should have content
        for (const auto& result : results) {
            CHECK(!result.empty());
        }

        // Sort each result's lines and compare - they should all be the same
        dstd::Vector<dstd::Vector<dstd::String>> sorted_results;
        for (const auto& result : results) {
            dstd::Vector<dstd::String> lines;
            dftu_utils_test::LineStream stream(result);
            dstd::String line;
            while (stream.next(line)) {
                if (!line.empty()) {
                    lines.push_back(line);
                }
            }
            std::sort(lines.begin(), lines.end());
            sorted_results.push_back(lines);
        }

        // All threads should have read the same data
        for (std::size_t i = 1; i < sorted_results.size(); ++i) {
            CHECK(sorted_results[0].size() == sorted_results[i].size());
            if (sorted_results[0].size() == sorted_results[i].size()) {
                CHECK(sorted_results[0] == sorted_results[i]);
            }
        }
    }
}

TEST_CASE("LINE_BYTES Stream - Edge Cases") {
    SUBCASE("Very small file") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(env.dir(), 10);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(0.1));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        auto stream = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(max_bytes)));
        REQUIRE(stream != nullptr);

        dstd::Vector<char> buffer(1024);
        dstd::String result;

        while (!stream->done()) {
            std::size_t bytes_read =
                dftu_utils_test::ok(stream->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                result.append(buffer.data(), bytes_read);
            }
        }

        CHECK(!result.empty());

        std::size_t line_count = std::count(result.begin(), result.end(), '\n');
        CHECK(line_count > 0);
        CHECK(line_count <= 10);
    }

    SUBCASE("Chunk boundary at exact line boundary") {
        TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // Create file with very uniform line lengths
        dstd::String txt_file = env.dir() + "/uniform.txt";
        dstd::String gz_file = env.dir() + "/uniform.gz";

        dftu_utils_test::FileOut f(txt_file);
        for (int i = 1; i <= 100; ++i) {
            f << "{\"id\":" << i << ",\"fixed\":\"XXXXXXXXXXXXXXXXXXXX\"}\n";
        }
        f.close();

        compress_file_to_gzip(txt_file, gz_file);
        dstd::fs::remove(txt_file).discard();

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(0.5));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        // Read in chunks that might align with line boundaries
        std::size_t max_bytes = reader->max_bytes();
        std::size_t mid = max_bytes / 2;

        auto stream1 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(mid)));
        auto stream2 = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(mid)
                               .to(max_bytes)));

        dstd::Vector<char> buffer(1024);
        dstd::String chunk1, chunk2;

        while (!stream1->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream1->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk1.append(buffer.data(), bytes_read);
            }
        }

        while (!stream2->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream2->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                chunk2.append(buffer.data(), bytes_read);
            }
        }

        CHECK(!chunk1.empty());
        CHECK(!chunk2.empty());

        // Verify both end/start with complete lines
        CHECK(chunk1.back() == '\n');
        CHECK(chunk2.back() == '\n');
    }

    SUBCASE("Empty range returns no data") {
        TestEnvironment env(100);
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(env.dir(), 100);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(0.5));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        // Request empty range
        auto stream = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(100)
                               .to(100)));
        REQUIRE(stream != nullptr);

        dstd::Vector<char> buffer(1024);
        dstd::String result;

        while (!stream->done()) {
            std::size_t bytes_read =
                dftu_utils_test::ok(stream->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                result.append(buffer.data(), bytes_read);
            }
            if (bytes_read == 0) {
                break;  // Prevent infinite loop
            }
        }

        // Should return minimal or no data
        CHECK(result.size() == 0);
    }
}

TEST_CASE("LINE_BYTES Stream - Arbitrary Boundary Tests" *
          doctest::test_suite("vg")) {
    SUBCASE("Specific boundary: 4MB (4194304 bytes)") {
        TestEnvironment env(valgrind_scale(10000, 100));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(10000, 100), 450, 550);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(2.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        // 4MB at full scale; mid-file when scaled down under Valgrind
        std::size_t boundary = (max_bytes > 4194304) ? 4194304 : max_bytes / 2;

        // Only test if file is large enough
        if (max_bytes > boundary) {
            dstd::Vector<char> buffer(512 * 1024);

            // Read chunk 1: [0, 4194304)
            auto stream1 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(0)
                                   .to(boundary)));
            dstd::String chunk1;
            while (!stream1->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream1->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) {
                    chunk1.append(buffer.data(), bytes_read);
                }
            }

            // Read chunk 2: [4194304, end)
            auto stream2 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(boundary)
                                   .to(max_bytes)));
            dstd::String chunk2;
            while (!stream2->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream2->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) {
                    chunk2.append(buffer.data(), bytes_read);
                }
            }

            CHECK(!chunk1.empty());
            CHECK(!chunk2.empty());

            // Check for overlap by looking at last/first lines
            dstd::Vector<dstd::String> lines1, lines2;
            dftu_utils_test::LineStream s1(chunk1);
            dstd::String line;
            while (s1.next(line)) {
                if (!line.empty()) lines1.push_back(line);
            }

            dftu_utils_test::LineStream s2(chunk2);
            while (s2.next(line)) {
                if (!line.empty()) lines2.push_back(line);
            }

            // Last line of chunk1 should be different from first line of chunk2
            if (!lines1.empty() && !lines2.empty()) {
                CHECK(lines1.back() != lines2.front());
            }
        }
    }

    SUBCASE("Prime number boundary") {
        TestEnvironment env(valgrind_scale(5000, 50));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(5000, 50), 230, 350);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(1.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        // Prime offset; large prime at full scale, smaller when scaled down
        std::size_t boundary = (max_bytes > 1299709) ? 1299709 : 10007;

        if (max_bytes > boundary) {
            dstd::Vector<char> buffer(256 * 1024);

            auto stream1 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(0)
                                   .to(boundary)));
            auto stream2 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(boundary)
                                   .to(max_bytes)));
            dstd::String chunk1, chunk2;

            while (!stream1->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream1->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) chunk1.append(buffer.data(), bytes_read);
            }

            while (!stream2->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream2->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) chunk2.append(buffer.data(), bytes_read);
            }

            // Concatenate and verify completeness
            dstd::String concat = chunk1 + chunk2;

            // Read full file for comparison
            auto stream_full = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(0)
                                   .to(max_bytes)));
            dstd::String full;
            while (!stream_full->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream_full->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) full.append(buffer.data(), bytes_read);
            }

            // Sort and compare to handle ordering differences
            dstd::Vector<dstd::String> concat_sorted, full_sorted;
            dftu_utils_test::LineStream cs(concat);
            dstd::String line;
            while (cs.next(line)) {
                if (!line.empty()) concat_sorted.push_back(line);
            }
            dftu_utils_test::LineStream fs(full);
            while (fs.next(line)) {
                if (!line.empty()) full_sorted.push_back(line);
            }

            std::sort(concat_sorted.begin(), concat_sorted.end());
            std::sort(full_sorted.begin(), full_sorted.end());

            CHECK(concat_sorted.size() == full_sorted.size());
            CHECK(concat_sorted == full_sorted);
        }
    }

    SUBCASE("Off-by-one boundary variations") {
        TestEnvironment env(valgrind_scale(5000, 50));
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), valgrind_scale(5000, 50));
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(1.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        std::size_t mid = max_bytes / 2;

        dstd::Vector<char> buffer(256 * 1024);

        // Test boundary, boundary-1, and boundary+1
        for (int offset = -1; offset <= 1; ++offset) {
            std::size_t boundary = mid + offset;
            if (boundary == 0 || boundary >= max_bytes) continue;

            auto s1 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(0)
                                   .to(boundary)));
            auto s2 = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(boundary)
                                   .to(max_bytes)));

            dstd::String c1, c2;
            while (!s1->done()) {
                std::size_t br =
                    dftu_utils_test::ok(s1->read(buffer.data(), buffer.size()));
                if (br > 0) c1.append(buffer.data(), br);
            }
            while (!s2->done()) {
                std::size_t br =
                    dftu_utils_test::ok(s2->read(buffer.data(), buffer.size()));
                if (br > 0) c2.append(buffer.data(), br);
            }

            // Extract and sort lines
            dstd::Vector<dstd::String> all_lines;
            dftu_utils_test::LineStream stream(c1 + c2);
            dstd::String line;
            while (stream.next(line)) {
                if (!line.empty()) all_lines.push_back(line);
            }

            std::sort(all_lines.begin(), all_lines.end());

            // Check for duplicates
            auto last = std::unique(all_lines.begin(), all_lines.end());
            std::size_t unique_count = std::distance(all_lines.begin(), last);

            INFO("Offset: ", offset, ", Boundary: ", boundary);
            CHECK(unique_count == all_lines.size());
        }
    }
}

TEST_CASE("LINE_BYTES Stream - Multiple Worker Counts") {
    SUBCASE("Variable worker counts (2, 4, 8, 16)") {
        TestEnvironment env(valgrind_scale(15000, 150));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(15000, 150), 90, 280);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(3.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();

        // Read full file once for reference
        auto stream_ref = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(max_bytes)));
        dstd::Vector<char> buffer(512 * 1024);
        dstd::String full_content;
        while (!stream_ref->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream_ref->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                full_content.append(buffer.data(), bytes_read);
            }
        }

        dstd::Vector<dstd::String> reference_lines;
        dftu_utils_test::LineStream ref_stream(full_content);
        dstd::String line;
        while (ref_stream.next(line)) {
            if (!line.empty()) reference_lines.push_back(line);
        }
        std::sort(reference_lines.begin(), reference_lines.end());

        // Test with different worker counts
        for (int raw_workers : {2, 4, 8, 16}) {
            int worker_count = valgrind_threads(raw_workers);
            std::size_t chunk_size = max_bytes / worker_count;

            dstd::Vector<dstd::String> chunks(worker_count);
            dstd::Vector<dstd::Thread> threads;

            for (int i = 0; i < worker_count; ++i) {
                threads.emplace_back([&, i]() {
                    // Create separate reader per thread
                    auto thread_reader = dftu_utils_test::ok(
                        ReaderFactory::create(gz_file, idx_file));
                    if (!thread_reader) return;

                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == worker_count - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = dftu_utils_test::ok(thread_reader->stream(
                        StreamConfig()
                            .stream_type(StreamType::LineBytes)
                            .range_type(RangeType::ByteRange)
                            .from(start)
                            .to(end)));
                    if (!stream) return;

                    dstd::Vector<char> buf(256 * 1024);
                    while (!stream->done()) {
                        std::size_t br = dftu_utils_test::ok(
                            stream->read(buf.data(), buf.size()));
                        if (br > 0) chunks[i].append(buf.data(), br);
                    }
                });
            }

            for (auto& t : threads) {
                t.join();
            }

            // Collect and sort lines
            dstd::Vector<dstd::String> worker_lines;
            for (const auto& chunk : chunks) {
                dftu_utils_test::LineStream s(chunk);
                dstd::String l;
                while (s.next(l)) {
                    if (!l.empty()) worker_lines.push_back(l);
                }
            }
            std::sort(worker_lines.begin(), worker_lines.end());

            // Check for duplicates
            auto last = std::unique(worker_lines.begin(), worker_lines.end());
            std::size_t unique_count =
                std::distance(worker_lines.begin(), last);

            INFO("Workers: ", worker_count);
            INFO("Total lines: ", worker_lines.size());
            INFO("Unique lines: ", unique_count);

            CHECK(unique_count == worker_lines.size());
            CHECK(worker_lines.size() == reference_lines.size());
            CHECK(worker_lines == reference_lines);
        }
    }
}

TEST_CASE("LINE_BYTES Stream - Event Ordering Verification") {
    SUBCASE("Ordering differences are acceptable but data is complete") {
        TestEnvironment env(valgrind_scale(8000, 80));
        REQUIRE(env.is_valid());

        dstd::String gz_file = create_variable_line_test_file(
            env.dir(), valgrind_scale(8000, 80), 100, 300);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(2.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->max_bytes();
        std::size_t chunk_size = max_bytes / 8;

        // Read in 8 chunks
        dstd::Vector<dstd::String> chunks;
        dstd::Vector<char> buffer(256 * 1024);

        for (int i = 0; i < 8; ++i) {
            std::size_t start = i * chunk_size;
            std::size_t end = (i == 7) ? max_bytes : (i + 1) * chunk_size;

            auto stream = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(start)
                                   .to(end)));
            dstd::String chunk;
            while (!stream->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) {
                    chunk.append(buffer.data(), bytes_read);
                }
            }
            chunks.push_back(chunk);
        }

        // Read sequentially
        auto stream_seq = dftu_utils_test::ok(
            reader->stream(StreamConfig()
                               .stream_type(StreamType::LineBytes)
                               .range_type(RangeType::ByteRange)
                               .from(0)
                               .to(max_bytes)));
        dstd::String sequential;
        while (!stream_seq->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                stream_seq->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                sequential.append(buffer.data(), bytes_read);
            }
        }

        // Extract and sort both
        dstd::Vector<dstd::String> chunked_lines, sequential_lines;

        for (const auto& chunk : chunks) {
            dftu_utils_test::LineStream s(chunk);
            dstd::String line;
            while (s.next(line)) {
                if (!line.empty()) chunked_lines.push_back(line);
            }
        }

        dftu_utils_test::LineStream seq_stream(sequential);
        dstd::String line;
        while (seq_stream.next(line)) {
            if (!line.empty()) sequential_lines.push_back(line);
        }

        std::sort(chunked_lines.begin(), chunked_lines.end());
        std::sort(sequential_lines.begin(), sequential_lines.end());

        // They should match after sorting (same data, possibly different order)
        CHECK(chunked_lines.size() == sequential_lines.size());
        CHECK(chunked_lines == sequential_lines);

        // But the original concatenation might differ (ordering)
        dstd::String concatenated;
        for (const auto& chunk : chunks) {
            concatenated += chunk;
        }

        // This might NOT match due to ordering, but that's okay
        // The sorted comparison above proves completeness
        INFO("Concatenated size: ", concatenated.size());
        INFO("Sequential size: ", sequential.size());
        INFO(
            "Byte sizes may differ due to line ordering, but sorted lines "
            "match");
    }
}

TEST_CASE("LINE_BYTES Stream - Buffer Size Tests") {
    SUBCASE("Test with different buffer sizes") {
        const std::size_t n_buf_lines = valgrind_scale(3000, 30);
        TestEnvironment env(n_buf_lines);
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), n_buf_lines, 100, 300);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(2.0));
            dftu_utils_test::build_indexer(*indexer);
        }

        auto reference_reader =
            dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
        REQUIRE(reference_reader != nullptr);

        std::size_t max_bytes = reference_reader->max_bytes();

        // Read full file with default buffer to get reference
        auto ref_stream = dftu_utils_test::ok(
            reference_reader->stream(StreamConfig()
                                         .stream_type(StreamType::LineBytes)
                                         .range_type(RangeType::ByteRange)
                                         .from(0)
                                         .to(max_bytes)
                                         .buffer_size(0)));  // Use default

        dstd::Vector<char> buffer(512 * 1024);
        dstd::String reference_content;
        while (!ref_stream->done()) {
            std::size_t bytes_read = dftu_utils_test::ok(
                ref_stream->read(buffer.data(), buffer.size()));
            if (bytes_read > 0) {
                reference_content.append(buffer.data(), bytes_read);
            }
        }

        dstd::Vector<dstd::String> reference_lines;
        dftu_utils_test::LineStream ref_stream_str(reference_content);
        dstd::String line;
        while (ref_stream_str.next(line)) {
            if (!line.empty()) reference_lines.push_back(line);
        }
        std::sort(reference_lines.begin(), reference_lines.end());

        CHECK(reference_lines.size() == n_buf_lines);

        // Test with various buffer sizes
        dstd::Vector<std::size_t> buffer_sizes = {
            32 * 1024,        // 32KB (smaller than default 64KB)
            64 * 1024,        // 64KB (default)
            256 * 1024,       // 256KB
            1 * 1024 * 1024,  // 1MB
            4 * 1024 * 1024,  // 4MB (StreamConfig default)
            8 * 1024 * 1024   // 8MB (large)
        };

        for (std::size_t buf_size : buffer_sizes) {
            INFO("Testing buffer size: ", buf_size, " bytes");

            auto reader =
                dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
            REQUIRE(reader != nullptr);

            // Test reading full file with this buffer size
            auto stream = dftu_utils_test::ok(
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::LineBytes)
                                   .range_type(RangeType::ByteRange)
                                   .from(0)
                                   .to(max_bytes)
                                   .buffer_size(buf_size)));

            dstd::String content;
            while (!stream->done()) {
                std::size_t bytes_read = dftu_utils_test::ok(
                    stream->read(buffer.data(), buffer.size()));
                if (bytes_read > 0) {
                    content.append(buffer.data(), bytes_read);
                }
            }

            dstd::Vector<dstd::String> lines;
            dftu_utils_test::LineStream stream_str(content);
            while (stream_str.next(line)) {
                if (!line.empty()) lines.push_back(line);
            }
            std::sort(lines.begin(), lines.end());

            CHECK(lines.size() == reference_lines.size());
            CHECK(lines == reference_lines);
        }
    }

    SUBCASE("Test chunked reads with different buffer sizes") {
        const std::size_t n_chunk_lines = valgrind_scale(2000, 20);
        TestEnvironment env(n_chunk_lines);
        REQUIRE(env.is_valid());

        dstd::String gz_file =
            create_variable_line_test_file(env.dir(), n_chunk_lines, 80, 200);
        REQUIRE(!gz_file.empty());

        dstd::String idx_file = env.get_index_path(gz_file);

        {
            auto indexer = dftu_utils_test::create_indexer(gz_file, idx_file,
                                                           mb_to_b(1.5));
            dftu_utils_test::build_indexer(*indexer);
        }

        std::size_t max_bytes;
        {
            auto reader =
                dftu_utils_test::ok(ReaderFactory::create(gz_file, idx_file));
            max_bytes = reader->max_bytes();
        }

        std::size_t mid_point = max_bytes / 2;

        dstd::Vector<std::size_t> buffer_sizes = {
            64 * 1024,        // 64KB
            512 * 1024,       // 512KB
            2 * 1024 * 1024,  // 2MB
            8 * 1024 * 1024   // 8MB
        };

        for (std::size_t buf_size : buffer_sizes) {
            INFO("Testing chunked read with buffer size: ", buf_size, " bytes");

            dstd::Vector<char> read_buffer(512 * 1024);

            // Read first half
            {
                auto reader = dftu_utils_test::ok(
                    ReaderFactory::create(gz_file, idx_file));
                auto stream1 = dftu_utils_test::ok(
                    reader->stream(StreamConfig()
                                       .stream_type(StreamType::LineBytes)
                                       .range_type(RangeType::ByteRange)
                                       .from(0)
                                       .to(mid_point)
                                       .buffer_size(buf_size)));

                dstd::String chunk1;
                while (!stream1->done()) {
                    std::size_t bytes_read = dftu_utils_test::ok(
                        stream1->read(read_buffer.data(), read_buffer.size()));
                    if (bytes_read > 0) {
                        chunk1.append(read_buffer.data(), bytes_read);
                    }
                }
                CHECK(!chunk1.empty());
            }

            // Read second half
            {
                auto reader = dftu_utils_test::ok(
                    ReaderFactory::create(gz_file, idx_file));
                auto stream2 = dftu_utils_test::ok(
                    reader->stream(StreamConfig()
                                       .stream_type(StreamType::LineBytes)
                                       .range_type(RangeType::ByteRange)
                                       .from(mid_point)
                                       .to(max_bytes)
                                       .buffer_size(buf_size)));

                dstd::String chunk2;
                while (!stream2->done()) {
                    std::size_t bytes_read = dftu_utils_test::ok(
                        stream2->read(read_buffer.data(), read_buffer.size()));
                    if (bytes_read > 0) {
                        chunk2.append(read_buffer.data(), bytes_read);
                    }
                }
                CHECK(!chunk2.empty());
            }

            // Read full file
            {
                auto reader = dftu_utils_test::ok(
                    ReaderFactory::create(gz_file, idx_file));
                auto stream_full = dftu_utils_test::ok(
                    reader->stream(StreamConfig()
                                       .stream_type(StreamType::LineBytes)
                                       .range_type(RangeType::ByteRange)
                                       .from(0)
                                       .to(max_bytes)
                                       .buffer_size(buf_size)));

                dstd::String full;
                while (!stream_full->done()) {
                    std::size_t bytes_read =
                        dftu_utils_test::ok(stream_full->read(
                            read_buffer.data(), read_buffer.size()));
                    if (bytes_read > 0) {
                        full.append(read_buffer.data(), bytes_read);
                    }
                }

                dstd::Vector<dstd::String> full_lines;
                dftu_utils_test::LineStream full_stream(full);
                dstd::String line;
                while (full_stream.next(line)) {
                    if (!line.empty()) full_lines.push_back(line);
                }

                CHECK(full_lines.size() == n_chunk_lines);
            }
        }
    }
}
