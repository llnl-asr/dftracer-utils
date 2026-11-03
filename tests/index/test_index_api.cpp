// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/file_group.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

namespace dstd = dftracer::utils::dstd;
namespace index = dftracer::utils::index;
namespace index_store = dftracer::utils::index::store;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace agg_tier = dftracer::utils::index::schemas::dft::agg::tier;
namespace kv = dftracer::utils::store;

using index::FileRef;
using index::Freshness;
using index::Index;
using index::SourceStat;
using index_store::IndexExtension;

constexpr std::size_t CHECKPOINT = 4096;
constexpr dstd::StringView PLUGIN = "test_ext";

constexpr IndexExtension PATH_KEYED[] = {
    IndexExtension::Zonemap, IndexExtension::Bloom, IndexExtension::Counts,
    IndexExtension::Postings};

constexpr IndexExtension BUILT_IN[] = {
    IndexExtension::Members, IndexExtension::Zonemap,  IndexExtension::Bloom,
    IndexExtension::Counts,  IndexExtension::Postings, IndexExtension::Stats,
    IndexExtension::Catalog, IndexExtension::Profile,  IndexExtension::Metadata,
    IndexExtension::Agg,     IndexExtension::Rowset};

dstd::String str(dstd::StringView view) {
    return dstd::String(view.data(), view.size());
}

dstd::String str(const dstd::String& value) {
    return dstd::String(value.data(), value.size());
}

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

// Stages a build the way a spill task does: it claims the build, stages the
// puts it made, and gives the build up.
void stage(index::FileBuild& build) {
    auto claim =
        dftracer::utils::default_runtime().submit(index::claim(build)).get();
    REQUIRE(claim);
    REQUIRE(run(claim->stage()).has_value());
}

template <class T>
T take(dstd::Expected<T, dstd::Error> result) {
    REQUIRE(result.has_value());
    return std::move(*result);
}

void ok(dstd::Expected<void, dstd::Error> result) {
    REQUIRE(result.has_value());
}

dstd::String build_into(const dstd::String& dir,
                        const dstd::Vector<dstd::String>& traces) {
    const auto index_path = dir + "/.dftindex";
    for (const auto& trace : traces)
        REQUIRE(dftu_utils_test::build_index(
            std::string(trace), std::string(index_path), CHECKPOINT));
    return index_path;
}

void put_plugin_extension(const dstd::String& index_path,
                          const dstd::Vector<dstd::String>& traces) {
    auto opened = Index::open(index_path);
    REQUIRE(opened.has_value());
    for (const auto& trace : traces) {
        const dstd::String key = index_store::internal::get_logical_path(trace);
        auto build = take(index::rebuild(*opened, key));
        index::put_plugin(
            build, PLUGIN, 3, 7,
            {{0, dstd::String("first")}, {1, dstd::String("second")}},
            dstd::nullopt);
        ok(run(build.commit()));
    }
}

dstd::Vector<dstd::String> create_traces(
    dftu_utils_test::TestEnvironment& env,
    const dftracer::utils::dstd::Path& dir) {
    dstd::Vector<dstd::String> traces;
    for (const char* name : {"c.pfw.gz", "a.pfw.gz", "b.pfw.gz"}) {
        const std::string created = env.create_dft_test_gzip_file(120);
        REQUIRE_FALSE(created.empty());
        const dstd::String renamed((dir / name).str());
        REQUIRE(
            dftracer::utils::dstd::fs::rename(created, renamed).has_value());
        traces.push_back(renamed);
    }
    return traces;
}

struct PartialRow {
    std::uint64_t chunk = 0;
    std::uint32_t cat = 0;
    std::uint32_t name = 0;
    std::uint32_t host = 0;
    std::uint32_t extra = 0;
    agg::PartialKind kind = agg::PartialKind::Event;
    dstd::String bytes;
};

dstd::Vector<PartialRow> partials(const index::Snapshot& snapshot,
                                  const FileRef& file) {
    dstd::Vector<PartialRow> rows;
    ok(index::for_each_agg_partial(
        snapshot, file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra, agg::PartialKind kind,
            dstd::Span<const std::uint8_t> bytes) {
            rows.push_back(
                {chunk, cat, name, host, extra, kind,
                 dstd::String(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size())});
            return true;
        }));
    return rows;
}

using Blocks = dstd::Vector<dstd::Pair<std::uint64_t, agg::ChunkColumns>>;

Blocks blocks(const index::Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<dstd::Pair<std::uint64_t, agg::ChunkColumns>> out;
    ok(index::for_each_agg_chunk_columns(
        snapshot, file,
        [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            out.emplace_back(chunk, block);
            return true;
        }));
    return out;
}

void check_block(const agg::ChunkColumns& got,
                 const agg::ChunkColumns& expected) {
    CHECK(got.chunk == expected.chunk);
    CHECK(got.rows == expected.rows);
    REQUIRE(got.ids.size() == expected.ids.size());
    REQUIRE(got.texts.size() == expected.texts.size());
    for (std::size_t i = 0; i < got.ids.size(); ++i)
        CHECK(got.ids[i] == expected.ids[i]);
    for (std::size_t i = 0; i < got.texts.size(); ++i)
        CHECK(str(got.texts[i]) == str(expected.texts[i]));
    CHECK(got.cat == expected.cat);
    CHECK(got.name == expected.name);
    CHECK(got.host == expected.host);
    CHECK(got.file == expected.file);
    CHECK(got.ts == expected.ts);
    CHECK(got.dur == expected.dur);
    CHECK(got.has_dur == expected.has_dur);
    CHECK(got.pid == expected.pid);
    CHECK(got.tid == expected.tid);
    CHECK(got.phase == expected.phase);
    REQUIRE(got.columns.size() == expected.columns.size());
    for (std::size_t i = 0; i < got.columns.size(); ++i) {
        CHECK(got.columns[i].path == expected.columns[i].path);
        CHECK(got.columns[i].kind == expected.columns[i].kind);
        CHECK(got.columns[i].rows == expected.columns[i].rows);
        CHECK(got.columns[i].bits == expected.columns[i].bits);
    }
}

