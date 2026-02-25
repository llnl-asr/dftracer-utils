#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::views;
using namespace dft_utils_test;

// Helper: write a custom DFTracer trace file with metadata and mixed categories
// Returns the plain text file path
static std::string create_view_test_trace(const std::string& dir) {
    std::string file_path = dir + "/view_test.trace";
    std::ofstream ofs(file_path);

    // Hash metadata events (ph="M")
    // HH: host hash
    ofs << R"({"name":"HH","ph":"M","pid":0,"tid":0,"args":{"value":"host_abc","hostname":"node01"}})"
        << "\n";
    // FH: file hashes
    ofs << R"({"name":"FH","ph":"M","pid":0,"tid":0,"args":{"value":"file_001","filename":"/data/train.h5"}})"
        << "\n";
    ofs << R"({"name":"FH","ph":"M","pid":0,"tid":0,"args":{"value":"file_002","filename":"/data/val.h5"}})"
        << "\n";
    // SH: script hash
    ofs << R"({"name":"SH","ph":"M","pid":0,"tid":0,"args":{"value":"script_x","cmd":"python train.py"}})"
        << "\n";

    // thread_name metadata (non-hash, should always be emitted when
    // include_metadata=true)
    ofs << R"({"name":"thread_name","ph":"M","pid":1000,"tid":2000,"args":{"name":"MainThread"}})"
        << "\n";

    // POSIX I/O events (reference file_001 and host_abc)
    ofs << R"({"name":"read","ph":"X","cat":"POSIX","pid":1000,"tid":2000,"ts":1000000,"dur":500,"args":{"ret":4096,"hhash":"host_abc","fhash":"file_001"}})"
        << "\n";
    ofs << R"({"name":"write","ph":"X","cat":"POSIX","pid":1000,"tid":2000,"ts":1001000,"dur":300,"args":{"ret":2048,"hhash":"host_abc","fhash":"file_001"}})"
        << "\n";
    ofs << R"({"name":"pread64","ph":"X","cat":"POSIX","pid":1000,"tid":2000,"ts":1002000,"dur":150,"args":{"ret":1024,"hhash":"host_abc","fhash":"file_002"}})"
        << "\n";

    // STDIO event
    ofs << R"({"name":"fwrite","ph":"X","cat":"STDIO","pid":1000,"tid":2000,"ts":1003000,"dur":200,"args":{"ret":512,"hhash":"host_abc"}})"
        << "\n";

    // Compute events (reference host_abc but NOT file hashes)
    ofs << R"({"name":"forward","ph":"X","cat":"compute","pid":1000,"tid":2000,"ts":1100000,"dur":50000,"args":{"hhash":"host_abc"}})"
        << "\n";
    ofs << R"({"name":"backward","ph":"X","cat":"compute","pid":1000,"tid":2000,"ts":1150000,"dur":60000,"args":{"hhash":"host_abc"}})"
        << "\n";

    // AI framework event (reference script hash)
    ofs << R"({"name":"DataLoader","ph":"X","cat":"ai_framework","pid":1000,"tid":2000,"ts":1210000,"dur":10000,"args":{"hhash":"host_abc","shash":"script_x"}})"
        << "\n";

    // An event with large duration for duration filter testing
    ofs << R"({"name":"checkpoint","ph":"X","cat":"checkpoint","pid":1000,"tid":2000,"ts":1300000,"dur":500000,"args":{"hhash":"host_abc"}})"
        << "\n";

    ofs.close();
    return file_path;
}

// Helper: create gzip + index from a plain trace file
static std::pair<std::string, std::string> compress_and_index(
    const std::string& plain_path) {
    std::string gz_path = plain_path + ".gz";
    compress_file_to_gzip(plain_path, gz_path);

    std::string idx_path = gz_path + ".idx";
    // Index will be auto-built by IndexedFileReaderUtility
    return {gz_path, idx_path};
}

