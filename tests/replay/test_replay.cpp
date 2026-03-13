#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/replay/replay.h>
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "testing_utilities.h"

using namespace dftracer::utils::utilities::replay;
namespace fs = std::filesystem;

TEST_CASE("DFTracer Replay - Basic functionality") {
    DFTRACER_UTILS_LOGGER_INIT();

    // Create a temporary trace file with sample data
    fs::path temp_dir = fs::temp_directory_path() / "dftracer_replay_test";
    fs::create_directories(temp_dir);

    std::string trace_file = (temp_dir / "test_trace.pfw").string();

    SUBCASE("Create sample trace file") {
        std::ofstream file(trace_file);
        REQUIRE(file.is_open());

        // Write sample trace entries based on the DFTracer format
        file << "[\n";
        file
            << R"({"id":1,"name":"opendir","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{"fhash":"abc123","level":1}})"
            << "\n";
        file
            << R"({"id":2,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1002000,"dur":2500,"ph":"X","args":{"fhash":"def456","size":1024,"level":1}})"
            << "\n";
        file
            << R"({"id":3,"name":"write","cat":"POSIX","pid":12345,"tid":12345,"ts":1005000,"dur":3000,"ph":"X","args":{"fhash":"ghi789","size":2048,"level":1}})"
            << "\n";
        file
            << R"({"id":4,"name":"fopen","cat":"STDIO","pid":12345,"tid":12345,"ts":1009000,"dur":500,"ph":"X","args":{"fhash":"jkl012","level":1}})"
            << "\n";
        file << "]";
        file.close();

        REQUIRE(fs::exists(trace_file));
    }

    SUBCASE("Test DFTracer sleep-based replay mode") {
        // First create the trace file
        {
            std::ofstream file(trace_file);
            file << "[\n";
            file
                << R"({"id":1,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{"fhash":"abc123","size":1024}})"
                << "\n";
            file
                << R"({"id":2,"name":"write","cat":"POSIX","pid":12345,"tid":12345,"ts":1002000,"dur":2500,"ph":"X","args":{"fhash":"def456","size":2048}})"
                << "\n";
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.maintain_timing = false;
        config.verbose = false;

        ReplayEngine engine(config);

        auto start_time = std::chrono::steady_clock::now();
        ReplayResult result = engine.replay(trace_file);
        auto end_time = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            end_time - start_time);

        CHECK(result.total_events > 0);
        CHECK(result.executed_events > 0);
        CHECK(result.failed_events == 0);
        CHECK(result.executed_events == result.total_events);

        std::cout << "Processed " << result.total_events << " events in "
                  << duration.count() << " microseconds" << std::endl;
    }

    SUBCASE("Test dry run mode") {
        // First create the trace file
        {
            std::ofstream file(trace_file);
            file << "[\n";
            file
                << R"({"id":1,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{"fhash":"abc123","size":1024}})"
                << "\n";
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = false;
        config.dry_run = true;
        config.maintain_timing = false;

        ReplayEngine engine(config);
        ReplayResult result = engine.replay(trace_file);

        CHECK(result.total_events > 0);
        CHECK(result.executed_events > 0);
        CHECK(result.failed_events == 0);

        std::cout << "Dry run: " << result.executed_events << "/"
                  << result.total_events << " executed" << std::endl;
    }

    SUBCASE("Test filter by category") {
        // First create the trace file with multiple categories
        {
            std::ofstream file(trace_file);
            file << "[\n";
            file
                << R"({"id":1,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{}})"
                << "\n";
            file
                << R"({"id":2,"name":"fopen","cat":"STDIO","pid":12345,"tid":12345,"ts":1002000,"dur":500,"ph":"X","args":{}})"
                << "\n";
            file
                << R"({"id":3,"name":"write","cat":"POSIX","pid":12345,"tid":12345,"ts":1003000,"dur":2000,"ph":"X","args":{}})"
                << "\n";
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.maintain_timing = false;
        config.filter_categories.insert("POSIX");

        ReplayEngine engine(config);
        ReplayResult result = engine.replay(trace_file);

        CHECK(result.total_events == 3);
        CHECK(result.executed_events == 2);  // Only POSIX events
        CHECK(result.filtered_events == 1);  // STDIO event filtered

        std::cout << "Category filter: " << result.executed_events << "/"
                  << result.total_events << " executed, "
                  << result.filtered_events << " filtered" << std::endl;
    }

    SUBCASE("Test filter by function") {
        // First create the trace file
        {
            std::ofstream file(trace_file);
            file << "[\n";
            file
                << R"({"id":1,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":1000000,"dur":1500,"ph":"X","args":{}})"
                << "\n";
            file
                << R"({"id":2,"name":"write","cat":"POSIX","pid":12345,"tid":12345,"ts":1002000,"dur":2500,"ph":"X","args":{}})"
                << "\n";
            file
                << R"({"id":3,"name":"open","cat":"POSIX","pid":12345,"tid":12345,"ts":1005000,"dur":500,"ph":"X","args":{}})"
                << "\n";
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.maintain_timing = false;
        config.filter_functions.insert("read");
        config.filter_functions.insert("write");

        ReplayEngine engine(config);
        ReplayResult result = engine.replay(trace_file);

        CHECK(result.total_events == 3);
        CHECK(result.executed_events == 2);  // Only read and write
        CHECK(result.filtered_events == 1);  // open filtered

        std::cout << "Function filter: " << result.executed_events << "/"
                  << result.total_events << " executed" << std::endl;
    }

    SUBCASE("Test max events limit") {
        // First create the trace file with many events
        {
            std::ofstream file(trace_file);
            file << "[\n";
            for (int i = 0; i < 10; i++) {
                file
                    << R"({"id":)" << i
                    << R"(,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":)"
                    << (1000000 + i * 1000)
                    << R"(,"dur":100,"ph":"X","args":{}})";
                if (i < 9) file << ",";
                file << "\n";
            }
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.maintain_timing = false;
        config.max_events = 5;

        ReplayEngine engine(config);
        ReplayResult result = engine.replay(trace_file);

        CHECK(result.executed_events <= 5);

        std::cout << "Max events limit: " << result.executed_events << "/"
                  << result.total_events << " executed (limit: 5)" << std::endl;
    }

    SUBCASE("Test sampling") {
        // First create the trace file with many events
        {
            std::ofstream file(trace_file);
            file << "[\n";
            for (int i = 0; i < 100; i++) {
                file
                    << R"({"id":)" << i
                    << R"(,"name":"read","cat":"POSIX","pid":12345,"tid":12345,"ts":)"
                    << (1000000 + i * 1000)
                    << R"(,"dur":100,"ph":"X","args":{}})";
                if (i < 99) file << ",";
                file << "\n";
            }
            file << "]";
            file.close();
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.maintain_timing = false;
        config.sampling_rate = 0.5;  // 50% sampling
        config.sample_deterministic = true;

        ReplayEngine engine(config);
        ReplayResult result = engine.replay(trace_file);

        // With 50% sampling, we expect roughly half the events
        // Allow some variance
        CHECK(result.executed_events > 30);
        CHECK(result.executed_events < 70);

        std::cout << "Sampling (50%): " << result.executed_events << "/"
                  << result.total_events << " executed" << std::endl;
    }

    // Cleanup
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

TEST_CASE("DFTracer Replay - Trace structure") {
    DFTRACER_UTILS_LOGGER_INIT();

    SUBCASE("Test Trace struct defaults") {
        Trace trace;

        CHECK(trace.pid == 0);
        CHECK(trace.tid == 0);
        CHECK(trace.time_start == 0);
        CHECK(trace.duration == 0.0);
        CHECK(trace.size == -1);
        CHECK(trace.offset == -1);
        CHECK(trace.is_valid == false);
        CHECK(trace.type == TraceType::Regular);
        CHECK(trace.is_metadata() == false);
    }

    SUBCASE("Test Trace helpers") {
        Trace trace;
        trace.time_start = 1000000;
        trace.duration = 500.0;
        trace.time_end = 1000500;
        trace.size = 1024;

        CHECK(trace.has_size() == true);
        CHECK(trace.has_offset() == false);
        CHECK(trace.has_valid_timing() == true);
        CHECK(trace.get_end_time() == 1000500);
    }
}

TEST_CASE("DFTracer Replay - ReplayResult statistics") {
    DFTRACER_UTILS_LOGGER_INIT();

    SUBCASE("Test result aggregation") {
        ReplayResult result;
        result.total_events = 100;
        result.executed_events = 80;
        result.filtered_events = 15;
        result.failed_events = 5;
        result.function_counts["read"] = 50;
        result.function_counts["write"] = 30;
        result.category_counts["POSIX"] = 80;
        result.total_bytes_read = 1024 * 1024;
        result.total_bytes_written = 512 * 1024;

        // Test print_summary doesn't crash
        result.print_summary(false);
        result.print_summary(true);
    }
}

TEST_CASE("DFTracer Replay - Real trace files") {
    DFTRACER_UTILS_LOGGER_INIT();

    // Test with real traces from trace_short directory
    std::string trace_dir = "trace_short/bert_v100-1.pfw";

    SUBCASE("Test with bert trace file") {
        if (!fs::exists(trace_dir)) {
            MESSAGE("Skipping real trace test - file not found: ", trace_dir);
            return;
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.no_sleep = true;  // Fast mode for testing
        config.maintain_timing = false;
        config.verbose = false;

        ReplayEngine engine(config);

        auto start_time = std::chrono::steady_clock::now();
        ReplayResult result = engine.replay(trace_dir);
        auto end_time = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);

        CHECK(result.total_events > 0);

        std::cout << "Processed " << result.total_events
                  << " events from bert trace in " << duration.count() << " ms"
                  << std::endl;
        std::cout << "Executed: " << result.executed_events
                  << ", Failed: " << result.failed_events << std::endl;
    }
}

TEST_CASE("DFTracer Replay - Call tree integration") {
    DFTRACER_UTILS_LOGGER_INIT();

    // Test with trace_short/cosmoflow_h100/nodes-1 directory
    std::string trace_dir = "trace_short/cosmoflow_h100/nodes-1";

    SUBCASE("Test call tree replay") {
        if (!fs::is_directory(trace_dir)) {
            MESSAGE("Skipping call tree test - directory not found: ",
                    trace_dir);
            return;
        }

        ReplayConfig config;
        config.dftracer_mode = true;
        config.no_sleep = true;
        config.maintain_timing = false;
        config.verbose = false;
        config.use_call_tree = true;
        config.hierarchical_replay = false;

        ReplayEngine engine(config);

        auto start_time = std::chrono::steady_clock::now();
        ReplayResult result = engine.replay_with_call_tree(trace_dir);
        auto end_time = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);

        CHECK(result.total_events > 0);
        CHECK(result.total_nodes > 0);

        std::cout << "Call tree replay: " << result.total_nodes << " nodes, "
                  << result.executed_events << " executed in "
                  << duration.count() << " ms" << std::endl;
    }
}