void check_file(const index::Snapshot& snapshot, const FileRef& file,
                bool has_plugin) {
    const auto hash = index::file_hash(snapshot, file);
    REQUIRE(hash.has_value());
    CHECK(file.stat().hash == *hash);

    const auto types = take(index::file_column_types(snapshot, file));
    CHECK_FALSE(types.empty());

    const auto members = take(index::members(snapshot, file));
    CHECK(members.size() > 0);
    for (std::size_t i = 0; i < members.size(); ++i)
        CHECK(members[i].member_idx == i);

    const auto spans = take(index::spans(snapshot, file));
    REQUIRE(spans.size() == members.size());
    for (std::size_t i = 0; i < spans.size(); ++i) {
        CHECK(spans[i].uc_offset == members[i].uc_offset);
        CHECK(spans[i].uc_size == members[i].uc_size);
        CHECK(spans[i].first_line_num == members[i].first_line_num);
        CHECK(spans[i].last_line_num == members[i].last_line_num);
    }

    const auto meta = take(index::file_metadata(snapshot, file));
    REQUIRE(meta.has_value());
    CHECK(meta->num_lines > 0);

    const auto chunks = take(index::chunk_stats(snapshot, file));
    CHECK(chunks.size() > 0);
    for (std::size_t i = 0; i < chunks.size(); ++i)
        CHECK(chunks[i].checkpoint_idx == i);

    const auto scalar = take(index::scalar_stats(snapshot, file));
    if (scalar) CHECK(scalar->stats.total_events > 0);

    const auto catalog = take(index::catalog(snapshot, file));
    CHECK(catalog.size() > 0);

    CHECK_FALSE(take(index::rowset(snapshot, file, "missing")).has_value());

    dstd::HashMap<std::uint32_t, dstd::String> texts;
    ok(index::agg_file_texts(snapshot, file, texts));

    const auto stored_blocks = blocks(snapshot, file);
    for (const auto& [chunk, block] : stored_blocks) {
        const auto one = take(index::agg_chunk_columns(snapshot, file, chunk));
        REQUIRE(one.has_value());
        check_block(*one, block);
        REQUIRE(block.texts.size() == block.ids.size());
        for (std::size_t i = 0; i < block.ids.size(); ++i) {
            const auto at = texts.find(block.ids[i]);
            CHECK(str(block.texts[i]) ==
                  (at == texts.end() ? dstd::String() : str(at->second)));
        }
    }
    const std::uint64_t absent_chunk =
        stored_blocks.empty() ? 0 : stored_blocks.back().first + 1000;
    CHECK_FALSE(take(index::agg_chunk_columns(snapshot, file, absent_chunk))
                    .has_value());

    for (IndexExtension ext : PATH_KEYED) {
        const auto paths = take(index::paths(snapshot, file, ext));
        CHECK(std::is_sorted(paths.begin(), paths.end()));
        for (const auto& path : paths) {
            const auto granules =
                take(index::granules(snapshot, file, ext, path));
            CHECK(std::is_sorted(granules.begin(), granules.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.first < b.first;
                                 }));
            if (ext == IndexExtension::Postings) {
                const auto posting_granules =
                    take(index::posting_granules(snapshot, file, path, 1));
                CHECK(take(index::has_posting(snapshot, file, path, 1)) ==
                      !posting_granules.empty());
            }
        }
    }

    for (IndexExtension ext : BUILT_IN) {
        const auto typed = index::extension_state(snapshot, file, ext);
        const bool current = index::extension_current(snapshot, file, ext);
        if (current) {
            REQUIRE(typed.has_value());
            CHECK(typed->ready);
        }
        const Freshness got =
            snapshot.extension(file, ext, typed ? typed->params_hash : 0);
        if (current) {
            CHECK(got == Freshness::Current);
        } else {
            CHECK(got != Freshness::Current);
        }
    }

    CHECK_FALSE(index::extension_state(snapshot, file, IndexExtension::Plugin));

    if (!has_plugin) return;
    const auto plugins = take(index::plugins(snapshot, file));
    REQUIRE(plugins.size() == 1);
    CHECK(str(plugins[0].name) == PLUGIN);
    CHECK(plugins[0].version == 3);
    CHECK(plugins[0].params_hash == 7);
    CHECK(plugins[0].ready);
    const auto data = take(index::plugin(snapshot, file, PLUGIN));
    REQUIRE(data.has_value());
    REQUIRE(data->granules.size() == 2);
    CHECK(str(data->granules[0].second) == "first");
    CHECK(str(data->granules[1].second) == "second");
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

TEST_CASE("the index api reads a built index and a plugin extension") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const dftracer::utils::dstd::Path root(env.get_dir());
    REQUIRE(dftracer::utils::dstd::fs::create_directories(root).has_value());
    const auto traces = create_traces(env, root);
    const auto index_path = build_into(dstd::String(root.str()), traces);
    put_plugin_extension(index_path, traces);

    auto opened = Index::open(index_path);
    REQUIRE(opened.has_value());
    const auto snapshot = take(opened->snapshot());

    const auto files = take(snapshot.files());
    REQUIRE(files.size() == 3);
    dstd::Vector<dstd::String> paths;
    for (const FileRef& file : files) paths.push_back(str(file.path()));
    CHECK(std::is_sorted(paths.begin(), paths.end()));
    dstd::Vector<dstd::String> expected;
    for (const auto& trace : traces)
        expected.push_back(index_store::internal::get_logical_path(trace));
    std::sort(expected.begin(), expected.end());
    CHECK(paths == expected);

    for (const FileRef& file : files) {
        CHECK(snapshot.file(file.path()).has_value());
        check_file(snapshot, file, true);
    }
    CHECK_FALSE(snapshot.file("/no/such/file").has_value());

    for (const auto& trace : traces) {
        dftracer::utils::common::InputFileStat st;
        REQUIRE(dftracer::utils::common::stat_input(trace, st));
        const dstd::String logical =
            index_store::internal::get_logical_path(trace);
        const Freshness got = snapshot.freshness(
            logical, stat_of(static_cast<std::uint64_t>(st.mtime), st.size));
        CHECK(got == Freshness::Current);
        CHECK(snapshot.freshness(logical, stat_of(st.mtime + 1, st.size)) ==
              Freshness::Stale);
    }
}

