#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using dftracer::utils::index::schemas::dft::ChunkStatistics;
using dftracer::utils::index::store::ColumnType;
using dftracer::utils::index::store::IndexDatabase;
namespace layout = dftracer::utils::index::store::layout;

namespace {

void register_files(IndexDatabase& db, const std::vector<std::string>& paths) {
    namespace internal = dftracer::utils::index::store::internal;
    auto w = db.begin_write();
    for (const auto& p : paths)
        dftu_utils_test::register_test_file(
            *w, internal::get_logical_path(p), internal::calculate_file_hash(p),
            static_cast<std::uint64_t>(internal::get_file_modification_time(p)),
            internal::file_size_bytes(p));
    w->commit();
}

void set_format_version(IndexDatabase& db, std::uint32_t version) {
    std::string body;
    layout::append_u32(body, version);
    db.db()->put(
        layout::format_key(),
        layout::with_header(layout::Ext::HOST, layout::host::FORMAT, body));
}

}  // namespace

TEST_SUITE("IndexDatabase") {
    TEST_CASE("normalizes legacy .idx-style input to root-local .dftindex") {
        auto root = dftu_utils_test::make_unique_test_path("idx_root");
        fs::create_directories(root);
        auto legacy_like = (root / "trace.pfw.gz.idx").string();

        IndexDatabase db(legacy_like);
        CHECK(fs::exists(root / ".dftindex"));
    }

    TEST_CASE("file registry is shared within one .dftindex root") {
        auto root = dftu_utils_test::make_unique_test_path("idx_shared");
        fs::create_directories(root);

        IndexDatabase db1((root / ".dftindex").string());
        IndexDatabase db2((root / "other-name.idx").string());

        {
            auto writer = db1.begin_write();
            writer->init_schema();
            writer->commit();
        }
        {
            auto writer = db2.begin_write();
            writer->init_schema();
            writer->commit();
        }

        const std::string p = (root / "a.pfw.gz").string();
        int id1;
        {
            auto writer = db1.begin_write();
            id1 = dftu_utils_test::register_test_file(*writer, p, 0x1111);
            writer->commit();
        }
        int id2 = db2.get_file_info_id(p);

        CHECK(id1 > 0);
        CHECK(id1 == id2);
    }

    TEST_CASE("clearing an extension removes its data and manifest entry") {
        auto root = dftu_utils_test::make_unique_test_path("idx_rebuild");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();

            file_id = dftu_utils_test::register_test_file(
                *writer, "trace.pfw.gz", 0xAAAA);

            std::vector<unsigned char> blob = {0xDE, 0xAD, 0xBE, 0xEF};
            dftu_utils_test::put_chunk_bloom(*writer, file_id, 0, "name",
                                             std::span(blob), 4);
            dftu_utils_test::put_file_bloom(*writer, file_id, "name",
                                            std::span(blob), 4);
            dftu_utils_test::index_records::put_path(
                *writer, dftu_utils_test::IndexExtension::BLOOM, file_id,
                "name");
            dftu_utils_test::index_records::put_rowset(*writer, file_id,
                                                       "files", "frame");
            dftu_utils_test::index_records::put_manifest(
                *writer, file_id,
                dftracer::utils::index::store::IndexExtension::ROWSET, 0);
            dftu_utils_test::mark_built(*writer, file_id);
            writer->commit();
        }

        CHECK(db.extension_current(
            file_id, dftracer::utils::index::store::IndexExtension::BLOOM));
        CHECK(dftu_utils_test::has_file_bloom(db, file_id, "name"));
        CHECK(db.rowset(file_id, "files") == "frame");

        int rebuilt_id;
        {
            auto writer = db.begin_write();
            rebuilt_id = dftu_utils_test::register_test_file(
                *writer, "trace.pfw.gz", 0xBBBB);
            dftu_utils_test::index_records::clear_file(
                *writer, dftracer::utils::index::store::IndexExtension::BLOOM,
                rebuilt_id);
            writer->commit();
        }
        CHECK(rebuilt_id == file_id);

        CHECK_FALSE(db.extension_current(
            file_id, dftracer::utils::index::store::IndexExtension::BLOOM));
        CHECK_FALSE(dftu_utils_test::has_file_bloom(db, file_id, "name"));
        CHECK(dftu_utils_test::bloom_chunks(db, file_id, "name") == 0);
        CHECK(db.rowset(file_id, "files") == "frame");
    }

    TEST_CASE("writer context batches multiple files and all are readable") {
        auto root = dftu_utils_test::make_unique_test_path("idx_writer_ctx");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();

        static constexpr int NUM_FILES = 100;
        static constexpr int BATCH_SIZE = 10;

        // Create file IDs first
        std::vector<int> file_ids;
        {
            auto writer = db.begin_write();
            for (int i = 0; i < NUM_FILES; ++i) {
                auto name = "file_" + std::to_string(i) + ".pfw.gz";
                int fid =
                    dftu_utils_test::register_test_file(*writer, name, i + 1);
                file_ids.push_back(fid);
            }
            writer->commit();
        }
        CHECK(file_ids.size() == NUM_FILES);

        // Write scalar stats in batches
        for (int batch_start = 0; batch_start < NUM_FILES;
             batch_start += BATCH_SIZE) {
            auto writer = db.begin_write();
            int batch_end = std::min(batch_start + BATCH_SIZE, NUM_FILES);
            for (int i = batch_start; i < batch_end; ++i) {
                ChunkStatistics stats;
                stats.total_events = static_cast<std::uint64_t>(i + 1) * 100;
                stats.min_nonzero_timestamp_us =
                    static_cast<std::uint64_t>(i + 1) * 7;
                dftu_utils_test::index_records::put_file_scalar_stats(
                    *writer, file_ids[i], stats, 1);
            }
            writer->commit();
        }

        // Verify ALL data is readable
        auto results = db.query_file_scalar_stats_batch(file_ids);
        CHECK(results.size() == NUM_FILES);

        std::uint64_t total_events = 0;
        for (int i = 0; i < NUM_FILES; ++i) {
            auto it = results.find(file_ids[i]);
            REQUIRE(it != results.end());
            CHECK(it->second.stats.total_events ==
                  static_cast<std::uint64_t>(i + 1) * 100);
            CHECK(it->second.stats.min_nonzero_timestamp_us ==
                  static_cast<std::uint64_t>(i + 1) * 7);
            total_events += it->second.stats.total_events;
        }
        CHECK(total_events == 505000);  // sum of 100+200+...+10000
    }

    // query_file_pids projects the distinct PIDs from the file's pid:tid count
    // map (FILE_PID_TID_COUNTS, written by the bloom/stats pass); same pid
    // across tids collapses to one.
    TEST_CASE("PID query - distinct PIDs from pid:tid counts") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_single");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = dftu_utils_test::register_test_file(
                *writer, "trace.pfw.gz", 0xAAAA);

            dftracer::utils::StringViewMap<std::uint64_t> counts;
            counts.emplace("1234:1", 3);
            counts.emplace("1234:2", 1);  // same pid, different tid
            counts.emplace("5678:1", 2);
            counts.emplace("9012:7", 5);
            dftu_utils_test::index_records::put_file_pid_tid_counts(
                *writer, file_id, counts);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(1234) == 1);
        CHECK(result.count(5678) == 1);
        CHECK(result.count(9012) == 1);
    }

    TEST_CASE("PID query - non-existent file returns empty set") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_empty");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();

        auto result = db.query_file_pids(999);
        CHECK(result.empty());
    }

    TEST_CASE("PID query - all file PIDs") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_all");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id1, file_id2, file_id3;
        {
            auto writer = db.begin_write();
            writer->init_schema();

            file_id1 = dftu_utils_test::register_test_file(
                *writer, "trace1.pfw.gz", 0xAAA1);
            file_id2 = dftu_utils_test::register_test_file(
                *writer, "trace2.pfw.gz", 0xAAA2);
            file_id3 = dftu_utils_test::register_test_file(
                *writer, "trace3.pfw.gz", 0xAAA3);

            auto pid_tid = [](std::initializer_list<const char*> keys) {
                dftracer::utils::StringViewMap<std::uint64_t> m;
                for (const char* k : keys) m.emplace(k, 1);
                return m;
            };
            dftu_utils_test::index_records::put_file_pid_tid_counts(
                *writer, file_id1, pid_tid({"1000:1", "1001:1"}));
            dftu_utils_test::index_records::put_file_pid_tid_counts(
                *writer, file_id2, pid_tid({"1000:1", "2000:1", "2001:1"}));
            dftu_utils_test::index_records::put_file_pid_tid_counts(
                *writer, file_id3, pid_tid({"3000:1"}));
            writer->commit();
        }

        auto all_pids = db.query_all_file_pids();
        CHECK(all_pids.size() == 3);

        CHECK(all_pids[file_id1].size() == 2);
        CHECK(all_pids[file_id1].count(1000) == 1);
        CHECK(all_pids[file_id1].count(1001) == 1);

        CHECK(all_pids[file_id2].size() == 3);
        CHECK(all_pids[file_id2].count(1000) == 1);
        CHECK(all_pids[file_id2].count(2000) == 1);
        CHECK(all_pids[file_id2].count(2001) == 1);

        CHECK(all_pids[file_id3].size() == 1);
        CHECK(all_pids[file_id3].count(3000) == 1);
    }

    TEST_CASE("PID query - large PIDs") {
        auto root = dftu_utils_test::make_unique_test_path("idx_pid_large");
        fs::create_directories(root);

        IndexDatabase db((root / ".dftindex").string());

        int file_id;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            file_id = dftu_utils_test::register_test_file(
                *writer, "trace.pfw.gz", 0xBBBB);

            dftracer::utils::StringViewMap<std::uint64_t> counts;
            counts.emplace("4294967295:1", 1);            // 32-bit max
            counts.emplace("4294967296:1", 1);            // just over 32-bit
            counts.emplace("18446744073709551615:1", 1);  // 64-bit max
            dftu_utils_test::index_records::put_file_pid_tid_counts(
                *writer, file_id, counts);
            writer->commit();
        }

        auto result = db.query_file_pids(file_id);
        CHECK(result.size() == 3);
        CHECK(result.count(0xFFFFFFFFULL) == 1);
        CHECK(result.count(0x100000000ULL) == 1);
        CHECK(result.count(0xFFFFFFFFFFFFFFFFULL) == 1);
    }

    TEST_CASE("columns come from the path catalog of every file") {
        using dftracer::utils::index::store::PathStat;
        using dftracer::utils::index::store::PathType;
        auto root = dftu_utils_test::make_unique_test_path("idx_columns");
        fs::create_directories(root);
        IndexDatabase db((root / ".dftindex").string());

        CHECK(db.query_all_columns().empty());

        auto stat = [](std::initializer_list<PathType> types,
                       std::uint64_t count) {
            PathStat s;
            for (auto t : types) {
                s.type = dftracer::utils::index::store::join(s.type, t);
                s.seen |=
                    static_cast<std::uint8_t>(1U << static_cast<unsigned>(t));
            }
            s.count = count;
            return s;
        };
        int f1, f2;
        {
            auto writer = db.begin_write();
            writer->init_schema();
            f1 = dftu_utils_test::register_test_file(*writer, "a.pfw.gz",
                                                     0x1111);
            f2 = dftu_utils_test::register_test_file(*writer, "b.pfw.gz",
                                                     0x2222);
            namespace rec = dftu_utils_test::index_records;
            rec::put_catalog_path(*writer, f1, "cat",
                                  stat({PathType::STRING}, 5));
            rec::put_catalog_path(*writer, f1, "args.mhost",
                                  stat({PathType::INT, PathType::BOOL}, 5));
            rec::put_catalog_path(*writer, f2, "args.mhost",
                                  stat({PathType::DOUBLE}, 2));
            rec::put_catalog_path(*writer, f2, "args.fhash",
                                  stat({PathType::STRING}, 2));
            rec::put_catalog_path(*writer, f2, "args.io.off",
                                  stat({PathType::UINT}, 1));
            rec::put_catalog_path(*writer, f2, "args.gone",
                                  stat({PathType::NULL_VALUE}, 0));
            writer->commit();
        }

        CHECK(db.query_all_columns() ==
              std::vector<std::string>{"cat", "fhash", "io.off", "mhost"});
        using C = std::pair<std::string, ColumnType>;
        CHECK(db.query_all_column_types() ==
              std::vector<C>{{"cat", ColumnType::String},
                             {"fhash", ColumnType::String},
                             {"io.off", ColumnType::Float64},
                             {"mhost", ColumnType::Float64}});

        auto one = db.catalog(f1);
        REQUIRE(one.size() == 2);
        CHECK(one[0].first == "args.mhost");
        CHECK(one[0].second.type == PathType::MIXED);
        CHECK(one[0].second.count == 5);
        CHECK(one[1].first == "cat");
    }
}

