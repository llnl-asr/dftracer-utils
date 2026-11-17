// SPDX-License-Identifier: MIT
// The stats a host seeds a plugin phase with: read from a current index,
// keyed by a fingerprint of the input files, and kept in a store under the
// index root. A stale or unknown source gives nothing, never an old value.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/cache/phase_seed_store.h>
#include <dftracer/utils/trace/index/cache/rollup_store.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::trace::index::cache;

namespace {

struct TestEvent {
    std::uint64_t ts;
    std::uint64_t dur;
    bool metadata = false;
};

// A function, since a global dstd::Vector would hold its block past the memory
// exit check.
dstd::Vector<TestEvent> sample_events() {
    return {{5000, 40},   {1200, 8},   {0, 3},
            {0, 0, true}, {9100, 900}, {3000, 20}};
}

InputStats reference_of(const dstd::Vector<TestEvent>& events) {
    InputStats ref;
    bool any_start = false;
    for (const TestEvent& e : events) {
        if (e.metadata) continue;
        ++ref.event_count;
        if (e.ts > 0 && (!any_start || e.ts < ref.ts_min)) {
            ref.ts_min = e.ts;
            any_start = true;
        }
        if (e.ts + e.dur > ref.ts_max) ref.ts_max = e.ts + e.dur;
    }
    return ref;
}

dstd::String trace_text(const dstd::Vector<TestEvent>& events) {
    dstd::String lines;
    for (const TestEvent& e : events) {
        if (e.metadata) {
            lines +=
                "{\"ph\":\"M\",\"name\":\"process_name\",\"cat\":\"dftracer\","
                "\"pid\":1,\"tid\":1,\"args\":{\"name\":\"app\"}}\n";
            continue;
        }
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":{}{}\n">(lines, e.ts, e.dur,
                                                  R"(,"args":{}})");
    }
    return lines;
}

dstd::String write_trace(const dstd::String& dir, const char* name,
                         const dstd::Vector<TestEvent>& events) {
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/" + name + ".pfw";
    const dstd::String lines = trace_text(events);
    dstd::fs::write_file(dstd::PathView(pfw), lines).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

void build_index(const dstd::String& gz) {
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::trace::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
}

views::detail::ScanPlan plan_of(const dstd::String& gz) {
    return views::detail::scan::from_file(
        dstd::String(gz),
        dstd::String(trace::internal::determine_index_path(gz, "")));
}

dstd::Optional<InputStats> index_stats(const views::detail::ViewPlan& plan) {
    return index_input_stats(plan);
}

}  // namespace

TEST_SUITE("PhaseSeedStore") {
    TEST_CASE("a current index gives the stats of every event but metadata") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz = write_trace(env.dir(), "t", sample_events());
        build_index(gz);
        const auto plan = plan_of(gz);

        const auto got = index_stats(*plan);
        REQUIRE(got.has_value());
        CHECK(*got == reference_of(sample_events()));
        CHECK(got->event_count == 5);
        CHECK(got->ts_min == 1200);
        CHECK(got->ts_max == 10000);
    }

    TEST_CASE("the stats of several files are merged") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::Vector<TestEvent> other = {{700, 5}, {20000, 15}, {650, 1}};
        const dstd::String a = write_trace(env.dir(), "a", sample_events());
        const dstd::String b = write_trace(env.dir(), "b", other);
        build_index(a);
        build_index(b);
        dstd::Vector<trace::views::ViewFile> files;
        for (const auto& gz : {a, b}) {
            files.push_back(trace::views::ViewFile{
                gz,
                dstd::String(trace::internal::determine_index_path(gz, ""))});
        }
        const auto plan = views::detail::scan::from_files(files);

