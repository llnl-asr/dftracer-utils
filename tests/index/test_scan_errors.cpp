#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/cache/lru.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace rdb = dftracer::utils::index::store;
namespace layout = dftracer::utils::index::store::layout;
namespace pk = dftracer::utils::index::store::layout::path_kind;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace cache = dftracer::utils::index::cache;

namespace {

constexpr std::size_t DAMAGED_BYTES = 16;

struct Store {
    std::string path;
    std::map<std::string, std::vector<fs::path>> ssts;
};

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name)
        : path(fs::temp_directory_path() / ("dftu_scan_errors_" + name + "_" +
                                            std::to_string(::getpid()))) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string index() const { return (path / ".dftindex").string(); }
};

// Writes `populate`'s keys plus one key past every other key into each listed
// family, then compacts the family into SSTs. The high key makes any seek land
// in the file.
Store make_store(
    const std::string& path, const std::vector<std::string_view>& cfs,
    const std::function<void(rdb::RocksDatabase&)>& populate = {}) {
    Store store;
    store.path = path;
    rdb::RocksDatabase db(path, rdb::RocksDatabase::OpenMode::ReadWrite);
    REQUIRE(db.is_open());
    if (populate) populate(db);
    for (const auto cf : cfs)
        REQUIRE(db.put(std::string(32, '\xFF'), "v", cf).ok());
    for (const auto cf : cfs) REQUIRE(db.compact(cf).ok());
    std::vector<::rocksdb::LiveFileMetaData> files;
    db.get()->GetLiveFilesMetaData(&files);
    for (const auto& f : files)
        store.ssts[f.column_family_name].push_back(fs::path(path) /
                                                   f.name.substr(1));
    db.close();
    for (const auto cf : cfs)
        REQUIRE_FALSE(store.ssts[std::string(cf)].empty());
    return store;
}

// Breaks the first data block, so the file still opens and the first read of
// its entries fails with a checksum error.
void damage(const Store& store, std::string_view cf) {
    for (const auto& f : store.ssts.at(std::string(cf))) {
        std::fstream out(f, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(out.is_open());
        const std::string zeros(DAMAGED_BYTES, '\0');
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
}

std::unique_ptr<rdb::RocksDatabase> open_db(const Store& store,
                                            rdb::RocksDatabase::OpenMode mode) {
    rdb::RocksDBManager::instance().reset(store.path);
    auto db = std::make_unique<rdb::RocksDatabase>(store.path, mode);
    REQUIRE(db->is_open());
    return db;
}

std::string with_u64(std::string key, std::uint64_t value) {
    append_u64_be(key, value);
    return key;
}

}  // namespace

TEST_CASE("a damaged SST ends a raw iterator with an error status") {
    TempDir dir("raw");
    const auto store = make_store(dir.index(), {rdb::cf::DEFAULT});
    damage(store, rdb::cf::DEFAULT);
    auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadWrite);

    auto it = db->new_iterator(rdb::cf::DEFAULT);
    it->SeekToFirst();
    CHECK_FALSE(it->Valid());
    CHECK_FALSE(it->status().ok());
}

