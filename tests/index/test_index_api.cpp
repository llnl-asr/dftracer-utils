#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace coro = dftracer::utils::coro;
namespace index = dftracer::utils::index;
namespace index_store = dftracer::utils::index::store;
namespace agg_tier = dftracer::utils::index::schemas::dft::agg::tier;

using index::FileRef;
using index::Freshness;
using index::Index;
using index::SourceStat;
using index_store::IndexDatabase;
using index_store::IndexExtension;

constexpr std::size_t CHECKPOINT = 4096;
constexpr std::string_view PLUGIN = "test_ext";

constexpr IndexExtension PATH_KEYED[] = {
    IndexExtension::Zonemap, IndexExtension::Bloom, IndexExtension::Counts,
    IndexExtension::Postings};

constexpr IndexExtension BUILT_IN[] = {
    IndexExtension::Members, IndexExtension::Zonemap,  IndexExtension::Bloom,
    IndexExtension::Counts,  IndexExtension::Postings, IndexExtension::Stats,
    IndexExtension::Catalog, IndexExtension::Profile,  IndexExtension::Metadata,
    IndexExtension::Agg,     IndexExtension::Rowset};

std::string str(dstd::StringView view) {
    return std::string(view.data(), view.size());
}

std::string str(const dstd::String& value) {
    return std::string(value.data(), value.size());
}

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

template <class T>
T take(dstd::Expected<T, dstd::Error> result) {
    REQUIRE(result.has_value());
    return std::move(*result);
}

std::string build_into(const std::string& dir,
                       const std::vector<std::string>& traces) {
    const auto index_path = dir + "/.dftindex";
    for (const auto& trace : traces)
        REQUIRE(dftu_utils_test::build_index(trace, index_path, CHECKPOINT));
    return index_path;
}

void put_plugin_extension(const std::string& index_path,
                          const std::vector<std::string>& traces) {
    namespace records = dftu_utils_test::index_records;
    IndexDatabase db(index_path);
    auto write = db.begin_write();
    for (const auto& trace : traces) {
        const int file_id = db.find_file(trace);
        REQUIRE(file_id >= 0);
        records::put_path(*write, IndexExtension::Plugin, file_id, PLUGIN);
        records::put_path_granule(*write, IndexExtension::Plugin, file_id,
                                  PLUGIN, 0, "first");
        records::put_path_granule(*write, IndexExtension::Plugin, file_id,
                                  PLUGIN, 1, "second");
        records::put_plugin_manifest(*write, file_id, PLUGIN, 3, 7);
    }
    write->commit();
}

std::vector<std::string> create_traces(dftu_utils_test::TestEnvironment& env,
                                       const std::filesystem::path& dir) {
    std::vector<std::string> traces;
    for (const char* name : {"c.pfw.gz", "a.pfw.gz", "b.pfw.gz"}) {
        const std::string created = env.create_dft_test_gzip_file(120);
        REQUIRE_FALSE(created.empty());
        const auto renamed = (dir / name).string();
        std::filesystem::rename(created, renamed);
        traces.push_back(renamed);
    }
    return traces;
}

