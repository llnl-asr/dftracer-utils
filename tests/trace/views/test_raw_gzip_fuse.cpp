#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/extensions/rowset_fold.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <rocksdb/iterator.h>
#include <zlib.h>

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "groupmap_oracle.h"
#include "test_view_common.h"
#include "testing_runtime.h"

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::extensions;
using dftracer::utils::StringIntern;
using dftu_utils_test::run_coro;
namespace dataframe = dftracer::utils::dataframe;

namespace gzi = dftracer::utils::index::gzip;

namespace {

ViewPlan count_plan(const std::string& gz, const std::string& idx) {
    ViewPlan plan;
    ViewFile f;
    f.file_path = gz;
    f.index_path = idx;
    plan.files.push_back(f);
    plan.phase = Phase::Events;  // aggregate data events, not metadata
    plan.group_by = {GroupKey::cat()};
    plan.agg = {AggSpec(AggOp::Count, "", "n")};
    return plan;
}

// FH/HH metadata (ph=M, cat "dftracer") plus data events (ph=X, POSIX/STDIO).
// The metadata must land in the row sets but NOT in a cat aggregation.
std::string create_meta_trace(TestEnvironment& env) {
    std::string pfw = env.get_dir() + "/mt.pfw";
    {
        std::ofstream ofs(pfw);
        ofs << R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"/data/a.bin","value":"fh1"}})"
            << "\n";
        ofs << R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":"M","args":{"name":"node0","value":"hh1"}})"
            << "\n";
        for (int i = 0; i < 20; ++i)
            ofs << R"({"ph":"X","name":"read","cat":")"
                << (i % 3 ? "POSIX" : "STDIO") << R"(","pid":1,"tid":1,"ts":)"
                << (100 + i) << R"(,"dur":2,"args":{"fhash":"fh1"}})" << "\n";
    }
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 600);
    fs::remove(pfw);
    return gz;
}

