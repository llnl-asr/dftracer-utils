#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

using dftracer::utils::utilities::composites::dft::indexing::
    ChunkDimensionStats;
using dftracer::utils::utilities::composites::dft::indexing::ChunkStatistics;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::IndexDatabaseSstWriterContext;
using dftracer::utils::utilities::indexer::SstArtifactRegistry;
using dftracer::utils::utilities::indexer::internal::IndexerCheckpoint;

namespace {

IndexerCheckpoint make_checkpoint(std::uint64_t idx, std::uint64_t uc_offset,
                                  std::uint64_t num_lines) {
    IndexerCheckpoint cp{};
    cp.checkpoint_idx = idx;
    cp.uc_offset = uc_offset;
    cp.uc_size = 64 * 1024;
    cp.c_offset = uc_offset / 2;
    cp.c_size = 32 * 1024;
    cp.bits = 8;
    cp.dict_compressed = std::vector<unsigned char>{0xAA, 0xBB, 0xCC};
    cp.num_lines = num_lines;
    cp.first_line_num = idx * num_lines + 1;
    cp.last_line_num = (idx + 1) * num_lines;
    return cp;
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
    IndexerCheckpoint cp_a = make_checkpoint(0, 0, 100);
    IndexerCheckpoint cp_b = make_checkpoint(1, 64 * 1024, 100);

    std::vector<std::uint32_t> read_lines{1, 5, 17, 42};
    std::vector<std::uint32_t> write_lines{2, 8, 23};
    std::vector<std::uint32_t> md_proc_lines{3, 9};
    std::unordered_set<std::uint64_t> pids{101, 102, 103};

    std::vector<unsigned char> bloom_blob_a{0x11, 0x22, 0x33, 0x44};
    std::vector<unsigned char> bloom_blob_b{0x55, 0x66, 0x77, 0x88};

    ChunkStatistics chunk_stats_a = make_chunk_stats(60);
    ChunkStatistics chunk_stats_b = make_chunk_stats(80);
    ChunkStatistics file_stats = make_chunk_stats(140);

    ChunkDimensionStats dim_stats_a =
        make_dim_stats("name", 3, "fsync", "read");
    ChunkDimensionStats dim_stats_b =
        make_dim_stats("name", 5, "close", "write");

    template <typename Sink>
    void populate(Sink& sink, int file_id) {
        sink.insert_checkpoint(file_id, cp_a);
        sink.insert_checkpoint(file_id, cp_b);
        sink.insert_file_metadata(file_id, /*checkpoint_size=*/64 * 1024,
                                  /*total_lines=*/200,
                                  /*total_uc_size=*/128 * 1024);
        sink.insert_event_range(file_id, cp_a.checkpoint_idx, "posix", "read",
                                read_lines);
        sink.insert_event_range(file_id, cp_b.checkpoint_idx, "posix", "write",
                                write_lines);
        sink.insert_metadata_lines(file_id, cp_a.checkpoint_idx, "PR",
                                   md_proc_lines);
        sink.insert_file_pids(file_id, pids);

        sink.insert_chunk_bloom_filter(
            file_id, cp_a.checkpoint_idx, "name",
            std::span<const unsigned char>(bloom_blob_a), /*num_entries=*/4);
        sink.insert_chunk_bloom_filter(
            file_id, cp_b.checkpoint_idx, "name",
            std::span<const unsigned char>(bloom_blob_b), /*num_entries=*/5);
        sink.insert_file_bloom_filter(
            file_id, "name", std::span<const unsigned char>(bloom_blob_a),
            /*num_entries=*/8);

        sink.insert_chunk_statistics(file_id, cp_a.checkpoint_idx,
                                     chunk_stats_a);
        sink.insert_chunk_statistics(file_id, cp_b.checkpoint_idx,
                                     chunk_stats_b);
        sink.insert_file_scalar_stats(file_id, file_stats, /*num_chunks=*/2);
        sink.insert_file_category_counts(file_id, file_stats.category_counts);
        sink.insert_file_pid_tid_counts(file_id, file_stats.pid_tid_counts);
        sink.insert_file_name_counts(file_id, file_stats.name_counts);

        sink.insert_index_dimension(file_id, "name");
        sink.insert_index_dimension(file_id, "cat");
        sink.insert_chunk_dimension_stats(file_id, cp_a.checkpoint_idx,
                                          dim_stats_a);
        sink.insert_chunk_dimension_stats(file_id, cp_b.checkpoint_idx,
                                          dim_stats_b);

        using dftracer::utils::utilities::hash::fnv1a_hash;
        const auto read_id = fnv1a_hash(std::string_view{"read"});
        const auto write_id = fnv1a_hash(std::string_view{"write"});
        sink.insert_name_dictionary_entry(read_id, "read");
        sink.insert_name_dictionary_entry(write_id, "write");
        sink.insert_name_file_posting(read_id, file_id);
        sink.insert_name_file_posting(write_id, file_id);
        sink.insert_name_chunk_posting(read_id, file_id, cp_a.checkpoint_idx);
        sink.insert_name_chunk_posting(write_id, file_id, cp_b.checkpoint_idx);

        sink.insert_hash_table_entry(
            static_cast<std::uint8_t>(IndexDatabase::HashType::FILE), "fh_1",
            "/path/to/trace.pfw.gz");
        sink.insert_hash_table_entry(
            static_cast<std::uint8_t>(IndexDatabase::HashType::HOST), "hh_1",
            "host-1");
        sink.insert_hash_table_entry(
            static_cast<std::uint8_t>(IndexDatabase::HashType::STRING), "sh_1",
            "some-string");

        // Aggregation / system_metrics sink writes. SstFileWriter requires
        // strictly ascending keys within a single SST, so the raw sink
        // API here exercises one merge per key. Cross-flush merges
        // targeting the same key are the AggregationVisitor's concern: it
        // rotates its SstWriterContext per flush so each SST is key-unique.
        sink.insert_aggregation_put("\xFF\xFD\x01", "name-one");
        sink.insert_aggregation_put("\xFF\xFD\x02", "name-two");
        sink.insert_aggregation_merge("agg-key-1", "operand-1");
        sink.insert_aggregation_merge("agg-key-2", "operand-2");
        sink.insert_system_metrics_merge("sys-key-1", "sys-1");
        sink.insert_system_metrics_merge("sys-key-2", "sys-2");
    }
};

void compare_cf_entries(const IndexDatabase& db_a, const IndexDatabase& db_b,
                        std::string_view cf_name);

void check_round_trip(const IndexDatabase& db_a, const IndexDatabase& db_b,
                      int file_id) {
    CHECK(db_a.get_checkpoint_size(file_id) ==
          db_b.get_checkpoint_size(file_id));
    CHECK(db_a.get_num_lines(file_id) == db_b.get_num_lines(file_id));
    CHECK(db_a.get_max_bytes(file_id) == db_b.get_max_bytes(file_id));

    auto cps_a = db_a.query_checkpoints(file_id);
    auto cps_b = db_b.query_checkpoints(file_id);
    REQUIRE(cps_a.size() == cps_b.size());
    for (std::size_t i = 0; i < cps_a.size(); ++i) {
        CHECK(cps_a[i].checkpoint_idx == cps_b[i].checkpoint_idx);
        CHECK(cps_a[i].uc_offset == cps_b[i].uc_offset);
        CHECK(cps_a[i].uc_size == cps_b[i].uc_size);
        CHECK(cps_a[i].c_offset == cps_b[i].c_offset);
        CHECK(cps_a[i].c_size == cps_b[i].c_size);
        CHECK(cps_a[i].num_lines == cps_b[i].num_lines);
        CHECK(cps_a[i].first_line_num == cps_b[i].first_line_num);
        CHECK(cps_a[i].last_line_num == cps_b[i].last_line_num);
    }

    auto er_a = db_a.query_event_ranges(file_id);
    auto er_b = db_b.query_event_ranges(file_id);
    REQUIRE(er_a.size() == er_b.size());
    for (std::size_t i = 0; i < er_a.size(); ++i) {
        CHECK(er_a[i].checkpoint_idx == er_b[i].checkpoint_idx);
        CHECK(er_a[i].cat == er_b[i].cat);
        CHECK(er_a[i].name == er_b[i].name);
        CHECK(er_a[i].line_numbers == er_b[i].line_numbers);
    }

    auto md_a = db_a.query_metadata_lines(file_id);
    auto md_b = db_b.query_metadata_lines(file_id);
    REQUIRE(md_a.size() == md_b.size());
    for (std::size_t i = 0; i < md_a.size(); ++i) {
        CHECK(md_a[i].checkpoint_idx == md_b[i].checkpoint_idx);
        CHECK(md_a[i].meta_type == md_b[i].meta_type);
        CHECK(md_a[i].line_numbers == md_b[i].line_numbers);
    }

    CHECK(db_a.query_file_pids(file_id) == db_b.query_file_pids(file_id));

    auto cbf_a = db_a.query_chunk_bloom_filters(file_id, "name");
    auto cbf_b = db_b.query_chunk_bloom_filters(file_id, "name");
    REQUIRE(cbf_a.size() == cbf_b.size());
    for (std::size_t i = 0; i < cbf_a.size(); ++i) {
        CHECK(cbf_a[i].checkpoint_idx == cbf_b[i].checkpoint_idx);
        CHECK(cbf_a[i].num_entries == cbf_b[i].num_entries);
        CHECK(cbf_a[i].bloom_data == cbf_b[i].bloom_data);
    }

    auto fbf_a = db_a.query_file_bloom_filter(file_id, "name");
    auto fbf_b = db_b.query_file_bloom_filter(file_id, "name");
    REQUIRE(fbf_a.has_value());
    REQUIRE(fbf_b.has_value());
    CHECK(fbf_a->num_entries == fbf_b->num_entries);
    CHECK(fbf_a->bloom_data == fbf_b->bloom_data);

    auto cs_a = db_a.query_chunk_statistics(file_id);
    auto cs_b = db_b.query_chunk_statistics(file_id);
    REQUIRE(cs_a.size() == cs_b.size());
    for (std::size_t i = 0; i < cs_a.size(); ++i) {
        CHECK(cs_a[i].checkpoint_idx == cs_b[i].checkpoint_idx);
        CHECK(cs_a[i].stats.total_events == cs_b[i].stats.total_events);
        CHECK(cs_a[i].stats.min_timestamp_us == cs_b[i].stats.min_timestamp_us);
        CHECK(cs_a[i].stats.max_timestamp_us == cs_b[i].stats.max_timestamp_us);
    }

    auto fss_a = db_a.query_file_scalar_stats_batch({file_id});
    auto fss_b = db_b.query_file_scalar_stats_batch({file_id});
    REQUIRE(fss_a.count(file_id) == 1);
    REQUIRE(fss_b.count(file_id) == 1);
    CHECK(fss_a[file_id].stats.total_events ==
          fss_b[file_id].stats.total_events);
    CHECK(fss_a[file_id].num_chunks == fss_b[file_id].num_chunks);

    auto cat_a = db_a.query_file_category_counts_batch({file_id});
    auto cat_b = db_b.query_file_category_counts_batch({file_id});
    REQUIRE(cat_a.count(file_id) == 1);
    REQUIRE(cat_b.count(file_id) == 1);
    CHECK(cat_a[file_id].size() == cat_b[file_id].size());
    for (const auto& [k, v] : cat_a[file_id]) {
        auto it = cat_b[file_id].find(k);
        REQUIRE(it != cat_b[file_id].end());
        CHECK(it->second == v);
    }

    auto pt_a = db_a.query_file_pid_tid_counts_batch({file_id});
    auto pt_b = db_b.query_file_pid_tid_counts_batch({file_id});
    REQUIRE(pt_a.count(file_id) == 1);
    REQUIRE(pt_b.count(file_id) == 1);
    CHECK(pt_a[file_id].size() == pt_b[file_id].size());

    auto ns_a = db_a.query_file_name_summaries_batch({file_id});
    auto ns_b = db_b.query_file_name_summaries_batch({file_id});
    REQUIRE(ns_a.count(file_id) == 1);
    REQUIRE(ns_b.count(file_id) == 1);
    CHECK(ns_a[file_id].counts.size() == ns_b[file_id].counts.size());
    CHECK(ns_a[file_id].unique_count == ns_b[file_id].unique_count);

    auto dims_a = db_a.query_index_dimensions(file_id);
    auto dims_b = db_b.query_index_dimensions(file_id);
    std::sort(dims_a.begin(), dims_a.end());
    std::sort(dims_b.begin(), dims_b.end());
    CHECK(dims_a == dims_b);

    auto cds_a = db_a.query_chunk_dimension_stats(file_id);
    auto cds_b = db_b.query_chunk_dimension_stats(file_id);
    REQUIRE(cds_a.size() == cds_b.size());
    for (std::size_t i = 0; i < cds_a.size(); ++i) {
        CHECK(cds_a[i].checkpoint_idx == cds_b[i].checkpoint_idx);
        CHECK(cds_a[i].dimension == cds_b[i].dimension);
        CHECK(cds_a[i].distinct_count == cds_b[i].distinct_count);
        CHECK(cds_a[i].min_value == cds_b[i].min_value);
        CHECK(cds_a[i].max_value == cds_b[i].max_value);
    }

    CHECK(db_a.query_name_id("read") == db_b.query_name_id("read"));
    CHECK(db_a.query_name_id("write") == db_b.query_name_id("write"));
    CHECK(db_a.query_name_by_id(*db_a.query_name_id("read")) ==
          db_b.query_name_by_id(*db_b.query_name_id("read")));

    auto fp_a = db_a.query_name_file_postings("read");
    auto fp_b = db_b.query_name_file_postings("read");
    std::sort(fp_a.begin(), fp_a.end());
    std::sort(fp_b.begin(), fp_b.end());
    CHECK(fp_a == fp_b);

    auto cp_read_a = db_a.query_name_chunk_postings("read", file_id);
    auto cp_read_b = db_b.query_name_chunk_postings("read", file_id);
    std::sort(cp_read_a.begin(), cp_read_a.end());
    std::sort(cp_read_b.begin(), cp_read_b.end());
    CHECK(cp_read_a == cp_read_b);

    CHECK(db_a.resolve_hash(IndexDatabase::HashType::FILE, "fh_1") ==
          db_b.resolve_hash(IndexDatabase::HashType::FILE, "fh_1"));
    CHECK(db_a.resolve_hash(IndexDatabase::HashType::HOST, "hh_1") ==
          db_b.resolve_hash(IndexDatabase::HashType::HOST, "hh_1"));
    CHECK(db_a.resolve_name_to_hash(IndexDatabase::HashType::FILE,
                                    "/path/to/trace.pfw.gz") ==
          db_b.resolve_name_to_hash(IndexDatabase::HashType::FILE,
                                    "/path/to/trace.pfw.gz"));
    CHECK(db_a.query_hash_table(IndexDatabase::HashType::FILE) ==
          db_b.query_hash_table(IndexDatabase::HashType::FILE));

    namespace cf = dftracer::utils::rocksdb::cf;
    compare_cf_entries(db_a, db_b, cf::AGGREGATION);
    compare_cf_entries(db_a, db_b, cf::SYSTEM_METRICS);
}

/// Compare all entries in `cf` between two databases. Uses raw iteration
/// over the CF; for merge-operand CFs rocksdb combines operands on read
/// automatically, so both DBs must return byte-identical values.
void compare_cf_entries(const IndexDatabase& db_a, const IndexDatabase& db_b,
                        std::string_view cf_name) {
    auto collect =
        [&](const IndexDatabase& db) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        auto it = db.db()->new_iterator(cf_name);
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            out.emplace(std::string(it->key().data(), it->key().size()),
                        std::string(it->value().data(), it->value().size()));
        }
        return out;
    };
    auto a_map = collect(db_a);
    auto b_map = collect(db_b);
    REQUIRE(a_map.size() == b_map.size());
    for (auto& [k, v] : a_map) {
        auto it = b_map.find(k);
        REQUIRE(it != b_map.end());
        CHECK(it->second == v);
    }
}

