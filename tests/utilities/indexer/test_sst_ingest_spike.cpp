#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using dftracer::utils::index::extensions::ChunkDimensionStats;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftracer::utils::index::schemas::dft::ChunkStatistics;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::IndexExtension;
using dftracer::utils::index::store::IndexWrite;
namespace records = dftracer::utils::index::store::records;
namespace layout = dftracer::utils::index::store::layout;
using dftracer::utils::index::extensions::kinds::value_hash;
using dftracer::utils::index::store::IndexDatabaseSstWriterContext;
using dftracer::utils::index::store::SstArtifactRegistry;

namespace {

GzipMemberRecord make_member(std::uint64_t idx, std::uint64_t uc_offset,
                             std::uint64_t num_lines) {
    GzipMemberRecord m{};
    m.member_idx = idx;
    m.uc_offset = uc_offset;
    m.uc_size = 64 * 1024;
    m.c_offset = uc_offset / 2;
    m.c_size = 32 * 1024;
    m.first_line_num = idx * num_lines + 1;
    m.last_line_num = (idx + 1) * num_lines;
    return m;
}

ChunkStatistics make_chunk_stats(std::uint64_t total_events) {
    ChunkStatistics stats;
    stats.total_events = total_events;
    stats.min_timestamp_us = 1000;
    stats.max_timestamp_us = 9000;
    stats.name_counts["read"] = total_events / 2;
    stats.name_counts["write"] = total_events - total_events / 2;
    stats.category_counts["posix"] = total_events;
    stats.pid_tid_counts["1:1"] = total_events;
    return stats;
}

ChunkDimensionStats make_dim_stats(std::string_view dim, std::uint64_t distinct,
                                   std::string_view min_val,
                                   std::string_view max_val) {
    ChunkDimensionStats ds;
    ds.dimension = std::string(dim);
    ds.distinct_count = distinct;
    ds.min_value = std::string(min_val);
    ds.max_value = std::string(max_val);
    ds.value_type = "string";
    return ds;
}

struct Fixture {
    GzipMemberRecord cp_a = make_member(0, 0, 100);
    GzipMemberRecord cp_b = make_member(1, 64 * 1024, 100);

    std::vector<unsigned char> bloom_blob_a{0x11, 0x22, 0x33, 0x44};
    std::vector<unsigned char> bloom_blob_b{0x55, 0x66, 0x77, 0x88};

    ChunkStatistics chunk_stats_a = make_chunk_stats(60);
    ChunkStatistics chunk_stats_b = make_chunk_stats(80);
    ChunkStatistics file_stats = make_chunk_stats(140);

    ChunkDimensionStats dim_stats_a =
        make_dim_stats("name", 3, "fsync", "read");
    ChunkDimensionStats dim_stats_b =
        make_dim_stats("name", 5, "close", "write");

    bool with_second_member = true;

