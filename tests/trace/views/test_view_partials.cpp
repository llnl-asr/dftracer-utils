// The view's aggregate, events and profiles reads answer from the chunk
// partials of the index: the traces are deleted once the indexes are built, and
// every result still equals the raw scan taken before.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <rocksdb/db.h>

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;
namespace ix = dftracer::utils::index;
namespace scan = dftracer::utils::trace::views::detail::scan;
namespace trace = dftracer::utils::trace;

namespace {

constexpr int FILES = 3;
constexpr int READS = 40;
constexpr int WRITES = 20;
constexpr int COUNTERS = 6;
constexpr int MIXED = 4;
constexpr std::size_t MEMBER_BYTES = 2048;
constexpr std::uint64_t BASE_TS = 1'000'000'000;
constexpr std::uint64_t WINDOW_US = 1'000'000;
constexpr double QUANTILE_TOLERANCE = 0.03;
// Files 0 and 1 ran on one host, file 2 on another; each file is one process.
const char* const HOSTS[FILES] = {"hosta", "hosta", "hostb"};

std::uint64_t read_dur(int file, int j) { return 10 + (j * 7 + file * 3) % 40; }
std::uint64_t write_dur(int j) { return 20 + j % 9; }

// Three files, one process and one host each, whose events all start inside
// one second. Each index is built with percentiles, so its partials hold the
// quantile stores too.
struct Fixture {
    TestEnvironment env{200};
    std::vector<std::string> gz;

    // `extras` keeps the string args epoch and step as extra group keys and
    // writes them on the events and counters; `mixed` adds counters named
    // like an event, so one name is both.
    explicit Fixture(bool extras = false, bool mixed = false) {
        REQUIRE(env.is_valid());
        for (int i = 0; i < FILES; ++i) {
            const std::string dir = env.get_dir() + "/f" + std::to_string(i);
            fs::create_directories(dir);
            const std::string plain = dir + "/t.pfw";
            const std::string pid = std::to_string(100 + i);
            const std::string host =
                std::string(R"("hhash":")") + HOSTS[i] + R"(")";
            {
                std::ofstream o(plain);
                auto extra_args = [&](int epoch, int step) {
                    return extras
                               ? std::string(R"(,"epoch":")") +
                                     std::to_string(epoch) + R"(","step":")" +
                                     std::to_string(step) + R"(")"
                               : std::string();
                };
                for (int j = 0; j < READS; ++j)
                    o << R"({"ph":"X","name":"read","cat":"POSIX","pid":)"
                      << pid << R"(,"tid":1,"ts":)" << (BASE_TS + j * 100)
                      << R"(,"dur":)" << read_dur(i, j) << R"(,"args":{)"
                      << host << R"(,"ret":)" << 512 * (1 + j % 4)
                      << extra_args(j % 2, j % 3) << "}}\n";
                for (int j = 0; j < WRITES; ++j)
                    o << R"({"ph":"X","name":"fwrite","cat":"STDIO","pid":)"
                      << pid << R"(,"tid":1,"ts":)"
                      << (BASE_TS + 10000 + j * 100) << R"(,"dur":)"
                      << write_dur(j) << R"(,"args":{)" << host
                      << extra_args(j % 2, j % 3) << "}}\n";
                for (int e = 0; e < COUNTERS; ++e)
                    o << R"({"ph":"C","name":"train","cat":"app","pid":)" << pid
                      << R"(,"tid":1,"ts":)" << (BASE_TS + 20000 + e * 100)
                      << R"(,"args":{)" << host << R"(,"dur":5)"
                      << extra_args(e / 3, e % 3) << "}}\n";
                if (mixed)
                    for (int e = 0; e < MIXED; ++e)
                        o << R"({"ph":"C","name":"read","cat":"POSIX","pid":)"
                          << pid << R"(,"tid":1,"ts":)"
                          << (BASE_TS + 30000 + e * 100) << R"(,"args":{)"
                          << host << R"(,"dur":7}})" << "\n";
            }
            const std::string out = plain + ".gz";
            REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
                plain, out, MEMBER_BYTES));
            fs::remove(plain);
            ix::IndexerOptions o;
            o.checkpoint_size = MEMBER_BYTES;
            o.aggregation.emplace().compute_percentiles = true;
            if (extras) o.aggregation->extra_group_keys = {"epoch", "step"};
            ix::Indexer::open({out}, o).build();
            gz.push_back(out);
        }
    }

    std::vector<ViewFile> files() const {
        std::vector<ViewFile> out;
        for (const auto& g : gz)
            out.push_back({g, determine_index_path(g, "")});
        return out;
    }

    View view() const { return View::from_files(files()); }

    void delete_traces() const {
        for (const auto& g : gz) fs::remove(g);
    }
};