TEST_CASE("index database readers report a damaged store") {
    TempDir dir("index_db");
    using Ext = layout::Ext;
    const std::string_view file_cf =
        layout::family_name(layout::family_of(Ext::Zonemap, pk::FILE));
    const auto store = make_store(
        dir.index(),
        {rdb::cf::DEFAULT, rdb::cf::MEMBERS, rdb::cf::GRANULE, rdb::cf::BLOB,
         rdb::cf::POSTINGS},
        [&](rdb::RocksDatabase& db) {
            REQUIRE(db.put(layout::manifest_key(1, Ext::Stats), "v").ok());
            REQUIRE(db.put(layout::plugin_manifest_key(1, "p"), "v").ok());
            REQUIRE(db.put(layout::file_by_path_key("a"), "v").ok());
            REQUIRE(db.put(layout::format_key(), "v").ok());
            REQUIRE(db.put(layout::file_prefix(Ext::Zonemap, pk::FILE, 1) + "a",
                           "v", file_cf)
                        .ok());
            REQUIRE(db.put(with_u64(layout::path_prefix(Ext::Postings,
                                                        pk::IN_FILE, 1, "a"),
                                    7),
                           "v", rdb::cf::POSTINGS)
                        .ok());
        });
    for (const auto cf : {rdb::cf::DEFAULT, rdb::cf::MEMBERS, rdb::cf::GRANULE,
                          rdb::cf::BLOB, rdb::cf::POSTINGS})
        damage(store, cf);
    rdb::RocksDBManager::instance().reset(store.path);
    rdb::IndexDatabase idx(dir.index(), rdb::IndexOpenMode::ReadOnly);

    CHECK_THROWS_AS(idx.extension_state(1, Ext::Stats), DFTUtilsException);
    CHECK_THROWS_AS(idx.plugin_extension_state(1, "p"), DFTUtilsException);
    CHECK_THROWS_AS(idx.plugin_extension_states(1), DFTUtilsException);
    CHECK_THROWS_AS(idx.extension_paths(1, Ext::Zonemap), DFTUtilsException);
    CHECK_THROWS_AS(idx.path_granules(1, Ext::Zonemap, "a"), DFTUtilsException);
    CHECK_THROWS_AS(idx.path_file_value(1, Ext::Zonemap, "a"),
                    DFTUtilsException);
    CHECK_THROWS_AS(idx.has_postings(1, "a"), DFTUtilsException);
    CHECK_THROWS_AS(idx.has_posting(1, "a", 7), DFTUtilsException);
    CHECK_THROWS_AS(idx.posting_granules(1, "a", 7), DFTUtilsException);
    CHECK_THROWS_AS(idx.get_file_info_id("a"), DFTUtilsException);
    CHECK_THROWS_AS(idx.get_format_version(), DFTUtilsException);
    CHECK_THROWS_AS(idx.query_all_file_registry(), DFTUtilsException);
    CHECK_THROWS_AS(idx.query_all_file_info_ids(), DFTUtilsException);
    CHECK_THROWS_AS(idx.find_stale_files({"a"}), DFTUtilsException);
}

TEST_CASE("opening an index reports a damaged registry") {
    TempDir dir("init_default");
    const auto store = make_store(dir.index(), {rdb::cf::DEFAULT});
    damage(store, rdb::cf::DEFAULT);
    rdb::RocksDBManager::instance().reset(store.path);
    CHECK_THROWS_AS(
        rdb::IndexDatabase(dir.index(), rdb::IndexOpenMode::ReadWrite),
        DFTUtilsException);
}

TEST_CASE("opening an index reports a damaged legacy rollup family") {
    TempDir dir("init_rollup");
    const auto store = make_store(dir.index(), {rdb::cf::ROLLUP});
    damage(store, rdb::cf::ROLLUP);
    rdb::RocksDBManager::instance().reset(store.path);
    CHECK_THROWS_AS(
        rdb::IndexDatabase(dir.index(), rdb::IndexOpenMode::ReadWrite),
        DFTUtilsException);
}

TEST_CASE("aggregation tier readers report a damaged store") {
    SUBCASE("system metrics rows") {
        TempDir dir("agg_system");
        const auto store = make_store(dir.index(), {rdb::cf::SYSTEM_METRICS});
        damage(store, rdb::cf::SYSTEM_METRICS);
        auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadOnly);
        CHECK_THROWS_AS(
            agg::tier::for_each_system_row(
                *db, 0, agg::AGG_KEY_NUM_SHARDS,
                [](std::string_view, std::string_view) { return true; }),
            DFTUtilsException);
    }
    SUBCASE("partials, dictionary and records") {
        TempDir dir("agg_blob");
        const auto store = make_store(
            dir.index(), {rdb::cf::BLOB}, [](rdb::RocksDatabase& db) {
                agg::AggInternTable table;
                REQUIRE(table.intern.insert_at_id(3, "name") ==
                        dstd::StringIntern::InsertStatus::Ok);
                agg::tier::flush_dictionary(db, table);
                agg::tier::widen_time_bounds(db, {1, 2});
                agg::tier::write_tracker(db, "t");
                agg::tier::write_config(db, agg::tier::Config{});
            });
        damage(store, rdb::cf::BLOB);
        auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadOnly);
        CHECK_THROWS_AS(
            agg::tier::for_each_partial(*db, 1, [](auto&&...) { return true; }),
            DFTUtilsException);
        agg::AggInternTable table;
        CHECK_THROWS_AS(agg::tier::load_dictionary(*db, table),
                        DFTUtilsException);
        CHECK_THROWS_AS(agg::tier::read_dictionary_entry(*db, 3),
                        DFTUtilsException);
        CHECK_THROWS_AS(agg::tier::read_time_bounds(*db), DFTUtilsException);
        CHECK_THROWS_AS(agg::tier::read_tracker(*db), DFTUtilsException);
        CHECK_THROWS_AS(agg::tier::read_config(*db), DFTUtilsException);
    }
    SUBCASE("clearing the tier") {
        TempDir dir("agg_clear");
        const auto store = make_store(dir.index(), {rdb::cf::DEFAULT});
        damage(store, rdb::cf::DEFAULT);
        auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadWrite);
        CHECK_THROWS_AS(agg::tier::clear(*db), DFTUtilsException);
    }
}