TEST_CASE("the typed agg readers read what the aggregation store reads") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const dftracer::utils::dstd::Path root(env.get_dir());
    REQUIRE(dftracer::utils::dstd::fs::create_directories(root).has_value());
    const auto traces = create_traces(env, root);

    index::IndexerOptions options;
    options.index_dir = root.str();
    options.checkpoint_size = CHECKPOINT;
    options.parallelism = 4;
    agg::AggregationConfig config;
    config.time_interval_us = 100000;
    options.aggregation = config;
    auto indexer = index::Indexer::open(traces, options);
    indexer.build();

    const dstd::String index_path = dstd::String((root / ".dftindex").str());
    auto opened = Index::open(index_path);
    REQUIRE(opened.has_value());
    const auto snapshot = take(opened->snapshot());

    auto dictionary = agg::make_intern_table();
    ok(index::agg_dictionary(snapshot, *dictionary));
    CHECK(dictionary->intern.size() > 0);

    const auto config_read = take(agg_tier::read_config(snapshot));
    REQUIRE(config_read.has_value());
    CHECK(config_read->params_hash == config.params_hash());
    CHECK(config_read->time_interval_us == config.time_interval_us);
    CHECK(config_read->group_by_file);

    const auto bounds = take(agg_tier::read_time_bounds(snapshot));
    REQUIRE(bounds.has_value());
    CHECK(bounds->min_bucket <= bounds->max_bucket);

    const auto raw_config = take(index::agg_meta(snapshot, "config"));
    REQUIRE(raw_config.has_value());
    CHECK_FALSE(raw_config->empty());
    CHECK_FALSE(take(index::agg_meta(snapshot, "absent")).has_value());

    const auto files = take(snapshot.files());
    REQUIRE(files.size() == traces.size());
    std::size_t block_count = 0;
    std::size_t partial_count = 0;
    for (const FileRef& file : files) {
        check_file(snapshot, file, false);
        partial_count += partials(snapshot, file).size();

        dstd::HashMap<std::uint32_t, dstd::String> texts;
        ok(index::agg_file_texts(snapshot, file, texts));
        REQUIRE(texts.size() > 0);
        for (const auto& [id, text] : texts)
            CHECK(dstd::String(dictionary->intern.resolve(id)) == str(text));
        ok(index::for_each_agg_chunk_columns(
            snapshot, file, [&](std::uint64_t, const agg::ChunkColumns&) {
                ++block_count;
                return true;
            }));
    }
    CHECK(block_count >= files.size());
    CHECK(partial_count >= files.size());
    CHECK_FALSE(snapshot.file("/no/such/file").has_value());
}

TEST_CASE("a copy at another format version is not current") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/versioned.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    auto build = take(opened->build(path, stat));
    put_members(build, 1);
    REQUIRE(run(build.commit()).has_value());

    auto store = kv::Store::open(root + "/store");
    REQUIRE(store.has_value());
    kv::NamespaceSpec spec;
    const dstd::String name =
        index_store::store_layout::namespace_name(IndexExtension::Profile);
    spec.name.assign(name.data(), name.size());
    spec.format_version =
        index_store::layout::ext_version(IndexExtension::Profile) + 1;
    auto ns = store->ns(spec);
    REQUIRE(ns.has_value());
    const kv::ScopeKey scope = kv::ScopeKey::file(path);
    kv::Batch batch = store->batch(*ns, scope);
    batch.put(dstd::ByteView("\x02\x00", 2), dstd::ByteView());
    REQUIRE(run(store->commit(std::move(batch))).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK_FALSE(
        index::extension_current(snapshot, *file, IndexExtension::Profile));
    CHECK(index::extension_current(snapshot, *file, IndexExtension::Members));
    CHECK(snapshot.extension(*file, IndexExtension::Profile, 0) !=
          Freshness::Current);
    CHECK_FALSE(
        index::extension_state(snapshot, *file, IndexExtension::Profile));
    CHECK(index::extension_state(snapshot, *file, IndexExtension::Members));
    CHECK_FALSE(take(index::schema(snapshot, *file)).has_value());
}

TEST_CASE("a committed FileBuild is visible whole and only after the commit") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/one.pfw.gz";
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
    const dstd::String path = "/traces/dropped.pfw.gz";
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
    const dstd::String good = "/traces/good.pfw.gz";
    const dstd::String bad = "/traces/bad.pfw.gz";
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
    const dstd::String path = "/traces/unfinished.pfw.gz";
    auto build = take(opened->build(path, stat_of(1, 2)));
    put_members(build, 1);
    index::put_chunk_statistics(build, 0, {});
    CHECK_FALSE(run(build.commit()).has_value());
    CHECK_FALSE(take(opened->snapshot()).file(path).has_value());
}

TEST_CASE("a group of FileBuilds becomes visible together") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String a = "/traces/group_a.pfw.gz";
    const dstd::String b = "/traces/group_b.pfw.gz";
    const dstd::String bad = "/traces/group_bad.pfw.gz";
    auto build_a = take(opened->build(a, stat_of(1, 2)));
    auto build_b = take(opened->build(b, stat_of(3, 4)));
    auto build_bad = take(opened->build(bad, stat_of(5, 6)));
    put_members(build_a, 1);
    put_stats(build_a, 2);
    put_members(build_b, 3);
    put_members(build_bad, 1);
    index::put_chunk_statistics(build_bad, 0, {});

    CHECK_FALSE(take(opened->snapshot()).file(a).has_value());
    index::FileBuild* group[] = {&build_a, &build_bad, &build_b};
    auto errors = dftracer::utils::default_runtime()
                      .submit(index::commit_group(
                          dstd::Span<index::FileBuild* const>(group, 3)))
                      .get();
    REQUIRE(errors.size() == 3);
    CHECK_FALSE(errors[0].has_value());
    CHECK(errors[1].has_value());
    CHECK_FALSE(errors[2].has_value());

    const auto after = take(opened->snapshot());
    const auto file_a = after.file(a);
    const auto file_b = after.file(b);
    REQUIRE(file_a.has_value());
    REQUIRE(file_b.has_value());
    CHECK(take(index::members(after, *file_a)).size() == 1);
    CHECK(take(index::members(after, *file_b)).size() == 3);
    CHECK(take(index::chunk_stats(after, *file_a)).size() == 2);
    CHECK_FALSE(after.file(bad).has_value());
}