void check_file(const IndexDatabase& db, const index::Snapshot& snapshot,
                const FileRef& file) {
    const std::string path = str(file.path());
    const int file_id = db.get_file_info_id(path);
    REQUIRE(file_id >= 0);

    const auto stat = db.get_file_stat(path);
    REQUIRE(stat.has_value());
    CHECK(file.stat().mtime == stat->mtime);
    CHECK(file.stat().size == stat->size);
    CHECK(file.stat().hash == db.get_file_hash(path).value_or(0));

    const auto old_members = db.query_gzip_members(file_id);
    const auto members = take(index::members(snapshot, file));
    REQUIRE(members.size() == old_members.size());
    CHECK(members.size() > 0);
    for (std::size_t i = 0; i < members.size(); ++i) {
        CHECK(members[i].member_idx == old_members[i].member_idx);
        CHECK(members[i].c_offset == old_members[i].c_offset);
        CHECK(members[i].c_size == old_members[i].c_size);
        CHECK(members[i].uc_offset == old_members[i].uc_offset);
        CHECK(members[i].uc_size == old_members[i].uc_size);
        CHECK(members[i].first_line_num == old_members[i].first_line_num);
        CHECK(members[i].last_line_num == old_members[i].last_line_num);
        CHECK(members[i].kind == old_members[i].kind);
        CHECK(members[i].bits == old_members[i].bits);
        const auto window = take(index::restart_window(snapshot, file, i));
        const auto old_window = db.query_restart_window(file_id, i);
        CHECK(window.has_value() == old_window.has_value());
        if (window && old_window) CHECK(str(*window) == *old_window);
    }

    const auto old_spans = db.query_chunk_spans(file_id);
    const auto spans = take(index::spans(snapshot, file));
    REQUIRE(spans.size() == old_spans.size());
    for (std::size_t i = 0; i < spans.size(); ++i) {
        CHECK(spans[i].uc_offset == old_spans[i].uc_offset);
        CHECK(spans[i].uc_size == old_spans[i].uc_size);
        CHECK(spans[i].first_line_num == old_spans[i].first_line_num);
        CHECK(spans[i].last_line_num == old_spans[i].last_line_num);
    }

    const std::vector<int> ids{file_id};
    const auto old_meta = db.query_file_metadata_batch(ids);
    const auto meta = take(index::file_metadata(snapshot, file));
    REQUIRE(meta.has_value());
    REQUIRE(old_meta.count(file_id) == 1);
    CHECK(meta->checkpoint_size == old_meta.at(file_id).checkpoint_size);
    CHECK(meta->num_lines == old_meta.at(file_id).num_lines);
    CHECK(meta->max_bytes == old_meta.at(file_id).max_bytes);
    CHECK(meta->truncated == old_meta.at(file_id).truncated);

    const auto old_chunks = db.query_chunk_statistics(file_id);
    const auto chunks = take(index::chunk_stats(snapshot, file));
    REQUIRE(chunks.size() == old_chunks.size());
    CHECK(chunks.size() > 0);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        CHECK(chunks[i].checkpoint_idx == old_chunks[i].checkpoint_idx);
        CHECK(chunks[i].stats.total_events == old_chunks[i].stats.total_events);
        CHECK(chunks[i].stats.min_timestamp_us ==
              old_chunks[i].stats.min_timestamp_us);
        CHECK(chunks[i].stats.max_timestamp_us ==
              old_chunks[i].stats.max_timestamp_us);
        CHECK(chunks[i].stats.duration_sum_us ==
              old_chunks[i].stats.duration_sum_us);
    }

    const auto old_scalar = db.query_file_scalar_stats_batch(ids);
    const auto scalar = take(index::scalar_stats(snapshot, file));
    CHECK(scalar.has_value() == (old_scalar.count(file_id) == 1));
    if (scalar && old_scalar.count(file_id) == 1) {
        CHECK(scalar->num_chunks == old_scalar.at(file_id).num_chunks);
        CHECK(scalar->stats.total_events ==
              old_scalar.at(file_id).stats.total_events);
        CHECK(scalar->stats.duration_count ==
              old_scalar.at(file_id).stats.duration_count);
    }

    const auto old_counts = db.query_file_pid_tid_counts_batch(ids);
    const auto counts = take(index::pid_tid_counts(snapshot, file));
    CHECK(counts.has_value() == (old_counts.count(file_id) == 1));
    if (counts && old_counts.count(file_id) == 1) {
        const auto& expected = old_counts.at(file_id);
        REQUIRE(counts->size() == expected.size());
        for (const auto& [key, count] : expected) {
            const auto at = counts->find(key);
            REQUIRE(at != counts->end());
            CHECK(at->second == count);
        }
    }

    const auto old_catalog = db.catalog(file_id);
    const auto catalog = take(index::catalog(snapshot, file));
    REQUIRE(catalog.size() == old_catalog.size());
    CHECK(catalog.size() > 0);
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        CHECK(str(catalog[i].first) == old_catalog[i].first);
        CHECK(catalog[i].second.seen == old_catalog[i].second.seen);
        CHECK(catalog[i].second.count == old_catalog[i].second.count);
    }

    const auto schema = take(index::schema(snapshot, file));
    const auto old_schema = db.file_schema(file_id);
    CHECK(schema.has_value() == old_schema.has_value());
    if (schema && old_schema) CHECK(str(*schema) == *old_schema);

    const auto old_chunk_meta = db.chunk_metadata(file_id);
    const auto chunk_meta = take(index::chunk_meta(snapshot, file));
    CHECK(chunk_meta.has_value() == old_chunk_meta.has_value());
    if (chunk_meta && old_chunk_meta) {
        REQUIRE(chunk_meta->size() == old_chunk_meta->size());
        for (const auto& [chunk, metadata] : *old_chunk_meta) {
            const auto at = chunk_meta->find(chunk);
            REQUIRE(at != chunk_meta->end());
            CHECK(at->second.records == metadata.records);
            CHECK(at->second.names == metadata.names);
            CHECK(at->second.paths == metadata.paths);
        }
    }

    const auto rowset = take(index::rowset(snapshot, file, "missing"));
    const auto old_rowset = db.rowset(file_id, "missing");
    CHECK(rowset.has_value() == old_rowset.has_value());

    for (IndexExtension ext : PATH_KEYED) {
        const auto old_paths = db.extension_paths(file_id, ext);
        const auto paths = take(index::paths(snapshot, file, ext));
        REQUIRE(paths.size() == old_paths.size());
        for (std::size_t i = 0; i < paths.size(); ++i) {
            CHECK(str(paths[i]) == old_paths[i]);
            const auto old_granules =
                db.path_granules(file_id, ext, old_paths[i]);
            const auto granules =
                take(index::granules(snapshot, file, ext, old_paths[i]));
            REQUIRE(granules.size() == old_granules.size());
            for (std::size_t g = 0; g < granules.size(); ++g) {
                CHECK(granules[g].first == old_granules[g].first);
                CHECK(str(granules[g].second) == old_granules[g].second);
            }
            const auto whole =
                take(index::path_file(snapshot, file, ext, old_paths[i]));
            const auto old_whole =
                db.path_file_value(file_id, ext, old_paths[i]);
            CHECK(whole.has_value() == old_whole.has_value());
            if (whole && old_whole) CHECK(str(*whole) == *old_whole);
            if (ext == IndexExtension::Postings) {
                CHECK(take(index::has_postings(snapshot, file, old_paths[i])) ==
                      db.has_postings(file_id, old_paths[i]));
                CHECK(
                    take(index::has_posting(snapshot, file, old_paths[i], 1)) ==
                    db.has_posting(file_id, old_paths[i], 1));
                const auto posting_granules = take(
                    index::posting_granules(snapshot, file, old_paths[i], 1));
                const auto old_posting_granules =
                    db.posting_granules(file_id, old_paths[i], 1);
                CHECK(std::equal(
                    posting_granules.begin(), posting_granules.end(),
                    old_posting_granules.begin(), old_posting_granules.end()));
            }
        }
    }

    for (IndexExtension ext : BUILT_IN) {
        const auto state = db.extension_state(file_id, ext);
        const bool current = db.extension_current(file_id, ext);
        const Freshness got =
            snapshot.extension(file, ext, state ? state->params_hash : 0);
        if (current) {
            CHECK(got == Freshness::Current);
        } else {
            CHECK(got != Freshness::Current);
        }
    }

    const auto old_plugins = db.plugin_extension_states(file_id);
    const auto plugins = take(index::plugins(snapshot, file));
    REQUIRE(plugins.size() == old_plugins.size());
    CHECK(plugins.size() == 1);
    for (std::size_t i = 0; i < plugins.size(); ++i) {
        CHECK(str(plugins[i].name) == old_plugins[i].first);
        CHECK(plugins[i].version == old_plugins[i].second.version);
        CHECK(plugins[i].params_hash == old_plugins[i].second.params_hash);
        CHECK(plugins[i].ready == old_plugins[i].second.ready);
    }
    const auto data = take(index::plugin(snapshot, file, PLUGIN));
    REQUIRE(data.has_value());
    const auto old_plugin_granules =
        db.path_granules(file_id, IndexExtension::Plugin, PLUGIN);
    REQUIRE(data->granules.size() == old_plugin_granules.size());
    for (std::size_t i = 0; i < data->granules.size(); ++i) {
        CHECK(data->granules[i].first == old_plugin_granules[i].first);
        CHECK(str(data->granules[i].second) == old_plugin_granules[i].second);
    }
    CHECK_FALSE(data->file.has_value());
    CHECK_FALSE(take(index::plugin(snapshot, file, "absent")).has_value());
}

