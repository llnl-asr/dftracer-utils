#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/index/build/file_group.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace coro = dftracer::utils::coro;
namespace index = dftracer::utils::index;
namespace index_store = dftracer::utils::index::store;
namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace agg_tier = dftracer::utils::index::schemas::dft::agg::tier;
namespace kv = dftracer::utils::store;

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
                const FileRef& file, bool has_plugin);

struct PartialRow {
    std::uint64_t chunk = 0;
    std::uint32_t cat = 0;
    std::uint32_t name = 0;
    std::uint32_t host = 0;
    std::uint32_t extra = 0;
    agg::PartialKind kind = agg::PartialKind::Event;
    std::string bytes;
};

std::vector<PartialRow> partials(const index::Snapshot& snapshot,
                                 const FileRef& file) {
    std::vector<PartialRow> rows;
    ok(index::for_each_agg_partial(
        snapshot, file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra, agg::PartialKind kind,
            dstd::Span<const std::uint8_t> bytes) {
            rows.push_back(
                {chunk, cat, name, host, extra, kind,
                 std::string(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size())});
            return true;
        }));
    return rows;
}

using Blocks = std::vector<std::pair<std::uint64_t, agg::ChunkColumns>>;

Blocks blocks(const index::Snapshot& snapshot, const FileRef& file) {
    std::vector<std::pair<std::uint64_t, agg::ChunkColumns>> out;
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

void check_file(const IndexDatabase& db, const index::Snapshot& snapshot,
                const FileRef& file, bool has_plugin) {
    const std::string path = str(file.path());
    const int file_id = db.get_file_info_id(path);
    REQUIRE(file_id >= 0);

    const auto stat = db.get_file_stat(path);
    REQUIRE(stat.has_value());
    CHECK(file.stat().mtime == stat->mtime);
    CHECK(file.stat().size == stat->size);
    CHECK(file.stat().hash == db.get_file_hash(path).value_or(0));

    const auto hash = index::file_hash(snapshot, file);
    const auto old_hash = db.get_file_hash(path);
    CHECK(hash.has_value() == old_hash.has_value());
    if (hash && old_hash) CHECK(*hash == *old_hash);

    const auto types = take(index::file_column_types(snapshot, file));
    const auto old_types = db.file_column_types(file_id);
    REQUIRE(types.size() == old_types.size());
    CHECK_FALSE(types.empty());
    for (std::size_t i = 0; i < types.size(); ++i) {
        CHECK(str(types[i].first) == old_types[i].first);
        CHECK(types[i].second == old_types[i].second);
    }

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
                  (at == texts.end() ? std::string() : str(at->second)));
        }
    }
    const std::uint64_t absent_chunk =
        stored_blocks.empty() ? 0 : stored_blocks.back().first + 1000;
    CHECK_FALSE(take(index::agg_chunk_columns(snapshot, file, absent_chunk))
                    .has_value());

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
        CHECK(index::extension_current(snapshot, file, ext) == current);
        const auto typed = index::extension_state(snapshot, file, ext);
        REQUIRE(typed.has_value() == state.has_value());
        if (state && typed) {
            CHECK(typed->version == state->version);
            CHECK(typed->params_hash == state->params_hash);
            CHECK(typed->ready == state->ready);
        }
        const Freshness got =
            snapshot.extension(file, ext, state ? state->params_hash : 0);
        if (current) {
            CHECK(got == Freshness::Current);
        } else {
            CHECK(got != Freshness::Current);
        }
    }

    CHECK_FALSE(index::extension_state(snapshot, file, IndexExtension::Plugin));

    if (!has_plugin) return;
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
        check_file(db, snapshot, file, true);
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