std::vector<AggSpec> specs() {
    return {{AggOp::Count, "", "n"},     {AggOp::Sum, "dur", "total"},
            {AggOp::Mean, "dur", "avg"}, {AggOp::Min, "dur", "lo"},
            {AggOp::Max, "dur", "hi"},   {AggOp::Pct, "dur", "p50", "", 0.5}};
}

// The aggregation of `p` by a full scan of the traces, the partials bypassed.
df::DataFrame scan_only(const scan::ScanPlan& p) {
    namespace detail = dftracer::utils::trace::views::detail;
    Runtime rt;
    df::DataFrame out;
    rt.run_blocking("scan-only", [&](CoroScope&) -> coro::CoroTask<void> {
        auto st = co_await detail::build_engine_agg_state(*p);
        out = detail::finalize_engine_result(*st, *p);
    });
    return detail::apply_agg_post_ops(std::move(out), *p);
}

using Rows = std::map<std::string, std::map<std::string, double>>;

Rows rows_of(const df::DataFrame& f, const std::vector<std::string>& keys) {
    Rows out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        std::string k;
        for (const auto& c : keys) k += bstr(f, r, c) + '|';
        auto& cells = out[k];
        for (std::size_t c = 0; c < f.names.size(); ++c)
            if (f.columns[c].type() != df::TypeId::String)
                cells[f.names[c]] = bnum(f, r, f.names[c]);
    }
    return out;
}

// `got` has the rows of `expected`: the counts, sums, means and extremes
// equal, the quantile within the accuracy of its sketch.
void same_rows(const df::DataFrame& got, const df::DataFrame& expected,
               const std::vector<std::string>& keys) {
    const Rows g = rows_of(got, keys);
    const Rows e = rows_of(expected, keys);
    REQUIRE(e.size() > 0);
    REQUIRE(g.size() == e.size());
    for (const auto& [key, cells] : e) {
        const auto it = g.find(key);
        REQUIRE_MESSAGE(it != g.end(), key);
        for (const auto& [name, value] : cells) {
            const auto cell = it->second.find(name);
            REQUIRE_MESSAGE(cell != it->second.end(), key, " ", name);
            if (name == "p50")
                CHECK_MESSAGE(std::abs(cell->second - value) <=
                                  QUANTILE_TOLERANCE * std::abs(value),
                              key, " ", name, " ", cell->second, " vs ", value);
            else
                CHECK_MESSAGE(cell->second == doctest::Approx(value), key, " ",
                              name);
        }
    }
}

// The raw scan of the events of the traces (profiles left out), grouped.
df::DataFrame scan_events(const Fixture& fx, std::vector<GroupKey> keys,
                          std::uint64_t window_us = 0) {
    auto p = scan::from_files(fx.files());
    p = scan::filter(
        p, Query::from_string(R"(cat in ["POSIX", "STDIO"])").value());
    p = scan::group_by(p, std::move(keys));
    if (window_us != 0) p = scan::time_bucket(p, window_us);
    return scan_only(scan::agg(p, specs()));
}

std::vector<std::string> column_families_of(const std::string& db_path) {
    std::vector<std::string> names;
    const auto status = ::rocksdb::DB::ListColumnFamilies(
        ::rocksdb::DBOptions(), db_path, &names);
    REQUIRE_MESSAGE(status.ok(), status.ToString());
    return names;
}

bool has_family(const std::vector<std::string>& names,
                std::string_view family) {
    return std::find(names.begin(), names.end(), family) != names.end();
}

// The path of the RocksDB database behind the index of `gz`.
std::string db_path_of(const std::string& gz) {
    ix::store::IndexDatabase db(determine_index_path(gz, ""),
                                ix::store::IndexOpenMode::ReadOnly);
    return db.db()->path();
}

}  // namespace

