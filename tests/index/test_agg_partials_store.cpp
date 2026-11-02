// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace layout = dftracer::utils::index::store::layout;
using dataframe::FieldStat;
using index::Indexer;
using index::IndexerOptions;

namespace {

constexpr int FILES = 48;
constexpr int EVENTS = 400;
constexpr std::size_t CHECKPOINT_SIZE = 8192;
constexpr std::uint64_t INTERVAL_US = 100'000;

using Key = dstd::Pair<std::string, std::string>;

const char* const NAMES[] = {"read", "write", "open", "close"};

std::string write_trace(const std::string& dir, int n,
                        std::map<Key, std::vector<double>>& durations,
                        int salt = 0) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS + salt; ++i) {
        const bool own = i % 5 == 0;
        const std::string cat =
            own ? "file" + std::to_string(n) : (i % 3 == 0 ? "STDIO" : "POSIX");
        const std::string name = own ? "step" : NAMES[i % 4];
        const int dur = 1 + (i * 7 + n + salt) % 500;
        char fhash[24];
        std::snprintf(fhash, sizeof fhash, "%016x", 0x1000 + i % 7);
        out << R"({"id":)" << i << R"(,"name":")" << name << R"(","cat":")"
            << cat << R"(","pid":)" << 1 + i % 3 << R"(,"tid":)" << 1 + i % 2
            << R"(,"ph":"X","ts":)" << 1000 + i * 997 << R"(,"dur":)" << dur
            << R"(,"args":{"ret":)" << (i * 13) % 4096 << R"(,"offset":)"
            << i * 512 << R"(,"level":)" << i % 3
            << R"(,"hhash":"a1b2c3d4e5f60718","fhash":")" << fhash << R"("}})"
            << "\n";
        std::string lower = cat;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        durations[{lower, name}].push_back(dur);
    }
    out << "]\n";
    const std::string pfw = dir + "/t" + std::to_string(n) + ".pfw";
    {
        std::ofstream file(pfw.c_str());
        file << out.str();
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    (void)dstd::fs::remove(pfw);
    return gz;
}

IndexerOptions options(const std::string& index_dir) {
    IndexerOptions o;
    o.index_dir = index_dir;
    o.checkpoint_size = CHECKPOINT_SIZE;
    o.parallelism = 4;
    agg::AggregationConfig c;
    c.time_interval_us = INTERVAL_US;
    c.compute_percentiles = true;
    o.aggregation = c;
    return o;
}

