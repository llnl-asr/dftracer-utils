// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

constexpr int FILES = 48;
constexpr int EVENTS = 400;
constexpr std::size_t CHECKPOINT_SIZE = 8192;
constexpr std::uint64_t INTERVAL_US = 100'000;

using Key = dstd::Pair<dstd::String, dstd::String>;

const char* const NAMES[] = {"read", "write", "open", "close"};

dstd::String write_trace(const dstd::String& dir, int n,
                         dstd::Map<Key, dstd::Vector<double>>& durations,
                         int salt = 0) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < EVENTS + salt; ++i) {
        const bool own = i % 5 == 0;
        const dstd::String cat = own ? "file" + dstd::to_string(n)
                                     : (i % 3 == 0 ? "STDIO" : "POSIX");
        const dstd::String name = own ? "step" : NAMES[i % 4];
        const int dur = 1 + (i * 7 + n + salt) % 500;
        char fhash[24];
        std::snprintf(fhash, sizeof fhash, "%016x", 0x1000 + i % 7);
        out += common::str_cat(
            R"({"id":)", i, R"(,"name":")", name, R"(","cat":")", cat,
            R"(","pid":)", 1 + i % 3, R"(,"tid":)", 1 + i % 2,
            R"(,"ph":"X","ts":)", 1000 + i * 997, R"(,"dur":)", dur,
            R"(,"args":{"ret":)", (i * 13) % 4096, R"(,"offset":)", i * 512,
            R"(,"level":)", i % 3, R"(,"hhash":"a1b2c3d4e5f60718","fhash":")",
            fhash, R"("}})", "\n");
        dstd::String lower = cat;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       dstd::to_ascii_lower);
        durations[{lower, name}].push_back(dur);
    }
    out += "]\n";
    const dstd::String pfw =
        dir + "/t" + dstd::String(dstd::to_string(n)) + ".pfw";
    {
        dstd::fs::write_file(dstd::PathView(pfw), out).discard();
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    dstd::fs::remove(pfw).discard();
    return gz;
}

IndexerOptions options(dstd::StringView index_dir) {
    IndexerOptions o;
    o.index_dir = dstd::String(index_dir);
    o.checkpoint_size = CHECKPOINT_SIZE;
    o.parallelism = 4;
    schemas::dft::agg::AggregationConfig c;
    c.time_interval_us = INTERVAL_US;
    c.compute_percentiles = true;
    o.aggregation = c;
    return o;
}