TEST_SUITE("ViewPartials") {
    TEST_CASE("aggregates answer from the partials with the traces deleted") {
        Fixture fx;
        const std::vector<std::vector<GroupKey>> groupings{
            {GroupKey::name()},
            {GroupKey::hhash()},
            {GroupKey::pid(), GroupKey::name()},
            {GroupKey::cat(), GroupKey::hhash()}};
        const std::vector<std::vector<std::string>> columns{
            {"name"}, {"hhash"}, {"pid", "name"}, {"cat", "hhash"}};
        std::vector<df::DataFrame> expected;
        for (const auto& g : groupings) expected.push_back(scan_events(fx, g));

        fx.delete_traces();
        for (std::size_t i = 0; i < groupings.size(); ++i) {
            // A scan answers over the events only; the profile counters of
            // the traces are a kind of their own in the partials.
            const df::DataFrame got =
                fx.view()
                    .filter(Query::from_string(R"(cat in ["POSIX", "STDIO"])")
                                .value())
                    .group_by(groupings[i])
                    .agg(specs())
                    .collect()
                    .get();
            same_rows(got, expected[i], columns[i]);
        }
    }

    TEST_CASE("events with a time window and profiles answer without reads") {
        Fixture fx;
        auto typed_view = [&] {
            return fx.view()
                .group_by({GroupKey::name(), GroupKey::pid()})
                .time_bucket(WINDOW_US)
                .agg({{AggOp::Count, "", "n"},
                      {AggOp::Sum, "dur", "total"},
                      {AggOp::Mean, "dur", "avg"}});
        };
        const df::DataFrame expected_events =
            scan_events(fx, {GroupKey::name(), GroupKey::pid()}, WINDOW_US);

        fx.delete_traces();
        const TypedResult typed = typed_view().collect_typed().get();

        auto strip = [](const df::DataFrame& f) {
            // the engine columns the typed frames share with the scan
            return rows_of(f, {"time_bucket", "name", "pid"});
        };
        const Rows events = strip(typed.regular);
        const Rows want = strip(expected_events);
        REQUIRE(events.size() == want.size());
        REQUIRE(events.size() == 2 * FILES);  // read and fwrite of each file
        for (const auto& [key, cells] : want) {
            const auto it = events.find(key);
            REQUIRE_MESSAGE(it != events.end(), key);
            for (const char* c : {"n", "total", "avg"})
                CHECK_MESSAGE(it->second.at(c) == doctest::Approx(cells.at(c)),
                              key, " ", c);
        }
        for (const auto& [key, cells] : events)
            CHECK_MESSAGE(key.rfind(std::to_string(BASE_TS) + "|", 0) == 0,
                          key);

        // Each file ran COUNTERS "train" counters of 5 us.
        const Rows profiles = strip(typed.aggregated);
        REQUIRE(profiles.size() == FILES);
        for (int i = 0; i < FILES; ++i) {
            const std::string key = std::to_string(BASE_TS) + "|train|" +
                                    std::to_string(100 + i) + "|";
            const auto it = profiles.find(key);
            REQUIRE_MESSAGE(it != profiles.end(), key);
            CHECK(it->second.at("n") == COUNTERS);
            CHECK(it->second.at("total") == 5.0 * COUNTERS);
            CHECK(it->second.at("avg") == 5.0);
        }
    }

    TEST_CASE("a query groups by host and keeps the hosts it is asked for") {
        Fixture fx;
        fx.delete_traces();
        auto ask = [&](bool by_host, std::vector<const char*> hosts) {
            trace::AggQuery q;
            for (int i = 0; i < FILES; ++i) {
                trace::AggQuerySource src;
                const std::string idx = determine_index_path(fx.gz[i], "");
                src.index_path = dstd::String(idx.data(), idx.size());
                src.files.emplace_back(fx.gz[i].data(), fx.gz[i].size());
                q.sources.push_back(std::move(src));
            }
            q.config.compute_percentiles = true;
            q.by_host = by_host;
            q.names.emplace_back("read");
            for (const char* h : hosts) q.hosts.emplace_back(h);
            auto r = run(trace::run_agg_query(q));
            REQUIRE_MESSAGE(r.has_value(), r.error().format());
            return std::move(*r);
        };

        const auto by_host = ask(true, {});
        CHECK(by_host.stats.chunks_reread == 0);
        std::map<std::string, std::uint64_t> counts;
        for (const auto& [k, p] : by_host.rows)
            counts[std::string(by_host.text(k.host))] += p.count;
        CHECK(counts == std::map<std::string, std::uint64_t>{
                            {"hosta", 2 * READS}, {"hostb", READS}});

        const auto only_b = ask(true, {"hostb"});
        CHECK(only_b.stats.chunks_reread == 0);
        std::uint64_t total = 0;
        for (const auto& [k, p] : only_b.rows) {
            CHECK(only_b.text(k.host) == "hostb");
            total += p.count;
        }
        CHECK(total == READS);

        // Without the host in the key the hosts merge.
        const auto merged = ask(false, {});
        REQUIRE(merged.rows.size() == 1);
        CHECK(merged.rows.begin()->second.count == FILES * READS);
        CHECK(merged.rows.begin()->first.host == dstd::StringIntern::NO_ID);
    }

    TEST_CASE("a query that may not read chunks counts what it left out") {
        Fixture fx;
        trace::AggQuery q;
        for (int i = 0; i < FILES; ++i) {
            trace::AggQuerySource src;
            const std::string idx = determine_index_path(fx.gz[i], "");
            src.index_path = dstd::String(idx.data(), idx.size());
            src.files.emplace_back(fx.gz[i].data(), fx.gz[i].size());
            q.sources.push_back(std::move(src));
        }
        q.config.compute_percentiles = true;
        q.reread = false;
        q.by_file = true;  // the partials hold no file
        auto r = run(trace::run_agg_query(q));
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->stats.chunks_reread == 0);
        CHECK(r->stats.unanswered > 0);
        CHECK(r->rows.empty());
    }

    TEST_CASE("a fresh index build writes no aggregation column family") {
        Fixture fx;
        for (const auto& g : fx.gz) {
            const auto names = column_families_of(db_path_of(g));
            CHECK_FALSE(has_family(names, ix::store::cf::LEGACY_AGGREGATION));
            CHECK(has_family(names, ix::store::cf::SYSTEM_METRICS));
        }
    }

    TEST_CASE("an index with the legacy aggregation column family answers") {
        Fixture fx;
        const std::string path = db_path_of(fx.gz[0]);
        ix::store::RocksDBManager::instance().reset(path);
        {
            // What an index of an earlier build holds: the per-key tier's
            // column family with rows in it.
            auto names = column_families_of(path);
            names.emplace_back(ix::store::cf::LEGACY_AGGREGATION);
            std::vector<::rocksdb::ColumnFamilyDescriptor> descriptors;
            for (const auto& n : names)
                descriptors.emplace_back(n, ::rocksdb::ColumnFamilyOptions());
            ::rocksdb::DBOptions options;
            options.create_missing_column_families = true;
            std::vector<::rocksdb::ColumnFamilyHandle*> handles;
            std::unique_ptr<::rocksdb::DB> db;
            const auto opened =
                ::rocksdb::DB::Open(options, path, descriptors, &handles, &db);
            REQUIRE_MESSAGE(opened.ok(), opened.ToString());
            REQUIRE(db->Put(::rocksdb::WriteOptions(), handles.back(), "key",
                            "value")
                        .ok());
            for (auto* h : handles) db->DestroyColumnFamilyHandle(h);
        }
        REQUIRE(has_family(column_families_of(path),
                           ix::store::cf::LEGACY_AGGREGATION));

        // A read-only open leaves it, and the aggregate is the partials'.
        const df::DataFrame got = View::from_files({fx.files()[0]})
                                      .group_by({GroupKey::name()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .collect()
                                      .get();
        const Rows rows = rows_of(got, {"name"});
        CHECK(rows.at("read|").at("n") == READS);
        CHECK(rows.at("fwrite|").at("n") == WRITES);

        // A read-write open drops it; the index still answers.
        {
            ix::store::IndexDatabase db(determine_index_path(fx.gz[0], ""));
        }
        ix::store::RocksDBManager::instance().reset(path);
        CHECK_FALSE(has_family(column_families_of(path),
                               ix::store::cf::LEGACY_AGGREGATION));
        const df::DataFrame again = View::from_files({fx.files()[0]})
                                        .group_by({GroupKey::name()})
                                        .agg({{AggOp::Count, "", "n"}})
                                        .collect()
                                        .get();
        CHECK(rows_of(again, {"name"}).at("read|").at("n") == READS);
    }

    TEST_CASE("profiles and aggregates group by extra keys without reads") {
        Fixture fx(/*extras=*/true);
        auto profiles_by = [&](const char* arg) {
            return fx.view()
                .group_by({GroupKey::name(), GroupKey::of_arg(arg)})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}})
                .collect_typed()
                .get();
        };
        auto events_by = [&](const char* arg) {
            return fx.view()
                .filter(
                    Query::from_string(R"(cat in ["POSIX", "STDIO"])").value())
                .group_by({GroupKey::name(), GroupKey::of_arg(arg)})
                .agg(specs())
                .collect()
                .get();
        };
        const TypedResult before_epoch = profiles_by("epoch");
        const TypedResult before_step = profiles_by("step");
        std::vector<df::DataFrame> expected;
        for (const char* arg : {"epoch", "step"}) {
            auto p = scan::from_files(fx.files());
            p = scan::filter(
                p, Query::from_string(R"(cat in ["POSIX", "STDIO"])").value());
            p = scan::group_by(p, {GroupKey::name(), GroupKey::of_arg(arg)});
            expected.push_back(scan_only(scan::agg(p, specs())));
        }

        fx.delete_traces();

        // Three counters of each epoch, two of each step, in each file.
        const Rows by_epoch =
            rows_of(profiles_by("epoch").aggregated, {"name", "epoch"});
        REQUIRE(by_epoch.size() == 2);
        CHECK(by_epoch.at("train|0|").at("n") == 3 * FILES);
        CHECK(by_epoch.at("train|1|").at("n") == 3 * FILES);
        CHECK(by_epoch.at("train|0|").at("total") == 15.0 * FILES);
        const Rows by_step =
            rows_of(profiles_by("step").aggregated, {"name", "step"});
        REQUIRE(by_step.size() == 3);
        for (const char* s : {"train|0|", "train|1|", "train|2|"})
            CHECK(by_step.at(s).at("n") == 2 * FILES);
        CHECK(by_epoch == rows_of(before_epoch.aggregated, {"name", "epoch"}));
        CHECK(by_step == rows_of(before_step.aggregated, {"name", "step"}));

        same_rows(events_by("epoch"), expected[0], {"name", "epoch"});
        same_rows(events_by("step"), expected[1], {"name", "step"});
    }

    TEST_CASE("a name used as an event and as a counter splits by kind") {
        Fixture fx(/*extras=*/false, /*mixed=*/true);
        fx.delete_traces();

        trace::AggQuery q;
        for (int i = 0; i < FILES; ++i) {
            trace::AggQuerySource src;
            const std::string idx = determine_index_path(fx.gz[i], "");
            src.index_path = dstd::String(idx.data(), idx.size());
            src.files.emplace_back(fx.gz[i].data(), fx.gz[i].size());
            q.sources.push_back(std::move(src));
        }
        q.config.compute_percentiles = true;
        q.by_kind = true;
        q.names.emplace_back("read");
        auto r = run(trace::run_agg_query(q));
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->stats.chunks_reread == 0);
        std::map<int, std::uint64_t> counts;
        for (const auto& [k, p] : r->rows)
            counts[static_cast<int>(k.kind)] += p.count;
        CHECK(counts ==
              std::map<int, std::uint64_t>{
                  {static_cast<int>(trace::AggKind::Event), FILES * READS},
                  {static_cast<int>(trace::AggKind::Profile), FILES * MIXED}});

        const TypedResult typed = fx.view()
                                      .group_by({GroupKey::name()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .collect_typed()
                                      .get();
        CHECK(rows_of(typed.regular, {"name"}).at("read|").at("n") ==
              FILES * READS);
        CHECK(rows_of(typed.aggregated, {"name"}).at("read|").at("n") ==
              FILES * MIXED);
    }
}