    void populate(IndexWrite& w, int file_id) {
        const auto path =
            "/traces/trace_" + std::to_string(file_id) + ".pfw.gz";
        records::put_file_record(
            w, path, {static_cast<std::uint32_t>(file_id), 7, 11, 13});
        records::clear_file(w, IndexExtension::MEMBERS, file_id);
        for (auto ext : {IndexExtension::ZONEMAP, IndexExtension::BLOOM,
                         IndexExtension::COUNTS, IndexExtension::POSTINGS,
                         IndexExtension::STATS})
            records::clear_file(w, ext, file_id);

        records::put_gzip_member(w, file_id, cp_a);
        if (with_second_member) records::put_gzip_member(w, file_id, cp_b);
        records::put_file_metadata(w, file_id, /*checkpoint_size=*/64 * 1024,
                                   /*total_lines=*/200,
                                   /*total_uc_size=*/128 * 1024,
                                   /*truncated=*/false);

        dftu_utils_test::put_chunk_bloom(w, file_id, cp_a.member_idx, "name",
                                         bloom_blob_a, /*num_entries=*/4);
        if (with_second_member)
            dftu_utils_test::put_chunk_bloom(w, file_id, cp_b.member_idx,
                                             "name", bloom_blob_b,
                                             /*num_entries=*/5);
        dftu_utils_test::put_file_bloom(w, file_id, "name", bloom_blob_a,
                                        /*num_entries=*/8);

        records::put_chunk_statistics(w, file_id, cp_a.member_idx,
                                      chunk_stats_a);
        if (with_second_member)
            records::put_chunk_statistics(w, file_id, cp_b.member_idx,
                                          chunk_stats_b);
        records::put_file_scalar_stats(w, file_id, file_stats,
                                       /*num_chunks=*/2);
        records::put_file_pid_tid_counts(w, file_id, file_stats.pid_tid_counts);

        records::put_path(w, IndexExtension::BLOOM, file_id, "name");
        dftu_utils_test::put_dimension_stats(w, file_id, cp_a.member_idx,
                                             dim_stats_a, 60);
        if (with_second_member)
            dftu_utils_test::put_dimension_stats(w, file_id, cp_b.member_idx,
                                                 dim_stats_b, 80);

        namespace kinds = dftracer::utils::index::extensions::kinds;
        const auto read_id = kinds::value_hash("read");
        const auto write_id = kinds::value_hash("write");
        records::put_path(w, IndexExtension::POSTINGS, file_id, "name");
        records::put_posting(w, file_id, "name", read_id);
        records::put_posting_granule(w, file_id, "name", read_id,
                                     cp_a.member_idx);
        if (with_second_member) {
            records::put_posting(w, file_id, "name", write_id);
            records::put_posting_granule(w, file_id, "name", write_id,
                                         cp_b.member_idx);
        }

        records::put_rowset(w, file_id, "files", "files-frame");
        records::put_rowset(w, file_id, "hosts", "hosts-frame");

        // One merge per key: the SST emitter combines same-key operands, so
        // each SST stays key-unique either way.
        w.put(layout::Family::AGGREGATION, "\xFF\xFD\x01", "name-one");
        w.put(layout::Family::AGGREGATION, "\xFF\xFD\x02", "name-two");
        w.merge(layout::Family::AGGREGATION, "agg-key-1", "operand-1");
        w.merge(layout::Family::AGGREGATION, "agg-key-2", "operand-2");
        w.merge(layout::Family::SYSTEM_METRICS, "sys-key-1", "sys-1");
        w.merge(layout::Family::SYSTEM_METRICS, "sys-key-2", "sys-2");

        records::put_manifest(w, file_id, IndexExtension::MEMBERS, 0);
        for (auto ext : {IndexExtension::ZONEMAP, IndexExtension::BLOOM,
                         IndexExtension::COUNTS, IndexExtension::POSTINGS,
                         IndexExtension::STATS, IndexExtension::ROWSET})
            records::put_manifest(w, file_id, ext, 42);
    }
};

/// All entries of `family` in `db`, keyed and valued by raw bytes.
std::map<std::string, std::string> entries(const IndexDatabase& db,
                                           layout::Family family) {
    std::map<std::string, std::string> out;
    auto it = db.db()->new_iterator(layout::family_name(family));
    for (it->SeekToFirst(); it->Valid(); it->Next())
        out.emplace(std::string(it->key().data(), it->key().size()),
                    std::string(it->value().data(), it->value().size()));
    return out;
}

// Merge-operand families combine on read, so both paths must return
// byte-identical keys and values in every family.
void check_same_bytes(const IndexDatabase& db_a, const IndexDatabase& db_b) {
    for (std::size_t f = 0; f < layout::FAMILY_COUNT; ++f) {
        const auto family = static_cast<layout::Family>(f);
        CAPTURE(layout::family_name(family));
        CHECK(entries(db_a, family) == entries(db_b, family));
    }
}

void check_readable(const IndexDatabase& db, int file_id) {
    CHECK(db.extension_current(file_id, IndexExtension::MEMBERS));
    CHECK(db.pruning_tier_current(file_id));
    CHECK(db.extension_state(file_id, IndexExtension::BLOOM)->params_hash ==
          42);
    CHECK(db.get_file_info_id("/traces/trace_" + std::to_string(file_id) +
                              ".pfw.gz") == file_id);
    CHECK(db.get_num_lines(file_id) == 200);
    CHECK(db.query_gzip_members(file_id).size() == 2);
    CHECK(dftu_utils_test::bloom_chunks(db, file_id, "name") == 2);
    CHECK(dftu_utils_test::has_file_bloom(db, file_id, "name"));
    CHECK(db.query_chunk_statistics(file_id).size() == 2);
    CHECK(db.query_file_scalar_stats_batch({file_id}).count(file_id) == 1);
    CHECK(db.query_file_pids(file_id).contains(1));
    CHECK(db.path_granules(file_id, IndexExtension::ZONEMAP, "name").size() ==
          2);
    CHECK(db.has_posting(file_id, "name", value_hash("read")));
    CHECK_FALSE(db.has_posting(file_id, "name", value_hash("open")));
    CHECK(db.posting_granules(file_id, "name", value_hash("write")) ==
          std::vector<std::uint64_t>{1});
    CHECK(db.rowset(file_id, "hosts") == "hosts-frame");
    CHECK(db.rowset(file_id, "files") == "files-frame");
}

}  // namespace

