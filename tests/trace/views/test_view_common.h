#ifndef DFTRACER_TESTS_VIEW_COMMON_H
#define DFTRACER_TESTS_VIEW_COMMON_H
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

// A view test binary has no cli_main guard, so a test that touches the
// aggregation tier cache (which holds read-only agg DBs open for reuse) would
// leak those DBs at exit. Close them once all tests finish but while doctest's
// main - and every RocksDB static - is still alive; test_run_end runs
// in-process before teardown, avoiding the static-destruction-order races an
// atexit hook would hit. Mirrors cli_main's guard for the CLIs.
namespace test_view_common_detail {
struct RocksDbCleanupListener : doctest::IReporter {
    explicit RocksDbCleanupListener(const doctest::ContextOptions&) {}
    void test_run_end(const doctest::TestRunStats&) override {
        dftracer::utils::rocksdb::mark_process_exiting_for_rocksdb();
    }
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_case_start(const doctest::TestCaseData&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};
}  // namespace test_view_common_detail
DOCTEST_REGISTER_LISTENER("rocksdb_cleanup", 1,
                          test_view_common_detail::RocksDbCleanupListener);

namespace test_view_common {

template <class T>
T run(coro::CoroTask<T> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

// A trace mixing POSIX and STDIO events so filters have something to cut.
inline std::string create_mixed_trace(TestEnvironment& env, int posix_n,
                                      int stdio_n) {
    std::string pfw = env.get_dir() + "/mixed.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < posix_n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    for (int i = 0; i < stdio_n; ++i) {
        ofs << R"({"ph":"X","name":"fwrite","cat":"STDIO","pid":1,"tid":1,"ts":)"
            << (5000 + i * 100) << R"(,"dur":)" << (20 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// A multi-member gzip trace: `n` events framed into gzip members of about
// `member_bytes` each, so the scan must cross member boundaries. Each member's
// last event straddles into the next member (content here, terminating newline
// there), which is exactly the case that must not drop events.
inline std::string create_multimember_trace(TestEnvironment& env, int n,
                                            std::size_t member_bytes) {
    std::string pfw = env.get_dir() + "/mm.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, member_bytes);
    fs::remove(pfw);
    return gz;
}

// A trace with ph="X" events plus ph="C" counter events (aggregator style).
inline std::string create_trace_with_counters(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/ctr.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < 10; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":10,"args":{}})" << "\n";
    // Two counter events reporting cpu percentages.
    ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":1000,"args":{"user_pct":40,"idle_pct":60}})"
        << "\n";
    ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":2000,"args":{"user_pct":80,"idle_pct":20}})"
        << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// Many ph="C" counter events over two names (cpu, gpu), enough to span several
// scan batches so a tiny memory budget forces multiple spills of each group.
inline std::string create_bulk_counters(TestEnvironment& env, int n) {
    std::string pfw = env.get_dir() + "/bulk.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i) {
        const char* nm = (i % 2 == 0) ? "cpu" : "gpu";
        ofs << R"({"ph":"C","name":")" << nm
            << R"(","cat":"sys","pid":0,"tid":0,"ts":)" << (1000 + i * 100)
            << R"(,"args":{"util":)" << (i % 100) << R"(}})" << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// A file of cpu counter events with the given user_pct readings (one per
// event).
inline std::string create_counter_file(TestEnvironment& env,
                                       const std::string& tag,
                                       const std::vector<int>& user_pcts) {
    // Each shard gets its own dir so its index (keyed off the parent dir) does
    // not collide with the other shard's.
    std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    std::string pfw = dir + "/ctr.pfw";
    std::ofstream ofs(pfw);
    int ts = 1000;
    for (int v : user_pcts) {
        ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":)"
            << ts << R"(,"args":{"user_pct":)" << v << R"(}})" << "\n";
        ts += 1000;
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

// Collects every streamed line.
struct StringSink : ExportSink {
    std::string buffer;
    void write(std::string_view data) override { buffer.append(data); }

    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start < buffer.size()) {
            auto nl = buffer.find('\n', start);
            if (nl == std::string::npos) break;
            out.emplace_back(buffer.substr(start, nl - start));
            start = nl + 1;
        }
        return out;
    }
};

inline std::size_t count_containing(const std::vector<std::string>& lines,
                                    const std::string& needle) {
    std::size_t n = 0;
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

// Helpers for reading a collect() result, now a columnar dataframe::DataFrame.
// A DataFrame stores names + typed columns with no group/value/text category,
// so tests address columns by name.
inline std::int64_t bcol(const dataframe::DataFrame& b, std::string_view name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

inline bool bhas(const dataframe::DataFrame& b, std::string_view name) {
    return bcol(b, name) >= 0;
}

// Numeric value at `row` of the column named `name`, widened to double
// regardless of the column's stored width (Int64/Uint64/Float64).
inline double bnum(const dataframe::DataFrame& b, std::int64_t row,
                   std::string_view name) {
    const auto& c = b.columns[static_cast<std::size_t>(bcol(b, name))];
    using T = dataframe::TypeId;
    if (c.type() == T::Int64)
        return static_cast<double>(c.data<std::int64_t>()[row]);
    if (c.type() == T::Uint64)
        return static_cast<double>(c.data<std::uint64_t>()[row]);
    if (c.type() == T::Int32)
        return static_cast<double>(c.data<std::int32_t>()[row]);
    if (c.type() == T::Uint32)
        return static_cast<double>(c.data<std::uint32_t>()[row]);
    REQUIRE(c.type() == T::Float64);  // a text column reads with bstr
    return c.data<double>()[row];
}

inline std::string bstr(const dataframe::DataFrame& b, std::int64_t row,
                        std::string_view name) {
    return std::string(
        b.columns[static_cast<std::size_t>(bcol(b, name))].string_at(row));
}

// A mixed(30 POSIX "read", 20 STDIO "fwrite") trace whose index is built once
// and shared read-only across query cases. Building a RocksDB index per case is
// very slow under Valgrind, so read-only cases reuse this instead of rebuilding
// the same data. doctest runs cases serially, so concurrent access is not a
// concern.
struct SharedIndexedTrace {
    TestEnvironment env{200};
    std::string gz;
    std::string idx;
    SharedIndexedTrace() {
        gz = create_mixed_trace(env, 30, 20);
        idx = determine_index_path(gz, "");
        StringSink sink;
        View::from_file(gz, idx).metadata(false).export_json(sink).get();
    }
};

// 50-event fixture (30 POSIX "read" + 20 STDIO "fwrite"), indexed once.
inline const SharedIndexedTrace& shared_trace() {
    static SharedIndexedTrace t;
    return t;
}

}  // namespace test_view_common
using namespace test_view_common;

#endif  // DFTRACER_TESTS_VIEW_COMMON_H