void check_root_summaries(const IndexDatabase& db_a,
                          const IndexDatabase& db_b) {
    auto r_a = db_a.query_root_scalar_stats();
    auto r_b = db_b.query_root_scalar_stats();
    REQUIRE(r_a.has_value());
    REQUIRE(r_b.has_value());
    CHECK(r_a->stats.total_events == r_b->stats.total_events);
    CHECK(r_a->num_chunks == r_b->num_chunks);
    CHECK(r_a->num_files == r_b->num_files);

    CHECK(db_a.query_root_category_counts() ==
          db_b.query_root_category_counts());
    CHECK(db_a.query_root_name_counts() == db_b.query_root_name_counts());
    CHECK(db_a.query_root_pid_tid_counts() == db_b.query_root_pid_tid_counts());
}

}  // namespace

TEST_SUITE("IndexDatabaseSstWriterContext") {
    TEST_CASE("round-trip: SST ingest matches direct RocksDB writes") {
        auto root_a = dft_utils_test::make_unique_test_path("sst_spike_db_a");
        auto root_b = dft_utils_test::make_unique_test_path("sst_spike_db_b");
        auto staging =
            dft_utils_test::make_unique_test_path("sst_spike_staging");
        fs::create_directories(root_a);
        fs::create_directories(root_b);
        fs::create_directories(staging);

        Fixture f;
        const int file_id = 1;

        IndexDatabase db_a((root_a / ".dftindex").string());
        {
            auto w = db_a.begin_write();
            w->init_schema();
            f.populate(*w, file_id);
            w->commit();
        }

        IndexDatabase db_b((root_b / ".dftindex").string());
        {
            auto w = db_b.begin_write();
            w->init_schema();
            w->commit();
        }

        SstArtifactRegistry registry;
        {
            IndexDatabaseSstWriterContext sst(staging.string(), "batch_0");
            f.populate(sst, file_id);
            registry.append(sst.commit());
        }

        CHECK(registry.metadata().size() == 1);
        CHECK(registry.checkpoints().size() == 1);
        CHECK(registry.manifest().size() == 1);

        db_b.bulk_ingest(registry);

        // Both paths must converge on the same root summaries after an
        // explicit rebuild on each side.
        db_a.rebuild_root_summaries();
        db_b.rebuild_root_summaries();

        check_round_trip(db_a, db_b, file_id);
        check_root_summaries(db_a, db_b);
    }

    TEST_CASE("bulk_ingest composes across multiple disjoint batches") {
        auto root_a = dft_utils_test::make_unique_test_path("sst_multi_db_a");
        auto root_b = dft_utils_test::make_unique_test_path("sst_multi_db_b");
        auto staging =
            dft_utils_test::make_unique_test_path("sst_multi_staging");
        fs::create_directories(root_a);
        fs::create_directories(root_b);
        fs::create_directories(staging);

        Fixture f1;
        Fixture f2;
        // Vary the second fixture so the comparison covers distinct data.
        f2.read_lines = {7, 11, 13};
        f2.write_lines = {4};
        f2.pids = {201, 202};

        IndexDatabase db_a((root_a / ".dftindex").string());
        {
            auto w = db_a.begin_write();
            w->init_schema();
            f1.populate(*w, /*file_id=*/1);
            f2.populate(*w, /*file_id=*/2);
            w->commit();
        }

        IndexDatabase db_b((root_b / ".dftindex").string());
        {
            auto w = db_b.begin_write();
            w->init_schema();
            w->commit();
        }

        SstArtifactRegistry registry;
        {
            IndexDatabaseSstWriterContext sst(staging.string(), "worker_0");
            f1.populate(sst, /*file_id=*/1);
            registry.append(sst.commit());
        }
        {
            IndexDatabaseSstWriterContext sst(staging.string(), "worker_1");
            f2.populate(sst, /*file_id=*/2);
            registry.append(sst.commit());
        }

        CHECK(registry.metadata().size() == 2);
        CHECK(registry.checkpoints().size() == 2);
        CHECK(registry.manifest().size() == 2);

        db_b.bulk_ingest(registry);

        db_a.rebuild_root_summaries();
        db_b.rebuild_root_summaries();

        check_round_trip(db_a, db_b, 1);
        check_round_trip(db_a, db_b, 2);
        check_root_summaries(db_a, db_b);
    }
}
