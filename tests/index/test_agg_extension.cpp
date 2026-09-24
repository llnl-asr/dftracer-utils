// The aggregation tier as the dftracer.agg extension: manifest entries, a
// rebuild of only the tier, and readers that use it only when it is current.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::Indexer;
using dftracer::utils::index::IndexerOptions;
using dftracer::utils::index::schemas::dft::agg::AggregationConfig;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::IndexExtension;
using dftracer::utils::trace::views::AggOp;
using dftracer::utils::trace::views::GroupKey;
using dftracer::utils::trace::views::View;
using dftracer::utils::trace::views::ViewFile;
using dftu_utils_test::TestEnvironment;

namespace {

constexpr std::string_view SENTINEL = "agg_extension.sentinel";

// `n` POSIX reads of 100 us each, one per millisecond from `start_us`.
std::string write_trace(const std::string& dir, const std::string& name, int n,
                        std::uint64_t start_us) {
    const std::string pfw = dir + "/" + name + ".pfw";
    {
        std::ofstream out(pfw);
        for (int i = 0; i < n; ++i)
            out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                   R"("tid":1,"ts":)"
                << start_us + static_cast<std::uint64_t>(i) * 1000
                << R"(,"dur":100,"args":{}})" << "\n";
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
    fs::remove(pfw);
    return gz;
}

IndexerOptions with_aggregation(std::uint64_t interval_us) {
    IndexerOptions o;
    AggregationConfig c;
    c.time_interval_us = interval_us;
    o.aggregation = c;
    return o;
}

std::string index_of(const std::string& trace) {
    return trace::internal::determine_index_path(trace, "");
}

int file_id(const std::string& trace) {
    IndexDatabase db(index_of(trace), index::store::IndexOpenMode::ReadOnly);
    return db.get_file_info_id(index::store::internal::get_logical_path(trace));
}

// A bloom granule no build writes: it survives only while the file's bloom
// data is not rewritten.
void plant_sentinel(const std::string& trace) {
    const int fid = file_id(trace);
    IndexDatabase db(index_of(trace));
    auto w = db.begin_write();
    index::store::records::put_path_granule(*w, IndexExtension::BLOOM, fid,
                                            SENTINEL, 0, "x");
    w->commit();
}

bool has_sentinel(const std::string& trace) {
    const int fid = file_id(trace);
    IndexDatabase db(index_of(trace), index::store::IndexOpenMode::ReadOnly);
    return !db.path_granules(fid, IndexExtension::BLOOM, SENTINEL).empty();
}

std::optional<index::ExtensionEntry> agg_entry(const Indexer& ix,
                                               const std::string& trace) {
    const auto manifest = ix.manifest();
    for (const auto& f : manifest)
        if (f.path == trace)
            for (const auto& e : f.extensions)
                if (e.name == "dftracer.agg") return e;
    return std::nullopt;
}

double count_reads(const std::vector<std::string>& traces) {
    std::vector<ViewFile> files;
    for (const auto& t : traces) files.push_back({t, index_of(t)});
    const auto df = View::from_files(std::move(files))
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "n"}})
                        .collect()
                        .get();
    REQUIRE(df.num_rows() == 1);
    for (std::size_t i = 0; i < df.names.size(); ++i)
        if (df.names[i] == "n") {
            const auto& c = df.columns[i];
            if (c.type() == dataframe::TypeId::Int64)
                return static_cast<double>(c.data<std::int64_t>()[0]);
            if (c.type() == dataframe::TypeId::Uint64)
                return static_cast<double>(c.data<std::uint64_t>()[0]);
            return c.data<double>()[0];
        }
    FAIL("no count column");
    return 0;
}

}  // namespace

TEST_SUITE("AggExtension") {
    TEST_CASE("an aggregated file lists a current dftracer.agg entry") {
        TestEnvironment env(10);
        const auto gz = write_trace(env.get_dir(), "a", 50, 1000);
        auto ix = Indexer::open({gz}, with_aggregation(1'000'000));
        ix.build();
        const auto e = agg_entry(ix, gz);
        REQUIRE(e);
        CHECK(e->ready);
        CHECK(e->current);
        CHECK(Indexer::open({gz}, with_aggregation(1'000'000))
                  .status()
                  .needs_work.empty());
    }

    TEST_CASE("an interval change rebuilds only the tier") {
        TestEnvironment env(10);
        const auto gz = write_trace(env.get_dir(), "a", 50, 1000);
        Indexer::open({gz}, with_aggregation(1'000'000)).build();
        plant_sentinel(gz);

        auto ix = Indexer::open({gz}, with_aggregation(100'000));
        const auto before = ix.status();
        CHECK(before.aggregation_needs_rebuild);
        CHECK(before.aggregation_interval_us == 1'000'000);
        const auto after = ix.build();
        CHECK_FALSE(after.aggregation_needs_rebuild);
        CHECK(after.aggregation_interval_us == 100'000);
        CHECK(after.needs_work.empty());
        CHECK(has_sentinel(gz));
        REQUIRE(agg_entry(ix, gz));
        CHECK(agg_entry(ix, gz)->current);
        CHECK(count_reads({gz}) == 50);
    }

    TEST_CASE("a changed source rebuilds the tier and that file only") {
        TestEnvironment env(10);
        const auto a = write_trace(env.get_dir(), "a", 30, 1000);
        const auto b = write_trace(env.get_dir(), "b", 20, 500'000);
        Indexer::open({a, b}, with_aggregation(1'000'000)).build();
        CHECK(count_reads({a, b}) == 50);
        plant_sentinel(a);

        index::store::RocksDBManager::instance().reset(index_of(a));
        write_trace(env.get_dir(), "b", 45, 500'000);
        fs::last_write_time(b,
                            fs::last_write_time(b) + std::chrono::seconds(10));

        auto ix = Indexer::open({a, b}, with_aggregation(1'000'000));
        CHECK(ix.status().aggregation_needs_rebuild);
        ix.build();
        CHECK(has_sentinel(a));
        CHECK(count_reads({a, b}) == 75);
        for (const auto& t : {a, b}) {
            REQUIRE(agg_entry(ix, t));
            CHECK(agg_entry(ix, t)->current);
        }

        // A fresh index over the same files gives the same answer.
        TestEnvironment fresh(10);
        const auto fa = fresh.get_dir() + "/a.pfw.gz";
        const auto fb = fresh.get_dir() + "/b.pfw.gz";
        fs::copy_file(a, fa);
        fs::copy_file(b, fb);
        Indexer::open({fa, fb}, with_aggregation(1'000'000)).build();
        CHECK(count_reads({fa, fb}) == 75);
    }

    TEST_CASE("a file without a current entry makes readers scan") {
        TestEnvironment env(10);
        const auto a = write_trace(env.get_dir(), "a", 30, 1000);
        Indexer::open({a}, with_aggregation(1'000'000)).build();
        // The tier answers this, and its cache keeps the index open, which a
        // later build must still be able to open for writing.
        CHECK(count_reads({a}) == 30);
        const auto b = write_trace(env.get_dir(), "b", 20, 500'000);
        // B joins the index without aggregation, so the tier holds only A.
        Indexer::open({a, b}).build();
        CHECK(count_reads({a, b}) == 50);
    }
}