double build_seconds(const std::vector<std::string>& files,
                     const std::string& index_dir) {
    auto ix = Indexer::open(files, options(index_dir));
    const auto t0 = std::chrono::steady_clock::now();
    ix.build();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

struct Stored {
    std::map<Key, agg::AggPartial> merged;
    std::size_t count = 0;
    std::set<std::uint32_t> chunks;
};

index::Snapshot open_index(const std::string& index_path) {
    return common::unwrap(index::plan::open_snapshot(index_path));
}

index::FileRef file_of(const index::Snapshot& snapshot,
                       const std::string& path) {
    auto file = snapshot.file(index::store::internal::get_logical_path(path));
    REQUIRE(file.has_value());
    return std::move(*file);
}

Stored read_partials(const index::Snapshot& snapshot,
                     const index::FileRef& file,
                     const agg::AggInternTable& dictionary) {
    Stored out;
    common::unwrap(index::for_each_agg_partial(
        snapshot, file,
        [&](std::uint32_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t, std::uint32_t, agg::PartialKind,
            dstd::Span<const std::uint8_t> bytes) {
            ++out.count;
            out.chunks.insert(chunk);
            auto p = agg::AggPartial::deserialize(
                dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
            REQUIRE(p.has_value());
            const Key key{std::string(dictionary.intern.resolve(cat)),
                          std::string(dictionary.intern.resolve(name))};
            out.merged.try_emplace(key).first->second.merge(*p);
            return true;
        }));
    return out;
}

// A sketch answers near the rank, within its relative accuracy.
void check_quantiles(const agg::AggPartial& p, const agg::QuantileStore& q,
                     std::vector<double> truth) {
    REQUIRE(q.count() == truth.size());
    std::sort(truth.begin(), truth.end());
    const std::size_t last = truth.size() - 1;
    for (double rank : {0.1, 0.5, 0.9}) {
        const double pos = rank * static_cast<double>(last);
        const auto lo = static_cast<std::size_t>(pos);
        const double got = q.quantile(rank);
        if (q.is_exact()) {
            const double frac = pos - static_cast<double>(lo);
            const double want =
                lo + 1 > last ? truth[lo]
                              : truth[lo] * (1.0 - frac) + truth[lo + 1] * frac;
            CHECK(got == doctest::Approx(want).epsilon(1e-12));
        } else {
            const double accuracy = 2 * p.accuracy;
            CHECK(got >= truth[lo == 0 ? 0 : lo - 1] * (1.0 - accuracy));
            CHECK(got <= truth[std::min(lo + 2, last)] * (1.0 + accuracy));
        }
    }
}

std::string index_dir_of(const dftu_utils_test::TestEnvironment& env) {
    return env.get_dir() + "/idx";
}

// Every aggregation row of `file`, keyed by text in place of dictionary ids,
// so two indexes compare whatever ids they gave.
std::map<std::string, std::string> agg_rows(const std::string& file,
                                            const std::string& index_dir) {
    const auto index_path =
        trace::internal::determine_index_path(file, index_dir);
    const auto snapshot = open_index(index_path);
    const auto stored_file = file_of(snapshot, file);
    auto dictionary = agg::make_intern_table();
    common::unwrap(index::agg_dictionary(snapshot, *dictionary));
    const auto text = [&](std::uint32_t id) {
        return std::string(dictionary->intern.resolve(id));
    };
    std::map<std::string, std::string> rows;
    common::unwrap(index::for_each_agg_partial(
        snapshot, stored_file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra, agg::PartialKind kind,
            dstd::Span<const std::uint8_t> bytes) {
            rows.emplace("p " + std::to_string(chunk) + " " + text(cat) + " " +
                             text(name) + " " + text(host) + " " + text(extra) +
                             " " + std::to_string(static_cast<int>(kind)),
                         std::string(bytes.begin(), bytes.end()));
            return true;
        }));
    common::unwrap(index::for_each_agg_chunk_columns(
        snapshot, stored_file,
        [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            std::ostringstream value;
            value << block.rows;
            for (const auto* locals :
                 {&block.cat, &block.name, &block.host, &block.file})
                for (const std::uint32_t local : *locals)
                    value << ' '
                          << dstd::StringView(block.texts[local].data(),
                                              block.texts[local].size());
            for (const auto* numbers :
                 {&block.ts, &block.dur, &block.pid, &block.tid})
                for (const std::uint64_t v : *numbers) value << ' ' << v;
            rows.emplace("c " + std::to_string(chunk), value.str());
            return true;
        }));
    return rows;
}

using Rows = std::map<std::string, std::string>;

// The first key whose row is missing from one side or differs, for a failed
// comparison.
std::string first_difference(const Rows& a, const Rows& b) {
    for (const auto& [key, value] : a) {
        const auto at = b.find(key);
        if (at == b.end()) return "only left: " + key;
        if (at->second != value)
            return "differs: " + key + " (" + std::to_string(value.size()) +
                   " vs " + std::to_string(at->second.size()) + " bytes)";
    }
    for (const auto& [key, value] : b)
        if (a.find(key) == a.end()) return "only right: " + key;
    return "same";
}

void check_rows(const Rows& a, const Rows& b) {
    CHECK_MESSAGE(a == b, first_difference(a, b), " of ", a.size(), " and ",
                  b.size(), " rows");
}

constexpr const char* CRASH_DIR_ENV = "DFTRACER_UTILS_TEST_AGG_CRASH_DIR";
constexpr int CRASH_FILES = 3;
constexpr std::uint64_t CRASH_RUNS = 2;

std::vector<std::string> crash_files(const std::string& dir) {
    std::vector<std::string> files;
    for (int i = 0; i < CRASH_FILES; ++i)
        files.push_back(dir + "/t" + std::to_string(i) + ".pfw.gz");
    return files;
}

}  // namespace