SourceStat stat_of(std::uint64_t mtime, std::uint64_t size,
                   std::uint64_t hash = 0) {
    SourceStat stat;
    stat.mtime = mtime;
    stat.size = size;
    stat.hash = hash;
    return stat;
}

index_store::IndexExtension MEMBERS = IndexExtension::Members;

void put_members(index::FileBuild& build, std::uint64_t count) {
    index::put_file_metadata(build, 4096, 10 * count, 100 * count, false);
    for (std::uint64_t i = 0; i < count; ++i) {
        dftracer::utils::index::gzip::GzipMemberRecord member;
        member.member_idx = i;
        member.c_offset = i * 50;
        member.c_size = 50;
        member.uc_offset = i * 100;
        member.uc_size = 100;
        member.first_line_num = i * 10 + 1;
        member.last_line_num = i * 10 + 10;
        index::put_gzip_member(build, member);
    }
    build.finish(MEMBERS, 11);
}

void put_stats(index::FileBuild& build, std::uint64_t chunks) {
    for (std::uint64_t i = 0; i < chunks; ++i) {
        dftracer::utils::index::schemas::dft::ChunkStatistics stats;
        stats.total_events = 5 + i;
        stats.min_timestamp_us = 100 * i;
        stats.max_timestamp_us = 100 * i + 50;
        index::put_chunk_statistics(build, i, stats);
    }
    build.finish(IndexExtension::Stats, 12);
}

void put_catalog(index::FileBuild& build) {
    index_store::PathStat stat;
    stat.type = index_store::PathType::Int;
    stat.seen = 1U << static_cast<unsigned>(index_store::PathType::Int);
    stat.count = 7;
    index::put_catalog_path(build, "args.size", stat);
    build.finish(IndexExtension::Catalog, 13);
}

}  // namespace