double build_seconds(const dstd::Vector<dstd::String>& files,
                     dstd::StringView index_dir) {
    auto ix = dftu_utils_test::ok(Indexer::open(files, options(index_dir)));
    const auto t0 = std::chrono::steady_clock::now();
    dftu_utils_test::ok(ix.build());
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

struct Stored {
    dstd::Map<Key, schemas::dft::agg::AggPartial> merged;
    std::size_t count = 0;
    dstd::Set<std::uint32_t> chunks;
};

trace::index::Snapshot open_index(const dstd::String& index_path) {
    return dftu_utils_test::ok(trace::index::plan::open_snapshot(index_path));
}

trace::index::FileRef file_of(const trace::index::Snapshot& snapshot,
                              dstd::StringView path) {
    auto file = snapshot.file(fileio::get_logical_path(path));
    REQUIRE(file.has_value());
    return std::move(*file);
}

dstd::Result<Stored> try_read_partials(
    const trace::index::Snapshot& snapshot, const trace::index::FileRef& file,
    const schemas::dft::agg::AggInternTable& dictionary) {
    Stored out;
    auto walked = trace::index::for_each_agg_partial(
        snapshot, file,
        [&](std::uint32_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t, std::uint32_t, schemas::dft::agg::PartialKind,
            dstd::Span<const std::uint8_t> bytes) {
            ++out.count;
            out.chunks.insert(chunk);
            auto p = schemas::dft::agg::AggPartial::deserialize(
                dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
            REQUIRE(p.has_value());
            const Key key{dstd::String(dictionary.intern.resolve(cat)),
                          dstd::String(dictionary.intern.resolve(name))};
            out.merged.try_emplace(key).first->second.merge(*p);
            return true;
        });
    if (!walked.has_value()) return walked.error();
    return out;
}

Stored read_partials(const trace::index::Snapshot& snapshot,
                     const trace::index::FileRef& file,
                     const schemas::dft::agg::AggInternTable& dictionary) {
    return dftu_utils_test::ok(try_read_partials(snapshot, file, dictionary));
}

// A sketch answers near the rank, within its relative accuracy.
void check_quantiles(const schemas::dft::agg::AggPartial& p,
                     const schemas::dft::agg::QuantileStore& q,
                     dstd::Vector<double> truth) {
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

dstd::String index_dir_of(const dftu_utils_test::TestEnvironment& env) {
    return env.dir() + "/idx";
}

// Every aggregation row of `file`, keyed by text in place of dictionary ids,
// so two indexes compare whatever ids they gave.
dstd::Map<dstd::String, dstd::String> agg_rows(dstd::StringView file,
                                               dstd::StringView index_dir) {
    const auto index_path = dstd::String(trace::internal::determine_index_path(
        dstd::String(file), dstd::String(index_dir)));
    const auto snapshot = open_index(index_path);
    const auto stored_file = file_of(snapshot, file);
    auto dictionary = schemas::dft::agg::make_intern_table();
    dftu_utils_test::ok(trace::index::agg_dictionary(snapshot, *dictionary));
    const auto text = [&](std::uint32_t id) {
        return dstd::String(dictionary->intern.resolve(id));
    };
    dstd::Map<dstd::String, dstd::String> rows;
    dftu_utils_test::ok(trace::index::for_each_agg_partial(
        snapshot, stored_file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra,
            schemas::dft::agg::PartialKind kind,
            dstd::Span<const std::uint8_t> bytes) {
            rows.emplace("p " + dstd::to_string(chunk) + " " + text(cat) + " " +
                             text(name) + " " + text(host) + " " + text(extra) +
                             " " + dstd::to_string(static_cast<int>(kind)),
                         dstd::String(bytes.begin(), bytes.end()));
            return true;
        }));
    dftu_utils_test::ok(trace::index::for_each_agg_file_partial(
        snapshot, stored_file,
        [&](std::uint64_t chunk, std::uint32_t file_id,
            schemas::dft::agg::PartialKind kind,
            dstd::Span<const std::uint8_t> bytes) {
            rows.emplace(
                "f " + dstd::to_string(chunk) + " " +
                    (file_id == trace::index::store::layout::agg_kind::NO_FILE
                         ? dstd::String("-")
                         : text(file_id)) +
                    " " + dstd::to_string(static_cast<int>(kind)),
                dstd::String(bytes.begin(), bytes.end()));
            return true;
        }));
    dftu_utils_test::ok(trace::index::for_each_agg_chunk_columns(
        snapshot, stored_file,
        [&](std::uint64_t chunk, const schemas::dft::agg::ChunkColumns& block) {
            dstd::String value;
            value += common::str_cat(block.rows);
            for (const auto* locals :
                 {&block.cat, &block.name, &block.host, &block.file})
                for (const std::uint32_t local : *locals)
                    value += common::str_cat(
                        ' ', dstd::StringView(block.texts[local].data(),
                                              block.texts[local].size()));
            for (const auto* numbers :
                 {&block.ts, &block.dur, &block.pid, &block.tid})
                for (const std::uint64_t v : *numbers)
                    value += common::str_cat(' ', v);
            rows.emplace("c " + dstd::to_string(chunk), value);
            return true;
        }));
    return rows;
}

using Rows = dstd::Map<dstd::String, dstd::String>;

// The first key whose row is missing from one side or differs, for a failed
// comparison.
dstd::String first_difference(const Rows& a, const Rows& b) {
    for (const auto& [key, value] : a) {
        const auto at = b.find(key);
        if (at == b.end()) return "only left: " + key;
        if (at->second != value)
            return "differs: " + key + " (" + dstd::to_string(value.size()) +
                   " vs " + dstd::to_string(at->second.size()) + " bytes)";
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

dstd::Vector<dstd::String> crash_files(const dstd::String& dir) {
    dstd::Vector<dstd::String> files;
    for (int i = 0; i < CRASH_FILES; ++i)
        files.push_back(dir + "/t" + dstd::to_string(i) + ".pfw.gz");
    return files;
}

}  // namespace

TEST_SUITE("AggPartialsStore") {
    TEST_CASE("48 small files store at most chunks x names partials") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        dstd::Map<Key, dstd::Vector<double>> durations;
        for (int i = 0; i < FILES; ++i)
            files.emplace_back(write_trace(env.dir(), i, durations));
        const auto index_dir = index_dir_of(env);

        const double seconds = build_seconds(files, index_dir);
        MESSAGE("index build of " << FILES << " files: " << seconds << " s");

        const auto index_path = dstd::String(
            trace::internal::determine_index_path(files[0], index_dir));
        const auto snapshot = open_index(index_path);
        auto dictionary = schemas::dft::agg::make_intern_table();
        dftu_utils_test::ok(
            trace::index::agg_dictionary(snapshot, *dictionary));

        dstd::Map<Key, schemas::dft::agg::AggPartial> all;
        std::size_t stored = 0;
        std::size_t bound = 0;
        for (int n = 0; n < FILES; ++n) {
            const auto stored_file = file_of(snapshot, files[n]);
            const auto members = dftu_utils_test::ok(trace::index::members(
                                                         snapshot, stored_file))
                                     .size();
            REQUIRE(members > 1);

            const auto file = read_partials(snapshot, stored_file, *dictionary);
            CHECK(file.count > 0);
            CHECK(file.chunks.size() <= members);
            CHECK(file.count <= members * file.merged.size());
            CHECK(file.count < static_cast<std::size_t>(EVENTS));
            stored += file.count;
            bound += members * file.merged.size();

            const Key own{"file" + dstd::to_string(n), "step"};
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

    TEST_CASE("per-file partials hold the events of each file hash") {
        dftu_utils_test::TestEnvironment env(10);
        // Sixteen hex digits, a text that is not a hash, and no hash at all.
        const char* const HASHES[] = {"00000000000010a1", "not-a-hash", ""};
        dstd::Map<dstd::String, std::uint64_t> want;
        dstd::Map<dstd::String, std::uint64_t> want_dur;
        dstd::String text = "[\n";
        for (int i = 0; i < EVENTS; ++i) {
            const dstd::String fhash = HASHES[i % 3];
            const int dur = 1 + i % 50;
            text += common::str_cat(
                R"({"id":)", i, R"(,"name":"read","cat":"POSIX","pid":1)",
                R"(,"tid":1,"ph":"X","ts":)", 1000 + i * 997, R"(,"dur":)", dur,
                R"(,"args":{"hhash":"a1b2c3d4e5f60718")",
                fhash.empty() ? dstd::String()
                              : common::str_cat(R"(,"fhash":")", fhash, "\""),
                "}}\n");
            ++want[fhash];
            want_dur[fhash] += static_cast<std::uint64_t>(dur);
        }
        text += "]\n";
        const dstd::String pfw = env.dir() + "/f.pfw";
        dstd::fs::write_file(dstd::PathView(pfw), text).discard();
        const dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            pfw, gz, CHECKPOINT_SIZE));
        dstd::fs::remove(pfw).discard();
        const auto index_dir = index_dir_of(env);
        build_seconds({gz}, index_dir);

        const auto index_path =
            dstd::String(trace::internal::determine_index_path(gz, index_dir));
        const auto snapshot = open_index(index_path);
        const auto file = file_of(snapshot, gz);
        auto dictionary = schemas::dft::agg::make_intern_table();
        dftu_utils_test::ok(
            trace::index::agg_dictionary(snapshot, *dictionary));
        dstd::Map<dstd::String, std::uint64_t> got;
        dstd::Map<dstd::String, std::uint64_t> got_dur;
        dstd::Set<std::uint32_t> chunks;
        dftu_utils_test::ok(trace::index::for_each_agg_file_partial(
            snapshot, file,
            [&](std::uint64_t chunk, std::uint32_t file_id,
                schemas::dft::agg::PartialKind kind,
                dstd::Span<const std::uint8_t> bytes) {
                CHECK(kind == schemas::dft::agg::PartialKind::Event);
                chunks.insert(static_cast<std::uint32_t>(chunk));
                const dstd::String name =
                    file_id == trace::index::store::layout::agg_kind::NO_FILE
                        ? dstd::String()
                        : dstd::String(dictionary->intern.resolve(file_id));
                auto p = schemas::dft::agg::AggPartial::deserialize(
                    dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
                REQUIRE(p.has_value());
                got[name] += p->count;
                got_dur[name] += static_cast<std::uint64_t>(p->duration.sum);
                return true;
            }));
        CHECK(chunks.size() > 1);
        CHECK(got == want);
        CHECK(got_dur == want_dur);
    }

    TEST_CASE("an index without partials is built again") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        dstd::Map<Key, dstd::Vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.emplace_back(write_trace(env.dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);

        const auto index_path = dstd::String(
            trace::internal::determine_index_path(files[0], index_dir));
        dstd::Vector<std::size_t> before;
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = schemas::dft::agg::make_intern_table();
            dftu_utils_test::ok(
                trace::index::agg_dictionary(snapshot, *dictionary));
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
            auto target =
                dftu_utils_test::ok(trace::index::Index::open(index_path));
            const auto config =
                dftu_utils_test::ok(schemas::dft::agg::tier::read_config(
                    dftu_utils_test::ok(target.snapshot())));
            REQUIRE(config);
            auto old = *config;
            old.params_hash ^= 1;
            dftu_utils_test::ok(
                schemas::dft::agg::tier::write_config(target, old));

            const auto snapshot = open_index(index_path);
            dstd::Vector<dstd::String> logical;
            dstd::Vector<trace::index::SourceStat> stats;
            for (const auto& f : files) {
                logical.push_back(fileio::get_logical_path(f));
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
                        build->finish(trace::index::store::IndexExtension::Agg,
                                      old.params_hash);
                        if (!co_await build->commit()) marked = false;
                    }
                },
                "stale aggregation index");
            CHECK(marked);
        }
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = schemas::dft::agg::make_intern_table();
            dftu_utils_test::ok(
                trace::index::agg_dictionary(snapshot, *dictionary));
            for (const auto& f : files)
                CHECK(read_partials(snapshot, file_of(snapshot, f), *dictionary)
                          .count == 0);
        }

        build_seconds(files, index_dir);

        const auto rebuilt = open_index(index_path);
        auto dictionary = schemas::dft::agg::make_intern_table();
        dftu_utils_test::ok(trace::index::agg_dictionary(rebuilt, *dictionary));
        for (std::size_t i = 0; i < files.size(); ++i)
            CHECK(
                read_partials(rebuilt, file_of(rebuilt, files[i]), *dictionary)
                    .count == before[i]);
    }

    TEST_CASE("partials of an older key layout are built again, not read") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        dstd::Map<Key, dstd::Vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.emplace_back(write_trace(env.dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);

        const auto index_path = dstd::String(
            trace::internal::determine_index_path(files[0], index_dir));
        dstd::Vector<std::size_t> before;
        {
            const auto snapshot = open_index(index_path);
            auto dictionary = schemas::dft::agg::make_intern_table();
            dftu_utils_test::ok(
                trace::index::agg_dictionary(snapshot, *dictionary));
            for (const auto& f : files) {
                before.push_back(
                    read_partials(snapshot, file_of(snapshot, f), *dictionary)
                        .count);
            }
        }

        // An index of an earlier build: no config record where it is kept now
        // and a partial whose key tail is the older 12 bytes.
        {
            auto target =
                dftu_utils_test::ok(trace::index::Index::open(index_path));
            REQUIRE(default_runtime()
                        .submit(trace::index::delete_agg_meta(target, "config"))
                        .get()
                        .has_value());

            auto store =
                dftracer::utils::store::Store::open(index_path + "/store");
            REQUIRE(store.has_value());
            auto ns = store->ns(trace::index::ext_spec(
                trace::index::store::IndexExtension::Agg));
            REQUIRE(ns.has_value());
            dstd::String old_key(
                1, static_cast<char>(
                       trace::index::store::layout::agg_kind::PARTIAL));
            old_key.append(12, '\0');
            const dstd::String old_value = "old";
            const auto snapshot = open_index(index_path);
            const auto file = file_of(snapshot, files[0]);
            const auto state = trace::index::extension_state(
                snapshot, file, trace::index::store::IndexExtension::Agg);
            REQUIRE(state.has_value());
            const dstd::Vector<trace::index::SourceStat> sources{file.stat()};
            dftracer::utils::store::Batch batch =
                store->batch(*ns,
                             dftracer::utils::store::ScopeKey::file(
                                 fileio::get_logical_path(files[0])),
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
            CHECK_FALSE(dftu_utils_test::ok(
                schemas::dft::agg::tier::read_config(snapshot)));
            auto dictionary = schemas::dft::agg::make_intern_table();
            dftu_utils_test::ok(
                trace::index::agg_dictionary(snapshot, *dictionary));
            CHECK_FALSE(try_read_partials(snapshot, file_of(snapshot, files[0]),
                                          *dictionary)
                            .has_value());
        }

        build_seconds(files, index_dir);

        const auto rebuilt = open_index(index_path);
        CHECK(
            dftu_utils_test::ok(schemas::dft::agg::tier::read_config(rebuilt)));
        auto dictionary = schemas::dft::agg::make_intern_table();
        dftu_utils_test::ok(trace::index::agg_dictionary(rebuilt, *dictionary));
        for (std::size_t i = 0; i < files.size(); ++i)
            CHECK(
                read_partials(rebuilt, file_of(rebuilt, files[i]), *dictionary)
                    .count == before[i]);
    }

    TEST_CASE("a chunk record with a malformed key is a Parse error") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        dstd::Map<Key, dstd::Vector<double>> durations;
        files.emplace_back(write_trace(env.dir(), 0, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);
        const auto index_path = dstd::String(
            trace::internal::determine_index_path(files[0], index_dir));

        const auto put_bad_key = [&](trace::index::store::IndexExtension ext,
                                     std::uint8_t kind) {
            auto store =
                dftracer::utils::store::Store::open(index_path + "/store");
            REQUIRE(store.has_value());
            auto ns = store->ns(trace::index::ext_spec(ext));
            REQUIRE(ns.has_value());
            dstd::String bad_key(1, static_cast<char>(kind));
            bad_key.append(5, '\0');
            const dstd::String value = "x";
            const auto snapshot = open_index(index_path);
            const auto file = file_of(snapshot, files[0]);
            const auto state =
                trace::index::extension_state(snapshot, file, ext);
            REQUIRE(state.has_value());
            const dstd::Vector<trace::index::SourceStat> sources{file.stat()};
            dftracer::utils::store::Batch batch =
                store->batch(*ns,
                             dftracer::utils::store::ScopeKey::file(
                                 fileio::get_logical_path(files[0])),
                             state->params_hash, &sources);
            batch.put(dstd::ByteView(bad_key), dstd::ByteView(value));
            REQUIRE(default_runtime()
                        .submit(store->commit(std::move(batch)))
                        .get()
                        .has_value());
            REQUIRE(default_runtime().submit(store->flush()).get().has_value());
        };
        put_bad_key(trace::index::store::IndexExtension::Members,
                    trace::index::store::layout::members::MEMBER);
        put_bad_key(trace::index::store::IndexExtension::Stats,
                    trace::index::store::layout::stats_kind::CHUNK_STATS);
        put_bad_key(trace::index::store::IndexExtension::Metadata,
                    trace::index::store::layout::metadata_kind::CHUNK);

        const auto snapshot = open_index(index_path);
        const auto file = file_of(snapshot, files[0]);
        const auto members = trace::index::members(snapshot, file);
        REQUIRE_FALSE(members.has_value());
        CHECK(members.error().is(dstd::Condition::Parse));
        const auto stats = trace::index::chunk_stats(snapshot, file);
        REQUIRE_FALSE(stats.has_value());
        CHECK(stats.error().is(dstd::Condition::Parse));
        const auto meta = trace::index::chunk_meta(snapshot, file);
        REQUIRE_FALSE(meta.has_value());
        CHECK(meta.error().is(dstd::Condition::Parse));
    }

    TEST_CASE("rows written through equal the rows held in memory") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        dstd::Map<Key, dstd::Vector<double>> durations;
        for (int i = 0; i < 3; ++i)
            files.emplace_back(write_trace(env.dir(), i, durations));
        const dstd::String held = env.dir() + "/held";
        const dstd::String through = env.dir() + "/through";

        const auto run_bytes =
            schemas::dft::agg::g_agg_run_bytes.exchange(UINT64_MAX);
        build_seconds(files, held);
        const auto runs = schemas::dft::agg::g_agg_runs_staged.load();
        // Each closed chunk fills a run.
        schemas::dft::agg::g_agg_run_bytes.store(1);
        build_seconds(files, through);
        CHECK(schemas::dft::agg::g_agg_runs_staged.load() > runs);
        for (const auto& f : files) {
            const auto rows = agg_rows(f, held);
            CHECK_FALSE(rows.empty());
            check_rows(agg_rows(f, through), rows);
        }

        // A changed file rebuilt through its staged write keeps none of the
        // rows it had.
        write_trace(env.dir(), 0, durations, /*salt=*/37);
        const auto rebuilt = schemas::dft::agg::g_agg_runs_staged.load();
        build_seconds(files, through);
        CHECK(schemas::dft::agg::g_agg_runs_staged.load() > rebuilt);
        schemas::dft::agg::g_agg_run_bytes.store(UINT64_MAX);
        const dstd::String fresh = env.dir() + "/fresh";
        build_seconds(files, fresh);
        schemas::dft::agg::g_agg_run_bytes.store(run_bytes);
        CHECK(agg_rows(files[0], through) != agg_rows(files[0], held));
        for (const auto& f : files)
            check_rows(agg_rows(f, through), agg_rows(f, fresh));
    }

    TEST_CASE("a file that grew by members has the rows of a fresh build") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Map<Key, dstd::Vector<double>> durations;
        const dstd::String file = write_trace(env.dir(), 0, durations);
        const dstd::String tail = write_trace(env.dir(), 1, durations);
        const auto index_dir = index_dir_of(env);
        build_seconds({file}, index_dir);

        const auto added =
            dftu_utils_test::ok(dstd::fs::read_file(dstd::PathView(tail)));
        REQUIRE(dstd::fs::append_file(dstd::PathView(file), added));
        build_seconds({file}, index_dir);
        const dstd::String fresh = env.dir() + "/fresh";
        build_seconds({file}, fresh);

        const auto index_path = dstd::String(
            trace::internal::determine_index_path(file, index_dir));
        const auto snapshot = open_index(index_path);
        const auto stored = file_of(snapshot, file);
        const auto span_count =
            dftu_utils_test::ok(trace::index::spans(snapshot, stored)).size();
        auto dictionary = schemas::dft::agg::make_intern_table();
        dftu_utils_test::ok(
            trace::index::agg_dictionary(snapshot, *dictionary));
        const auto partials = read_partials(snapshot, stored, *dictionary);
        REQUIRE(!partials.chunks.empty());
        CHECK(*partials.chunks.rbegin() < span_count);
        check_rows(agg_rows(file, index_dir), agg_rows(file, fresh));
    }

    // A build killed after it staged rows of a changed file leaves each file
    // with its old rows or its new rows whole, and the next build ends well.
    TEST_CASE("a build killed while it writes through leaves whole rows") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Map<Key, dstd::Vector<double>> durations;
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < CRASH_FILES; ++i)
            files.emplace_back(write_trace(env.dir(), i, durations));
        const auto index_dir = index_dir_of(env);
        build_seconds(files, index_dir);
        dstd::Vector<dstd::Map<dstd::String, dstd::String>> before;
        for (const auto& f : files) before.push_back(agg_rows(f, index_dir));

        for (int i = 0; i < CRASH_FILES; ++i)
            write_trace(env.dir(), i, durations, /*salt=*/37);
        ::setenv(CRASH_DIR_ENV, env.dir().c_str(), 1);
        const auto child = fresh_process::run("a build killed after runs");
        ::unsetenv(CRASH_DIR_ENV);
        REQUIRE(child.ok());

        const dstd::String fresh = env.dir() + "/fresh";
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

    // Run by the case above in a new process. The fold ends the process with
    // exit code 0 once it has staged CRASH_RUNS runs, so no write commits after
    // that; a build that ends first exits with 1.
    TEST_CASE("a build killed after runs" * doctest::skip()) {
        fresh_process::announce();
        const auto env_dir = common::Env::get<dstd::StringView>(CRASH_DIR_ENV);
        REQUIRE(env_dir.has_value());
        const dstd::String dir(*env_dir);
        const auto files = crash_files(dir);
        schemas::dft::agg::g_agg_run_bytes.store(1);
        schemas::dft::agg::g_agg_exit_after_runs.store(CRASH_RUNS);
        build_seconds(files, dir + "/idx");
        std::_Exit(1);
    }

    TEST_CASE("an aggregation fold with no file aborts when written") {
        auto result = fresh_process::run(
            "an aggregation fold with no file ends the process");
        CHECK(result.aborted_with("writes the data of one file"));
    }

    TEST_CASE("an aggregation fold with no file ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String index_path = dstd::String(env.dir()) + "/idx";
        auto target =
            dftu_utils_test::ok(trace::index::Index::open(index_path));
        auto build = dftu_utils_test::ok(
            target.build("unused.pfw.gz", trace::index::SourceStat{1, 1, 1}));
        dstd::StringIntern intern;
        schemas::dft::agg::AggregationFold fold(
            intern, schemas::dft::agg::intern_for_index(index_path),
            schemas::dft::agg::standard_aggregation_config());
        dftu_utils_test::ok(fold.write(build));
    }
}
