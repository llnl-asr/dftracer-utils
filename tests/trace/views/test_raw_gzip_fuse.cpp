#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/views/aggfold.h>
#include <dftracer/utils/trace/views/bloom_fold.h>
#include <dftracer/utils/trace/views/dict_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/index_fold_driver.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>

#include "groupmap_oracle.h"
#include "test_view_common.h"
#include "testing_runtime.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::StringIntern;
using dftu_utils_test::run_coro;
namespace dataframe = dftracer::utils::dataframe;

namespace idx = dftracer::utils::utilities::indexer;
namespace gzi = dftracer::utils::utilities::indexer::internal::gzip;

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
// The metadata must land in the dictionary but NOT in a cat aggregation.
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
    // BloomFold + DictFold (the index).
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
            View::from_file(gz, ref_idx).export_json(s).get();
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
        DictFold dict(intern);
        std::array<Fold*, 3> folds{&agg, &bloom, &dict};
        IndexFoldDriver driver(intern, folds, gz, idx);

        gzi::GzipBuildArtifacts arts;
        run_coro(
            [&](CoroScope& scope) -> dftracer::utils::coro::CoroTask<void> {
                idx::internal::Indexer::VisitorList vl;
                vl.emplace_back(driver);
                auto a = co_await gzi::build_gzip_index_artifacts(
                    gz, idx::internal::Indexer::DEFAULT_CHECKPOINT_SIZE, vl,
                    &scope);
                REQUIRE(a.has_value());
                arts = std::move(*a);
                co_return;
            },
            "raw-gzip-fuse");
        driver.seal();

        // Persist the index this one pass produced.
        int fid = -1;
        {
            idx::IndexDatabase db(idx);
            auto logical = idx::internal::get_logical_path(gz);
            const auto hash = idx::internal::calculate_file_hash(gz);
            const auto mtime = static_cast<std::uint64_t>(
                idx::internal::get_file_modification_time(gz));
            const auto bytes = idx::internal::file_size_bytes(gz);
            auto w = db.begin_write();
            fid = w->get_or_create_file_info(
                logical, hash,
                idx::IndexFileEntryCapability::INDEXING_COMPLETE |
                    idx::IndexFileEntryCapability::MEMBERS |
                    idx::IndexFileEntryCapability::FILE_SUMMARY,
                mtime, bytes);
            for (const auto& m : arts.members) w->insert_gzip_member(fid, m);
            w->insert_file_metadata(fid, arts.checkpoint_size, arts.total_lines,
                                    arts.total_uc_size);
            bloom.write_to_sink(*w, fid);
            dict.write_to_sink(*w);
            w->add_file_capability(fid, idx::IndexFileEntryCapability::BLOOM);
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
        // the hash dictionary harvested from the very metadata the agg
        // excluded.
        idx::IndexDatabase rd(idx, idx::IndexOpenMode::ReadOnly);
        int rid = rd.get_file_info_id(idx::internal::get_logical_path(gz));
        REQUIRE(rid >= 0);
        CHECK(rd.has_bloom_data(rid));
        CHECK(!arts.members.empty());
        CHECK(!rd.query_chunk_bloom_filters(rid, "name").empty());
        auto fh = rd.query_hash_table(idx::IndexDatabase::HashType::FILE);
        CHECK(fh.count("fh1") == 1);  // metadata went into the dictionary
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
            View::from_file(gz, ref_idx).export_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .collect()
                       .get();

        std::string boot_idx = determine_index_path(gz, env.get_dir() + "/bi");
        REQUIRE_FALSE(fs::exists(boot_idx));
        auto boot = View::from_file(gz, boot_idx)
                        .group_by({GroupKey::cat()})
                        .agg({{AggOp::Count, "", "n"}})
                        .collect()
                        .collect()
                        .get();

        CHECK(boot.names == ref.names);            // same group + value columns
        CHECK(boot.num_rows() == ref.num_rows());  // same groups, no metadata

        idx::IndexDatabase rd(boot_idx, idx::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(idx::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.has_bloom_data(fid));  // built in that one collect pass
    }

    // A full export of a first-touch file routes through run_export's
    // bootstrap: one raw-gzip pass streams every original line to the sink
    // verbatim (both ph="M" metadata records and all data events, unlike the
    // indexed scanner which reconstructs only FH metadata) AND leaves a
    // complete bloom index behind.
    TEST_CASE("View.export_json on a first-touch file bootstraps every line") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_meta_trace(env);  // 2 metadata + 20 data events

        std::string idx = determine_index_path(gz, env.get_dir() + "/ei");
        REQUIRE_FALSE(fs::exists(idx));
        StringSink boot;
        View::from_file(gz, idx).export_json(boot).get();  // bootstrap path

        auto lines = boot.lines();
        CHECK(lines.size() == 22);
        CHECK(count_containing(lines, R"("name":"FH")") == 1);  // metadata
        CHECK(count_containing(lines, R"("name":"HH")") == 1);  // metadata
        CHECK(count_containing(lines, R"("ph":"X")") == 20);    // data events

        idx::IndexDatabase rd(idx, idx::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(idx::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.has_bloom_data(fid));  // index built in the dump pass
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
            View::from_file(gz, ref_idx).export_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .query(R"(cat == "POSIX")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .collect()
                       .get();
        REQUIRE(ref.num_rows() == 1);  // only POSIX

        std::string fresh = determine_index_path(gz, env.get_dir() + "/ff");
        REQUIRE_FALSE(fs::exists(fresh));
        auto got = View::from_file(gz, fresh)
                       .query(R"(cat == "POSIX")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .collect()
                       .get();
        CHECK(got.num_rows() == ref.num_rows());  // filter applied: 1 group
        CHECK(rows_equal(got, ref));              // same counts

        idx::IndexDatabase rd(fresh, idx::IndexOpenMode::ReadOnly);
        int fid = rd.get_file_info_id(idx::internal::get_logical_path(gz));
        REQUIRE(fid >= 0);
        CHECK(rd.has_bloom_data(fid));  // built in the filtered bootstrap pass
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
            View::from_file(gz, ref_idx).export_json(s).get();
        }
        auto ref = View::from_file(gz, ref_idx)
                       .query(R"(fhash == "fh1")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .collect()
                       .get();

        std::string fresh = determine_index_path(gz, env.get_dir() + "/nf");
        auto got = View::from_file(gz, fresh)
                       .query(R"(fhash == "fh1")")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "n"}})
                       .collect()
                       .collect()
                       .get();
        CHECK(rows_equal(got, ref));  // correctly filtered via the fallback
    }
}