// collect() yields a columnar DataFrame; two results are equal when they carry
// the same named columns and every cell matches (String via string_at, numeric
// by widened value).
bool rows_equal(const dftracer::utils::dataframe::DataFrame& a,
                const dftracer::utils::dataframe::DataFrame& b) {
    if (a.names != b.names) return false;
    if (a.num_rows() != b.num_rows()) return false;
    for (std::size_t c = 0; c < a.columns.size(); ++c) {
        const auto& ca = a.columns[c];
        const auto& cb = b.columns[c];
        if (ca.type() != cb.type()) return false;
        for (std::int64_t i = 0; i < a.num_rows(); ++i) {
            if (ca.type() == dataframe::TypeId::String) {
                if (ca.string_at(i) != cb.string_at(i)) return false;
            } else if (ca.type() == dataframe::TypeId::Int64) {
                if (ca.data<std::int64_t>()[i] != cb.data<std::int64_t>()[i])
                    return false;
            } else if (ca.type() == dataframe::TypeId::Uint64) {
                if (ca.data<std::uint64_t>()[i] != cb.data<std::uint64_t>()[i])
                    return false;
            } else {
                if (ca.data<double>()[i] != cb.data<double>()[i]) return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_SUITE("RawGzipFuse") {
    // Step 3 core: one pass over an UNINDEXED gzip answers an aggregation AND
    // builds the full index (members + bloom + hash), with no prior index.
    // build_gzip_index_artifacts walks the members and decompresses once;
    // IndexFoldDriver fans that single parse to AggFold (the query) and
    // BloomFold + RowSetFold (the index).
    TEST_CASE("one raw-gzip pass fuses aggregation with the index build") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz =
            create_meta_trace(env);  // FH/HH metadata + data events

        // Reference: build the index eagerly, aggregate through the normal
        // fused scan.
        std::string ref_idx = determine_index_path(gz, env.get_dir() + "/refi");
        {
            StringSink s;
            View::from_file(gz, ref_idx).sink_json(s).get();
        }
        gmoracle::GroupMap ref;
        {
            ViewPlan rp = count_plan(gz, ref_idx);
            ensure_schema(rp);
            StringIntern intern;
            gmoracle::OracleAggFold a(rp, intern);
            std::array<Fold*, 1> folds{&a};
            ViewDefinition vd = make_vdef(rp, /*for_aggregation=*/true);
            fuse(rp, vd, folds, intern).get();
            ref = a.map();
        }
        REQUIRE(ref.size() == 2);  // POSIX + STDIO

        // Prototype: no index for this path. One pass builds it while
        // aggregating.
        std::string idx = determine_index_path(gz, env.get_dir() + "/onei");
        ViewPlan pp = count_plan(gz, idx);
        ensure_schema(pp);
        StringIntern intern;
        gmoracle::OracleAggFold agg(pp, intern);
        BloomFold bloom(intern);
        RowSetFold dict(intern,
                        dftracer::utils::index::indexed_rowsets(
                            dftracer::utils::index::get_schema("dftracer")));
        std::array<Fold*, 3> folds{&agg, &bloom, &dict};
        IndexFoldDriver driver(intern, folds, gz, idx);

        gzi::GzipBuildArtifacts arts;
        run_coro(
            [&](CoroScope& scope) -> dftracer::utils::coro::CoroTask<void> {
                dftracer::utils::index::gzip::CheckpointIndexer::VisitorList vl;
                vl.emplace_back(driver);
                auto a = co_await gzi::build_gzip_index_artifacts(
                    gz,
                    dftracer::utils::index::gzip::CheckpointIndexer::
                        DEFAULT_CHECKPOINT_SIZE,
                    vl, &scope);
                REQUIRE(a.has_value());
                arts = std::move(*a);
                co_return;
            },
            "raw-gzip-fuse");
        driver.seal();

        // Persist the index this one pass produced.
        int fid = -1;
        {
            dftracer::utils::index::store::IndexDatabase db(idx);
            auto logical =
                dftracer::utils::index::store::internal::get_logical_path(gz);
            const auto hash =
                dftracer::utils::index::store::internal::calculate_file_hash(
                    gz);
            const auto mtime = static_cast<std::uint64_t>(
                dftracer::utils::index::store::internal::
                    get_file_modification_time(gz));
            const auto bytes =
                dftracer::utils::index::store::internal::file_size_bytes(gz);
            auto w = db.begin_write();
            fid = dftu_utils_test::register_test_file(*w, logical, hash, mtime,
                                                      bytes);
            dftracer::utils::index::gzip::persist_gzip_index_artifacts(*w, fid,
                                                                       arts);
            bloom.write(*w, fid);
            dict.write(*w, fid);
            w->commit();
        }

        // Per-consumer filtering: the aggregation from the one pass matches the
        // eager path's groups (the data-event cats), and the ph="M" metadata is
        // NOT counted as a "dftracer" group despite the folds being fed it.
        gmoracle::GroupMap proto = agg.map();
        CHECK(proto.size() == ref.size());
        for (const auto& [k, v] : ref) CHECK(proto.count(k) == 1);
        CHECK(proto.count("dftracer") == 0);  // metadata kept out of the agg

        // The index that same pass produced is complete: bloom, members, and
        // the row sets harvested from the very metadata the agg excluded.
        dftracer::utils::index::store::IndexDatabase rd(
            idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        int rid = rd.get_file_info_id(
            dftracer::utils::index::store::internal::get_logical_path(gz));
        REQUIRE(rid >= 0);
        CHECK(rd.pruning_tier_current(rid));
        CHECK(!arts.members.empty());
        CHECK(dftu_utils_test::bloom_chunks(rd, rid, "name") > 0);
        CHECK(rd.rowset(rid, "files").has_value());
    }

    // End to end through the public View API: a collect on a file with no index
    // routes through the bootstrap, so it answers the aggregation AND leaves a
    // complete bloom index behind. The eager-indexed collect never builds
    // bloom, so has_bloom_data being set is proof the bootstrap ran.
    TEST_CASE("View.collect on a first-touch file bootstraps the index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_meta_trace(env);

        std::string ref_idx = determine_index_path(gz, env.get_dir() + "/ri");
        {
            StringSink s;
            View::from_file(gz, ref_idx).sink_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .get();

        std::string boot_idx = determine_index_path(gz, env.get_dir() + "/bi");
        REQUIRE_FALSE(fs::exists(boot_idx));
        auto boot = View::from_file(gz, boot_idx)
                        .group_by({GroupKey::cat()})
                        .agg({{AggOp::Count, "", "n"}})
                        .collect()
                        .get();

        CHECK(boot.names == ref.names);            // same group + value columns
        CHECK(boot.num_rows() == ref.num_rows());  // same groups, no metadata

        dftracer::utils::index::store::IndexDatabase rd(
            boot_idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(
            dftracer::utils::index::store::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.pruning_tier_current(fid));  // built in that one collect pass
    }

    // A full export of a first-touch file routes through run_export's
    // bootstrap: one raw-gzip pass streams every original line to the sink
    // verbatim (both ph="M" metadata records and all data events, unlike the
    // indexed scanner which reconstructs only FH metadata) AND leaves a
    // complete bloom index behind.
    TEST_CASE("View.sink_json on a first-touch file bootstraps the data") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_meta_trace(env);  // 2 metadata + 20 data events

        std::string idx = determine_index_path(gz, env.get_dir() + "/ei");
        REQUIRE_FALSE(fs::exists(idx));
        StringSink boot;
        View::from_file(gz, idx).sink_json(boot).get();  // bootstrap path

        auto lines = boot.lines();
        CHECK(lines.size() == 20);
        CHECK(count_containing(lines, R"("ph":"M")") == 0);
        CHECK(count_containing(lines, R"("ph":"X")") == 20);

        StringSink all;
        View::from_file(gz, idx).phase(Phase::Any).sink_json(all).get();
        CHECK(all.lines().size() == 22);
        CHECK(count_containing(all.lines(), R"("name":"FH")") == 1);
        CHECK(count_containing(all.lines(), R"("name":"HH")") == 1);

        dftracer::utils::index::store::IndexDatabase rd(
            idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(
            dftracer::utils::index::store::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.pruning_tier_current(fid));  // index built in the dump pass
    }

    // A filtered first-touch aggregation whose predicate names only
    // POD-evaluable fields (cat here) now takes the bootstrap: it applies the
    // filter in the one pass AND leaves a complete index behind. Correctly
    // filtered (1 group, POSIX), not the bootstrap's unfiltered 2.
    TEST_CASE("a POD-evaluable filtered first-touch aggregation bootstraps") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_meta_trace(env);  // cats POSIX + STDIO

        std::string ref_idx = determine_index_path(gz, env.get_dir() + "/fr");
        {
            StringSink s;
            View::from_file(gz, ref_idx).sink_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .duql(R"(cat == "POSIX")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .get();
        REQUIRE(ref.num_rows() == 1);  // only POSIX

        std::string fresh = determine_index_path(gz, env.get_dir() + "/ff");
        REQUIRE_FALSE(fs::exists(fresh));
        auto got = View::from_file(gz, fresh)
                       .duql(R"(cat == "POSIX")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .get();
        CHECK(got.num_rows() == ref.num_rows());  // filter applied: 1 group
        CHECK(rows_equal(got, ref));              // same counts

        dftracer::utils::index::store::IndexDatabase rd(
            fresh, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(
            dftracer::utils::index::store::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.pruning_tier_current(
            fid));  // built in the filtered bootstrap pass
    }

    // A filter naming a non-POD field (an arg) is not bootstrap-evaluable, so
    // the query falls through to the scan and still returns the right result.
    TEST_CASE("a non-POD filtered first-touch aggregation still answers") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_meta_trace(env);  // data events carry fhash=fh1

        std::string ref_idx = determine_index_path(gz, env.get_dir() + "/nr");
        {
            StringSink s;
            View::from_file(gz, ref_idx).sink_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .duql(R"(fhash == "fh1")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .get();

        std::string fresh = determine_index_path(gz, env.get_dir() + "/nf");
        auto got = View::from_file(gz, fresh)
                       .duql(R"(fhash == "fh1")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .get();
        CHECK(rows_equal(got, ref));  // correctly filtered via the fallback
    }
}

namespace {

// Logs every step as "checkpoint:name:ts" per event, by name so the log does
// not depend on the order the intern assigned ids in.
class RecordingFold final : public Fold {
   public:
    explicit RecordingFold(const StringIntern& intern) : intern_(&intern) {}
    bool accepts(const ScanShape&) const override { return true; }
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<RecordingFold>(*intern_);
    }
    void step(const FoldBatch& batch) override {
        for (const auto& e : batch.events)
            log.push_back(std::to_string(batch.unit.checkpoint_idx) + ":" +
                          std::string(intern_->resolve(e.name_id)) + ":" +
                          std::to_string(e.ts));
    }
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold&) override {}
    dftracer::utils::coro::CoroTask<bool> finalize(
        const CoverageSet&) override {
        co_return false;
    }
    std::vector<std::string> log;

   private:
    const StringIntern* intern_;
};

// `members` gzip members of `events` events each; every fifth member ends
// inside a line that the next member finishes.
std::string write_multi_member(const std::string& dir, int members = 40,
                               int events = 300) {
    const std::string path = dir + "/multi.pfw.gz";
    std::string carry;
    int ts = 0;
    for (int m = 0; m < members; ++m) {
        std::string text = carry;
        carry.clear();
        if (m == 0) text += "[\n";
        for (int i = 0; i < events; ++i, ++ts)
            text += R"({"id":)" + std::to_string(ts) + R"(,"name":"op)" +
                    std::to_string(ts % 7) +
                    R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                    std::to_string(ts * 10) + R"(,"dur":5,"ph":"X"})" + "\n";
        if (m % 5 == 4 && m + 1 < members) {
            const std::string tail = R"({"id":)" + std::to_string(ts) +
                                     R"(,"name":"cut","cat":"POSIX","pid":1,)"
                                     R"("tid":1,"ts":)" +
                                     std::to_string(ts * 10) +
                                     R"(,"dur":5,"ph":"X"})" + "\n";
            ++ts;
            text += tail.substr(0, tail.size() / 2);
            carry = tail.substr(tail.size() / 2);
        }
        gzFile g = gzopen(path.c_str(), m == 0 ? "wb" : "ab");
        REQUIRE(g != nullptr);
        REQUIRE(gzwrite(g, text.data(), static_cast<unsigned>(text.size())) ==
                static_cast<int>(text.size()));
        gzclose(g);
    }
    return path;
}

std::vector<std::string> record_index_pass(const std::string& gz,
                                           const std::string& idx,
                                           std::size_t parse_ahead) {
    StringIntern intern;
    RecordingFold rec(intern);
    std::array<Fold*, 1> folds{&rec};
    IndexFoldDriver driver(intern, folds, gz, idx);
    driver.set_parse_ahead(parse_ahead);
    run_coro(
        [&](CoroScope& scope) -> dftracer::utils::coro::CoroTask<void> {
            dftracer::utils::index::gzip::CheckpointIndexer::VisitorList vl;
            vl.emplace_back(driver);
            auto a = co_await gzi::build_gzip_index_artifacts(
                gz,
                dftracer::utils::index::gzip::CheckpointIndexer::
                    DEFAULT_CHECKPOINT_SIZE,
                vl, &scope);
            REQUIRE(a.has_value());
            co_return;
        },
        "parse-ahead");
    driver.seal();
    return rec.log;
}

}  // namespace

class ThrowingFold final : public Fold {
   public:
    bool accepts(const ScanShape&) const override { return true; }
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<ThrowingFold>();
    }
    void step(const FoldBatch&) override {
        if (++batches == 12) throw std::runtime_error("bad batch");
    }
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold&) override {}
    dftracer::utils::coro::CoroTask<bool> finalize(
        const CoverageSet&) override {
        co_return false;
    }
    int batches = 0;
};

// Every live key and value of every family of an index, in order.
std::vector<std::pair<std::string, std::string>> index_contents(
    const std::string& index_path) {
    namespace store = dftracer::utils::index::store;
    store::RocksDBManager::instance().reset(index_path);
    store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t f = 0; f < store::layout::FAMILY_COUNT; ++f) {
        const auto family = static_cast<store::layout::Family>(f);
        auto it = db.db()->new_iterator(store::layout::family_name(family));
        if (!it) continue;
        for (it->SeekToFirst(); it->Valid(); it->Next())
            out.emplace_back(std::string(store::layout::family_name(family)) +
                                 "|" + it->key().ToString(),
                             it->value().ToString());
    }
    return out;
}

TEST_CASE("a first-touch query under a small budget spills the same index") {
    TestEnvironment env(10);
    REQUIRE(env.is_valid());
    const std::string gz = write_multi_member(env.get_dir());
    auto count = [&](const std::string& idx, std::uint64_t budget) {
        return View::from_file(gz, idx)
            .memory_budget(budget)
            .group_by({GroupKey::cat()})
            .agg({{AggOp::Count, "", "n"}})
            .collect()
            .get()
            .num_rows();
    };
    const std::string held = determine_index_path(gz, env.get_dir() + "/held");
    const std::string spilled =
        determine_index_path(gz, env.get_dir() + "/spilled");
    // The default budget holds every chunk, and the tier is still written
    // as sorted runs.
    const std::string runs = determine_index_path(gz, env.get_dir() + "/runs");
    CHECK(count(held, dftracer::utils::NO_SPILL_BUDGET) ==
          count(spilled, 4096));
    CHECK(count(runs, 0) == count(held, dftracer::utils::NO_SPILL_BUDGET));
    CHECK_FALSE(fs::exists(spilled + ".spill"));
    CHECK_FALSE(fs::exists(runs + ".spill"));
    const auto expected = index_contents(held);
    CHECK(expected == index_contents(spilled));
    CHECK(expected == index_contents(runs));
}

TEST_CASE("a fold that throws while parsing ahead ends the index pass") {
    TestEnvironment env(10);
    REQUIRE(env.is_valid());
    const std::string gz = write_multi_member(env.get_dir());
    StringIntern intern;
    dftracer::utils::index::extensions::BloomFold bloom(intern);
    ThrowingFold thrower;
    std::array<Fold*, 2> folds{&bloom, &thrower};
    IndexFoldDriver driver(intern, folds, gz, env.get_dir() + "/idx");
    driver.set_parse_ahead(8);
    bool threw = false;
    run_coro(
        [&](CoroScope& scope) -> dftracer::utils::coro::CoroTask<void> {
            dftracer::utils::index::gzip::CheckpointIndexer::VisitorList vl;
            vl.emplace_back(driver);
            try {
                co_await gzi::build_gzip_index_artifacts(
                    gz,
                    dftracer::utils::index::gzip::CheckpointIndexer::
                        DEFAULT_CHECKPOINT_SIZE,
                    vl, &scope);
            } catch (const std::runtime_error& e) {
                threw = std::string(e.what()) == "bad batch";
            }
        },
        "fold-throws");
    CHECK(threw);
    CHECK(thrower.batches == 12);
}

TEST_CASE("parsing ahead steps the folds with the same events in order") {
    TestEnvironment env(10);
    REQUIRE(env.is_valid());
    const std::string gz = write_multi_member(env.get_dir());
    const auto inline_log =
        record_index_pass(gz, env.get_dir() + "/idx_inline", 0);
    const auto ahead_log =
        record_index_pass(gz, env.get_dir() + "/idx_ahead", 8);
    // 40 * 300 events plus the 7 that straddle a member boundary.
    CHECK(inline_log.size() == 40 * 300 + 7);
    CHECK(ahead_log == inline_log);
    std::size_t straddled = 0;
    for (const auto& l : ahead_log)
        if (l.find(":cut:") != std::string::npos) ++straddled;
    CHECK(straddled == 7);
}

TEST_CASE(
    "parsing ahead in pieces keeps the events of a large member in order") {
    TestEnvironment env(10);
    REQUIRE(env.is_valid());
    // Each member inflates to about 7 MB, above the driver's parse piece.
    const std::string gz = write_multi_member(env.get_dir(), 6, 60000);
    const auto inline_log =
        record_index_pass(gz, env.get_dir() + "/idx_inline", 0);
    const auto ahead_log =
        record_index_pass(gz, env.get_dir() + "/idx_ahead", 8);
    CHECK(inline_log.size() == 6 * 60000 + 1);
    CHECK(ahead_log == inline_log);
}