TEST_CASE("rollup and lookup stores report a damaged store") {
    TempDir dir("rollup");
    const auto store =
        make_store(dir.index(), {rdb::cf::ROLLUP}, [](rdb::RocksDatabase& db) {
            REQUIRE(
                db.put(cache::rollup_desc_key(5), "d", rdb::cf::ROLLUP).ok());
            cache::persist_lookup(db, 5, "value");
        });
    damage(store, rdb::cf::ROLLUP);

    SUBCASE("lookups") {
        auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadOnly);
        CHECK_THROWS_AS(cache::rollup_exists(*db, 5), DFTUtilsException);
        CHECK_THROWS_AS(cache::read_rollup(*db, 5), DFTUtilsException);
        CHECK_THROWS_AS(cache::lookup_exists(*db, 5), DFTUtilsException);
        CHECK_THROWS_AS(cache::read_lookup(*db, 5), DFTUtilsException);

        trace::views::detail::ViewPlan plan;
        plan.record_schema = std::make_shared<index::RecordSchema>();
        CHECK_THROWS_AS(cache::find_subsuming_rollup(*db, plan),
                        DFTUtilsException);
    }
    SUBCASE("usage records") {
        auto db = open_db(store, rdb::RocksDatabase::OpenMode::ReadWrite);
        CHECK_THROWS_AS(cache::touch_usage(*db, rdb::cf::ROLLUP, 5),
                        DFTUtilsException);
        CHECK_THROWS_AS(
            cache::evict_lru(*db, rdb::cf::ROLLUP, 5,
                             [](rdb::RocksDatabase::Batch&, std::uint64_t) {}),
            DFTUtilsException);
    }
}

TEST_CASE("merging a shard set reports a damaged file registry") {
    TempDir dir("shards");
    const std::string shard = (dir.path / "s0" / ".dftindex").string();
    {
        rdb::IndexDatabase idx(shard, rdb::IndexOpenMode::ReadWrite);
        auto write = idx.begin_write();
        layout::FileRecord record;
        record.file_id = 1;
        record.size = 10;
        rdb::records::put_file_record(*write, "a.pfw.gz", record);
        write->commit();
    }
    rdb::RocksDBManager::instance().reset(shard);
    // Not write_shard_set: reading the registry would cache the data block that
    // the damage below has to reach.
    rdb::IndexShardManifest manifest;
    manifest.format_version = rdb::IndexDatabase::FORMAT_VERSION;
    manifest.shards.push_back({"s0/.dftindex", 1, 1, 1, 0});
    rdb::write_shard_manifest(dir.path.string(), manifest);
    rdb::RocksDBManager::instance().reset(shard);

    const auto store = make_store(shard, {rdb::cf::DEFAULT});
    damage(store, rdb::cf::DEFAULT);
    rdb::RocksDBManager::instance().reset(shard);

    const std::string out = (dir.path / "merged").string();
    CHECK_THROWS_AS(trace::views::merge_shard_set(dir.path.string(), out),
                    DFTUtilsException);
}
