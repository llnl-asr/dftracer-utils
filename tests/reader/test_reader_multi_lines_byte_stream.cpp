#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/reader/stream.h>
#include <dftracer/utils/reader/stream_type.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dft_utils_test;

// Helper function to create a test file with variable line lengths
static std::string create_variable_line_test_file(
    const std::string& test_dir, std::size_t num_lines,
    std::size_t min_line_length = 50, std::size_t max_line_length = 500) {
    std::string txt_file = test_dir + "/variable_lines_multi.txt";
    std::string gz_file = test_dir + "/variable_lines_multi.gz";

    // Create text file with variable line lengths
    std::ofstream f(txt_file);
    if (!f.is_open()) {
        return "";
    }

    std::mt19937 rng(12345);  // Fixed seed for reproducibility
    std::uniform_int_distribution<std::size_t> dist(min_line_length,
                                                    max_line_length);

    for (std::size_t i = 1; i <= num_lines; ++i) {
        std::size_t line_length = dist(rng);

        // Create JSON line with variable padding to reach desired length
        std::ostringstream line;
        line << "{\"id\":" << i << ",\"name\":\"event_" << i
             << "\",\"data\":\"";

        // Fill with padding to reach target length
        std::size_t current_len = line.str().length();
        std::size_t padding_needed = (line_length > current_len + 10)
                                         ? (line_length - current_len - 10)
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

    fs::remove(txt_file);
    return gz_file;
}

TEST_CASE("MULTI_LINES_BYTES Stream - Sequential Read Tests") {
    SUBCASE("Complete file read - single chunk") {
        TestEnvironment env(1000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 1000);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        // Build index
        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(0.5));
            REQUIRE(indexer != nullptr);
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);
        REQUIRE(reader->is_valid());

        std::size_t max_bytes = reader->get_max_bytes();
        auto stream =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(0)
                               .to(max_bytes));
        REQUIRE(stream != nullptr);

        // Read entire file
        std::vector<char> buffer(1024 * 1024);  // 1MB buffer
        std::string result;

        while (!stream->done()) {
            std::size_t bytes_read = stream->read(buffer.data(), buffer.size());
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

    SUBCASE("Adjacent chunks no overlap") {
        TestEnvironment env(10000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 10000, 100, 300);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(1.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        std::size_t chunk_size = max_bytes / 2;  // Split file in half

        // Read first chunk
        auto stream1 =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(0)
                               .to(chunk_size));
        REQUIRE(stream1 != nullptr);

        std::vector<char> buffer(1024 * 1024);
        std::string chunk1;
        while (!stream1->done()) {
            std::size_t bytes_read =
                stream1->read(buffer.data(), buffer.size());
            if (bytes_read > 0) {
                chunk1.append(buffer.data(), bytes_read);
            }
        }

        // Read second chunk
        auto stream2 =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(chunk_size)
                               .to(max_bytes));
        REQUIRE(stream2 != nullptr);

        std::string chunk2;
        while (!stream2->done()) {
            std::size_t bytes_read =
                stream2->read(buffer.data(), buffer.size());
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

        if (chunk1_last_newline != std::string::npos &&
            chunk2_first_newline != std::string::npos) {
            std::string last_line1 = chunk1.substr(chunk1_last_newline + 1);
            std::string first_line2 = chunk2.substr(0, chunk2_first_newline);

            // They should be different (no overlap)
            CHECK(last_line1 != first_line2);
        }
    }

    SUBCASE("Adjacent chunks concatenation equals full read") {
        TestEnvironment env(5000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 5000, 80, 250);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(1));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        std::size_t mid_point = max_bytes / 2;

        std::vector<char> buffer(512 * 1024);

        // Read first half
        auto stream1 =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(0)
                               .to(mid_point));
        std::string chunk1;
        while (!stream1->done()) {
            std::size_t bytes_read =
                stream1->read(buffer.data(), buffer.size());
            if (bytes_read > 0) {
                chunk1.append(buffer.data(), bytes_read);
            }
        }

        // Read second half
        auto stream2 =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(mid_point)
                               .to(max_bytes));
        std::string chunk2;
        while (!stream2->done()) {
            std::size_t bytes_read =
                stream2->read(buffer.data(), buffer.size());
            if (bytes_read > 0) {
                chunk2.append(buffer.data(), bytes_read);
            }
        }

        // Read full file
        auto stream_full =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(0)
                               .to(max_bytes));
        std::string full;
        while (!stream_full->done()) {
            std::size_t bytes_read =
                stream_full->read(buffer.data(), buffer.size());
            if (bytes_read > 0) {
                full.append(buffer.data(), bytes_read);
            }
        }

        // Concatenate chunks
        std::string concatenated = chunk1 + chunk2;

        // Sort both for comparison (to handle potential ordering differences)
        std::vector<std::string> concat_lines, full_lines;

        std::istringstream concat_stream(concatenated);
        std::string line;
        while (std::getline(concat_stream, line)) {
            concat_lines.push_back(line);
        }

        std::istringstream full_stream(full);
        while (std::getline(full_stream, line)) {
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
        TestEnvironment env(3000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 3000);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(0.5));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        std::size_t num_chunks = 8;
        std::size_t chunk_size = max_bytes / num_chunks;

        std::vector<std::string> chunks;
        std::vector<char> buffer(256 * 1024);

        // Read all chunks
        for (std::size_t i = 0; i < num_chunks; ++i) {
            std::size_t start = i * chunk_size;
            std::size_t end =
                (i == num_chunks - 1) ? max_bytes : (i + 1) * chunk_size;

            auto stream =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(start)
                                   .to(end));
            REQUIRE(stream != nullptr);

            std::string chunk;
            while (!stream->done()) {
                std::size_t bytes_read =
                    stream->read(buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    chunk.append(buffer.data(), bytes_read);
                }
            }

            chunks.push_back(chunk);
        }

        // Extract all lines from all chunks
        std::vector<std::string> all_lines;
        for (const auto& chunk : chunks) {
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty()) {
                    all_lines.push_back(line);
                }
            }
        }

        // Check for duplicates (sort and check unique)
        std::vector<std::string> sorted_lines = all_lines;
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