TEST_CASE("the index api reads what IndexDatabase reads") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::filesystem::path root(env.get_dir());
    std::filesystem::create_directories(root);
    const auto traces = create_traces(env, root);
    const auto index_path = build_into(root.string(), traces);
    put_plugin_extension(index_path, traces);

    IndexDatabase db(index_path, index_store::IndexOpenMode::ReadOnly);
    auto opened = Index::open(index_path);
    REQUIRE(opened.has_value());
    const auto snapshot = take(opened->snapshot());

    const auto files = take(snapshot.files());
    REQUIRE(files.size() == 3);
    std::vector<std::string> paths;
    for (const FileRef& file : files) paths.push_back(str(file.path()));
    CHECK(std::is_sorted(paths.begin(), paths.end()));
    std::vector<std::string> registered;
    for (const auto& [path, entry] : db.query_all_file_registry())
        registered.push_back(std::string(path));
    std::sort(registered.begin(), registered.end());
    CHECK(paths == registered);

    for (const FileRef& file : files) {
        CHECK(snapshot.file(file.path()).has_value());
        check_file(db, snapshot, file);
    }
    CHECK_FALSE(snapshot.file("/no/such/file").has_value());

    for (const auto& trace : traces) {
        dftracer::utils::InputFileStat st;
        REQUIRE(dftracer::utils::stat_input(trace, st));
        const std::string logical =
            index_store::internal::get_logical_path(trace);
        const bool fresh =
            db.check_freshness(trace) == IndexDatabase::Freshness::Fresh;
        const Freshness got = snapshot.freshness(
            logical, stat_of(static_cast<std::uint64_t>(st.mtime), st.size));
        CHECK(fresh);
        CHECK(got == Freshness::Current);
        CHECK(snapshot.freshness(logical, stat_of(st.mtime + 1, st.size)) ==
              Freshness::Stale);
    }
}

TEST_CASE("a committed FileBuild is visible whole and only after the commit") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const std::string path = "/traces/one.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    const auto before = take(opened->snapshot());
    CHECK_FALSE(before.file(path).has_value());

    auto build = take(opened->build(path, stat));
    put_members(build, 2);
    put_stats(build, 3);
    put_catalog(build);

    const auto during = take(opened->snapshot());
    CHECK_FALSE(during.file(path).has_value());
    CHECK(during.freshness(path, stat) == Freshness::Missing);

    REQUIRE(run(build.commit()).has_value());

    const auto after = take(opened->snapshot());
    const auto file = after.file(path);
    REQUIRE(file.has_value());
    CHECK(file->stat().mtime == stat.mtime);
    CHECK(file->stat().size == stat.size);
    CHECK(file->stat().hash == stat.hash);
    CHECK(take(index::members(after, *file)).size() == 2);
    CHECK(take(index::spans(after, *file)).size() == 2);
    CHECK(take(index::chunk_stats(after, *file)).size() == 3);
    CHECK(take(index::catalog(after, *file)).size() == 1);
    CHECK(after.extension(*file, IndexExtension::Members, 11) ==
          Freshness::Current);
    CHECK(after.extension(*file, IndexExtension::Stats, 12) ==
          Freshness::Current);
    CHECK(after.extension(*file, IndexExtension::Catalog, 13) ==
          Freshness::Current);
    CHECK(after.extension(*file, IndexExtension::Stats, 99) ==
          Freshness::Stale);
    CHECK(after.extension(*file, IndexExtension::Zonemap, 0) ==
          Freshness::Missing);

    CHECK_FALSE(before.file(path).has_value());
    CHECK_FALSE(during.file(path).has_value());
    CHECK(take(opened->snapshot()).files()->size() == 1);

    SUBCASE("a snapshot taken before a rebuild keeps the old data") {
        const SourceStat changed = stat_of(2000, 5000, 10);
        auto rebuild = take(opened->build(path, changed));
        put_members(rebuild, 1);
        put_stats(rebuild, 1);
        REQUIRE(run(rebuild.commit()).has_value());

        const auto latest = take(opened->snapshot());
        const auto fresh = latest.file(path);
        REQUIRE(fresh.has_value());
        CHECK(fresh->stat().mtime == changed.mtime);
        CHECK(take(index::members(latest, *fresh)).size() == 1);
        CHECK(take(index::chunk_stats(latest, *fresh)).size() == 1);
        CHECK(latest.freshness(path, changed) == Freshness::Current);
        CHECK(latest.freshness(path, stat) == Freshness::Stale);
        CHECK(latest.extension(*fresh, IndexExtension::Catalog, 13) ==
              Freshness::Stale);

        CHECK(take(index::members(after, *file)).size() == 2);
        CHECK(take(index::chunk_stats(after, *file)).size() == 3);
        CHECK(after.freshness(path, stat) == Freshness::Current);
    }

    SUBCASE("remove drops every extension") {
        REQUIRE(run(opened->remove(path)).has_value());
        const auto gone = take(opened->snapshot());
        CHECK_FALSE(gone.file(path).has_value());
        CHECK(gone.freshness(path, stat) == Freshness::Missing);
        CHECK(take(gone.files()).empty());
        CHECK(after.file(path).has_value());
        REQUIRE(run(opened->remove(path)).has_value());
        REQUIRE(run(opened->remove("/traces/unknown.pfw.gz")).has_value());
    }
}

