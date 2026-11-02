// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_VIEW_COMMON_H
#define DFTRACER_UTILS_TESTS_VIEW_COMMON_H
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace test_view_common {

template <class T>
T run(dftracer::utils::coro::CoroTask<T> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

// A trace mixing POSIX and STDIO events so filters have something to cut.
inline std::string create_mixed_trace(dftu_utils_test::TestEnvironment& env,
                                      int posix_n, int stdio_n) {
    std::string pfw = env.get_dir() + "/mixed.pfw";
    std::ofstream ofs(pfw.c_str());
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
    (void)dftracer::utils::dstd::fs::remove(pfw);
    return gz;
}

// A multi-member gzip trace: `n` events framed into gzip members of about
// `member_bytes` each, so the scan must cross member boundaries. Each member's
// last event straddles into the next member (content here, terminating newline
// there), which is exactly the case that must not drop events.
inline std::string create_multimember_trace(
    dftu_utils_test::TestEnvironment& env, int n, std::size_t member_bytes) {
    std::string pfw = env.get_dir() + "/mm.pfw";
    std::ofstream ofs(pfw.c_str());
    for (int i = 0; i < n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, member_bytes);
    (void)dftracer::utils::dstd::fs::remove(pfw);
    return gz;
}

// A trace with ph="X" events plus ph="C" counter events (aggregator style).
inline std::string create_trace_with_counters(
    dftu_utils_test::TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/ctr.pfw";
    std::ofstream ofs(pfw.c_str());
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
    (void)dftracer::utils::dstd::fs::remove(pfw);
    return gz;
}

// Many ph="C" counter events over two names (cpu, gpu), enough to span several
// scan batches so a tiny memory budget forces multiple spills of each group.
inline std::string create_bulk_counters(dftu_utils_test::TestEnvironment& env,
                                        int n) {
    std::string pfw = env.get_dir() + "/bulk.pfw";
    std::ofstream ofs(pfw.c_str());
    for (int i = 0; i < n; ++i) {
        const char* nm = (i % 2 == 0) ? "cpu" : "gpu";
        ofs << R"({"ph":"C","name":")" << nm
            << R"(","cat":"sys","pid":0,"tid":0,"ts":)" << (1000 + i * 100)
            << R"(,"args":{"util":)" << (i % 100) << R"(}})" << "\n";
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    (void)dftracer::utils::dstd::fs::remove(pfw);
    return gz;
}

// A file of cpu counter events with the given user_pct readings (one per
// event).
inline std::string create_counter_file(dftu_utils_test::TestEnvironment& env,
                                       const std::string& tag,
                                       const std::vector<int>& user_pcts) {
    // Each shard gets its own dir so its index (keyed off the parent dir) does
    // not collide with the other shard's.
    std::string dir = env.get_dir() + "/" + tag;
    REQUIRE(dftracer::utils::dstd::fs::create_directories(dir).has_value());
    std::string pfw = dir + "/ctr.pfw";
    std::ofstream ofs(pfw.c_str());
    int ts = 1000;
    for (int v : user_pcts) {
        ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":)"
            << ts << R"(,"args":{"user_pct":)" << v << R"(}})" << "\n";
        ts += 1000;
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    (void)dftracer::utils::dstd::fs::remove(pfw);
    return gz;
}

// Collects every streamed line.
struct StringSink : dftracer::utils::trace::views::ExportSink {
    std::string buffer;
    void write(dftracer::utils::dstd::StringView data) override {
        buffer.append(data);
    }

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

// Helpers for reading a collect() result, now a columnar
// dftracer::utils::dataframe::DataFrame. A DataFrame stores names + typed
// columns with no group/value/text category, so tests address columns by name.
inline std::int64_t bcol(const dftracer::utils::dataframe::DataFrame& b,
                         dftracer::utils::dstd::StringView name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}

inline bool bhas(const dftracer::utils::dataframe::DataFrame& b,
                 dftracer::utils::dstd::StringView name) {
    return bcol(b, name) >= 0;
}

// Numeric value at `row` of the column named `name`, widened to double
// regardless of the column's stored width (Int64/Uint64/Float64).
inline double bnum(const dftracer::utils::dataframe::DataFrame& b,
                   std::int64_t row, dftracer::utils::dstd::StringView name) {
    const auto c =
        b.columns[static_cast<std::size_t>(bcol(b, name))].materialize();
    using T = dftracer::utils::dataframe::TypeId;
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

inline std::string bstr(const dftracer::utils::dataframe::DataFrame& b,
                        std::int64_t row,
                        dftracer::utils::dstd::StringView name) {
    return std::string(
        b.columns[static_cast<std::size_t>(bcol(b, name))].string_at(row));
}

// A mixed(30 POSIX "read", 20 STDIO "fwrite") trace whose index is built once
// and shared read-only across query cases. Building an index per case is
// very slow under Valgrind, so read-only cases reuse this instead of rebuilding
// the same data. doctest runs cases serially, so concurrent access is not a
// concern.
struct SharedIndexedTrace {
    dftu_utils_test::TestEnvironment env{200};
    std::string gz;
    std::string idx;
    SharedIndexedTrace() {
        gz = create_mixed_trace(env, 30, 20);
        idx = dftracer::utils::trace::internal::determine_index_path(gz, "");
        StringSink sink;
        dftracer::utils::trace::views::View::from_file(gz, idx)
            .sink_json(sink)
            .get();
    }
};

// 50-event fixture (30 POSIX "read" + 20 STDIO "fwrite"), indexed once.
inline const SharedIndexedTrace& shared_trace() {
    static SharedIndexedTrace t;
    return t;
}

}  // namespace test_view_common

#endif  // DFTRACER_UTILS_TESTS_VIEW_COMMON_H