TEST_CASE("MULTI_LINES_BYTES Stream - Parallel/Threaded Tests") {
    SUBCASE("Parallel read with 4 threads") {
        TestEnvironment env(10000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 10000, 100, 400);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(2.0));
            indexer->build();
        }

        std::size_t max_bytes;
        {
            auto reader = ReaderFactory::create(gz_file, idx_file);
            REQUIRE(reader != nullptr);
            max_bytes = reader->get_max_bytes();
        }

        const int num_threads = 4;
        std::size_t chunk_size = max_bytes / num_threads;

        std::vector<std::string> chunks(num_threads);
        std::vector<std::thread> threads;
        std::atomic<bool> error_occurred{false};

        // Launch threads to read different chunks in parallel
        // Each thread creates its own reader to avoid sharing indexer
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    // Create separate reader per thread
                    auto reader = ReaderFactory::create(gz_file, idx_file);
                    if (!reader) {
                        error_occurred = true;
                        return;
                    }

                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == num_threads - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = reader->stream(
                        StreamConfig()
                            .stream_type(StreamType::MULTI_LINES_BYTES)
                            .range_type(RangeType::BYTE_RANGE)
                            .from(start)
                            .to(end));
                    if (!stream) {
                        error_occurred = true;
                        return;
                    }

                    std::vector<char> buffer(512 * 1024);
                    std::string& chunk = chunks[i];

                    while (!stream->done()) {
                        std::size_t bytes_read =
                            stream->read(buffer.data(), buffer.size());
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
        std::vector<std::string> all_lines;
        for (const auto& chunk : chunks) {
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty()) {
                    all_lines.push_back(line);
                }
            }
        }

        // Sort and check for duplicates
        std::vector<std::string> sorted_lines = all_lines;
        std::sort(sorted_lines.begin(), sorted_lines.end());

        auto last = std::unique(sorted_lines.begin(), sorted_lines.end());
        std::size_t unique_count = std::distance(sorted_lines.begin(), last);

        INFO("Total lines: ", all_lines.size());
        INFO("Unique lines: ", unique_count);

        // All lines should be unique (no overlaps between threads)
        CHECK(unique_count == all_lines.size());
    }

    SUBCASE("Parallel read with 16 threads - stress test") {
        TestEnvironment env(20000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 20000, 80, 350);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(4.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        const int num_threads = 16;
        std::size_t chunk_size = max_bytes / num_threads;

        std::vector<std::string> chunks(num_threads);
        std::vector<std::thread> threads;
        std::atomic<int> errors{0};
        std::atomic<int> completed{0};

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == num_threads - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = reader->stream(
                        StreamConfig()
                            .stream_type(StreamType::MULTI_LINES_BYTES)
                            .range_type(RangeType::BYTE_RANGE)
                            .from(start)
                            .to(end));
                    if (!stream) {
                        errors++;
                        return;
                    }

                    std::vector<char> buffer(256 * 1024);
                    std::string& chunk = chunks[i];

                    while (!stream->done()) {
                        std::size_t bytes_read =
                            stream->read(buffer.data(), buffer.size());
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
        CHECK(completed == num_threads);

        // Verify completeness
        std::vector<std::string> all_lines;
        for (const auto& chunk : chunks) {
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
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
}

TEST_CASE("MULTI_LINES_BYTES Stream - Specific Boundary Tests") {
    SUBCASE("4MB boundary (4194304 bytes) - the original issue") {
        TestEnvironment env(10000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 10000, 100, 300);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(2.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        std::size_t boundary = 4194304;  // 4MB

        // Only test if file is large enough
        if (max_bytes > boundary) {
            std::vector<char> buffer(512 * 1024);

            // Read chunk 1: [0, 4194304)
            auto stream1 =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(0)
                                   .to(boundary));
            std::string chunk1;
            while (!stream1->done()) {
                std::size_t bytes_read =
                    stream1->read(buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    chunk1.append(buffer.data(), bytes_read);
                }
            }

            // Read chunk 2: [4194304, end)
            auto stream2 =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(boundary)
                                   .to(max_bytes));
            std::string chunk2;
            while (!stream2->done()) {
                std::size_t bytes_read =
                    stream2->read(buffer.data(), buffer.size());
                if (bytes_read > 0) {
                    chunk2.append(buffer.data(), bytes_read);
                }
            }

            CHECK(!chunk1.empty());
            CHECK(!chunk2.empty());

            // Check for overlap by looking at last/first lines
            std::vector<std::string> lines1, lines2;
            std::istringstream s1(chunk1);
            std::string line;
            while (std::getline(s1, line)) {
                if (!line.empty()) lines1.push_back(line);
            }

            std::istringstream s2(chunk2);
            while (std::getline(s2, line)) {
                if (!line.empty()) lines2.push_back(line);
            }

            // Last line of chunk1 should be different from first line of chunk2
            if (!lines1.empty() && !lines2.empty()) {
                CHECK(lines1.back() != lines2.front());
            }

            // Check for no duplicates overall
            std::vector<std::string> all_lines = lines1;
            all_lines.insert(all_lines.end(), lines2.begin(), lines2.end());
            std::sort(all_lines.begin(), all_lines.end());
            auto last = std::unique(all_lines.begin(), all_lines.end());
            CHECK(std::distance(all_lines.begin(), last) ==
                  static_cast<std::ptrdiff_t>(all_lines.size()));
        }
    }

    SUBCASE("Prime number boundary") {
        TestEnvironment env(5000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 5000, 80, 250);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(1.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();
        std::size_t boundary = 1299709;  // Prime number

        if (max_bytes > boundary) {
            std::vector<char> buffer(256 * 1024);

            auto stream1 =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(0)
                                   .to(boundary));
            auto stream2 =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(boundary)
                                   .to(max_bytes));

            std::string chunk1, chunk2;

            while (!stream1->done()) {
                std::size_t bytes_read =
                    stream1->read(buffer.data(), buffer.size());
                if (bytes_read > 0) chunk1.append(buffer.data(), bytes_read);
            }

            while (!stream2->done()) {
                std::size_t bytes_read =
                    stream2->read(buffer.data(), buffer.size());
                if (bytes_read > 0) chunk2.append(buffer.data(), bytes_read);
            }

            // Concatenate and verify completeness
            std::string concat = chunk1 + chunk2;

            // Read full file for comparison
            auto stream_full =
                reader->stream(StreamConfig()
                                   .stream_type(StreamType::MULTI_LINES_BYTES)
                                   .range_type(RangeType::BYTE_RANGE)
                                   .from(0)
                                   .to(max_bytes));
            std::string full;
            while (!stream_full->done()) {
                std::size_t bytes_read =
                    stream_full->read(buffer.data(), buffer.size());
                if (bytes_read > 0) full.append(buffer.data(), bytes_read);
            }

            // Sort and compare to handle ordering differences
            std::vector<std::string> concat_sorted, full_sorted;
            std::istringstream cs(concat);
            std::string line;
            while (std::getline(cs, line)) {
                if (!line.empty()) concat_sorted.push_back(line);
            }
            std::istringstream fs(full);
            while (std::getline(fs, line)) {
                if (!line.empty()) full_sorted.push_back(line);
            }

            std::sort(concat_sorted.begin(), concat_sorted.end());
            std::sort(full_sorted.begin(), full_sorted.end());

            CHECK(concat_sorted.size() == full_sorted.size());
            CHECK(concat_sorted == full_sorted);
        }
    }
}

TEST_CASE("MULTI_LINES_BYTES Stream - Variable Worker Counts") {
    SUBCASE("Test with 2, 4, 8, 16 workers") {
        TestEnvironment env(15000);
        REQUIRE(env.is_valid());

        std::string gz_file =
            create_variable_line_test_file(env.get_dir(), 15000, 90, 280);
        REQUIRE(!gz_file.empty());

        std::string idx_file = env.get_index_path(gz_file);

        {
            auto indexer =
                IndexerFactory::create(gz_file, idx_file, mb_to_b(3.0));
            indexer->build();
        }

        auto reader = ReaderFactory::create(gz_file, idx_file);
        REQUIRE(reader != nullptr);

        std::size_t max_bytes = reader->get_max_bytes();

        // Read full file once for reference
        auto stream_ref =
            reader->stream(StreamConfig()
                               .stream_type(StreamType::MULTI_LINES_BYTES)
                               .range_type(RangeType::BYTE_RANGE)
                               .from(0)
                               .to(max_bytes));
        std::vector<char> buffer(512 * 1024);
        std::string full_content;
        while (!stream_ref->done()) {
            std::size_t bytes_read =
                stream_ref->read(buffer.data(), buffer.size());
            if (bytes_read > 0) {
                full_content.append(buffer.data(), bytes_read);
            }
        }

        std::vector<std::string> reference_lines;
        std::istringstream ref_stream(full_content);
        std::string line;
        while (std::getline(ref_stream, line)) {
            if (!line.empty()) reference_lines.push_back(line);
        }
        std::sort(reference_lines.begin(), reference_lines.end());

        // Test with different worker counts
        for (int num_workers : {2, 4, 8, 16}) {
            std::size_t chunk_size = max_bytes / num_workers;

            std::vector<std::string> chunks(num_workers);
            std::vector<std::thread> threads;

            for (int i = 0; i < num_workers; ++i) {
                threads.emplace_back([&, i]() {
                    std::size_t start = i * chunk_size;
                    std::size_t end = (i == num_workers - 1)
                                          ? max_bytes
                                          : (i + 1) * chunk_size;

                    auto stream = reader->stream(
                        StreamConfig()
                            .stream_type(StreamType::MULTI_LINES_BYTES)
                            .range_type(RangeType::BYTE_RANGE)
                            .from(start)
                            .to(end));
                    if (!stream) return;

                    std::vector<char> buf(256 * 1024);
                    while (!stream->done()) {
                        std::size_t br = stream->read(buf.data(), buf.size());
                        if (br > 0) chunks[i].append(buf.data(), br);
                    }
                });
            }

            for (auto& t : threads) {
                t.join();
            }

            // Collect and sort lines
            std::vector<std::string> worker_lines;
            for (const auto& chunk : chunks) {
                std::istringstream s(chunk);
                std::string l;
                while (std::getline(s, l)) {
                    if (!l.empty()) worker_lines.push_back(l);
                }
            }
            std::sort(worker_lines.begin(), worker_lines.end());

            // Check for duplicates
            auto last = std::unique(worker_lines.begin(), worker_lines.end());
            std::size_t unique_count =
                std::distance(worker_lines.begin(), last);

            INFO("Workers: ", num_workers);
            INFO("Total lines: ", worker_lines.size());
            INFO("Unique lines: ", unique_count);

            CHECK(unique_count == worker_lines.size());
            CHECK(worker_lines.size() == reference_lines.size());
            CHECK(worker_lines == reference_lines);
        }
    }
}