TEST_SUITE("AggPartialsStore") {
    TEST_CASE("48 small files store at most chunks x names partials") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        std::map<Key, std::vector<double>> durations;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i, durations));
        const auto index_dir = index_dir_of(env);

        const double seconds = build_seconds(files, index_dir);
        MESSAGE("index build of " << FILES << " files: " << seconds << " s");

        const auto index_path =
            trace::internal::determine_index_path(files[0], index_dir);
        const auto snapshot = open_index(index_path);
        auto dictionary = agg::make_intern_table();
        common::unwrap(index::agg_dictionary(snapshot, *dictionary));

        std::map<Key, agg::AggPartial> all;
        std::size_t stored = 0;
        std::size_t bound = 0;
        for (int n = 0; n < FILES; ++n) {
            const auto stored_file = file_of(snapshot, files[n]);
            const auto members =
                common::unwrap(index::members(snapshot, stored_file)).size();
            REQUIRE(members > 1);

            const auto file = read_partials(snapshot, stored_file, *dictionary);
            CHECK(file.count > 0);
            CHECK(file.chunks.size() <= members);
            CHECK(file.count <= members * file.merged.size());
            CHECK(file.count < static_cast<std::size_t>(EVENTS));
            stored += file.count;
            bound += members * file.merged.size();

            const Key own{"file" + std::to_string(n), "step"};
            const auto mine = file.merged.find(own);
            REQUIRE(mine != file.merged.end());
            CHECK(mine->second.count == durations.at(own).size());

            for (const auto& [key, p] : file.merged)
                all.try_emplace(key).first->second.merge(p);
        }
        MESSAGE("partials stored: " << stored << ", bound: " << bound
                                    << ", events: " << FILES * EVENTS);
        CHECK(stored <= bound);

        REQUIRE(all.size() == durations.size());
        for (const auto& [key, p] : all) {
            INFO("group: ", key.first, " ", key.second);
            REQUIRE(durations.count(key) == 1);
            CHECK(p.count == durations.at(key).size());
            check_quantiles(p, p.duration_q, durations.at(key));
        }
    }

    TEST_CASE("an index without partials is built again") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        std::map<Key, std::vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.push_back(write_trace(env.get_dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);

        const auto index_path =
            trace::internal::determine_index_path(files[0], index_dir);
        std::vector<std::size_t> before;
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = agg::make_intern_table();
            common::unwrap(index::agg_dictionary(snapshot, *dictionary));
            for (const auto& f : files) {
                before.push_back(
                    read_partials(snapshot, file_of(snapshot, f), *dictionary)
                        .count);
                REQUIRE(before.back() > 0);
            }
        }

        // An index of an earlier build: another params hash and no partials.
        // The empty copy of each file replaces its whole tier.
        {
            auto target = common::unwrap(index::Index::open(index_path));
            const auto config = common::unwrap(
                agg::tier::read_config(common::unwrap(target.snapshot())));
            REQUIRE(config);
            auto old = *config;
            old.params_hash ^= 1;
            agg::tier::write_config(target, old);

            const auto snapshot = open_index(index_path);
            std::vector<std::string> logical;
            std::vector<index::SourceStat> stats;
            for (const auto& f : files) {
                logical.push_back(index::store::internal::get_logical_path(f));
                stats.push_back(file_of(snapshot, f).stat());
            }

            bool marked = true;
            dftu_utils_test::run_coro(
                [&](coro::Scope&) -> coro::CoroTask<void> {
                    for (std::size_t i = 0; i < logical.size(); ++i) {
                        auto build = target.build(logical[i], stats[i]);
                        if (!build) {
                            marked = false;
                            co_return;
                        }
                        build->finish(index::store::IndexExtension::Agg,
                                      old.params_hash);
                        if (!co_await build->commit()) marked = false;
                    }
                },
                "stale aggregation index");
            CHECK(marked);
        }
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = agg::make_intern_table();
            common::unwrap(index::agg_dictionary(snapshot, *dictionary));
            for (const auto& f : files)
                CHECK(read_partials(snapshot, file_of(snapshot, f), *dictionary)
                          .count == 0);
        }

        build_seconds(files, index_dir);

        const auto rebuilt = open_index(index_path);
        auto dictionary = agg::make_intern_table();
        common::unwrap(index::agg_dictionary(rebuilt, *dictionary));
        for (std::size_t i = 0; i < files.size(); ++i)
            CHECK(
                read_partials(rebuilt, file_of(rebuilt, files[i]), *dictionary)
                    .count == before[i]);
    }

    TEST_CASE("partials of an older key layout are built again, not read") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        std::map<Key, std::vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.push_back(write_trace(env.get_dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);

        const auto index_path =
            trace::internal::determine_index_path(files[0], index_dir);
        std::vector<std::size_t> before;
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = agg::make_intern_table();
            common::unwrap(index::agg_dictionary(snapshot, *dictionary));
            for (const auto& f : files) {
                before.push_back(
                    read_partials(snapshot, file_of(snapshot, f), *dictionary)
                        .count);
            }
        }

        // An index of an earlier build: no config record where it is kept now
        // and a partial whose key tail is the older 12 bytes.
        {
            auto target = common::unwrap(index::Index::open(index_path));
            REQUIRE(default_runtime()
                        .submit(index::delete_agg_meta(target, "config"))
                        .get()
                        .has_value());

            namespace kv = dftracer::utils::store;
            auto store = kv::Store::open(index_path + "/store");
            REQUIRE(store.has_value());
            auto ns =
                store->ns(index::ext_spec(index::store::IndexExtension::Agg));
            REQUIRE(ns.has_value());
            std::string old_key(1,
                                static_cast<char>(layout::agg_kind::PARTIAL));
            old_key.append(12, '\0');
            const std::string old_value = "old";
            const auto snapshot = open_index(index_path);
            const auto file = file_of(snapshot, files[0]);
            const auto state = index::extension_state(
                snapshot, file, index::store::IndexExtension::Agg);
            REQUIRE(state.has_value());
            const dstd::Vector<index::SourceStat> sources{file.stat()};
            kv::Batch batch = store->batch(
                *ns,
                kv::ScopeKey::file(
                    index::store::internal::get_logical_path(files[0])),
                state->params_hash, &sources);
            batch.put(dstd::ByteView(old_key), dstd::ByteView(old_value));
            REQUIRE(default_runtime()
                        .submit(store->commit(std::move(batch)))
                        .get()
                        .has_value());
            REQUIRE(default_runtime().submit(store->flush()).get().has_value());
        }
        {
            const auto snapshot = open_index(index_path);
            CHECK_FALSE(common::unwrap(agg::tier::read_config(snapshot)));
            auto dictionary = agg::make_intern_table();
            common::unwrap(index::agg_dictionary(snapshot, *dictionary));
            CHECK_THROWS(read_partials(snapshot, file_of(snapshot, files[0]),
                                       *dictionary));
        }

        build_seconds(files, index_dir);

        const auto rebuilt = open_index(index_path);
        CHECK(common::unwrap(agg::tier::read_config(rebuilt)));
        auto dictionary = agg::make_intern_table();
        common::unwrap(index::agg_dictionary(rebuilt, *dictionary));
        for (std::size_t i = 0; i < files.size(); ++i)
            CHECK(
                read_partials(rebuilt, file_of(rebuilt, files[i]), *dictionary)
                    .count == before[i]);
    }

    TEST_CASE("rows written through equal the rows held in memory") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        std::map<Key, std::vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.push_back(write_trace(env.get_dir(), i, durations));
        const std::string held = env.get_dir() + "/held";
        const std::string through = env.get_dir() + "/through";

        const auto run_bytes = agg::g_agg_run_bytes.exchange(UINT64_MAX);
        build_seconds(files, held);
        const auto runs = agg::g_agg_runs_staged.load();
        // Each closed chunk fills a run.
        agg::g_agg_run_bytes.store(1);
        build_seconds(files, through);
        CHECK(agg::g_agg_runs_staged.load() > runs);
        for (const auto& f : files) {
            const auto rows = agg_rows(f, held);
            CHECK_FALSE(rows.empty());
            check_rows(agg_rows(f, through), rows);
        }

        // A changed file rebuilt through its staged write keeps none of the
        // rows it had.
        write_trace(env.get_dir(), 0, durations, /*salt=*/37);
        const auto rebuilt = agg::g_agg_runs_staged.load();
        build_seconds(files, through);
        CHECK(agg::g_agg_runs_staged.load() > rebuilt);
        agg::g_agg_run_bytes.store(UINT64_MAX);
        const std::string fresh = env.get_dir() + "/fresh";
        build_seconds(files, fresh);
        agg::g_agg_run_bytes.store(run_bytes);
        CHECK(agg_rows(files[0], through) != agg_rows(files[0], held));
        for (const auto& f : files)
            check_rows(agg_rows(f, through), agg_rows(f, fresh));
    }

    // A build killed after it staged rows of a changed file leaves each file
    // with its old rows or its new rows whole, and the next build ends well.
    TEST_CASE("a build killed while it writes through leaves whole rows") {
        dftu_utils_test::TestEnvironment env(10);
        std::map<Key, std::vector<double>> durations;
        std::vector<std::string> files;
        for (int i = 0; i < CRASH_FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);
        std::vector<std::map<std::string, std::string>> before;
        for (const auto& f : files) before.push_back(agg_rows(f, index_dir));

        for (int i = 0; i < CRASH_FILES; ++i)
            write_trace(env.get_dir(), i, durations, /*salt=*/37);
        ::setenv(CRASH_DIR_ENV, env.get_dir().c_str(), 1);
        const auto child = fresh_process::run("a build killed after runs");
        ::unsetenv(CRASH_DIR_ENV);
        REQUIRE(child.ok());

        const std::string fresh = env.get_dir() + "/fresh";
        build_seconds(files, fresh);
        std::size_t kept = 0;
        for (int i = 0; i < CRASH_FILES; ++i) {
            const auto rows = agg_rows(files[i], index_dir);
            const auto after = agg_rows(files[i], fresh);
            CHECK((rows == before[i] || rows == after));
            if (rows == before[i]) ++kept;
        }
        MESSAGE("files with their old rows after the kill: " << kept);
        CHECK(kept > 0);

        build_seconds(files, index_dir);
        for (const auto& f : files)
            check_rows(agg_rows(f, index_dir), agg_rows(f, fresh));
    }

    // Run by the case above in a new process, which it ends with _exit once a
    // fold has staged runs, so no write commits after that.
    TEST_CASE("a build killed after runs" * doctest::skip()) {
        fresh_process::announce();
        const auto env_dir = common::Env::get<dstd::StringView>(CRASH_DIR_ENV);
        REQUIRE(env_dir.has_value());
        const std::string dir(*env_dir);
        const auto files = crash_files(dir);
        agg::g_agg_run_bytes.store(1);
        std::atomic<bool> done{false};
        std::thread build([&] {
            build_seconds(files, dir + "/idx");
            done.store(true);
        });
        while (agg::g_agg_runs_staged.load() < CRASH_RUNS && !done.load())
            std::this_thread::yield();
        ::_exit(done.load() ? 1 : 0);
    }
}
