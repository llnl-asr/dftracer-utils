// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_VIEW_COMMON_H
#define DFTRACER_UTILS_TESTS_VIEW_COMMON_H
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
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
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace test_view_common {

template <class T>
T run(dftracer::utils::coro::CoroTask<T> task) {
    return dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(task)).get());
}

template <class T>
T run(dftracer::utils::coro::CoroTask<dftracer::utils::dstd::Result<T>> task) {
    return dftu_utils_test::ok(
        dftracer::utils::default_runtime().submit(std::move(task)).get());
}

// The error a View, a plan or a result kept, as a Result<void>: success when
// it holds none.
inline dftracer::utils::dstd::Result<void> status_of(
    const dftracer::utils::trace::views::View& v) {
    return v.status();
}
inline dftracer::utils::dstd::Result<void> status_of(
    const dftracer::utils::dataframe::LazyFrame& lf) {
    return lf.status();
}
template <class T>
dftracer::utils::dstd::Result<void> status_of(
    const dftracer::utils::dataframe::LazyResult<T>& result) {
    return result.plans().front().status();
}
template <class T>
dftracer::utils::dstd::Result<void> status_of(
    const dftracer::utils::dstd::Result<T>& result) {
    if (result.has_value()) return {};
    return result.error();
}

// Checks that `what` kept or returned an error of `code` whose message holds
// `text`.
template <class T>
void expect_refused(const T& what, const char* text,
                    dftracer::utils::common::ErrorCode code =
                        dftracer::utils::common::ErrorCode::InvalidArgument) {
    const auto status = status_of(what);
    REQUIRE_MESSAGE(!status.has_value(), "expected an error");
    const dftracer::utils::dstd::String message(status.error().message());
    CAPTURE(message);
    CHECK(dftracer::utils::common::errorcode_of(status.error()) == code);
    CHECK(message.find(text) != dftracer::utils::dstd::String::npos);
}

// A trace mixing POSIX and STDIO events so filters have something to cut.
inline dftracer::utils::dstd::String create_mixed_trace(
    dftu_utils_test::TestEnvironment& env, int posix_n, int stdio_n) {
    dftracer::utils::dstd::String pfw = env.dir() + "/mixed.pfw";
    dftu_utils_test::FileOut ofs(pfw);
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
    dftracer::utils::dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// A multi-member gzip trace: `n` events framed into gzip members of about
// `member_bytes` each, so the scan must cross member boundaries. Each member's
// last event straddles into the next member (content here, terminating newline
// there), which is exactly the case that must not drop events.
inline dftracer::utils::dstd::String create_multimember_trace(
    dftu_utils_test::TestEnvironment& env, int n, std::size_t member_bytes) {
    dftracer::utils::dstd::String pfw = env.dir() + "/mm.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < n; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    }
    ofs.close();
    dftracer::utils::dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, member_bytes);
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// A trace with ph="X" events plus ph="C" counter events (aggregator style).
inline dftracer::utils::dstd::String create_trace_with_counters(
    dftu_utils_test::TestEnvironment& env) {
    dftracer::utils::dstd::String pfw = env.dir() + "/ctr.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < 10; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":10,"args":{}})" << "\n";
    // Two counter events reporting cpu percentages.
    ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":1000,"args":{"user_pct":40,"idle_pct":60}})"
        << "\n";
    ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":2000,"args":{"user_pct":80,"idle_pct":20}})"
        << "\n";
    ofs.close();
    dftracer::utils::dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// Many ph="C" counter events over two names (cpu, gpu), enough to span several
// scan batches so a tiny memory budget forces multiple spills of each group.
inline dftracer::utils::dstd::String create_bulk_counters(
    dftu_utils_test::TestEnvironment& env, int n) {
    dftracer::utils::dstd::String pfw = env.dir() + "/bulk.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < n; ++i) {
        const char* nm = (i % 2 == 0) ? "cpu" : "gpu";
        ofs << R"({"ph":"C","name":")" << nm
            << R"(","cat":"sys","pid":0,"tid":0,"ts":)" << (1000 + i * 100)
            << R"(,"args":{"util":)" << (i % 100) << R"(}})" << "\n";
    }
    ofs.close();
    dftracer::utils::dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// A file of cpu counter events with the given user_pct readings (one per