TEST_SUITE("ViewReaderUtility") {
    TEST_CASE("ViewReader - IO view matches POSIX/STDIO events") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(
                0, std::numeric_limits<std::size_t>::max())  // 0,0 means read
                                                             // everything
            .with_view(ViewDefinition::io_view());

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);
        // Should match: read, write, pread64, fwrite (4 I/O events)
        // + thread_name metadata (1) + referenced hash metadata (HH, FH x2)
        CHECK(output.events_scanned > 0);
        CHECK(output.events_matched > 0);

        // Count actual I/O events (non-metadata)
        std::uint64_t io_events = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"ph\":\"X\"") != std::string::npos) {
                io_events++;
            }
        }
        CHECK(io_events == 4);  // read, write, pread64, fwrite
    }

    TEST_CASE("ViewReader - Compute view matches compute/ai_framework events") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::compute_view());

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        std::uint64_t compute_events = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"ph\":\"X\"") != std::string::npos) {
                compute_events++;
            }
        }
        // forward, backward, DataLoader = 3 compute/ai_framework events
        CHECK(compute_events == 3);
    }

    TEST_CASE("ViewReader - Smart metadata: FH only included when referenced") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        // IO view -- references file_001 and file_002
        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::io_view());

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        int fh_count = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"name\":\"FH\"") != std::string::npos) {
                fh_count++;
            }
        }
        // IO events reference file_001 and file_002
        CHECK(fh_count == 2);

        // Now check compute view -- no fhash references
        ViewReaderInput compute_input;
        compute_input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::compute_view());

        auto compute_output = reader.process(compute_input).get();
        CHECK(compute_output.success);

        int compute_fh_count = 0;
        for (const auto& ev : compute_output.events) {
            if (ev.find("\"name\":\"FH\"") != std::string::npos) {
                compute_fh_count++;
            }
        }
        // Compute events don't reference any file hash
        CHECK(compute_fh_count == 0);
    }

    TEST_CASE("ViewReader - Smart metadata: SH only with script references") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        // Compute view includes ai_framework which has shash=script_x
        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::compute_view());

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        int sh_count = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"name\":\"SH\"") != std::string::npos) {
                sh_count++;
            }
        }
        // DataLoader references shash=script_x
        CHECK(sh_count == 1);

        // IO view -- no shash references
        ViewReaderInput io_input;
        io_input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::io_view());

        auto io_output = reader.process(io_input).get();
        int io_sh_count = 0;
        for (const auto& ev : io_output.events) {
            if (ev.find("\"name\":\"SH\"") != std::string::npos) {
                io_sh_count++;
            }
        }
        CHECK(io_sh_count == 0);
    }

    TEST_CASE("ViewReader - thread_name always emitted with metadata") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(ViewDefinition::compute_view());

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        int thread_name_count = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"name\":\"thread_name\"") != std::string::npos) {
                thread_name_count++;
            }
        }
        CHECK(thread_name_count == 1);
    }

    TEST_CASE("ViewReader - no metadata when include_metadata=false") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        auto io_view = ViewDefinition::io_view();
        io_view.with_include_metadata(false);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(io_view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        // No metadata events should be present
        for (const auto& ev : output.events) {
            CHECK(ev.find("\"ph\":\"M\"") == std::string::npos);
        }

        // Should still have I/O events
        CHECK(output.events_matched > 0);
    }

    TEST_CASE("ViewReader - Custom predicate with name filter") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewDefinition view;
        view.with_name("read_only");
        ViewPredicate pred;
        pred.with_bloom_dim("name", {"read"});
        view.with_predicate(std::move(pred));

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        std::uint64_t matched_events = 0;
        for (const auto& ev : output.events) {
            if (ev.find("\"ph\":\"X\"") != std::string::npos) {
                matched_events++;
                CHECK(ev.find("\"name\":\"read\"") != std::string::npos);
            }
        }
        CHECK(matched_events == 1);
    }

    TEST_CASE("ViewReader - Duration filter") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        // Match only events with dur > 100000 us
        ViewDefinition view;
        view.with_name("long_events");
        ViewPredicate pred;
        pred.with_min_duration(100000.0);
        // Need at least one bloom dim or the predicate won't match anything
        // since matches_predicate checks bloom dims first
        // Actually no -- if dim_sets is empty, the bloom loop is skipped and
        // we proceed to time/duration checks
        view.with_predicate(std::move(pred));
        view.with_include_metadata(false);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);

        // Only the checkpoint event has dur=500000
        for (const auto& ev : output.events) {
            CHECK(ev.find("\"ph\":\"X\"") != std::string::npos);
        }
        CHECK(output.events_matched >= 1);
    }

    TEST_CASE("ViewReader - Time range filter") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        // Only match events with ts between 1100000 and 1200000
        ViewDefinition view;
        view.with_name("time_window");
        ViewPredicate pred;
        pred.with_time_range(1100000.0, 1200000.0);
        view.with_predicate(std::move(pred));
        view.with_include_metadata(false);

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);
        // Events in this time range: forward (ts=1100000), backward
        // (ts=1150000)
        CHECK(output.events_matched == 2);
    }

    TEST_CASE("ViewReader - Multiple predicates (OR between groups)") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewDefinition view;
        view.with_name("multi_pred");
        view.with_include_metadata(false);

        // Predicate 1: POSIX reads
        ViewPredicate pred1;
        pred1.with_bloom_dim("name", {"read"}).with_bloom_dim("cat", {"POSIX"});
        view.with_predicate(std::move(pred1));

        // Predicate 2: compute operations
        ViewPredicate pred2;
        pred2.with_bloom_dim("cat", {"compute"});
        view.with_predicate(std::move(pred2));

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);
        // Pred1 matches: read (1 event)
        // Pred2 matches: forward, backward (2 events)
        // Total: 3 events
        CHECK(output.events_matched == 3);
    }

    TEST_CASE("ViewReader - Empty events for non-matching filter") {
        TestEnvironment env(100);
        std::string plain = create_view_test_trace(env.get_dir());
        auto [gz_path, idx_path] = compress_and_index(plain);

        ViewDefinition view;
        view.with_name("no_match");
        view.with_include_metadata(false);

        ViewPredicate pred;
        pred.with_bloom_dim("name", {"nonexistent_operation"});
        view.with_predicate(std::move(pred));

        ViewReaderInput input;
        input.with_file_path(gz_path)
            .with_idx_path(idx_path)
            .with_checkpoint_size(1024)
            .with_byte_range(0, std::numeric_limits<std::size_t>::max())
            .with_view(view);

        ViewReaderUtility reader;
        auto output = reader.process(input).get();

        CHECK(output.success);
        CHECK(output.events_matched == 0);
        CHECK(output.events.empty());
    }
}