TEST_CASE("a FileGroup publishes a repeated path in order and counts files") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    namespace internal = dftracer::utils::index::store::internal;
    const dstd::String plain = "/traces/rep.pfw.gz";
    const dstd::String dotted = "/traces/./rep.pfw.gz";
    const dstd::String other = "/traces/other.pfw.gz";
    const dstd::String bad = "/traces/bad.pfw.gz";
    const auto key = [](const dstd::String& path) {
        return internal::get_logical_path(path);
    };
    const auto run_task = [](auto task) {
        return dftracer::utils::default_runtime().submit(std::move(task)).get();
    };

    index::build::FileGroup group;
    auto first = take(opened->build(key(plain), stat_of(1, 2)));
    put_members(first, 1);
    auto second = take(opened->build(key(dotted), stat_of(1, 2)));
    put_members(second, 4);
    auto third = take(opened->build(other, stat_of(3, 4)));
    put_members(third, 2);
    auto broken = take(opened->build(bad, stat_of(5, 6)));
    put_members(broken, 1);
    index::put_chunk_statistics(broken, 0, {});

    CHECK(run_task(group.add(std::move(first), 0, key(plain))).empty());
    CHECK(run_task(group.add(std::move(second), 1, key(dotted))).empty());
    CHECK(run_task(group.add(std::move(third), 2, key(other))).empty());
    CHECK(run_task(group.add(std::move(broken), 3, bad)).empty());
    auto result = run_task(group.commit());

    REQUIRE(result.failures.size() == 1);
    CHECK(result.failures[0].tag == 3);
    CHECK(result.published == 3);
    CHECK(result.publishes == 2);
    CHECK(group.size() == 0);

    const auto after = take(opened->snapshot());
    const auto file = after.file(key(plain));
    REQUIRE(file.has_value());
    CHECK(take(index::members(after, *file)).size() == 4);
    CHECK(after.file(other).has_value());
    CHECK_FALSE(after.file(bad).has_value());
}

TEST_CASE("a FileBuild writes the data the typed readers read") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/written.pfw.gz";
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

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK(index::file_hash(snapshot, *file).value_or(0) == 5);
    CHECK(take(index::members(snapshot, *file)).size() == 3);
    CHECK(str(*take(index::restart_window(snapshot, *file, 1))) == "window");
    CHECK_FALSE(take(index::restart_window(snapshot, *file, 0)).has_value());
    CHECK(take(index::chunk_stats(snapshot, *file)).size() == 2);
    CHECK(take(index::catalog(snapshot, *file)).size() == 1);
    CHECK(take(index::schema(snapshot, *file)).value_or("") == "schema-x");
    CHECK(take(index::paths(snapshot, *file, IndexExtension::Zonemap)).size() ==
          1);
    CHECK(take(index::granules(snapshot, *file, IndexExtension::Zonemap, "ts"))
              .size() == 2);
    CHECK(take(index::has_posting(snapshot, *file, "name", 42)));
    CHECK_FALSE(take(index::has_posting(snapshot, *file, "name", 43)));
    CHECK(take(index::posting_granules(snapshot, *file, "name", 42)).size() ==
          1);
    const auto plugins = take(index::plugins(snapshot, *file));
    REQUIRE(plugins.size() == 1);
    CHECK(plugins[0].version == 3);
    CHECK(plugins[0].params_hash == 7);
    CHECK(plugins[0].ready);
    const auto data = take(index::plugin(snapshot, *file, PLUGIN));
    REQUIRE(data.has_value());
    CHECK(data->granules.size() == 2);
    CHECK(data->file.value_or("") == "whole");
    CHECK(take(index::plugin_file(snapshot, *file, PLUGIN)).value_or("") ==
          "whole");
}

namespace {

void put_zonemap(index::FileBuild& build, dstd::StringView path,
                 std::uint64_t granules, std::uint64_t params_hash) {
    index::put_path(build, IndexExtension::Zonemap, path);
    for (std::uint64_t g = 0; g < granules; ++g) {
        index::put_path_granule(build, IndexExtension::Zonemap, path, g,
                                "zone");
    }
    build.finish(IndexExtension::Zonemap, params_hash);
}

dstd::Vector<dstd::String> zonemap_paths(const index::Snapshot& snapshot,
                                         const FileRef& file) {
    dstd::Vector<dstd::String> out;
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
    const dstd::String path = "/traces/tier.pfw.gz";
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
    const dstd::String path = "/traces/staged.pfw.gz";
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
            stage(build);
            const auto during = take(opened->snapshot());
            const auto file = during.file(path);
            REQUIRE(file.has_value());
            CHECK(take(index::chunk_stats(during, *file)).size() == 3);
        }
        stage(build);
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
        stage(build);
        index::put_path(build, IndexExtension::Zonemap, "dur");
        index::put_path_granule(build, IndexExtension::Zonemap, "dur", 0, "z");
        REQUIRE(run(build.commit()).has_value());

        const auto after = take(opened->snapshot());
        const auto file = after.file(path);
        REQUIRE(file.has_value());
        CHECK(zonemap_paths(after, *file) ==
              dstd::Vector<dstd::String>{"dur", "ts"});
        CHECK(take(index::granules(after, *file, IndexExtension::Zonemap, "ts"))
                  .size() == 3);
        CHECK(
            take(index::granules(after, *file, IndexExtension::Zonemap, "dur"))
                .size() == 1);
    }

    SUBCASE("a staged build that fails an extension leaves no half copy") {
        auto build = take(opened->build(path, stat));
        put_zonemap(build, "ts", 3, 15);
        stage(build);
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
            stage(build);
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

    dstd::String config;
    for (const std::uint64_t word : {std::uint64_t{60}, std::uint64_t{7}}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            config.push_back(static_cast<char>((word >> shift) & 0xFF));
    }
    config.push_back(0);

    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
    REQUIRE(run(index::put_agg_meta(*opened, "config", config)).has_value());

    auto read = take(agg_tier::read_config(take(opened->snapshot())));
    REQUIRE(read.has_value());
    CHECK(read->time_interval_us == 60);
    CHECK(read->params_hash == 7);
    CHECK_FALSE(read->group_by_file);

    config[15] = 9;
    config[16] = 1;
    REQUIRE(run(index::put_agg_meta(*opened, "config", config)).has_value());
    read = take(agg_tier::read_config(take(opened->snapshot())));
    REQUIRE(read.has_value());
    CHECK(read->params_hash == 9);
    CHECK(read->group_by_file);

    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
    CHECK_FALSE(
        take(agg_tier::read_config(take(opened->snapshot()))).has_value());
    REQUIRE(run(index::delete_agg_meta(*opened, "config")).has_value());
}