TEST_CASE("a FileBuild that is never committed leaves no trace") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string path = "/traces/dropped.pfw.gz";
    {
        auto build = take(opened->build(path, stat_of(1, 2)));
        put_members(build, 2);
        put_stats(build, 2);
    }
    const auto snapshot = take(opened->snapshot());
    CHECK_FALSE(snapshot.file(path).has_value());
    CHECK(take(snapshot.files()).empty());
    CHECK(snapshot.freshness(path, stat_of(1, 2)) == Freshness::Missing);
}

TEST_CASE("freshness reports current, stale, missing and failed") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string good = "/traces/good.pfw.gz";
    const std::string bad = "/traces/bad.pfw.gz";
    const SourceStat stat = stat_of(500, 800);

    auto build = take(opened->build(good, stat));
    put_members(build, 1);
    index::fail_extension(build, IndexExtension::Stats, 12);
    REQUIRE(run(build.commit()).has_value());

    auto failing = take(opened->build(bad, stat));
    index::fail_extension(failing, IndexExtension::Members, 11);
    REQUIRE(run(failing.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    CHECK(snapshot.freshness(good, stat) == Freshness::Current);
    CHECK(snapshot.freshness(good, stat_of(501, 800)) == Freshness::Stale);
    CHECK(snapshot.freshness(good, stat_of(500, 801)) == Freshness::Stale);
    CHECK(snapshot.freshness("/traces/none.pfw.gz", stat) ==
          Freshness::Missing);
    CHECK(snapshot.freshness(bad, stat) == Freshness::Failed);

    const auto file = snapshot.file(good);
    REQUIRE(file.has_value());
    CHECK(snapshot.extension(*file, IndexExtension::Stats, 12) ==
          Freshness::Failed);
    CHECK(snapshot.extension(*file, IndexExtension::Members, 11) ==
          Freshness::Current);
    CHECK(snapshot.extension(*file, IndexExtension::Plugin, 0) ==
          Freshness::Missing);
}

TEST_CASE("a FileBuild with an unfinished extension does not commit") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string path = "/traces/unfinished.pfw.gz";
    auto build = take(opened->build(path, stat_of(1, 2)));
    put_members(build, 1);
    index::put_chunk_statistics(build, 0, {});
    CHECK_FALSE(run(build.commit()).has_value());
    CHECK_FALSE(take(opened->snapshot()).file(path).has_value());
}

TEST_CASE("a FileBuild writes the data IndexDatabase reads") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const std::string path = "/traces/written.pfw.gz";
    const SourceStat stat = stat_of(70, 90, 5);

    auto build = take(opened->build(path, stat));
    put_members(build, 3);
    index::put_restart_window(build, 1, "window");
    put_stats(build, 2);
    put_catalog(build);
    index::put_profile(build, "schema-x");
    build.finish(IndexExtension::Profile, 14);
    index::put_path(build, IndexExtension::Zonemap, "ts");
    index::put_path_granule(build, IndexExtension::Zonemap, "ts", 0, "zone0");
    index::put_path_granule(build, IndexExtension::Zonemap, "ts", 1, "zone1");
    build.finish(IndexExtension::Zonemap, 15);
    index::put_path(build, IndexExtension::Postings, "name");
    index::put_posting(build, "name", 42);
    index::put_posting_granule(build, "name", 42, 3);
    build.finish(IndexExtension::Postings, 16);
    index::put_plugin(build, PLUGIN, 3, 7,
                      {{0, dstd::String("a")}, {1, dstd::String("b")}},
                      dstd::String("whole"));
    REQUIRE(run(build.commit()).has_value());

    IndexDatabase db(root, index_store::IndexOpenMode::ReadOnly);
    const int file_id = db.get_file_info_id(path);
    REQUIRE(file_id >= 0);
    CHECK(db.get_file_hash(path).value_or(0) == 5);
    CHECK(db.query_gzip_members(file_id).size() == 3);
    CHECK(db.query_restart_window(file_id, 1).value_or("") == "window");
    CHECK(db.query_chunk_statistics(file_id).size() == 2);
    CHECK(db.catalog(file_id).size() == 1);
    CHECK(db.file_schema(file_id).value_or("") == "schema-x");
    CHECK(db.extension_paths(file_id, IndexExtension::Zonemap).size() == 1);
    CHECK(db.path_granules(file_id, IndexExtension::Zonemap, "ts").size() == 2);
    CHECK(db.has_posting(file_id, "name", 42));
    CHECK(db.posting_granules(file_id, "name", 42).size() == 1);
    const auto state = db.plugin_extension_state(file_id, PLUGIN);
    REQUIRE(state.has_value());
    CHECK(state->version == 3);
    CHECK(state->params_hash == 7);
    CHECK(state->ready);
    CHECK(db.path_file_value(file_id, IndexExtension::Plugin, PLUGIN)
              .value_or("") == "whole");

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK(take(index::schema(snapshot, *file)).value_or("") == "schema-x");
    CHECK(take(index::has_posting(snapshot, *file, "name", 42)));
    const auto data = take(index::plugin(snapshot, *file, PLUGIN));
    REQUIRE(data.has_value());
    CHECK(data->granules.size() == 2);
    CHECK(data->file.value_or("") == "whole");
}