TEST_CASE("the typed agg readers read what the aggregation store reads") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::filesystem::path root(env.get_dir());
    std::filesystem::create_directories(root);
    const auto traces = create_traces(env, root);

    index::IndexerOptions options;
    options.index_dir = root.string();
    options.checkpoint_size = CHECKPOINT;
    options.parallelism = 4;
    agg::AggregationConfig config;
    config.time_interval_us = 100000;
    options.aggregation = config;
    auto indexer = index::Indexer::open(traces, options);
    indexer.build();

    const std::string index_path = (root / ".dftindex").string();
    IndexDatabase db(index_path, index_store::IndexOpenMode::ReadOnly);
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
        check_file(db, snapshot, file, false);
        partial_count += partials(snapshot, file).size();

        dstd::HashMap<std::uint32_t, dstd::String> texts;
        ok(index::agg_file_texts(snapshot, file, texts));
        REQUIRE(texts.size() > 0);
        for (const auto& [id, text] : texts)
            CHECK(std::string(dictionary->intern.resolve(id)) == str(text));
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
    const std::string path = "/traces/versioned.pfw.gz";
    const SourceStat stat = stat_of(1000, 4000, 9);

    auto build = take(opened->build(path, stat));
    put_members(build, 1);
    REQUIRE(run(build.commit()).has_value());

    auto store = kv::Store::open(root + "/store");
    REQUIRE(store.has_value());
    kv::NamespaceSpec spec;
    const std::string name =
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

    IndexDatabase db(root, index_store::IndexOpenMode::ReadOnly);
    const int file_id = db.get_file_info_id(path);
    REQUIRE(file_id >= 0);
    CHECK_FALSE(db.extension_current(file_id, IndexExtension::Profile));
    CHECK(db.extension_current(file_id, IndexExtension::Members));

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

TEST_CASE("a group of FileBuilds becomes visible together") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    auto opened = Index::open(env.get_dir() + "/.dftindex");
    REQUIRE(opened.has_value());
    const std::string a = "/traces/group_a.pfw.gz";
    const std::string b = "/traces/group_b.pfw.gz";
    const std::string bad = "/traces/group_bad.pfw.gz";
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
    const std::string plain = "/traces/rep.pfw.gz";
    const std::string dotted = "/traces/./rep.pfw.gz";
    const std::string other = "/traces/other.pfw.gz";
    const std::string bad = "/traces/bad.pfw.gz";
    const auto key = [](const std::string& path) {
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

    std::string config;
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

TEST_CASE("many FileBuild commits mixed with old writes and closes") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    namespace records = dftu_utils_test::index_records;
    const std::string root = env.get_dir() + "/.dftindex";
    for (int round = 0; round < 12; ++round) {
        auto opened = Index::open(root);
        REQUIRE(opened.has_value());
        const std::string path =
            "/traces/many_" + std::to_string(round) + ".pfw.gz";
        const SourceStat stat =
            stat_of(1000 + static_cast<std::uint64_t>(round), 4000, 9);
        auto build = take(opened->build(path, stat));
        put_members(build, 2);
        put_stats(build, 2);
        REQUIRE(run(build.commit()).has_value());

        IndexDatabase db(root);
        const int file_id = db.get_file_info_id(path);
        CHECK(file_id >= 0);
        {
            auto write = db.begin_write();
            const int other = dftu_utils_test::register_test_file(
                *write, path + ".old", stat.hash, stat.mtime, stat.size);
            records::put_path(*write, IndexExtension::Zonemap, other, "a");
            records::put_manifest(*write, other, IndexExtension::Zonemap, 5);
            write->commit();
        }
        if (round % 3 == 2) {
            index_store::close_index_writers();
        }
    }
}

TEST_CASE("FileBuild commits from concurrent tasks and an open reader") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string root = env.get_dir() + "/.dftindex";
    IndexDatabase db(root);
    CHECK(db.get_file_info_id("/traces/none.pfw.gz") < 0);
    auto opened = Index::open(root);
    REQUIRE(opened.has_value());
    constexpr int FILES = 24;
    const auto one = [](Index index, int i) -> coro::CoroTask<bool> {
        const std::string path =
            "/traces/conc_" + std::to_string(i) + ".pfw.gz";
        auto build = index.build(path, stat_of(1000, 4000, 9));
        if (!build) co_return false;
        put_members(*build, 2);
        auto committed = co_await build->commit();
        co_return committed.has_value();
    };
    const auto all = [&]() -> coro::CoroTask<int> {
        std::vector<coro::CoroTask<bool>> tasks;
        for (int i = 0; i < FILES; ++i) tasks.push_back(one(*opened, i));
        auto results = co_await coro::when_all(std::move(tasks));
        int ok = 0;
        for (bool r : results) ok += r ? 1 : 0;
        co_return ok;
    };
    for (int round = 0; round < 4; ++round) {
        CHECK(dftracer::utils::default_runtime().submit(all()).get() == FILES);
    }
    for (int i = 0; i < FILES; ++i) {
        CHECK(db.get_file_info_id("/traces/conc_" + std::to_string(i) +
                                  ".pfw.gz") >= 0);
    }
}