TEST_CASE("remove_extension drops one extension of many files in one record") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const SourceStat stat = stat_of(1000, 4000, 9);
    const dstd::Vector<dstd::String> paths{
        "/traces/a.pfw.gz", "/traces/b.pfw.gz", "/traces/c.pfw.gz"};
    for (const dstd::String& path : paths) {
        auto build = take(opened->build(path, stat));
        put_members(build, 1);
        put_zonemap(build, "ts", 2, 15);
        index::put_path(build, IndexExtension::Bloom, "name");
        build.finish(IndexExtension::Bloom, 16);
        REQUIRE(run(build.commit()).has_value());
    }
    const auto before = take(opened->snapshot());

    const dstd::Vector<dstd::StringView> targets{
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
    for (const dstd::String& path : paths) {
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

TEST_CASE("a second build of an extension replaces the first copy") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/co.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    auto first = take(opened->build(path, stat));
    put_members(first, 1);
    put_zonemap(first, "a", 1, 5);
    REQUIRE(run(first.commit()).has_value());

    auto second = take(opened->build(path, stat));
    put_zonemap(second, "b", 2, 6);
    REQUIRE(run(second.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK(zonemap_paths(snapshot, *file) == dstd::Vector<dstd::String>{"b"});
    CHECK(take(index::granules(snapshot, *file, IndexExtension::Zonemap, "a"))
              .empty());
    CHECK(take(index::granules(snapshot, *file, IndexExtension::Zonemap, "b"))
              .size() == 2);
    CHECK(snapshot.extension(*file, IndexExtension::Zonemap, 6) ==
          Freshness::Current);
    CHECK(snapshot.extension(*file, IndexExtension::Zonemap, 5) ==
          Freshness::Stale);
}

TEST_CASE("commits land across many openings of one root") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    constexpr int ROUNDS = 12;
    for (int round = 0; round < ROUNDS; ++round) {
        auto opened = Index::open(root);
        REQUIRE(opened.has_value());
        const dstd::String path =
            "/traces/many_" + dstd::to_string(round) + ".pfw.gz";
        const SourceStat stat =
            stat_of(1000 + static_cast<std::uint64_t>(round), 4000, 9);
        auto build = take(opened->build(path, stat));
        put_members(build, 2);
        put_stats(build, 2);
        REQUIRE(run(build.commit()).has_value());
        CHECK(take(opened->snapshot()).file(path).has_value());
    }
    auto reopened = Index::open(root);
    REQUIRE(reopened.has_value());
    const auto snapshot = take(reopened->snapshot());
    CHECK(take(snapshot.files()).size() == ROUNDS);
}

TEST_CASE("FileBuild commits from concurrent tasks and an open reader") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    const auto reader = take(opened->snapshot());
    CHECK_FALSE(reader.file("/traces/none.pfw.gz").has_value());
    constexpr int FILES = 24;
    const auto one = [](Index index, int i) -> coro::CoroTask<bool> {
        const dstd::String path =
            "/traces/conc_" + dstd::to_string(i) + ".pfw.gz";
        auto build = index.build(path, stat_of(1000, 4000, 9));
        if (!build) co_return false;
        put_members(*build, 2);
        auto committed = co_await build->commit();
        co_return committed.has_value();
    };
    const auto all = [&]() -> coro::CoroTask<int> {
        dstd::Vector<coro::CoroTask<bool>> tasks;
        for (int i = 0; i < FILES; ++i) tasks.push_back(one(*opened, i));
        auto results = co_await coro::when_all(std::move(tasks));
        int ok = 0;
        for (bool r : results) ok += r ? 1 : 0;
        co_return ok;
    };
    for (int round = 0; round < 4; ++round) {
        CHECK(dftracer::utils::default_runtime().submit(all()).get() == FILES);
    }
    const auto after = take(opened->snapshot());
    for (int i = 0; i < FILES; ++i) {
        CHECK(after.file("/traces/conc_" + dstd::to_string(i) + ".pfw.gz")
                  .has_value());
    }
    CHECK(take(after.files()).size() == FILES);
    CHECK(take(reader.files()).empty());
}

TEST_CASE("a rebuilt extension replaces only its own copy") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    const dstd::String path = "/traces/reopen.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);
    {
        auto opened = Index::open(root);
        REQUIRE(opened.has_value());
        auto build = take(opened->build(path, stat));
        put_members(build, 2);
        index::put_path(build, IndexExtension::Bloom, "args.name");
        index::put_path_granule(build, IndexExtension::Bloom, "args.name", 0,
                                "old");
        build.finish(IndexExtension::Bloom, 1);
        REQUIRE(run(build.commit()).has_value());
    }
    auto reopened = Index::open(root);
    REQUIRE(reopened.has_value());
    auto build = take(reopened->build(path, stat));
    index::put_path(build, IndexExtension::Bloom, "args.name");
    index::put_path_granule(build, IndexExtension::Bloom, "args.name", 1,
                            "new");
    build.finish(IndexExtension::Bloom, 2);
    REQUIRE(run(build.commit()).has_value());

    const auto snapshot = take(reopened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    const auto granules = take(
        index::granules(snapshot, *file, IndexExtension::Bloom, "args.name"));
    REQUIRE(granules.size() == 1);
    CHECK(granules[0].first == 1);
    CHECK(str(granules[0].second) == "new");
    CHECK(snapshot.extension(*file, IndexExtension::Bloom, 2) ==
          Freshness::Current);
    CHECK(snapshot.extension(*file, IndexExtension::Bloom, 1) ==
          Freshness::Stale);
    CHECK(take(index::members(snapshot, *file)).size() == 2);
    CHECK(snapshot.extension(*file, IndexExtension::Members, 11) ==
          Freshness::Current);
}

TEST_CASE("rebuilding one plugin leaves the other plugin of the file whole") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/plugins.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    auto first = take(opened->build(path, stat));
    put_members(first, 1);
    index::put_plugin(first, "alpha", 3, 10, {{0, dstd::String("old")}},
                      dstd::nullopt);
    index::put_plugin(first, "beta", 7, 20,
                      {{0, dstd::String("beta0")}, {1, dstd::String("beta1")}},
                      dstd::String("beta-file"));
    REQUIRE(run(first.commit()).has_value());

    SUBCASE("a rewrite of alpha leaves beta") {
        auto again = take(opened->build(path, stat));
        index::put_plugin(again, "alpha", 3, 30, {{1, dstd::String("new")}},
                          dstd::nullopt);
        REQUIRE(run(again.commit()).has_value());

        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        const auto alpha = take(index::plugin(snapshot, *file, "alpha"));
        REQUIRE(alpha.has_value());
        REQUIRE(alpha->granules.size() == 1);
        CHECK(alpha->granules[0].first == 1);
        CHECK(str(alpha->granules[0].second) == "new");

        const auto beta = take(index::plugin(snapshot, *file, "beta"));
        REQUIRE(beta.has_value());
        CHECK(beta->granules.size() == 2);
        CHECK(beta->file.value_or("") == "beta-file");

        const auto plugins = take(index::plugins(snapshot, *file));
        REQUIRE(plugins.size() == 2);
        for (const auto& entry : plugins) {
            CHECK(entry.ready);
            if (str(entry.name) == "alpha") CHECK(entry.params_hash == 30);
            if (str(entry.name) == "beta") CHECK(entry.params_hash == 20);
        }
    }

    SUBCASE("a failed plugin reads as not ready with its params hash") {
        auto again = take(opened->build(path, stat));
        index::fail_plugin(again, "alpha", 3, 0x77);
        REQUIRE(run(again.commit()).has_value());

        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(path);
        REQUIRE(file.has_value());
        const auto plugins = take(index::plugins(snapshot, *file));
        REQUIRE(plugins.size() == 2);
        for (const auto& entry : plugins) {
            if (str(entry.name) == "alpha") {
                CHECK_FALSE(entry.ready);
                CHECK(entry.params_hash == 0x77);
            } else {
                CHECK(entry.ready);
                CHECK(entry.params_hash == 20);
            }
        }
    }
}

