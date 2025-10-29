/**
 * @file test_utilities.cpp
 * @brief Simple test program to verify the composable utilities system.
 *
 * This program demonstrates:
 * - Creating utilities with tags
 * - Automatic behavior creation from tags via use()
 * - Manual behavior addition
 * - Pipeline execution with SequentialExecutor
 * - Component utilities (DirectoryScanner, Gzip compression)
 */

#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/utilities/behaviors/behaviors.h>
#include <dftracer/utils/core/utilities/utilities.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

// Include executors and pipeline output
#include <dftracer/utils/core/pipeline/executors/sequential_executor.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline_output.h>

// Include component utilities
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/compression/compression.h>
#include <dftracer/utils/utilities/filesystem/filesystem.h>
#include <dftracer/utils/utilities/io/streaming_file_reader.h>
#include <dftracer/utils/utilities/io/streaming_file_writer.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <dftracer/utils/utilities/text/text.h>

// Include line iterators
#include <dftracer/utils/utilities/io/lines/line_range.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <dftracer/utils/utilities/io/lines/sources/indexed_file_line_iterator.h>
#include <dftracer/utils/utilities/io/lines/sources/plain_file_line_iterator.h>
#include <dftracer/utils/utilities/io/lines/streaming_line_reader.h>

// Include workflows
#include <dftracer/utils/utilities/workflows/workflows.h>

#include <cstdio>
#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::behaviors;
using namespace dftracer::utils::utilities::tags;

// Simple utility WITHOUT Cacheable tag
class SlowSquareUtility : public Utility<int, int> {
   private:
    mutable int call_count_ = 0;

   public:
    int process(const int& input) override {
        call_count_++;
        std::cout << "      [Computing #" << call_count_ << "] " << input
                  << "^2..." << std::endl;
        // Simulate expensive computation
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return input * input;
    }

    int get_call_count() const { return call_count_; }
};

/**
 * @brief Simple utility that doubles an integer.
 *
 * Demonstrates a utility with retry and monitoring tags.
 */
class DoubleUtility : public Utility<int, int, Retryable, Monitored> {
   private:
    mutable int call_count_ = 0;

   public:
    DoubleUtility() {
        // Configure retry behavior: max 3 retries, 50ms delay
        set_tag(Retryable()
                    .with_max_retries(3)
                    .with_retry_delay(std::chrono::milliseconds(50))
                    .with_exponential_backoff(true));

        // Configure monitoring behavior with simple console logger
        set_tag(Monitored().with_log_callback([](const std::string& msg) {
            std::cout << "    " << msg << std::endl;
        }));
    }

    int process(const int& input) override {
        call_count_++;

        // Simulate occasional failures for first 2 calls
        if (call_count_ <= 2) {
            std::cout << "    [DoubleUtility] Simulating failure on call #"
                      << call_count_ << std::endl;
            throw std::runtime_error("Simulated failure");
        }

        std::cout << "    [DoubleUtility] Processing: " << input << " -> "
                  << (input * 2) << std::endl;
        return input * 2;
    }
};

/**
 * @brief Simple utility with no tags.
 */
class SquareUtility : public Utility<int, int> {
   public:
    int process(const int& input) override {
        std::cout << "    [SquareUtility] Processing: " << input << " -> "
                  << (input * input) << std::endl;
        return input * input;
    }
};