namespace {

void put_zonemap(index::FileBuild& build, std::string_view path,
                 std::uint64_t granules, std::uint64_t params_hash) {
    index::put_path(build, IndexExtension::Zonemap, path);
    for (std::uint64_t g = 0; g < granules; ++g) {
        index::put_path_granule(build, IndexExtension::Zonemap, path, g,
                                "zone");
    }
    build.finish(IndexExtension::Zonemap, params_hash);
}

std::vector<std::string> zonemap_paths(const index::Snapshot& snapshot,
                                       const FileRef& file) {
    std::vector<std::string> out;
    for (const auto& path :
         take(index::paths(snapshot, file, IndexExtension::Zonemap)))
        out.push_back(str(path));
    return out;
}

}  // namespace

TEST_CASE("a rebuild keeps the extensions it neither writes nor clears") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string path = "/traces/tier.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    auto first = take(opened->build(path, stat));
    put_members(first, 2);
    put_stats(first, 3);
    put_catalog(first);
    REQUIRE(run(first.commit()).has_value());

    SUBCASE("a build of one extension leaves the others") {
        auto tier = take(opened->build(path, stat));
        put_zonemap(tier, "ts", 2, 15);
        REQUIRE(run(tier.commit()).has_value());

        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        CHECK(snapshot.extension(*file, IndexExtension::Members, 11) ==
              Freshness::Current);
        CHECK(snapshot.extension(*file, IndexExtension::Stats, 12) ==
              Freshness::Current);
        CHECK(snapshot.extension(*file, IndexExtension::Catalog, 13) ==
              Freshness::Current);
        CHECK(snapshot.extension(*file, IndexExtension::Zonemap, 15) ==
              Freshness::Current);
        CHECK(take(index::members(snapshot, *file)).size() == 2);
        CHECK(take(index::chunk_stats(snapshot, *file)).size() == 3);
        CHECK(take(index::catalog(snapshot, *file)).size() == 1);
    }

    SUBCASE("a full rebuild with clears replaces the extensions") {
        const SourceStat changed = stat_of(2000, 5000, 10);
        auto full = take(opened->build(path, changed));
        put_members(full, 1);
        full.clear(IndexExtension::Catalog);
        full.clear(IndexExtension::Zonemap);
        REQUIRE(run(full.commit()).has_value());

        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        CHECK(file->stat().mtime == changed.mtime);
        CHECK(take(index::members(snapshot, *file)).size() == 1);
        CHECK(snapshot.extension(*file, IndexExtension::Catalog, 13) ==
              Freshness::Missing);
        CHECK(take(index::catalog(snapshot, *file)).empty());
        CHECK(snapshot.extension(*file, IndexExtension::Zonemap, 15) ==
              Freshness::Missing);
        CHECK(snapshot.extension(*file, IndexExtension::Stats, 12) ==
              Freshness::Stale);
    }

    SUBCASE("the clear lands in the record of the commit") {
        const auto before = take(opened->snapshot());
        auto rebuild = take(opened->build(path, stat));
        put_stats(rebuild, 1);
        rebuild.clear(IndexExtension::Catalog);
        REQUIRE(run(rebuild.commit()).has_value());

        const auto before_file = before.file(path);
        REQUIRE(before_file.has_value());
        CHECK(take(index::catalog(before, *before_file)).size() == 1);
        CHECK(take(index::chunk_stats(before, *before_file)).size() == 3);
        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        CHECK(take(index::catalog(after, *file)).empty());
        CHECK(take(index::chunk_stats(after, *file)).size() == 1);
        CHECK(after.extension(*file, IndexExtension::Catalog, 13) ==
              Freshness::Missing);
    }

    SUBCASE("clearing an extension that is written is an error") {
        auto written_first = take(opened->build(path, stat));
        put_stats(written_first, 1);
        written_first.clear(IndexExtension::Stats);
        CHECK_FALSE(run(written_first.commit()).has_value());

        auto cleared_first = take(opened->build(path, stat));
        cleared_first.clear(IndexExtension::Stats);
        put_stats(cleared_first, 1);
        CHECK_FALSE(run(cleared_first.commit()).has_value());

        auto plugin = take(opened->build(path, stat));
        plugin.clear(IndexExtension::Plugin);
        CHECK_FALSE(run(plugin.commit()).has_value());

        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        CHECK(take(index::chunk_stats(snapshot, *file)).size() == 3);
    }
}