// event).
inline dftracer::utils::dstd::String create_counter_file(
    dftu_utils_test::TestEnvironment& env,
    const dftracer::utils::dstd::String& tag,
    const dftracer::utils::dstd::Vector<int>& user_pcts) {
    // Each shard gets its own dir so its index (keyed off the parent dir) does
    // not collide with the other shard's.
    dftracer::utils::dstd::String dir = env.dir() + "/" + tag;
    REQUIRE(dftracer::utils::dstd::fs::create_directories(dir).has_value());
    dftracer::utils::dstd::String pfw = dir + "/ctr.pfw";
    dftu_utils_test::FileOut ofs(pfw);
    int ts = 1000;
    for (int v : user_pcts) {
        ofs << R"({"ph":"C","name":"cpu","cat":"sys","pid":0,"tid":0,"ts":)"
            << ts << R"(,"args":{"user_pct":)" << v << R"(}})" << "\n";
        ts += 1000;
    }
    ofs.close();
    dftracer::utils::dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dftracer::utils::dstd::fs::remove(pfw).discard();
    return gz;
}

// Collects every streamed line.
struct StringSink : dftracer::utils::trace::views::ExportSink {
    dftracer::utils::dstd::String buffer;
    void write(dftracer::utils::dstd::StringView data) override {
        buffer.append(data);
    }

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> lines() const {
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> out;
        std::size_t start = 0;
        while (start < buffer.size()) {
            auto nl = buffer.find('\n', start);
            if (nl == dftracer::utils::dstd::String::npos) break;
            out.emplace_back(buffer.substr(start, nl - start));
            start = nl + 1;
        }
        return out;
    }
};

inline std::size_t count_containing(
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& lines,
    const dftracer::utils::dstd::String& needle) {
    std::size_t n = 0;
    for (const auto& l : lines)
        if (l.find(needle) != dftracer::utils::dstd::String::npos) ++n;
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

inline dftracer::utils::dstd::String bstr(
    const dftracer::utils::dataframe::DataFrame& b, std::int64_t row,
    dftracer::utils::dstd::StringView name) {
    return dftracer::utils::dstd::String(
        b.columns[static_cast<std::size_t>(bcol(b, name))].string_at(row));
}

// One text key per row over the named columns, in the order given, sorted, so
// two results compare as multisets of rows when their row order is
// unspecified. A null cell has its own key.
inline dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>
sorted_row_keys(
    const dftracer::utils::dataframe::DataFrame& b,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& cols) {
    using T = dftracer::utils::dataframe::TypeId;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
    dftracer::utils::dstd::Vector<dftracer::utils::dataframe::Series> mat;
    for (const auto& name : cols)
        mat.push_back(
            b.columns[static_cast<std::size_t>(bcol(b, name))].materialize());
    for (std::int64_t r = 0; r < b.num_rows(); ++r) {
        dftracer::utils::dstd::String key;
        for (const auto& col : mat) {
            if (col.is_null(r)) {
                key += "\x01null";
            } else if (col.type() == T::Float64) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", col.data<double>()[r]);
                key += buf;
            } else {
                key += dftracer::utils::dataframe::cell_to_string(col, r);
            }
            key += '\x1f';
        }
        keys.push_back(std::move(key));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

inline dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>
sorted_row_keys(const dftracer::utils::dataframe::DataFrame& b) {
    auto cols = b.names;
    std::sort(cols.begin(), cols.end());
    return sorted_row_keys(b, cols);
}

// A mixed(30 POSIX "read", 20 STDIO "fwrite") trace whose index is built once
// and shared read-only across query cases. Building an index per case is
// very slow under Valgrind, so read-only cases reuse this instead of rebuilding
// the same data. doctest runs cases serially, so concurrent access is not a
// concern.
struct SharedIndexedTrace {
    dftu_utils_test::TestEnvironment env{200};
    dftracer::utils::dstd::String gz;
    dftracer::utils::dstd::String idx;
    SharedIndexedTrace() {
        gz = create_mixed_trace(env, 30, 20);
        idx = dftracer::utils::trace::internal::determine_index_path(gz, "");
        StringSink sink;
        dftu_utils_test::ok(dftracer::utils::trace::views::View::from_file(
                                dftracer::utils::dstd::String(gz),
                                dftracer::utils::dstd::String(idx))
                                .sink_json(sink)
                                .get());
    }
};

// 50-event fixture (30 POSIX "read" + 20 STDIO "fwrite"), indexed once.
inline const SharedIndexedTrace& shared_trace() {
    static SharedIndexedTrace t;
    return t;
}

}  // namespace test_view_common

#endif  // DFTRACER_UTILS_TESTS_VIEW_COMMON_H