        const auto got = index_stats(*plan);
        REQUIRE(got.has_value());
        CHECK(got->event_count == 8);
        CHECK(got->ts_min == 650);
        CHECK(got->ts_max == 20015);
    }

    TEST_CASE("a file that grew since its index gives no stats") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz =
            write_trace(env.dir(), "grown", sample_events());
        build_index(gz);
        const auto plan = plan_of(gz);
        REQUIRE(index_stats(*plan).has_value());

        dstd::Vector<TestEvent> grown = sample_events();
        grown.push_back({60000, 1});
        write_trace(env.dir(), "grown", grown);
        CHECK_FALSE(index_stats(*plan).has_value());
    }

    TEST_CASE("a file touched since its index gives no stats") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz =
            write_trace(env.dir(), "touched", sample_events());
        build_index(gz);
        const auto plan = plan_of(gz);
        REQUIRE(index_stats(*plan).has_value());

        const auto before = dstd::fs::mtime_ns(dstd::PathView(gz)).value();
        REQUIRE(dstd::fs::set_mtime_ns(dstd::PathView(gz),
                                       before + 10'000'000'000ULL)
                    .has_value());
        CHECK_FALSE(index_stats(*plan).has_value());
    }

    TEST_CASE("a file with no index gives no stats") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz = write_trace(env.dir(), "bare", sample_events());
        const auto plan = plan_of(gz);
        CHECK_FALSE(index_stats(*plan).has_value());
    }

    TEST_CASE("a plan that pins a file has no stats, key or cache") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz =
            write_trace(env.dir(), "pinned", sample_events());
        build_index(gz);
        views::detail::ViewPlan pinned = *plan_of(gz);
        REQUIRE(index_stats(pinned).has_value());
        pinned.files[0].pinned_c_end = 10;
        pinned.files[0].pinned_uc_end = 10;
        CHECK_FALSE(index_stats(pinned).has_value());
        CHECK_FALSE(input_fingerprint(pinned).has_value());
        CHECK(phase_seed_cache_path(pinned).empty());
    }

    TEST_CASE(
        "the fingerprint follows the path, stat and bytes of the inputs") {
        dftu_utils_test::TestEnvironment env(0);
        // Stored (not compressed) gzip, so a same-length edit keeps the size.
        const dstd::String gz = env.dir() + "/keyed.pfw.gz";
        dftu_utils_test::write_stored_gz_trace(gz, trace_text(sample_events()));
        const auto key = input_fingerprint(*plan_of(gz));
        REQUIRE(key.has_value());
        CHECK(input_fingerprint(*plan_of(gz)) == key);

        // Another size.
        dstd::Vector<TestEvent> grown = sample_events();
        grown.push_back({60000, 1});
        dftu_utils_test::write_stored_gz_trace(gz, trace_text(grown));
        const auto grown_key = input_fingerprint(*plan_of(gz));
        REQUIRE(grown_key.has_value());
        CHECK(*grown_key != *key);

        // The same size and mtime, other bytes.
        const auto stamp = dstd::fs::mtime_ns(dstd::PathView(gz)).value();
        const auto grown_size = dstd::fs::file_size(dstd::PathView(gz)).value();
        dstd::Vector<TestEvent> shifted = grown;
        shifted[0].ts = 5001;
        dftu_utils_test::write_stored_gz_trace(gz, trace_text(shifted));
        REQUIRE(dstd::fs::set_mtime_ns(dstd::PathView(gz), stamp).has_value());
        CHECK(dstd::fs::file_size(dstd::PathView(gz)).value() == grown_size);
        const auto shifted_key = input_fingerprint(*plan_of(gz));
        REQUIRE(shifted_key.has_value());
        CHECK(*shifted_key != *grown_key);

        // The same bytes, another mtime.
        REQUIRE(
            dstd::fs::set_mtime_ns(dstd::PathView(gz), stamp + 3'000'000'000ULL)
                .has_value());
        const auto touched_key = input_fingerprint(*plan_of(gz));
        REQUIRE(touched_key.has_value());
        CHECK(*touched_key != *shifted_key);
    }

    TEST_CASE("a size change with the old mtime set back changes the key") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz = env.dir() + "/sized.pfw.gz";
        dftu_utils_test::write_stored_gz_trace(gz, trace_text(sample_events()));
        const auto stamp = dstd::fs::mtime_ns(dstd::PathView(gz)).value();
        const auto key = input_fingerprint(*plan_of(gz));
        REQUIRE(key.has_value());

        dstd::Vector<TestEvent> grown = sample_events();
        grown.push_back({60000, 1});
        dftu_utils_test::write_stored_gz_trace(gz, trace_text(grown));
        REQUIRE(dstd::fs::set_mtime_ns(dstd::PathView(gz), stamp).has_value());
        const auto grown_key = input_fingerprint(*plan_of(gz));
        REQUIRE(grown_key.has_value());
        CHECK(*grown_key != *key);
    }

    TEST_CASE("the same bytes under another logical path change the key") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String text = trace_text(sample_events());
        dstd::String one = env.dir() + "/one.pfw.gz";
        dstd::String two = env.dir() + "/two.pfw.gz";
        dftu_utils_test::write_stored_gz_trace(one, text);
        dftu_utils_test::write_stored_gz_trace(two, text);
        const auto stamp = dstd::fs::mtime_ns(dstd::PathView(one)).value();
        REQUIRE(dstd::fs::set_mtime_ns(dstd::PathView(two), stamp).has_value());
        const auto one_key = input_fingerprint(*plan_of(one));
        const auto two_key = input_fingerprint(*plan_of(two));
        REQUIRE(one_key.has_value());
        REQUIRE(two_key.has_value());
        CHECK(*one_key != *two_key);
    }

    TEST_CASE("a file named twice in a plan changes the key") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz =
            write_trace(env.dir(), "twice", sample_events());
        const trace::views::ViewFile file{
            gz, dstd::String(trace::internal::determine_index_path(gz, ""))};
        const auto once =
            input_fingerprint(*views::detail::scan::from_files({file}));
        const auto twice =
            input_fingerprint(*views::detail::scan::from_files({file, file}));
        REQUIRE(once.has_value());
        REQUIRE(twice.has_value());
        CHECK(*once != *twice);
    }

    TEST_CASE("one stale file among two gives no stats") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String a = write_trace(env.dir(), "a", sample_events());
        const dstd::String b = write_trace(env.dir(), "b", sample_events());
        build_index(a);
        build_index(b);
        dstd::Vector<trace::views::ViewFile> files;
        for (const auto& gz : {a, b}) {
            files.push_back(trace::views::ViewFile{
                gz,
                dstd::String(trace::internal::determine_index_path(gz, ""))});
        }
        const auto plan = views::detail::scan::from_files(files);
        REQUIRE(index_stats(*plan).has_value());

        dstd::Vector<TestEvent> grown = sample_events();
        grown.push_back({60000, 1});
        write_trace(env.dir(), "b", grown);
        CHECK_FALSE(index_stats(*plan).has_value());
        // The first file alone is still current.
        CHECK(index_stats(*plan_of(a)).has_value());
    }

    TEST_CASE("the fingerprint does not depend on the order of the files") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String a = write_trace(env.dir(), "a", sample_events());
        const dstd::String b = write_trace(env.dir(), "b", {{1, 2}});
        auto key_of = [&](const dstd::String& first,
                          const dstd::String& second) {
            dstd::Vector<trace::views::ViewFile> files;
            for (const auto& gz : {first, second}) {
                files.push_back(trace::views::ViewFile{
                    gz, dstd::String(
                            trace::internal::determine_index_path(gz, ""))});
            }
            return input_fingerprint(*views::detail::scan::from_files(files));
        };
        const auto forward = key_of(a, b);
        REQUIRE(forward.has_value());
        CHECK(key_of(b, a) == forward);
    }

    TEST_CASE("the cache lives under the index root and keeps the stats") {
        dftu_utils_test::TestEnvironment env(0);
        const dstd::String gz =
            write_trace(env.dir(), "stored", sample_events());
        const auto plan = plan_of(gz);
        const dstd::String path = phase_seed_cache_path(*plan);
        REQUIRE_FALSE(path.empty());
        CHECK(path == trace::index::phase_seeds_dir(cache_dir(*plan)));

        auto writer = CacheStore::open(path, CacheStore::Mode::ReadWrite);
        REQUIRE(writer);
        const InputStats stats = reference_of(sample_events());
        CHECK_FALSE(read_input_stats(*writer, 7).value().has_value());
        REQUIRE(persist_input_stats(*writer, 7, stats).has_value());
        CHECK(read_input_stats(*writer, 7).value() == stats);
        // Another fingerprint finds nothing.
        CHECK_FALSE(read_input_stats(*writer, 8).value().has_value());

        auto reader = CacheStore::open(path, CacheStore::Mode::ReadOnly);
        REQUIRE(reader);
        CHECK(read_input_stats(*reader, 7).value() == stats);
    }

    TEST_CASE("an entry of another format or size is not used") {
        dftu_utils_test::TestEnvironment env(0);
        auto writer =
            CacheStore::open(env.dir() + "/seeds", CacheStore::Mode::ReadWrite);
        REQUIRE(writer);
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> entries;
        entries.emplace_back(dstd::String("\x01"), dstd::String(25, '\x09'));
        REQUIRE(writer->put(1, entries, 64).has_value());
        CHECK_FALSE(read_input_stats(*writer, 1).value().has_value());

        entries.clear();
        entries.emplace_back(dstd::String("\x01"), dstd::String("\x01short"));
        REQUIRE(writer->put(2, entries, 64).has_value());
        CHECK_FALSE(read_input_stats(*writer, 2).value().has_value());
    }
}