TEST_CASE("staging commits the batches so far and publishes nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string path = "/traces/staged.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);
    constexpr std::uint64_t ROUNDS = 4;
    constexpr std::uint64_t PER_ROUND = 60;

    auto old_build = take(opened->build(path, stat));
    put_members(old_build, 1);
    put_stats(old_build, 3);
    REQUIRE(run(old_build.commit()).has_value());

    SUBCASE("a staged rebuild is invisible until commit and then whole") {
        auto build = take(opened->build(path, stat));
        for (std::uint64_t round = 0; round < ROUNDS; ++round) {
            for (std::uint64_t i = 0; i < PER_ROUND; ++i) {
                dftracer::utils::index::schemas::dft::ChunkStatistics stats;
                stats.total_events = 1 + i;
                index::put_chunk_statistics(build, round * PER_ROUND + i,
                                            stats);
            }
            REQUIRE(run(index::stage(build)).has_value());
            const auto during = take(opened->snapshot());
            const auto file = during.file(path);
            REQUIRE(file.has_value());
            CHECK(take(index::chunk_stats(during, *file)).size() == 3);
        }
        REQUIRE(run(index::stage(build)).has_value());
        build.finish(IndexExtension::Stats, 12);
        REQUIRE(run(build.commit()).has_value());

        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        const auto rows = take(index::chunk_stats(after, *file));
        CHECK(rows.size() == ROUNDS * PER_ROUND);
        CHECK(take(index::members(after, *file)).size() == 1);
    }

    SUBCASE("puts after a stage join the same copy") {
        auto build = take(opened->build(path, stat));
        put_zonemap(build, "ts", 3, 15);
        REQUIRE(run(index::stage(build)).has_value());
        index::put_path(build, IndexExtension::Zonemap, "dur");
        index::put_path_granule(build, IndexExtension::Zonemap, "dur", 0, "z");
        REQUIRE(run(build.commit()).has_value());

        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        CHECK(zonemap_paths(after, *file) ==
              std::vector<std::string>{"dur", "ts"});
        CHECK(take(index::granules(after, *file, IndexExtension::Zonemap, "ts"))
                  .size() == 3);
        CHECK(
            take(index::granules(after, *file, IndexExtension::Zonemap, "dur"))
                .size() == 1);
    }

    SUBCASE("a staged build that fails an extension leaves no half copy") {
        auto build = take(opened->build(path, stat));
        put_zonemap(build, "ts", 3, 15);
        REQUIRE(run(index::stage(build)).has_value());
        index::fail_extension(build, IndexExtension::Zonemap, 15);
        REQUIRE(run(build.commit()).has_value());

        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        CHECK(after.extension(*file, IndexExtension::Zonemap, 15) ==
              Freshness::Failed);
        CHECK(zonemap_paths(after, *file).empty());
    }

    SUBCASE("an abandoned staged build leaves the old copy") {
        {
            auto build = take(opened->build(path, stat));
            for (std::uint64_t i = 0; i < PER_ROUND; ++i)
                index::put_chunk_statistics(build, i, {});
            REQUIRE(run(index::stage(build)).has_value());
        }
        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        CHECK(take(index::chunk_stats(after, *file)).size() == 3);
    }
}

TEST_CASE("agg meta is replaced and deleted in one record at a time") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());

    std::string config;
    for (const std::uint64_t word : {std::uint64_t{60}, std::uint64_t{7}}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            config.push_back(static_cast<char>((word >> shift) & 0xFF));
    }
    config.push_back(0);

    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
    REQUIRE(run(index::put_agg_meta(*opened, "config", config)).has_value());

    IndexDatabase db(root, index_store::IndexOpenMode::ReadOnly);
    auto read = agg_tier::read_config(db);
    REQUIRE(read.has_value());
    CHECK(read->time_interval_us == 60);
    CHECK(read->params_hash == 7);
    CHECK_FALSE(read->group_by_file);

    config[15] = 9;
    config[16] = 1;
    REQUIRE(run(index::put_agg_meta(*opened, "config", config)).has_value());
    IndexDatabase again(root, index_store::IndexOpenMode::ReadOnly);
    read = agg_tier::read_config(again);
    REQUIRE(read.has_value());
    CHECK(read->params_hash == 9);
    CHECK(read->group_by_file);

    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
    IndexDatabase gone(root, index_store::IndexOpenMode::ReadOnly);
    CHECK_FALSE(agg_tier::read_config(gone).has_value());
    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
}