TEST_CASE("a finished extension without data is a current empty copy") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/empty.pfw.gz";
    auto build = take(opened->build(path, stat_of(1, 2)));
    put_members(build, 1);
    build.finish(IndexExtension::Catalog, 5);
    REQUIRE(run(build.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK(snapshot.extension(*file, IndexExtension::Catalog, 5) ==
          Freshness::Current);
    CHECK(index::extension_current(snapshot, *file, IndexExtension::Catalog));
    CHECK(take(index::catalog(snapshot, *file)).empty());
}

TEST_CASE("erasing a path of a staged copy hides only its records") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/erase.pfw.gz";
    auto build = take(opened->build(path, stat_of(1, 2)));
    put_members(build, 1);
    index::put_path(build, IndexExtension::Zonemap, "args.name");
    index::put_path_granule(build, IndexExtension::Zonemap, "args.name", 0,
                            "name-old");
    index::put_path(build, IndexExtension::Zonemap, "args.dur");
    index::put_path_granule(build, IndexExtension::Zonemap, "args.dur", 0,
                            "dur-old");
    stage(build);

    index::erase_path(build, IndexExtension::Zonemap, "args.name");
    index::put_path(build, IndexExtension::Zonemap, "args.name");
    index::put_path_granule(build, IndexExtension::Zonemap, "args.name", 1,
                            "name-new");
    build.finish(IndexExtension::Zonemap, 1);
    REQUIRE(run(build.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    CHECK(zonemap_paths(snapshot, *file) ==
          dstd::Vector<dstd::String>{"args.dur", "args.name"});
    const auto name = take(
        index::granules(snapshot, *file, IndexExtension::Zonemap, "args.name"));
    REQUIRE(name.size() == 1);
    CHECK(name[0].first == 1);
    CHECK(str(name[0].second) == "name-new");
    const auto dur = take(
        index::granules(snapshot, *file, IndexExtension::Zonemap, "args.dur"));
    REQUIRE(dur.size() == 1);
    CHECK(str(dur[0].second) == "dur-old");
}

TEST_CASE("commits from many threads all land") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    constexpr int THREADS = 4;
    constexpr int ROUNDS = 10;
    dstd::Vector<std::thread> threads;
    std::atomic<int> failed{0};
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&opened, &failed, t] {
            const dstd::String path = "/traces/t" + dstd::to_string(t);
            for (int round = 0; round < ROUNDS; ++round) {
                auto build = opened->build(path, stat_of(1, 2));
                if (!build) {
                    ++failed;
                    continue;
                }
                put_members(*build, 1);
                index::put_catalog_path(*build, "r" + dstd::to_string(round),
                                        index_store::PathStat{});
                build->finish(IndexExtension::Catalog,
                              static_cast<std::uint64_t>(round));
                if (!run(build->commit())) ++failed;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(failed.load() == 0);

    const auto snapshot = take(opened->snapshot());
    for (int t = 0; t < THREADS; ++t) {
        const auto file = snapshot.file("/traces/t" + dstd::to_string(t));
        REQUIRE(file.has_value());
        CHECK(snapshot.extension(*file, IndexExtension::Catalog, ROUNDS - 1) ==
              Freshness::Current);
        const auto catalog = take(index::catalog(snapshot, *file));
        REQUIRE(catalog.size() == 1);
        CHECK(str(catalog[0].first) == "r" + dstd::to_string(ROUNDS - 1));
    }
}

TEST_CASE("a commit lands from a worker, a process worker and a plain thread") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::size_t previous = dftracer::utils::default_runtime().threads();

    SUBCASE("the only worker of the runtime") {
        dftracer::utils::configure_default_runtime({.threads = 1});
        bool committed = false;
        dftracer::utils::default_runtime().block_on(
            dftracer::utils::coro::named(
                "index-worker-commit", [&]() -> coro::CoroTask<void> {
                    auto build = opened->build("/traces/worker", stat_of(1, 2));
                    if (!build) co_return;
                    put_members(*build, 1);
                    auto done = co_await build->commit();
                    committed = done.has_value();
                }()));
        dftracer::utils::configure_default_runtime({.threads = previous});
        CHECK(committed);
        CHECK(take(opened->snapshot()).file("/traces/worker").has_value());
    }

    SUBCASE("a worker of the process runtime") {
        bool committed = false;
        dftracer::utils::default_runtime().block_on(
            dftracer::utils::coro::named(
                "index-async-commit", [&]() -> coro::CoroTask<void> {
                    auto build = opened->build("/traces/async", stat_of(1, 2));
                    if (!build) co_return;
                    put_members(*build, 1);
                    auto done = co_await build->commit();
                    committed = done.has_value();
                }()));
        CHECK(committed);
        CHECK(take(opened->snapshot()).file("/traces/async").has_value());
    }

    SUBCASE("a plain thread drives the task") {
        auto build = take(opened->build("/traces/plain", stat_of(1, 2)));
        put_members(build, 1);
        CHECK(build.commit().get().has_value());
        CHECK(take(opened->snapshot()).file("/traces/plain").has_value());
    }
}

namespace {

dstd::String write_bytes(const dftracer::utils::dstd::Path& path,
                         dstd::StringView contents) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    return dstd::String(path.str());
}

SourceStat stat_of_file(const dstd::String& path) {
    dftracer::utils::common::InputFileStat st;
    REQUIRE(dftracer::utils::common::stat_input(path, st));
    return stat_of(static_cast<std::uint64_t>(st.mtime), st.size);
}

void register_traces(const Index& index,
                     const dstd::Vector<dstd::String>& paths) {
    for (const auto& path : paths) {
        auto build = take(index.build(path, stat_of_file(path)));
        put_members(build, 1);
        REQUIRE(run(build.commit()).has_value());
    }
}

}  // namespace

