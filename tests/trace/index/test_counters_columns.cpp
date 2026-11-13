// SPDX-License-Identifier: MIT
// The counters of a typed collect, answered from the aggregation tier's chunk
// column blocks: the rows, their per-second means and the shard splits.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <tuple>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

constexpr std::uint64_t BASE_TS = 1'000;

// A member of the trace and a chunk of the index, so the counters span several
// column blocks.
constexpr std::size_t CHECKPOINT_SIZE = 2048;

// Three seconds of sys counters on two hosts: `util` is a fraction and `num`
// an integer, and every third event is a pre-aggregated counter, which the
// counters skip. App counters and plain events share the trace and are not
// counters at all.
dstd::String write_trace(const dstd::String& dir) {
    const dstd::String pfw = dir + "/counters.pfw";
    {
        dstd::String out;
        for (int i = 0; i < 24; ++i) {
            const dstd::String host = (i % 2 == 0) ? "h1" : "h2";
            const std::uint64_t ts =
                BASE_TS + static_cast<std::uint64_t>(i) * 130'000;
            out += common::str_cat(
                R"({"ph":"C","name":"cpu","cat":"sys","pid":1,"tid":1,)",
                R"("ts":)", ts, R"(,"args":{"hhash":")", host, R"(","util":)",
                (i + 0.5), R"(,"num":)", (10 + i), "}}\n");
            if (i % 6 == 0)
                out += common::str_cat(
                    R"({"ph":"C","name":"mem","cat":"sys","pid":1,)",
                    R"("tid":1,"ts":)", ts + 7'000, R"(,"args":{"hhash":")",
                    host, R"(","used":)", (1000 + i), "}}\n");
            if (i % 3 == 0)
                out += common::str_cat(
                    R"({"ph":"C","name":"cpu","cat":"sys","pid":1,)",
                    R"("tid":1,"ts":)", ts + 11'000, R"(,"args":{"hhash":")",
                    host, R"(","count":5,"util":99.0}})", "\n");
            out += common::str_cat(
                R"({"ph":"C","name":"faults","cat":"app","pid":1,)",
                R"("tid":1,"ts":)", ts + 3'000, R"(,"args":{"count":2}})",
                "\n");
            out += common::str_cat(
                R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)",
                R"("tid":1,"ts":)", ts + 5'000,
                R"(,"dur":10,"args":{"size":4096}})", "\n");
        }
        dstd::fs::write_file(dstd::PathView(pfw), out).discard();
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    dstd::fs::remove(pfw).discard();
    return gz;
}

IndexerOptions with_aggregation(const dstd::String& index_dir) {
    IndexerOptions o;
    o.index_dir = index_dir;
    o.checkpoint_size = CHECKPOINT_SIZE;
    schemas::dft::agg::AggregationConfig c;
    c.time_interval_us = 1'000'000;
    o.aggregation = c;
    return o;
}

struct Group {
    std::uint64_t count = 0;
    std::uint64_t ts = UINT64_MAX;
    std::uint64_t te = 0;
    dstd::Map<dstd::String, double> sum;
};

using Expected =
    dstd::Map<dstd::Tuple<dstd::String, dstd::String, std::uint64_t>, Group>;