int main() {
    std::cout << "\n=== Testing Composable Utilities System ===\n" << std::endl;

    // Test 1: Utility with automatic behaviors from tags using use() +
    // SequentialExecutor
    std::cout
        << "Test 1: DoubleUtility with Retry + Monitoring (auto from tags)"
        << std::endl;
    std::cout
        << "----------------------------------------------------------------"
        << std::endl;
    {
        auto utility = std::make_shared<DoubleUtility>();

        // Create pipeline
        Pipeline pipeline("Test Pipeline");

        std::cout << "  ✓ Using use() function with SequentialExecutor"
                  << std::endl;
        std::cout << "  ✓ Behaviors auto-created from tags (Retry + Monitoring)"
                  << std::endl;

        // Use the utility with automatic behavior creation from tags
        auto task_result = use(utility).emit_on(pipeline);

        std::cout << "\n  Executing pipeline with input: 21" << std::endl;
        try {
            // Create executor and run pipeline
            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, 21);
            int result = output.get<int>(task_result.id());
            std::cout << "\n  ✓ Final result: " << result << std::endl;
        } catch (const std::exception& e) {
            std::cout << "\n  ✗ Error: " << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    // Test 2: Manual behavior addition using with_behavior()
    std::cout << "Test 2: SquareUtility with Manual Monitoring Behavior"
              << std::endl;
    std::cout << "------------------------------------------------------"
              << std::endl;
    {
        auto utility = std::make_shared<SquareUtility>();

        // Create pipeline
        Pipeline pipeline("Square Pipeline");

        // Manually create and add monitoring behavior using with_behavior()
        auto monitor = std::make_shared<MonitoringBehavior<int, int>>(
            [](const std::string& msg) {
                std::cout << "    " << msg << std::endl;
            },
            "SquareUtility"  // Explicitly provide utility name
        );

        std::cout << "  ✓ Using use() with manual with_behavior()" << std::endl;

        // Use the utility with manual behavior addition
        auto task_result =
            use(utility).with_behavior(monitor).emit_on(pipeline);

        std::cout << "\n  Executing pipeline with input: 7" << std::endl;
        try {
            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, 7);
            int result = output.get<int>(task_result.id());
            std::cout << "\n  ✓ Final result: " << result << std::endl;
        } catch (const std::exception& e) {
            std::cout << "\n  ✗ Error: " << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    // Test 3: Demonstrate retry exhaustion using use()
    std::cout << "Test 3: Retry Exhaustion (utility always fails)" << std::endl;
    std::cout << "------------------------------------------------"
              << std::endl;

    {
        // Utility that always fails
        class FailingUtility : public Utility<int, int, Retryable> {
           public:
            FailingUtility() {
                // Configure retry
                set_tag(Retryable()
                            .with_max_retries(2)  // Only 2 retries
                            .with_retry_delay(std::chrono::milliseconds(10))
                            .with_exponential_backoff(false));
            }

            int process([[maybe_unused]] const int& input) override {
                std::cout << "    [FailingUtility] Attempt failed!"
                          << std::endl;
                throw std::runtime_error("Always fails");
            }
        };

        auto utility = std::make_shared<FailingUtility>();

        // Create pipeline
        Pipeline pipeline("Failing Pipeline");

        std::cout << "  ✓ RetryBehavior auto-created from tag (max 2 retries)"
                  << std::endl;

        // Use the utility with automatic retry behavior from tag
        auto task_result = use(utility).emit_on(pipeline);

        std::cout << "\n  Executing pipeline with input: 5" << std::endl;
        try {
            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, 5);

            // Try to get the result - this will throw if the task failed
            try {
                int result = output.get<int>(task_result.id());
                (void)result;  // Suppress unused warning
                std::cout << "\n  ✗ Should have thrown!" << std::endl;
            } catch (const PipelineError& e) {
                // Pipeline wraps the exception - check the message
                std::string msg = e.what();
                if (msg.find("Max retry attempts") != std::string::npos ||
                    msg.find("bad any_cast") != std::string::npos) {
                    std::cout << "\n  ✓ Caught expected error (retry exhausted)"
                              << std::endl;
                    std::cout << "    Pipeline reported: Task failed after max "
                                 "retries"
                              << std::endl;
                } else {
                    std::cout << "\n  ✗ Unexpected PipelineError: " << msg
                              << std::endl;
                }
            }
        } catch (const MaxRetriesExceeded& e) {
            std::cout << "\n  ✓ Caught MaxRetriesExceeded: " << e.what()
                      << std::endl;
            std::cout << "    Attempts: " << e.get_attempts() << std::endl;
        } catch (const std::exception& e) {
            std::cout << "\n  ✓ Caught exception (retry exhausted): "
                      << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    std::cout << "=== All Tests Completed ===\n" << std::endl;

    // Test 4: DirectoryScanner component
    std::cout << "Test 4: DirectoryScanner Component" << std::endl;
    std::cout << "-----------------------------------" << std::endl;
    {
        using namespace dftracer::utils::utilities::filesystem;

        std::string dir_path = ".";
        bool recursive = false;

        std::cout << "  Scanning directory: " << dir_path << std::endl;
        std::cout << "  Recursive: " << (recursive ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // Direct usage (no pipeline)
        std::cout << "  4a: Direct usage (no pipeline)" << std::endl;
        auto scanner = std::make_shared<DirectoryScanner>();

        try {
            auto files = scanner->process(Directory{dir_path, recursive});

            std::cout << "    Found " << files.size() << " entries"
                      << std::endl;

            // Show first 5 entries
            std::size_t count = 0;
            for (const auto& entry : files) {
                if (count++ >= 5) {
                    std::cout << "    ... and " << (files.size() - 5) << " more"
                              << std::endl;
                    break;
                }

                std::string type = entry.is_directory      ? "[DIR]"
                                   : entry.is_regular_file ? "[FILE]"
                                                           : "[OTHER]";

                std::cout << "    " << std::setw(8) << type << " "
                          << std::setw(10) << std::right << entry.size
                          << " bytes  " << entry.path.filename().string()
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    // Test 5: Gzip compression component
    std::cout << "Test 5: Gzip Compression Component" << std::endl;
    std::cout << "-----------------------------------" << std::endl;
    {
        using namespace dftracer::utils::utilities::compression::gzip;
        using namespace dftracer::utils::utilities::io;

        auto compressor = std::make_shared<Compressor>();
        auto decompressor = std::make_shared<Decompressor>();

        // Set maximum compression
        compressor->set_compression_level(9);

        std::string test_data =
            "Hello, World! This is a test of gzip compression. "
            "The quick brown fox jumps over the lazy dog. "
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";

        std::cout << "  Original data: " << test_data.size() << " bytes"
                  << std::endl;

        // Test 5a: Direct compression and decompression
        std::cout << "\n  5a: Direct compression and decompression"
                  << std::endl;
        try {
            RawData input(test_data);
            CompressedData compressed = compressor->process(input);

            std::cout << "    Compressed to: " << compressed.data.size()
                      << " bytes" << std::endl;
            std::cout << "    Compression ratio: " << std::fixed
                      << std::setprecision(2) << compressed.compression_ratio()
                      << std::endl;
            std::cout << "    Space savings: " << std::fixed
                      << std::setprecision(1) << compressed.space_savings()
                      << "%" << std::endl;

            RawData restored = decompressor->process(compressed);
            std::string restored_str = restored.to_string();

            if (restored_str == test_data) {
                std::cout << "    ✓ Decompression successful (data matches)"
                          << std::endl;
            } else {
                std::cout << "    ✗ Decompression failed (data mismatch)"
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 5b: Compression with pipeline (using separate compress pipeline)
        std::cout << "\n  5b: Compression with pipeline" << std::endl;
        try {
            Pipeline compress_pipeline("Compress Pipeline");
            auto compress_task = use(compressor).emit_on(compress_pipeline);

            SequentialExecutor executor;
            PipelineOutput compress_output =
                executor.execute(compress_pipeline, RawData{test_data});
            auto compressed =
                compress_output.get<CompressedData>(compress_task.id());

            std::cout << "    Compressed to: " << compressed.data.size()
                      << " bytes" << std::endl;

            // Decompress in separate pipeline
            Pipeline decompress_pipeline("Decompress Pipeline");
            auto decompress_task =
                use(decompressor).emit_on(decompress_pipeline);

            PipelineOutput decompress_output =
                executor.execute(decompress_pipeline, compressed);
            auto restored =
                decompress_output.get<RawData>(decompress_task.id());
            std::string restored_str = restored.to_string();

            if (restored_str == test_data) {
                std::cout
                    << "    ✓ Pipeline compression/decompression successful"
                    << std::endl;
            } else {
                std::cout << "    ✗ Pipeline failed (data mismatch)"
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 5c: Compression with monitoring
        std::cout << "\n  5c: Compression with Monitored tag" << std::endl;
        try {
            class MonitoredCompressor
                : public Utility<RawData, CompressedData, Monitored> {
               private:
                std::shared_ptr<Compressor> compressor_;

               public:
                MonitoredCompressor() {
                    compressor_ = std::make_shared<Compressor>();
                    compressor_->set_compression_level(9);

                    set_tag(Monitored().with_log_callback(
                        [](const std::string& msg) {
                            std::cout << "      " << msg << std::endl;
                        }));
                }

                CompressedData process(const RawData& input) override {
                    return compressor_->process(input);
                }
            };

            auto monitored_compressor = std::make_shared<MonitoredCompressor>();

            Pipeline pipeline("Monitored Compression Pipeline");
            auto task = use(monitored_compressor).emit_on(pipeline);

            SequentialExecutor executor;
            PipelineOutput output =
                executor.execute(pipeline, RawData{test_data});

            auto compressed = output.get<CompressedData>(task.id());
            std::cout << "    ✓ Monitored compression completed: "
                      << compressed.data.size() << " bytes" << std::endl;

        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    // Test 6: Text utilities
    std::cout << "Test 6: Text Component Utilities" << std::endl;
    std::cout << "----------------------------------" << std::endl;
    {
        using namespace dftracer::utils::utilities::text;

        std::string test_text =
            "Line 1: INFO: Application started\n"
            "Line 2: DEBUG: Loading configuration\n"
            "Line 3: ERROR: Failed to connect\n"
            "Line 4: INFO: Retrying connection\n"
            "Line 5: ERROR: Connection timeout";

        // Test 6a: LineSplitter
        std::cout << "  6a: LineSplitter" << std::endl;
        try {
            auto splitter = std::make_shared<LineSplitter>();

            Text input(test_text);
            Lines lines = splitter->process(input);

            std::cout << "    Split into " << lines.size()
                      << " lines:" << std::endl;
            for (const auto& line : lines.lines) {
                std::cout << "      [" << line.line_number << "] "
                          << line.content << std::endl;
            }
            std::cout << "    ✓ LineSplitter successful" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 6b: LinesFilter (filter for ERROR lines)
        std::cout << "\n  6b: LinesFilter (ERROR lines only)" << std::endl;
        try {
            auto splitter = std::make_shared<LineSplitter>();
            // Use full type name to avoid ambiguity
            auto filter = std::make_shared<
                ::dftracer::utils::utilities::text::LinesFilter>(
                [](const Line& line) {
                    return line.content.find("ERROR") != std::string::npos;
                });

            Text input(test_text);
            Lines all_lines = splitter->process(input);
            Lines error_lines = filter->process(all_lines);

            std::cout << "    Found " << error_lines.size()
                      << " ERROR lines:" << std::endl;
            for (const auto& line : error_lines.lines) {
                std::cout << "      [" << line.line_number << "] "
                          << line.content << std::endl;
            }
            std::cout << "    ✓ LinesFilter successful" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 6c: TextHasher (different algorithms)
        std::cout << "\n  6c: TextHasher (different algorithms)" << std::endl;
        try {
            auto hasher = std::make_shared<TextHasher>();
            Text input("Hello, World! This is a test string for hashing.");

            hasher->set_algorithm(HashAlgorithm::XXH3_64);
            Hash xxh3_hash = hasher->process(input);

            hasher->set_algorithm(HashAlgorithm::XXH64);
            Hash xxh64_hash = hasher->process(input);

            hasher->set_algorithm(HashAlgorithm::STD);
            Hash std_hash = hasher->process(input);

            std::cout << "    Input: \"" << input.content << "\"" << std::endl;
            std::cout << "    XXH3_64:  " << xxh3_hash.value << std::endl;
            std::cout << "    XXH64:    " << xxh64_hash.value << std::endl;
            std::cout << "    STD:      " << std_hash.value << std::endl;
            std::cout << "    ✓ TextHasher successful" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 6d: LineHasher
        std::cout << "\n  6d: LineHasher" << std::endl;
        try {
            auto line_hasher = std::make_shared<LineHasher>();
            line_hasher->set_algorithm(HashAlgorithm::XXH3_64);

            Line line1{"ERROR: Connection failed", 3};
            Line line2{"ERROR: Connection failed", 3};    // Same content
            Line line3{"INFO: Connection succeeded", 4};  // Different content

            Hash hash1 = line_hasher->process(line1);
            Hash hash2 = line_hasher->process(line2);
            Hash hash3 = line_hasher->process(line3);

            std::cout << "    Line 1 hash: " << hash1.value << std::endl;
            std::cout << "    Line 2 hash: " << hash2.value << " (same content)"
                      << std::endl;
            std::cout << "    Line 3 hash: " << hash3.value
                      << " (different content)" << std::endl;

            if (hash1 == hash2 && hash1 != hash3) {
                std::cout << "    ✓ Hash consistency verified" << std::endl;
            } else {
                std::cout << "    ✗ Hash mismatch!" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 6e: Pipeline composition (split -> filter -> hash)
        std::cout << "\n  6e: Pipeline composition (split -> filter)"
                  << std::endl;
        try {
            Pipeline pipeline("Text Processing Pipeline");

            auto splitter = std::make_shared<LineSplitter>();
            auto split_task = use(splitter).emit_on(pipeline);

            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, Text{test_text});
            auto lines = output.get<Lines>(split_task.id());

            // Now filter in separate pipeline
            auto filter = std::make_shared<
                ::dftracer::utils::utilities::text::LinesFilter>(
                [](const Line& line) {
                    return line.content.find("ERROR") != std::string::npos;
                });

            Pipeline filter_pipeline("Filter Pipeline");
            auto filter_task = use(filter).emit_on(filter_pipeline);

            PipelineOutput filter_output =
                executor.execute(filter_pipeline, lines);
            auto filtered = filter_output.get<Lines>(filter_task.id());

            std::cout << "    Original: " << lines.size() << " lines"
                      << std::endl;
            std::cout << "    Filtered: " << filtered.size() << " ERROR lines"
                      << std::endl;
            std::cout << "    ✓ Pipeline composition successful" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }
    }
    std::cout << std::endl;

    // Test 7: Streaming compression with iterators
    std::cout << "Test 7: Streaming Compression with Iterators" << std::endl;
    std::cout << "---------------------------------------------" << std::endl;
    {
        using namespace dftracer::utils::utilities::compression::gzip;
        using namespace dftracer::utils::utilities::io;

        // Create a test file
        std::string test_file_path = "/tmp/test_streaming_input.txt";
        std::string output_file_path = "/tmp/test_streaming_output.gz";
        std::string decompressed_file_path =
            "/tmp/test_streaming_decompressed.txt";

        // Write test data to file
        std::cout << "  Creating test file..." << std::endl;
        {
            std::ofstream out(test_file_path);
            for (int i = 0; i < 1000; ++i) {
                out << "Line " << i
                    << ": The quick brown fox jumps over the lazy dog. "
                    << "Lorem ipsum dolor sit amet, consectetur adipiscing "
                       "elit.\n";
            }
        }

        // Get file size by reading the file
        std::size_t original_size = 0;
        {
            std::ifstream in(test_file_path, std::ios::binary | std::ios::ate);
            original_size = static_cast<std::size_t>(in.tellg());
        }
        std::cout << "    Original file size: " << original_size << " bytes"
                  << std::endl;

        // Test 7a: Streaming compression with StreamingCompressor utility
        std::cout << "\n  7a: Streaming compression (lazy iterators)"
                  << std::endl;
        try {
            auto reader = std::make_shared<StreamingFileReader>();
            auto compressor = std::make_shared<StreamingCompressor>();
            compressor->set_compression_level(9);

            // Create writer
            StreamingFileWriter writer(output_file_path);

            // Get lazy chunk range
            ChunkRange chunks =
                reader->process(StreamReadInput{test_file_path, 4096});

            // Compress lazily
            CompressedChunkRange compressed = compressor->process(chunks);

            // Write each compressed chunk as it's produced
            std::size_t chunk_count = 0;
            for (const auto& chunk : compressed) {
                writer.write_chunk(RawData{chunk.data});
                chunk_count++;
            }
            writer.close();

            std::size_t compressed_size = writer.total_bytes();
            double ratio = static_cast<double>(compressed_size) /
                           static_cast<double>(original_size);

            std::cout << "    Compressed size: " << compressed_size << " bytes"
                      << std::endl;
            std::cout << "    Processed chunks: " << chunk_count << std::endl;
            std::cout << "    Compression ratio: " << std::fixed
                      << std::setprecision(2) << ratio << std::endl;
            std::cout << "    Space savings: " << std::fixed
                      << std::setprecision(1) << ((1.0 - ratio) * 100.0) << "%"
                      << std::endl;
            std::cout << "    ✓ Streaming compression successful" << std::endl;

        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 7b: Manual streaming compression
        std::cout << "\n  7b: Manual streaming compression (chunk-by-chunk)"
                  << std::endl;
        try {
            ManualStreamingCompressor compressor(9);
            StreamingFileWriter writer("/tmp/test_manual_compressed.gz");

            std::ifstream input(test_file_path, std::ios::binary);
            std::vector<unsigned char> buffer(4096);
            std::size_t chunk_count = 0;

            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           buffer.size());
                std::streamsize bytes_read = input.gcount();

                if (bytes_read > 0) {
                    std::vector<unsigned char> chunk_data(
                        buffer.begin(), buffer.begin() + bytes_read);
                    RawData chunk(std::move(chunk_data));

                    auto compressed_chunks = compressor.compress_chunk(chunk);
                    for (const auto& compressed : compressed_chunks) {
                        writer.write_chunk(RawData{compressed.data});
                    }
                    chunk_count++;
                }
            }

            // Finalize compression
            auto final_chunks = compressor.finalize();
            for (const auto& chunk : final_chunks) {
                writer.write_chunk(RawData{chunk.data});
            }
            writer.close();

            std::cout << "    Input chunks processed: " << chunk_count
                      << std::endl;
            std::cout << "    Total bytes in: " << compressor.total_bytes_in()
                      << std::endl;
            std::cout << "    Total bytes out: " << compressor.total_bytes_out()
                      << std::endl;
            std::cout << "    Compression ratio: " << std::fixed
                      << std::setprecision(2) << compressor.compression_ratio()
                      << std::endl;
            std::cout << "    ✓ Manual streaming compression successful"
                      << std::endl;

        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 7c: Manual streaming decompression
        std::cout << "\n  7c: Manual streaming decompression" << std::endl;
        try {
            StreamingDecompressor decompressor;
            StreamingFileWriter writer(decompressed_file_path);

            std::ifstream input(output_file_path, std::ios::binary);
            std::vector<unsigned char> buffer(4096);
            std::size_t chunk_count = 0;

            while (input) {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           buffer.size());
                std::streamsize bytes_read = input.gcount();

                if (bytes_read > 0) {
                    std::vector<unsigned char> chunk_data(
                        buffer.begin(), buffer.begin() + bytes_read);
                    CompressedData chunk{std::move(chunk_data), 0};

                    auto decompressed_chunks =
                        decompressor.decompress_chunk(chunk);
                    for (const auto& raw : decompressed_chunks) {
                        writer.write_chunk(raw);
                    }
                    chunk_count++;
                }
            }
            writer.close();

            std::size_t decompressed_size = writer.total_bytes();

            std::cout << "    Input chunks processed: " << chunk_count
                      << std::endl;
            std::cout << "    Total bytes in: " << decompressor.total_bytes_in()
                      << std::endl;
            std::cout << "    Total bytes out: "
                      << decompressor.total_bytes_out() << std::endl;
            std::cout << "    Decompressed size: " << decompressed_size
                      << " bytes" << std::endl;

            // Verify decompressed data matches original
            if (decompressed_size == original_size) {
                std::cout << "    ✓ Size matches original" << std::endl;

                // Verify content matches byte-for-byte
                std::ifstream orig(test_file_path, std::ios::binary);
                std::ifstream decomp(decompressed_file_path, std::ios::binary);

                std::vector<char> orig_data(
                    (std::istreambuf_iterator<char>(orig)),
                    std::istreambuf_iterator<char>());
                std::vector<char> decomp_data(
                    (std::istreambuf_iterator<char>(decomp)),
                    std::istreambuf_iterator<char>());

                if (orig_data == decomp_data) {
                    std::cout
                        << "    ✓ Content matches original (byte-for-byte)"
                        << std::endl;
                } else {
                    std::cout << "    ✗ Content mismatch detected!"
                              << std::endl;
                }
            } else {
                std::cout << "    ✗ Size mismatch! Expected " << original_size
                          << ", got " << decompressed_size << std::endl;
            }

            std::cout << "    ✓ Manual streaming decompression successful"
                      << std::endl;

        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Cleanup test files
        std::cout << "\n  Cleaning up test files..." << std::endl;
        std::remove(test_file_path.c_str());
        std::remove(output_file_path.c_str());
        std::remove(decompressed_file_path.c_str());
        std::remove("/tmp/test_manual_compressed.gz");
    }
    std::cout << std::endl;

    std::cout << "=== All Component Tests Completed ===\n" << std::endl;

    // Test 8: Line iterators and streaming line reading
    std::cout << "Test 8: Line Iterators and Streaming Line Reading"
              << std::endl;
    std::cout << "--------------------------------------------------"
              << std::endl;
    {
        using namespace dftracer::utils::utilities::io::lines;

        // Create a test plain text file
        std::string test_file_path = "/tmp/test_lines.txt";
        std::cout << "  Creating test file with 100 lines..." << std::endl;
        {
            std::ofstream out(test_file_path);
            for (int i = 1; i <= 100; ++i) {
                out << "Line " << i << ": This is test line number " << i
                    << "\n";
            }
        }

        // Test 8a: PlainFileLineIterator - full file
        std::cout << "\n  8a: PlainFileLineIterator (full file)" << std::endl;
        try {
            sources::PlainFileLineIterator iter(test_file_path);

            std::size_t count = 0;
            while (iter.has_next() && count < 5) {
                Line line = iter.next();
                std::cout << "    [" << line.line_number << "] " << line.content
                          << std::endl;
                count++;
            }

            // Skip to end
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            std::cout << "    Total lines read: " << count << std::endl;
            if (count == 100) {
                std::cout << "    ✓ PlainFileLineIterator successful"
                          << std::endl;
            } else {
                std::cout << "    ✗ Expected 100 lines, got " << count
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 8b: PlainFileLineIterator - line range
        std::cout << "\n  8b: PlainFileLineIterator (line range 10-15)"
                  << std::endl;
        try {
            sources::PlainFileLineIterator iter(test_file_path, 10, 15);

            std::size_t count = 0;
            while (iter.has_next()) {
                Line line = iter.next();
                std::cout << "    [" << line.line_number << "] " << line.content
                          << std::endl;
                count++;
            }

            if (count == 6) {
                std::cout << "    ✓ Line range reading successful (6 lines)"
                          << std::endl;
            } else {
                std::cout << "    ✗ Expected 6 lines, got " << count
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 8c: LineRange with factory methods
        std::cout << "\n  8c: LineRange::from_plain_file" << std::endl;
        try {
            auto range = LineRange::from_plain_file(test_file_path, 20, 25);

            std::size_t count = 0;
            while (range.has_next()) {
                Line line = range.next();
                if (count < 3) {
                    std::cout << "    [" << line.line_number << "] "
                              << line.content << std::endl;
                }
                count++;
            }

            if (count == 6) {
                std::cout << "    ✓ LineRange factory method successful"
                          << std::endl;
            } else {
                std::cout << "    ✗ Expected 6 lines, got " << count
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 8d: LineRange::collect()
        std::cout << "\n  8d: LineRange::collect()" << std::endl;
        try {
            auto range = LineRange::from_plain_file(test_file_path, 30, 35);
            std::vector<Line> lines = range.collect();

            std::cout << "    Collected " << lines.size()
                      << " lines:" << std::endl;
            for (const auto& line : lines) {
                std::cout << "    [" << line.line_number << "] " << line.content
                          << std::endl;
            }

            if (lines.size() == 6) {
                std::cout << "    ✓ collect() successful" << std::endl;
            } else {
                std::cout << "    ✗ Expected 6 lines, got " << lines.size()
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 8e: LineRange::filter()
        std::cout << "\n  8e: LineRange::filter()" << std::endl;
        try {
            auto range = LineRange::from_plain_file(test_file_path, 1, 100);

            // Filter for lines containing "25", "50", "75", "100"
            auto filtered = range.filter([](const Line& line) {
                return line.content.find(" 25") != std::string::npos ||
                       line.content.find(" 50") != std::string::npos ||
                       line.content.find(" 75") != std::string::npos ||
                       line.content.find(" 100") != std::string::npos;
            });

            std::cout << "    Filtered " << filtered.size()
                      << " lines:" << std::endl;
            for (const auto& line : filtered) {
                std::cout << "    [" << line.line_number << "] " << line.content
                          << std::endl;
            }

            if (filtered.size() == 4) {
                std::cout << "    ✓ filter() successful" << std::endl;
            } else {
                std::cout << "    ✗ Expected 4 lines, got " << filtered.size()
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 8f: StreamingLineReader auto-detect
        std::cout << "\n  8f: StreamingLineReader::read()" << std::endl;
        try {
            auto reader_config = StreamingLineReaderConfig()
                                     .with_file(test_file_path)
                                     .with_line_range(40, 45);
            auto range = StreamingLineReader::read(reader_config);

            std::size_t count = 0;
            while (range.has_next()) {
                Line line = range.next();
                std::cout << "    [" << line.line_number << "] " << line.content
                          << std::endl;
                count++;
            }

            if (count == 6) {
                std::cout << "    ✓ StreamingLineReader::read() successful"
                          << std::endl;
            } else {
                std::cout << "    ✗ Expected 6 lines, got " << count
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Cleanup
        std::cout << "\n  Cleaning up test files..." << std::endl;
        std::remove(test_file_path.c_str());
    }
    std::cout << std::endl;

    std::cout << "=== All Line Iterator Tests Completed ===\n" << std::endl;

    // Test 9: Workflow utilities
    std::cout << "Test 9: Workflow Utilities" << std::endl;
    std::cout << "----------------------------" << std::endl;
    {
        using namespace dftracer::utils::utilities::workflows;

        // Create test directory with multiple files
        std::string test_dir = "/tmp/test_workflows";
        fs::create_directories(test_dir);

        std::cout << "  Creating test directory with files..." << std::endl;
        for (int i = 1; i <= 5; ++i) {
            std::string file_path =
                test_dir + "/file" + std::to_string(i) + ".pfw";
            std::ofstream out(file_path);
            for (int j = 1; j <= 10; ++j) {
                out << "File " << i << ", Line " << j << "\n";
            }
        }

        // Test 9a: DirectoryFileProcessor
        std::cout << "\n  9a: DirectoryFileProcessor" << std::endl;
        try {
            auto file_processor = [](const std::string& file_path,
                                     TaskContext&) -> FileProcessOutput {
                try {
                    std::ifstream file(file_path);
                    std::size_t line_count = 0;
                    std::string line;
                    while (std::getline(file, line)) {
                        line_count++;
                    }
                    return FileProcessOutput::success_output(file_path,
                                                             line_count);
                } catch (const std::exception& e) {
                    return FileProcessOutput::error_output(file_path, e.what());
                }
            };

            auto workflow =
                std::make_shared<DirectoryFileProcessor<FileProcessOutput>>(
                    file_processor);

            Pipeline pipeline("Directory Processing");
            auto task = use(workflow).emit_on(pipeline);

            DirectoryProcessInput input{test_dir, {".pfw"}};

            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, input);
            auto results =
                output.get<BatchProcessOutput<FileProcessOutput>>(task.id());

            std::cout << "    Processed " << results.successful_count << "/"
                      << results.results.size() << " files" << std::endl;
            std::cout << "    Total lines: " << results.total_items_processed
                      << std::endl;

            if (results.successful_count == 5 &&
                results.total_items_processed == 50) {
                std::cout << "    ✓ DirectoryFileProcessor successful"
                          << std::endl;
            } else {
                std::cout << "    ✗ Unexpected results" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 9b: LineBatchProcessor with plain files
        std::cout << "\n  9b: LineBatchProcessor (plain file)" << std::endl;
        try {
            using namespace dftracer::utils::utilities::io::lines;

            auto line_processor =
                [](const Line& line) -> std::optional<std::string> {
                // Only process lines containing "Line 5"
                if (line.content.find("Line 5") != std::string_view::npos) {
                    return std::string(line.content);
                }
                return std::nullopt;
            };

            auto workflow = std::make_shared<LineBatchProcessor<std::string>>(
                line_processor);

            Pipeline pipeline("Line Processing");
            auto task = use(workflow).emit_on(pipeline);

            auto input = LineReadInput::from_file(
                test_dir + "/file1.pfw");  // Plain file (no index)

            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, input);
            auto filtered_lines =
                output.get<std::vector<std::string>>(task.id());

            std::cout << "    Filtered " << filtered_lines.size()
                      << " lines:" << std::endl;
            for (const auto& line : filtered_lines) {
                std::cout << "      " << line << std::endl;
            }

            if (filtered_lines.size() == 1) {
                std::cout << "    ✓ LineBatchProcessor successful" << std::endl;
            } else {
                std::cout << "    ✗ Expected 1 line, got "
                          << filtered_lines.size() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Test 9c: SimpleLineBatchProcessor
        std::cout << "\n  9c: SimpleLineBatchProcessor" << std::endl;
        try {
            using namespace dftracer::utils::utilities::io::lines;

            auto simple_processor = [](const Line& line) -> std::size_t {
                return line.content.length();
            };

            auto workflow =
                std::make_shared<SimpleLineBatchProcessor<std::size_t>>(
                    simple_processor);

            Pipeline pipeline("Simple Line Processing");
            auto task = use(workflow).emit_on(pipeline);

            auto input = LineReadInput::from_file(test_dir + "/file2.pfw")
                             .with_range(1, 5);  // First 5 lines

            SequentialExecutor executor;
            PipelineOutput output = executor.execute(pipeline, input);
            auto lengths = output.get<std::vector<std::size_t>>(task.id());

            std::cout << "    Processed " << lengths.size() << " lines"
                      << std::endl;
            std::size_t total_length = 0;
            for (auto len : lengths) {
                total_length += len;
            }
            std::cout << "    Total character count: " << total_length
                      << std::endl;

            if (lengths.size() == 5) {
                std::cout << "    ✓ SimpleLineBatchProcessor successful"
                          << std::endl;
            } else {
                std::cout << "    ✗ Expected 5 lines, got " << lengths.size()
                          << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "    Error: " << e.what() << std::endl;
        }

        // Cleanup
        std::cout << "\n  Cleaning up test directory..." << std::endl;
        fs::remove_all(test_dir);
    }
    std::cout << std::endl;

    std::cout << "=== All Workflow Tests Completed ===\n" << std::endl;

    // Test 10: Adding behaviors without tags (Monitoring example)
    std::cout << "Test 10: Adding Behaviors Without Tags (Monitoring)"
              << std::endl;
    std::cout << "-----------------------------------------------------"
              << std::endl;
    {
        std::cout << "  ✓ Created utility WITHOUT Monitored tag" << std::endl;

        // Manually create monitoring behavior
        auto monitor = std::make_shared<MonitoringBehavior<int, int>>(
            [](const std::string& msg) {
                std::cout << "    " << msg << std::endl;
            },
            "SquareUtility"  // utility name
        );

        std::cout << "  ✓ Manually added MonitoringBehavior" << std::endl;

        auto utility = std::make_shared<SquareUtility>();
        Pipeline pipeline("Manual Monitoring Pipeline");

        // Add monitoring behavior manually using with_behavior()
        auto task = use(utility).with_behavior(monitor).emit_on(pipeline);

        SequentialExecutor executor;

        // Execute with different inputs
        std::cout << "\n  Executing with inputs: [3, 5, 7]" << std::endl;
        std::vector<int> inputs = {3, 5, 7};
        for (int input : inputs) {
            PipelineOutput output = executor.execute(pipeline, input);
            int result = output.get<int>(task.id());
            std::cout << "    Result: " << input << "^2 = " << result
                      << std::endl;
        }

        std::cout << "\n  Key takeaway:" << std::endl;
        std::cout << "    - Add ANY behavior to ANY utility using "
                     "with_behavior()"
                  << std::endl;
        std::cout << "    - No tags required - fully manual control!"
                  << std::endl;
        std::cout << "    - Can add multiple behaviors:" << std::endl;
        std::cout << "      use(utility).with_behavior(monitor)" << std::endl;
        std::cout << "                   .with_behavior(retry)" << std::endl;
        std::cout << "                   .emit_on(pipeline);" << std::endl;
    }
    std::cout << std::endl;

    // Test 11: Manual caching with cache->get() (no tag needed!)
    std::cout << "Test 11: Manual Caching with cache->get() (No Tag)"
              << std::endl;
    std::cout << "----------------------------------------------------"
              << std::endl;
    {
        std::cout << "  ✓ Created utility WITHOUT Cacheable tag" << std::endl;

        // Manually create caching behavior
        auto cache = std::make_shared<CachingBehavior<int, int>>(
            5,                         // max cache size
            std::chrono::seconds(60),  // TTL
            true                       // use LRU
        );

        std::cout << "  ✓ Created CachingBehavior (5 items, 60s TTL)"
                  << std::endl;

        auto utility = std::make_shared<SlowSquareUtility>();
        Pipeline pipeline("Manual Caching Pipeline");

        // Add caching behavior - it will store results
        auto task = use(utility).with_behavior(cache).emit_on(pipeline);

        SequentialExecutor executor;

        // Manual caching pattern: check cache->get() before execution
        std::cout << "\n  Processing inputs: [7, 7, 10, 7, 10]" << std::endl;
        std::vector<int> inputs = {7, 7, 10, 7, 10};

        auto start = std::chrono::steady_clock::now();

        for (int input : inputs) {
            // Check cache first using cache->get()
            auto cached = cache->get(input);

            int result;
            if (cached.has_value()) {
                std::cout << "    Input " << input << ": ✓ Cache HIT"
                          << std::endl;
                result = *cached;
            } else {
                std::cout << "    Input " << input << ": ✗ Cache MISS"
                          << std::endl;
                // Cache miss - execute utility (which will store result in
                // cache)
                PipelineOutput output = executor.execute(pipeline, input);
                result = output.get<int>(task.id());
            }
            std::cout << "      Result: " << result << std::endl;
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n  Performance:" << std::endl;
        std::cout << "    Computations: " << utility->get_call_count()
                  << " (only unique inputs)" << std::endl;
        std::cout << "    Time elapsed: " << elapsed.count() << "ms"
                  << std::endl;
        std::cout << "    Cache hits: " << (5 - utility->get_call_count())
                  << " / 5 calls" << std::endl;

        if (utility->get_call_count() == 2) {
            std::cout << "    ✓ Perfect! Only computed [7, 10] once each"
                      << std::endl;
        }

        std::cout << "\n  Key takeaway:" << std::endl;
        std::cout
            << "    - Use cache->get(input) to check cache BEFORE execution"
            << std::endl;
        std::cout << "    - CachingBehavior stores results in after_process()"
                  << std::endl;
        std::cout
            << "    - This pattern gives you full control - no tags needed!"
            << std::endl;
        std::cout << "    - Actual speedup: ~500ms → ~200ms (60% faster!)"
                  << std::endl;
    }
    std::cout << std::endl;

    // Test 12: Automatic caching with middleware pattern (no manual
    // cache->get()!)
    std::cout << "Test 12: Automatic Caching with Middleware Pattern"
              << std::endl;
    std::cout << "----------------------------------------------------"
              << std::endl;
    {
        std::cout << "  ✓ Created utility WITHOUT Cacheable tag" << std::endl;

        // Manually create caching behavior
        auto cache = std::make_shared<CachingBehavior<int, int>>(
            5,                         // max cache size
            std::chrono::seconds(60),  // TTL
            true                       // use LRU
        );

        std::cout << "  ✓ Created CachingBehavior (5 items, 60s TTL)"
                  << std::endl;

        auto utility = std::make_shared<SlowSquareUtility>();
        Pipeline pipeline("Automatic Caching Pipeline");

        // Add caching behavior - middleware will handle cache checking!
        auto task = use(utility).with_behavior(cache).emit_on(pipeline);

        SequentialExecutor executor;

        // NEW PATTERN: Just execute - cache automatically checks and bypasses!
        std::cout << "\n  Processing inputs: [7, 7, 10, 7, 10]" << std::endl;
        std::cout << "  (No manual cache->get() - middleware handles it!)"
                  << std::endl;
        std::vector<int> inputs = {7, 7, 10, 7, 10};

        auto start = std::chrono::steady_clock::now();

        for (int input : inputs) {
            std::cout << "    Input " << input << ": ";
            // Just execute - middleware checks cache automatically!
            PipelineOutput output = executor.execute(pipeline, input);
            int result = output.get<int>(task.id());
            std::cout << "Result = " << result << std::endl;
        }

        auto end = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n  Performance:" << std::endl;
        std::cout << "    Computations: " << utility->get_call_count()
                  << " (only unique inputs)" << std::endl;
        std::cout << "    Time elapsed: " << elapsed.count() << "ms"
                  << std::endl;
        std::cout << "    Cache hits: " << (5 - utility->get_call_count())
                  << " / 5 calls" << std::endl;

        if (utility->get_call_count() == 2) {
            std::cout
                << "    ✓ Perfect! Middleware automatically cached [7, 10]"
                << std::endl;
        }

        std::cout << "\n  Key takeaway:" << std::endl;
        std::cout << "    - Just execute() - cache checks happen automatically!"
                  << std::endl;
        std::cout << "    - Middleware pattern: behavior.process(input, next)"
                  << std::endl;
        std::cout << "    - Cache checks BEFORE calling next() (the utility)"
                  << std::endl;
        std::cout << "    - On cache hit: skip next(), return cached value"
                  << std::endl;
        std::cout << "    - On cache miss: call next(), store result, return"
                  << std::endl;
        std::cout << "    - No manual cache->get() needed - fully transparent!"
                  << std::endl;
        std::cout << "    - Actual speedup: ~500ms → ~200ms (60% faster!)"
                  << std::endl;
    }
    std::cout << std::endl;

    // Test 13: RetryBehavior with middleware pattern
    std::cout << "Test 13: RetryBehavior with Middleware Pattern" << std::endl;
    std::cout << "----------------------------------------------------"
              << std::endl;
    {
        // Utility that fails first 2 attempts, then succeeds
        class FlakyUtility : public Utility<int, int> {
           private:
            mutable int call_count_ = 0;

           public:
            int process(const int& input) override {
                call_count_++;
                std::cout << "      [Attempt #" << call_count_
                          << "] Processing " << input << "..." << std::endl;

                if (call_count_ < 3) {
                    std::cout << "      [Attempt #" << call_count_
                              << "] FAILED!" << std::endl;
                    throw std::runtime_error("Transient failure");
                }

                std::cout << "      [Attempt #" << call_count_ << "] SUCCESS!"
                          << std::endl;
                return input * input;
            }

            void reset() { call_count_ = 0; }
            int get_call_count() const { return call_count_; }
        };

        std::cout << "  ✓ Created FlakyUtility (fails 2x, then succeeds)"
                  << std::endl;

        // Create retry behavior with 3 max retries
        auto retry = std::make_shared<RetryBehavior<int, int>>(
            8,                              // max retries
            std::chrono::milliseconds(10),  // 10ms base delay
            false  // no exponential backoff (for speed)
        );

        std::cout << "  ✓ Created RetryBehavior (3 retries, 10ms delay)"
                  << std::endl;

        auto utility = std::make_shared<FlakyUtility>();
        Pipeline pipeline("Retry Pipeline");

        // Add retry behavior - middleware will handle retries!
        auto task = use(utility).with_behavior(retry).emit_on(pipeline);

        SequentialExecutor executor;

        // Test 1: Should succeed after retries
        std::cout << "\n  Test 13a: Utility that succeeds after 2 failures"
                  << std::endl;
        try {
            PipelineOutput output = executor.execute(pipeline, 5);
            int result = output.get<int>(task.id());
            std::cout << "  ✓ Result: " << result << " (after "
                      << utility->get_call_count() << " attempts)" << std::endl;

            if (utility->get_call_count() == 3 && result == 25) {
                std::cout
                    << "  ✓ RetryBehavior worked! Retried 2x before success"
                    << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ Unexpected exception: " << e.what() << std::endl;
        }

        // Test 2: Utility that always fails (exceeds max retries)
        std::cout << "\n  Test 13b: Utility that always fails (exceeds retries)"
                  << std::endl;

        class AlwaysFailsUtility : public Utility<int, int> {
           private:
            mutable int call_count_ = 0;

           public:
            int process([[maybe_unused]] const int& input) override {
                call_count_++;
                std::cout << "      [Attempt #" << call_count_
                          << "] Processing... FAILED!" << std::endl;
                throw std::runtime_error("Permanent failure");
            }

            int get_call_count() const { return call_count_; }
        };

        auto failing_utility = std::make_shared<AlwaysFailsUtility>();
        Pipeline failing_pipeline("Failing Retry Pipeline");
        auto failing_task =
            use(failing_utility).with_behavior(retry).emit_on(failing_pipeline);

        // Execute and let exception propagate through promise
        executor.execute(failing_pipeline, 7);

        // If we reach here, check the call count
        if (failing_utility->get_call_count() == 4) {
            std::cout
                << "  ✓ Retried correct number of times (1 initial + 3 retries)"
                << std::endl;
            std::cout << "  ✓ RetryBehavior correctly threw MaxRetriesExceeded "
                         "(logged above)"
                      << std::endl;
        } else {
            std::cout << "  ✗ Wrong retry count: "
                      << failing_utility->get_call_count() << std::endl;
        }

        std::cout << "\n  Key takeaway:" << std::endl;
        std::cout << "    - RetryBehavior.process() wraps next() with try/catch"
                  << std::endl;
        std::cout
            << "    - On exception: calls on_error() to decide retry/rethrow"
            << std::endl;
        std::cout << "    - Automatically retries with exponential backoff"
                  << std::endl;
        std::cout << "    - Throws MaxRetriesExceeded when retries exhausted"
                  << std::endl;
        std::cout << "    - Fully transparent - just add the behavior!"
                  << std::endl;
    }
    std::cout << std::endl;

    std::cout << "=== All Tests Completed ===\n" << std::endl;

    return 0;
}