TEST_CASE("freshness follows the size and the mtime of the trace file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const dftracer::utils::dstd::Path dir(env.get_dir());
    REQUIRE(dftracer::utils::dstd::fs::create_directories(dir).has_value());
    auto opened = Index::open((dir / ".dftindex").str());
    REQUIRE(opened.has_value());

    SUBCASE("the stored stat is the file's size and mtime") {
        const auto a = write_bytes(dir / "a.pfw", "hello world");
        register_traces(*opened, {a});
        const auto snapshot = take(opened->snapshot());
        const auto file = snapshot.file(a);
        REQUIRE(file.has_value());
        CHECK(file->stat().size ==
              dftracer::utils::dstd::fs::file_size(a).value());
        CHECK(file->stat().mtime != 0);
    }

    SUBCASE("an unchanged index reports nothing stale") {
        const auto a = write_bytes(dir / "a.pfw", "aaa");
        const auto b = write_bytes(dir / "b.pfw", "bbbbb");
        register_traces(*opened, {a, b});
        const auto snapshot = take(opened->snapshot());
        CHECK(snapshot.freshness(a, stat_of_file(a)) == Freshness::Current);
        CHECK(snapshot.freshness(b, stat_of_file(b)) == Freshness::Current);
    }

    SUBCASE("a size change is stale") {
        const auto a = write_bytes(dir / "a.pfw", "original");
        register_traces(*opened, {a});
        write_bytes(dir / "a.pfw", "original plus more bytes");
        const auto snapshot = take(opened->snapshot());
        CHECK(snapshot.freshness(a, stat_of_file(a)) == Freshness::Stale);
    }

    SUBCASE("an mtime change with the same size is stale") {
        const auto a = write_bytes(dir / "a.pfw", "same-size-content");
        register_traces(*opened, {a});
        REQUIRE(dftracer::utils::dstd::fs::set_mtime_ns(
                    a, dftracer::utils::dstd::fs::mtime_ns(a).value() +
                           48ULL * 3600 * 1'000'000'000ULL)
                    .has_value());
        const auto snapshot = take(opened->snapshot());
        CHECK(snapshot.freshness(a, stat_of_file(a)) == Freshness::Stale);
    }

    SUBCASE("added and removed files differ from the indexed set") {
        const auto a = write_bytes(dir / "a.pfw", "aaa");
        const auto b = write_bytes(dir / "b.pfw", "bbb");
        register_traces(*opened, {a, b});
        const auto added = write_bytes(dir / "c.pfw", "ccc");
        const auto snapshot = take(opened->snapshot());

        CHECK(snapshot.freshness(added, stat_of_file(added)) ==
              Freshness::Missing);
        CHECK_FALSE(snapshot.file(added).has_value());

        const dstd::Set<dstd::String> wanted{a, added};
        dstd::Set<dstd::String> indexed;
        for (const FileRef& file : take(snapshot.files()))
            indexed.insert(str(file.path()));
        dstd::Vector<dstd::String> removed;
        for (const auto& path : indexed)
            if (wanted.count(path) == 0) removed.push_back(path);
        dstd::Vector<dstd::String> unindexed;
        for (const auto& path : wanted)
            if (indexed.count(path) == 0) unindexed.push_back(path);
        CHECK(removed == dstd::Vector<dstd::String>{b});
        CHECK(unindexed == dstd::Vector<dstd::String>{added});
    }
}

TEST_CASE("a snapshot sees a commit made after the first one") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string trace = env.create_dft_test_gzip_file(50);
    REQUIRE_FALSE(trace.empty());
    const std::string index_path = env.get_dir() + "/.dftindex";
    const dstd::String key = index_store::internal::get_logical_path(trace);

    auto opened = Index::open(index_path);
    REQUIRE(opened.has_value());
    const auto before = take(opened->snapshot());
    CHECK_FALSE(before.file(key).has_value());

    REQUIRE(dftu_utils_test::build_index(std::string(trace),
                                         std::string(index_path), CHECKPOINT));

    const auto after = take(opened->snapshot());
    const auto file = after.file(key);
    REQUIRE(file.has_value());
    const auto meta = take(index::file_metadata(after, *file));
    REQUIRE(meta.has_value());
    CHECK(meta->num_lines > 0);
    CHECK_FALSE(before.file(key).has_value());
}

