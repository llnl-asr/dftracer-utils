// The chunk partials the index build stores: how many there are, that their
// quantiles hold, and that an index without them is built again.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <rocksdb_exit_listener.h>
#include <testing_utilities.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace layout = dftracer::utils::index::store::layout;
using dataframe::FieldStat;
using index::Indexer;
using index::IndexerOptions;
using index::store::IndexDatabase;

namespace {

constexpr int FILES = 48;
constexpr int EVENTS = 400;
constexpr std::size_t CHECKPOINT_SIZE = 8192;
constexpr std::uint64_t INTERVAL_US = 100'000;

using Key = std::pair<std::string, std::string>;

const char* const NAMES[] = {"read", "write", "open", "close"};

// Events of file `n`: POSIX and STDIO calls shared by every file, and a
// category and name only this file has.
std::string write_trace(const std::string& dir, int n,
                        std::map<Key, std::vector<double>>& durations) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        const bool own = i % 5 == 0;
        const std::string cat =
            own ? "file" + std::to_string(n) : (i % 3 == 0 ? "STDIO" : "POSIX");
        const std::string name = own ? "step" : NAMES[i % 4];
        const int dur = 1 + (i * 7 + n) % 500;
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
        std::ofstream file(pfw);
        file << out.str();
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
        pfw, gz, CHECKPOINT_SIZE));
    fs::remove(pfw);
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

// Every partial of `fid`, merged per (category, name).
Stored read_partials(const IndexDatabase& db, int fid,
                     const agg::AggInternTable& dictionary) {
    Stored out;
    agg::tier::for_each_partial(
        *db.db(), fid,
        [&](std::uint32_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t, std::uint32_t, agg::PartialKind,
            std::span<const std::uint8_t> bytes) {
            ++out.count;
            out.chunks.insert(chunk);
            auto p = agg::AggPartial::deserialize(
                dstd::Span<const std::uint8_t>(bytes.data(), bytes.size()));
            REQUIRE(p.has_value());
            const Key key{std::string(dictionary.intern.resolve(cat)),
                          std::string(dictionary.intern.resolve(name))};
            out.merged.try_emplace(key).first->second.merge(*p);
            return true;
        });
    return out;
}

// A sketch answers with a value near the rank, so it lies between the values
// next to the rank, widened by its relative accuracy.
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
        IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
        auto dictionary = agg::make_intern_table();
        agg::tier::load_dictionary(*db.db(), *dictionary);

        std::map<Key, agg::AggPartial> all;
        std::size_t stored = 0;
        std::size_t bound = 0;
        for (int n = 0; n < FILES; ++n) {
            const int fid = db.get_file_info_id(
                index::store::internal::get_logical_path(files[n]));
            REQUIRE(fid >= 0);
            const auto members = db.query_gzip_members(fid).size();
            REQUIRE(members > 1);

            const auto file = read_partials(db, fid, *dictionary);
            // A file holds the 8 shared groups at most plus its own.
            CHECK(file.count > 0);
            CHECK(file.chunks.size() <= members);
            CHECK(file.count <= members * file.merged.size());
            CHECK(file.count < static_cast<std::size_t>(EVENTS));
            stored += file.count;
            bound += members * file.merged.size();

            // The group only this file has holds all of its events.
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
        std::vector<int> ids;
        std::vector<std::size_t> before;
        {
            IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
            auto dictionary = agg::make_intern_table();
            agg::tier::load_dictionary(*db.db(), *dictionary);
            for (const auto& f : files) {
                ids.push_back(db.get_file_info_id(
                    index::store::internal::get_logical_path(f)));
                before.push_back(
                    read_partials(db, ids.back(), *dictionary).count);
                REQUIRE(before.back() > 0);
            }
        }

        // What an index of an earlier build holds: another params hash and no
        // partials.
        {
            IndexDatabase db(index_path);
            const auto config = agg::tier::read_config(*db.db());
            REQUIRE(config);
            const auto old_hash = config->params_hash ^ 1;
            agg::tier::write_config(
                *db.db(),
                {config->time_interval_us, old_hash, config->group_by_file});
            agg::tier::put_files(*db.db(), ids, old_hash);
            auto w = db.begin_write();
            const auto begin = layout::kind_prefix(layout::Ext::Agg,
                                                   layout::agg_kind::PARTIAL);
            w->delete_range(layout::Family::Blob, begin,
                            layout::prefix_end(begin));
            w->commit();
        }
        {
            IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
            auto dictionary = agg::make_intern_table();
            agg::tier::load_dictionary(*db.db(), *dictionary);
            for (int id : ids)
                CHECK(read_partials(db, id, *dictionary).count == 0);
        }

        build_seconds(files, index_dir);

        IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
        auto dictionary = agg::make_intern_table();
        agg::tier::load_dictionary(*db.db(), *dictionary);
        for (std::size_t i = 0; i < ids.size(); ++i)
            CHECK(read_partials(db, ids[i], *dictionary).count == before[i]);
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
        std::vector<int> ids;
        std::vector<std::size_t> before;
        {
            IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
            auto dictionary = agg::make_intern_table();
            agg::tier::load_dictionary(*db.db(), *dictionary);
            for (const auto& f : files) {
                ids.push_back(db.get_file_info_id(
                    index::store::internal::get_logical_path(f)));
                before.push_back(
                    read_partials(db, ids.back(), *dictionary).count);
            }
        }

        // What an index of an earlier build holds: no config record where it
        // is kept now, and a partial whose key tail is the older 12 bytes.
        {
            IndexDatabase db(index_path);
            auto w = db.begin_write();
            auto config = layout::file_prefix(
                layout::Ext::Agg, layout::agg_kind::META, layout::INDEX_WIDE);
            config.append("config");
            w->del(layout::Family::Blob, config);
            auto old_key =
                layout::file_prefix(layout::Ext::Agg, layout::agg_kind::PARTIAL,
                                    static_cast<std::uint32_t>(ids[0]));
            old_key.append(12, '\0');
            w->put(layout::Family::Blob, old_key,
                   layout::with_header(layout::Ext::Agg,
                                       layout::agg_kind::PARTIAL, "old"));
            w->commit();
        }
        {
            IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
            CHECK_FALSE(agg::tier::read_config(*db.db()));
            auto dictionary = agg::make_intern_table();
            agg::tier::load_dictionary(*db.db(), *dictionary);
            CHECK_THROWS(read_partials(db, ids[0], *dictionary));
        }

        build_seconds(files, index_dir);

        IndexDatabase db(index_path, index::store::IndexOpenMode::ReadOnly);
        CHECK(agg::tier::read_config(*db.db()));
        auto dictionary = agg::make_intern_table();
        agg::tier::load_dictionary(*db.db(), *dictionary);
        for (std::size_t i = 0; i < ids.size(); ++i)
            CHECK(read_partials(db, ids[i], *dictionary).count == before[i]);
    }
}