namespace {

std::string write_file(const fs::path& path, std::string_view contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    return path.string();
}

}  // namespace

TEST_SUITE("IndexDatabase staleness") {
    TEST_CASE("get_file_stat round-trips stored mtime and size") {
        auto root = dftu_utils_test::make_unique_test_path("stale_stat");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "hello world");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        auto stat = db.get_file_stat(a);
        REQUIRE(stat.has_value());
        CHECK(stat->size == fs::file_size(a));
        CHECK(stat->mtime != 0);
    }

    TEST_CASE("fresh index reports nothing stale") {
        auto root = dftu_utils_test::make_unique_test_path("stale_fresh");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbbbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a, b});

        auto result = db.find_stale_files({a, b});
        CHECK_FALSE(result.stale());
        CHECK(result.changed.empty());
        CHECK(result.added.empty());
        CHECK(result.removed.empty());
    }

    TEST_CASE("size change is detected as changed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_size");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "original");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        write_file(root / "a.pfw", "original plus more bytes");

        auto result = db.find_stale_files({a});
        CHECK(result.stale());
        REQUIRE(result.changed.size() == 1);
        CHECK(result.changed[0] == a);
    }

    TEST_CASE("mtime change with same size is detected as changed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_mtime");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "same-size-content");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        auto bumped = fs::last_write_time(a) + std::chrono::hours(48);
        fs::last_write_time(a, bumped);

        auto result = db.find_stale_files({a});
        REQUIRE(result.changed.size() == 1);
        CHECK(result.changed[0] == a);
    }

    TEST_CASE("newly added file is reported as added") {
        auto root = dftu_utils_test::make_unique_test_path("stale_added");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        auto b = write_file(root / "b.pfw", "bbb");
        auto result = db.find_stale_files({a, b});
        CHECK(result.changed.empty());
        REQUIRE(result.added.size() == 1);
        CHECK(result.added[0] == b);
    }

    TEST_CASE("file removed from disk is reported as removed") {
        auto root = dftu_utils_test::make_unique_test_path("stale_removed");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a, b});

        auto result = db.find_stale_files({a});
        REQUIRE(result.removed.size() == 1);
        CHECK(result.removed[0] == b);
    }

    TEST_CASE("another format version forces a full rebuild") {
        auto root = dftu_utils_test::make_unique_test_path("stale_schema");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        set_format_version(db, IndexDatabase::FORMAT_VERSION + 1);

        auto result = db.find_stale_files({a});
        CHECK(result.format_outdated);
        CHECK(result.stale());
    }

    TEST_CASE("size or mtime change makes a file stale") {
        auto root = dftu_utils_test::make_unique_test_path("fresh_stat");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "original");
        auto b = write_file(root / "b.pfw", "same-size-content");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a, b});

        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Fresh);

        write_file(root / "a.pfw", "original plus more");  // size change
        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Stale);

        auto bumped = fs::last_write_time(b) + std::chrono::hours(48);
        fs::last_write_time(b, bumped);  // mtime change, same size
        CHECK(db.check_freshness(b) == IndexDatabase::Freshness::Stale);
    }

    TEST_CASE("unregistered files and other formats are not fresh") {
        auto root = dftu_utils_test::make_unique_test_path("fresh_negatives");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");
        auto b = write_file(root / "b.pfw", "bbb");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        // Registered file with no changes is fresh; unregistered is stale.
        CHECK(db.check_freshness(a) == IndexDatabase::Freshness::Fresh);
        CHECK(db.check_freshness(b) == IndexDatabase::Freshness::Stale);

        set_format_version(db, IndexDatabase::FORMAT_VERSION + 1);
        CHECK(db.check_freshness(a) ==
              IndexDatabase::Freshness::FormatOutdated);
    }

    TEST_CASE("an index from the previous layout is outdated, not misread") {
        auto root = dftu_utils_test::make_unique_test_path("stale_v14");
        fs::create_directories(root);
        auto a = write_file(root / "a.pfw", "aaa");

        IndexDatabase db((root / ".dftindex").string());
        db.init_schema();
        register_files(db, {a});

        // The previous layout kept its version under another key, so the
        // format key is absent.
        db.db()->del(layout::format_key());

        CHECK(db.format_outdated());
        CHECK(db.check_freshness(a) ==
              IndexDatabase::Freshness::FormatOutdated);
        auto result = db.find_stale_files({a});
        CHECK(result.format_outdated);
        CHECK(result.stale());
    }
}

TEST_SUITE("IndexDatabase format") {
    TEST_CASE("opening an index of another layout does not stamp it current") {
        auto root = dftu_utils_test::make_unique_test_path("format_old");
        const auto path = (root / ".dftindex").string();
        {
            IndexDatabase db(path);
            db.db()->del(layout::format_key());
            db.db()->put("_schema_version", std::string("\x00\x00\x00\x11", 4));
        }
        dftracer::utils::index::store::RocksDBManager::instance().reset(path);
        IndexDatabase reopened(path);
        CHECK(reopened.format_outdated());
    }
}