TEST_CASE("remove_extension drops one extension of many files in one record") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const SourceStat stat = stat_of(1000, 4000, 9);
    const std::vector<std::string> paths{"/traces/a.pfw.gz", "/traces/b.pfw.gz",
                                         "/traces/c.pfw.gz"};
    for (const std::string& path : paths) {
        auto build = take(opened->build(path, stat));
        put_members(build, 1);
        put_zonemap(build, "ts", 2, 15);
        index::put_path(build, IndexExtension::Bloom, "name");
        build.finish(IndexExtension::Bloom, 16);
        REQUIRE(run(build.commit()).has_value());
    }
    const auto before = take(opened->snapshot());

    const std::vector<dstd::StringView> targets{
        dstd::StringView(paths[0].data(), paths[0].size()),
        dstd::StringView(paths[2].data(), paths[2].size()),
        dstd::StringView("/traces/unknown.pfw.gz")};
    REQUIRE(
        run(index::remove_extension(*opened, IndexExtension::Zonemap, targets))
            .has_value());
    CHECK_FALSE(
        run(index::remove_extension(*opened, IndexExtension::Plugin, targets))
            .has_value());
    REQUIRE(run(index::remove_extension(*opened, IndexExtension::Zonemap, {}))
                .has_value());

    const auto after = take(opened->snapshot());
    for (const std::string& path : paths) {
        const auto old_file = before.file(path);
        REQUIRE(old_file.has_value());
        CHECK(zonemap_paths(before, *old_file).size() == 1);
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        const bool removed = path != paths[1];
        CHECK(after.extension(*file, IndexExtension::Zonemap, 15) ==
              (removed ? Freshness::Missing : Freshness::Current));
        CHECK(zonemap_paths(after, *file).size() == (removed ? 0U : 1U));
        CHECK(after.extension(*file, IndexExtension::Bloom, 16) ==
              Freshness::Current);
        CHECK(after.extension(*file, IndexExtension::Members, 11) ==
              Freshness::Current);
    }
}

TEST_CASE("an old-API write after a FileBuild commit is not skipped") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    namespace records = dftu_utils_test::index_records;
    const std::string root = env.get_dir() + "/.dftindex";
    const std::string path = "/traces/co.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);
    IndexDatabase db(root);
    int file_id = -1;

    const auto old_write = [&](std::string_view name, std::uint64_t params,
                               bool register_file) {
        auto write = db.begin_write();
        if (register_file) {
            file_id = dftu_utils_test::register_test_file(
                *write, path, stat.hash, stat.mtime, stat.size);
        }
        records::put_path(*write, IndexExtension::Zonemap, file_id, name);
        records::put_path_granule(*write, IndexExtension::Zonemap, file_id,
                                  name, 0, "old");
        records::put_manifest(*write, file_id, IndexExtension::Zonemap, params);
        write->commit();
    };

    old_write("a", 5, true);
    REQUIRE(file_id >= 0);
    CHECK(db.extension_paths(file_id, IndexExtension::Zonemap) ==
          std::vector<std::string>{"a"});

    auto opened = Index::open(root);
    REQUIRE(opened.has_value());

    SUBCASE("a rebuild with other params") {
        auto build = take(opened->build(path, stat));
        put_zonemap(build, "b", 2, 6);
        REQUIRE(run(build.commit()).has_value());
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap) ==
              std::vector<std::string>{"b"});

        old_write("c", 5, false);
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap) ==
              std::vector<std::string>{"c"});
        CHECK(db.path_granules(file_id, IndexExtension::Zonemap, "b").empty());
        CHECK(db.path_granules(file_id, IndexExtension::Zonemap, "c").size() ==
              1);
    }

    SUBCASE("a clear") {
        auto build = take(opened->build(path, stat));
        build.clear(IndexExtension::Zonemap);
        REQUIRE(run(build.commit()).has_value());
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap).empty());

        old_write("c", 5, false);
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap) ==
              std::vector<std::string>{"c"});
        CHECK(db.path_granules(file_id, IndexExtension::Zonemap, "c").size() ==
              1);
    }

    SUBCASE("a remove_extension") {
        const dstd::StringView target(path.data(), path.size());
        REQUIRE(run(index::remove_extension(*opened, IndexExtension::Zonemap,
                                            {&target, 1}))
                    .has_value());
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap).empty());

        old_write("c", 5, false);
        CHECK(db.extension_paths(file_id, IndexExtension::Zonemap) ==
              std::vector<std::string>{"c"});
    }
}