TEST_CASE("an index root without a store reads nothing and stays absent") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const auto root = dftracer::utils::dstd::Path(env.get_dir()) / "empty";
    REQUIRE(dftracer::utils::dstd::fs::create_directories(root).has_value());
    const dstd::String trace = dstd::String((root / "a.pfw.gz").str());

    auto opened = Index::open((root / ".dftindex").str());
    REQUIRE(opened.has_value());
    const auto snapshot = take(opened->snapshot());
    CHECK(take(snapshot.files()).empty());
    CHECK_FALSE(snapshot.file(trace).has_value());
    CHECK(snapshot.freshness(trace, stat_of(1, 2)) == Freshness::Missing);
    CHECK_FALSE(dftracer::utils::dstd::fs::exists(root / ".dftindex").value());
}

TEST_CASE("pid and tid counts round trip with large pids") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/pids.pfw.gz";
    auto build = take(opened->build(path, stat_of(1, 2)));
    put_members(build, 1);
    dstd::StringViewMap<std::uint64_t> counts;
    counts.emplace("1234:1", 3);
    counts.emplace("1234:2", 1);
    counts.emplace("4294967296:1", 1);
    counts.emplace("18446744073709551615:1", 2);
    index::put_file_pid_tid_counts(build, counts);
    build.finish(IndexExtension::Stats, 1);
    REQUIRE(run(build.commit()).has_value());

    auto other = take(opened->build("/traces/nopids.pfw.gz", stat_of(1, 2)));
    put_members(other, 1);
    REQUIRE(run(other.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto file = snapshot.file(path);
    REQUIRE(file.has_value());
    const auto read = take(index::pid_tid_counts(snapshot, *file));
    REQUIRE(read.has_value());
    REQUIRE(read->size() == counts.size());
    for (const auto& [key, count] : counts) {
        const auto at = read->find(key);
        REQUIRE(at != read->end());
        CHECK(at->second == count);
    }
    const auto none = snapshot.file("/traces/nopids.pfw.gz");
    REQUIRE(none.has_value());
    CHECK_FALSE(take(index::pid_tid_counts(snapshot, *none)).has_value());
}

TEST_CASE("column types come from the path catalog of a file") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const auto stat = [](std::initializer_list<index_store::PathType> types,
                         std::uint64_t count) {
        index_store::PathStat out;
        for (auto type : types) {
            out.type = index_store::join(out.type, type);
            out.seen |=
                static_cast<std::uint8_t>(1U << static_cast<unsigned>(type));
        }
        out.count = count;
        return out;
    };
    using index_store::PathType;

    auto first = take(opened->build("/traces/a.pfw.gz", stat_of(1, 2)));
    put_members(first, 1);
    index::put_catalog_path(first, "cat", stat({PathType::String}, 5));
    index::put_catalog_path(first, "args.mhost",
                            stat({PathType::Int, PathType::Bool}, 5));
    first.finish(IndexExtension::Catalog, 1);
    REQUIRE(run(first.commit()).has_value());

    auto second = take(opened->build("/traces/b.pfw.gz", stat_of(1, 2)));
    put_members(second, 1);
    index::put_catalog_path(second, "args.mhost", stat({PathType::Double}, 2));
    index::put_catalog_path(second, "args.fhash", stat({PathType::String}, 2));
    index::put_catalog_path(second, "args.gone",
                            stat({PathType::NullValue}, 0));
    second.finish(IndexExtension::Catalog, 1);
    REQUIRE(run(second.commit()).has_value());

    const auto snapshot = take(opened->snapshot());
    const auto one = snapshot.file("/traces/a.pfw.gz");
    REQUIRE(one.has_value());
    const auto catalog = take(index::catalog(snapshot, *one));
    REQUIRE(catalog.size() == 2);
    CHECK(str(catalog[0].first) == "args.mhost");
    CHECK(catalog[0].second.type == PathType::Mixed);
    CHECK(catalog[0].second.count == 5);
    CHECK(str(catalog[1].first) == "cat");

    const auto two = snapshot.file("/traces/b.pfw.gz");
    REQUIRE(two.has_value());
    const auto types = take(index::file_column_types(snapshot, *two));
    REQUIRE(types.size() == 2);
    CHECK(str(types[0].first) == "args.fhash");
    CHECK(types[0].second == index_store::ColumnType::String);
    CHECK(str(types[1].first) == "args.mhost");
    CHECK(types[1].second == index_store::ColumnType::Float64);
}

TEST_CASE("a legacy index path normalizes to the root-local .dftindex") {
    namespace internal = dftracer::utils::index::store::internal;
    CHECK(internal::normalize_index_root("/data/run/trace.pfw.gz.idx") ==
          "/data/run/.dftindex");
    CHECK(internal::normalize_index_root("/data/run/.dftindex") ==
          "/data/run/.dftindex");
    CHECK(internal::normalize_index_root("/data/run") == "/data/run/.dftindex");
}

TEST_CASE("a view or shard root normalizes to itself") {
    namespace internal = dftracer::utils::index::store::internal;
    CHECK(internal::normalize_index_root("/d/.dftindex") == "/d/.dftindex");
    CHECK(internal::normalize_index_root("/d/.dftindex/store") ==
          "/d/.dftindex");
    CHECK(internal::normalize_index_root("/d/.dftindex/views/q-1a2b") ==
          "/d/.dftindex/views/q-1a2b");
    CHECK(internal::normalize_index_root(
              "/d/.dftindex/views/q.x-1a2b/parts/p.pfw.gz") ==
          "/d/.dftindex/views/q.x-1a2b");
    CHECK(internal::normalize_index_root("/d/.dftindex/views/q.x-1a2b/store") ==
          "/d/.dftindex/views/q.x-1a2b");
    CHECK(internal::normalize_index_root("/d/.dftindex/shards/3/store") ==
          "/d/.dftindex/shards/3");
    CHECK(internal::normalize_index_root("/d/views/q/t.pfw.gz") ==
          "/d/views/q/.dftindex");
}

TEST_CASE("opening a view root keeps its store inside the view root") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    const dstd::String view = index::view_root(root, "v1");
    auto opened = Index::open(view);
    REQUIRE(opened.has_value());
    const dstd::String path = "/traces/one.pfw.gz";
    auto build = take(opened->build(path, stat_of(1000, 4000, 9)));
    put_members(build, 1);
    REQUIRE(run(build.commit()).has_value());

    CHECK(dftracer::utils::dstd::fs::exists(root + "/views/v1/store").value());
    CHECK_FALSE(dftracer::utils::dstd::fs::exists(root + "/store").value());
    index::release_root(view);
}
