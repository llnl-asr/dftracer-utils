#ifndef DFTRACER_UTILS_TESTS_ROCKSDB_EXIT_LISTENER_H
#define DFTRACER_UTILS_TESTS_ROCKSDB_EXIT_LISTENER_H

#include <dftracer/utils/index/store/database.h>
#include <doctest/doctest.h>

// A test binary has no cli_main guard, so a test that touches the
// aggregation tier cache (which holds read-only agg DBs open for reuse) would
// leak those DBs at exit. Close them once all tests finish but while doctest's
// main - and every RocksDB static - is still alive; test_run_end runs
// in-process before teardown, avoiding the static-destruction-order races an
// atexit hook would hit. Mirrors cli_main's guard for the CLIs.
namespace rocksdb_exit_listener_detail {
struct RocksDbCleanupListener : doctest::IReporter {
    explicit RocksDbCleanupListener(const doctest::ContextOptions&) {}
    void test_run_end(const doctest::TestRunStats&) override {
        dftracer::utils::index::store::mark_process_exiting_for_rocksdb();
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
}  // namespace rocksdb_exit_listener_detail
DOCTEST_REGISTER_LISTENER("rocksdb_cleanup", 1,
                          rocksdb_exit_listener_detail::RocksDbCleanupListener);

#endif  // DFTRACER_UTILS_TESTS_ROCKSDB_EXIT_LISTENER_H