TEST_SUITE("IndexDatabaseSstWriterContext") {
    TEST_CASE("SST ingest matches a direct write byte for byte") {
        auto root_a = dftu_utils_test::make_unique_test_path("sst_spike_db_a");
        auto root_b = dftu_utils_test::make_unique_test_path("sst_spike_db_b");
        auto staging =
            dftu_utils_test::make_unique_test_path("sst_spike_staging");
        fs::create_directories(staging);

        Fixture f1;
        Fixture f2;
        f2.chunk_stats_b = make_chunk_stats(90);

        IndexDatabase db_a((root_a / ".dftindex").string());
        {
            auto w = db_a.begin_write();
            f1.populate(*w, 1);
            f2.populate(*w, 2);
            w->commit();
        }

        IndexDatabase db_b((root_b / ".dftindex").string());
        SstArtifactRegistry registry;
        {
            IndexDatabaseSstWriterContext sst(staging.string(), "worker_0");
            f1.populate(sst, 1);
            registry.append(sst.commit());
        }
        {
            IndexDatabaseSstWriterContext sst(staging.string(), "worker_1");
            f2.populate(sst, 2);
            registry.append(sst.commit());
        }
        CHECK(registry.files(layout::Family::MEMBERS).size() == 2);
        db_b.bulk_ingest(registry);

        check_same_bytes(db_a, db_b);
        check_readable(db_b, 1);
        check_readable(db_b, 2);
    }

    TEST_CASE("re-indexing a file leaves nothing of its fewer members") {
        auto root = dftu_utils_test::make_unique_test_path("sst_reindex_db");
        auto staging =
            dftu_utils_test::make_unique_test_path("sst_reindex_staging");
        fs::create_directories(staging);

        IndexDatabase db((root / ".dftindex").string());
        Fixture two;
        {
            auto w = db.begin_write();
            two.populate(*w, 1);
            w->commit();
        }
        REQUIRE(db.query_gzip_members(1).size() == 2);

        Fixture one;
        one.with_second_member = false;

        SUBCASE("through a write batch") {
            auto w = db.begin_write();
            one.populate(*w, 1);
            w->commit();
        }
        SUBCASE("through an SST ingest") {
            SstArtifactRegistry registry;
            IndexDatabaseSstWriterContext sst(staging.string(), "reindex");
            one.populate(sst, 1);
            registry.append(sst.commit());
            db.bulk_ingest(registry);
        }

        CHECK(db.pruning_tier_current(1));
        CHECK(db.query_gzip_members(1).size() == 1);
        CHECK(db.query_chunk_statistics(1).size() == 1);
        CHECK(dftu_utils_test::bloom_chunks(db, 1, "name") == 1);
        CHECK(db.path_granules(1, IndexExtension::ZONEMAP, "name").size() == 1);
        CHECK_FALSE(db.has_posting(1, "name", value_hash("write")));
        CHECK(db.posting_granules(1, "name", value_hash("write")).empty());
    }

    TEST_CASE("data without a manifest entry is not current") {
        auto root = dftu_utils_test::make_unique_test_path("sst_no_manifest");
        IndexDatabase db((root / ".dftindex").string());
        {
            auto w = db.begin_write();
            records::put_gzip_member(*w, 1, make_member(0, 0, 10));
            w->commit();
        }
        CHECK_FALSE(db.extension_current(1, IndexExtension::MEMBERS));
        CHECK(db.query_gzip_members(1).empty());
    }
}