// The counters the trace above must give: one row per (host, name, second),
// with the pre-aggregated counter of every third event left out.
Expected expected_groups() {
    Expected out;
    for (int i = 0; i < 24; ++i) {
        const dstd::String host = (i % 2 == 0) ? "h1" : "h2";
        const std::uint64_t ts =
            BASE_TS + static_cast<std::uint64_t>(i) * 130'000;
        const std::uint64_t bucket = (ts / 1'000'000) * 1'000'000;
        Group& cpu = out[{host, "cpu", bucket}];
        ++cpu.count;
        cpu.ts = std::min(cpu.ts, ts);
        cpu.te = std::max(cpu.te, ts);
        cpu.sum["util"] += i + 0.5;
        cpu.sum["num"] += 10 + i;
        if (i % 6 == 0) {
            Group& mem = out[{host, "mem", bucket}];
            ++mem.count;
            mem.ts = std::min(mem.ts, ts + 7'000);
            mem.te = std::max(mem.te, ts + 7'000);
            mem.sum["used"] += 1000 + i;
        }
    }
    return out;
}

dstd::Vector<dstd::String> string_column(const dataframe::DataFrame& df,
                                         const dstd::String& name) {
    dstd::Vector<dstd::String> out;
    for (std::size_t i = 0; i < df.names.size(); ++i)
        if (dstd::StringView(df.names[i]) == dstd::StringView(name)) {
            const auto s = df.columns[i].materialize();
            for (std::int64_t r = 0; r < df.num_rows(); ++r)
                out.emplace_back(s.string_at(r));
        }
    return out;
}

dstd::Vector<double> f64_column(const dataframe::DataFrame& df,
                                const dstd::String& name) {
    dstd::Vector<double> out;
    for (std::size_t i = 0; i < df.names.size(); ++i)
        if (dstd::StringView(df.names[i]) == dstd::StringView(name)) {
            const auto s = df.columns[i].materialize();
            for (std::int64_t r = 0; r < df.num_rows(); ++r)
                out.push_back(s.data<double>()[r]);
        }
    return out;
}

// The row keys of a counters frame, (host, name, time_bucket).
dstd::Vector<dstd::Tuple<dstd::String, dstd::String, std::uint64_t>> row_keys(
    const dataframe::DataFrame& df) {
    const auto hosts = string_column(df, "host_hash");
    const auto names = string_column(df, "name");
    const auto buckets = f64_column(df, "time_bucket");
    dstd::Vector<dstd::Tuple<dstd::String, dstd::String, std::uint64_t>> out;
    for (std::size_t r = 0; r < hosts.size(); ++r)
        out.emplace_back(hosts[r], names[r],
                         static_cast<std::uint64_t>(buckets[r]));
    return out;
}

void check_against(const dataframe::DataFrame& df, const Expected& want) {
    REQUIRE(df.num_rows() == static_cast<std::int64_t>(want.size()));
    const auto hosts = string_column(df, "host_hash");
    const auto names = string_column(df, "name");
    const auto buckets = f64_column(df, "time_bucket");
    const auto ts = f64_column(df, "ts");
    const auto te = f64_column(df, "te");
    const auto count = f64_column(df, "count");
    REQUIRE(hosts.size() == want.size());
    std::size_t seen = 0;
    for (const auto& [key, group] : want) {
        const auto& [host, name, bucket] = key;
        std::size_t at = hosts.size();
        for (std::size_t r = 0; r < hosts.size(); ++r)
            if (hosts[r] == host && names[r] == name &&
                static_cast<std::uint64_t>(buckets[r]) == bucket)
                at = r;
        REQUIRE_MESSAGE(at < hosts.size(), host, " ", name, " ", bucket);
        ++seen;
        CHECK(count[at] == static_cast<double>(group.count));
        CHECK(ts[at] == static_cast<double>(group.ts));
        CHECK(te[at] == static_cast<double>(group.te));
        for (const char* metric_text : {"num", "used", "util"}) {
            const dstd::String metric(metric_text);
            const auto it = group.sum.find(metric);
            const auto column = f64_column(df, metric);
            if (it == group.sum.end()) {
                // A metric no group of the frame carries has no column at all.
                CHECK((column.empty() || std::isnan(column[at])));
                continue;
            }
            REQUIRE_FALSE(column.empty());
            CHECK(
                column[at] ==
                doctest::Approx(it->second / static_cast<double>(group.count)));
        }
    }
    CHECK(seen == want.size());
}

std::uint16_t shard_of(
    const dstd::Tuple<dstd::String, dstd::String, std::uint64_t>& key) {
    return schemas::dft::agg::system_shard(std::get<0>(key), std::get<1>(key));
}

// What the column blocks of the index of `gz` in `idx` hold: how many there
// are, how many carry a sys counter and how many rows are anything else.
struct Blocks {
    std::size_t total = 0;
    std::size_t counters = 0;
    std::size_t other_rows = 0;
};

Blocks blocks_of(const dstd::String& gz, const dstd::String& idx) {
    Blocks out;
    const auto index_path =
        dstd::String(trace::internal::determine_index_path(gz, idx));
    const auto snapshot =
        dftu_utils_test::ok(trace::index::plan::open_snapshot(index_path));
    const auto file = snapshot.file(fileio::get_logical_path(gz));
    REQUIRE(file.has_value());
    dftu_utils_test::ok(trace::index::for_each_agg_chunk_columns(
        snapshot, *file,
        [&](std::uint64_t, const schemas::dft::agg::ChunkColumns& block) {
            ++out.total;
            bool holds_counter = false;
            for (std::uint64_t r = 0; r < block.rows; ++r)
                if (block.phase[r] == static_cast<std::uint8_t>(
                                          trace::RecordPhase::Counter) &&
                    dstd::StringView(block.texts[block.cat[r]]) == "sys")
                    holds_counter = true;
                else
                    ++out.other_rows;
            if (holds_counter) ++out.counters;
            return true;
        }));
    return out;
}

}  // namespace

TEST_SUITE("CountersColumns") {
    TEST_CASE("the counters of the index are the per-second means") {
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_trace(env.dir());
        const dstd::String idx = env.dir() + "/idx";
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(gz))},
                              with_aggregation(idx)))
                .build());
        const auto typed =
            dftu_utils_test::ok(trace::views::View::from_files(
                                    {{dstd::String(gz), dstd::String(idx)}})
                                    .collect_typed()
                                    .get());
        check_against(typed.counters, expected_groups());

        // The trace spans several chunks, and the counters still come out as
        // the means of the trace text.
        const auto blocks = blocks_of(gz, idx);
        CHECK(blocks.total > 1);
        CHECK(blocks.counters > 1);
        CHECK(blocks.other_rows == 0);
    }

    TEST_CASE("a shard split splits the counters without a repeat") {
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_trace(env.dir());
        const dstd::String idx = env.dir() + "/idx";
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(gz))},
                              with_aggregation(idx)))
                .build());
        const auto whole =
            dftu_utils_test::ok(trace::views::View::from_files(
                                    {{dstd::String(gz), dstd::String(idx)}})
                                    .collect_typed()
                                    .get());
        check_against(whole.counters, expected_groups());

        // The split falls on the second shard the groups name, so both halves
        // hold rows.
        dstd::Set<std::uint16_t> shards;
        for (const auto& [key, group] : expected_groups())
            shards.insert(shard_of(key));
        REQUIRE(shards.size() > 1);
        int boundary = *std::next(shards.begin());
        Expected lo;
        Expected hi;
        for (const auto& [key, group] : expected_groups())
            (shard_of(key) < boundary ? lo : hi).emplace(key, group);
        CHECK_FALSE(lo.empty());
        CHECK_FALSE(hi.empty());

        const auto first =
            dftu_utils_test::ok(trace::views::View::from_files(
                                    {{dstd::String(gz), dstd::String(idx)}})
                                    .collect_typed(0, boundary)
                                    .get());
        const auto second =
            dftu_utils_test::ok(trace::views::View::from_files(
                                    {{dstd::String(gz), dstd::String(idx)}})
                                    .collect_typed(boundary, 4096)
                                    .get());
        check_against(first.counters, lo);
        check_against(second.counters, hi);

        // The union of the splits is the whole frame, no row twice.
        CHECK(first.counters.num_rows() + second.counters.num_rows() ==
              whole.counters.num_rows());
        dstd::Vector<dstd::Tuple<dstd::String, dstd::String, std::uint64_t>>
            keys = row_keys(first.counters);
        for (const auto& k : row_keys(second.counters)) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
        auto want = row_keys(whole.counters);
        std::sort(want.begin(), want.end());
        CHECK(keys == want);
    }
}
