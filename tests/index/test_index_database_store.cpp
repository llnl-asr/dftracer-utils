#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/store/index_database.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace index_store = dftracer::utils::index::store;

using index_store::IndexDatabase;
using index_store::IndexExtension;
namespace records = dftu_utils_test::index_records;

constexpr std::size_t CHECKPOINT = 4096;

/// The plugin extension the comparison writes into both indexes.
constexpr std::string_view PLUGIN = "test_ext";

constexpr IndexExtension PATH_KEYED[] = {
    index_store::IndexExtension::Zonemap, index_store::IndexExtension::Bloom,
    index_store::IndexExtension::Counts, index_store::IndexExtension::Postings,
    index_store::IndexExtension::Plugin};

constexpr IndexExtension ALL_EXT[] = {
    index_store::IndexExtension::Members,  index_store::IndexExtension::Zonemap,
    index_store::IndexExtension::Bloom,    index_store::IndexExtension::Counts,
    index_store::IndexExtension::Postings, index_store::IndexExtension::Stats,
    index_store::IndexExtension::Catalog,  index_store::IndexExtension::Profile,
    index_store::IndexExtension::Metadata, index_store::IndexExtension::Plugin,
    index_store::IndexExtension::Agg,      index_store::IndexExtension::Rowset};

void note(std::string& out, const char* what, std::string_view value) {
    out += what;
    out += '=';
    out += value;
    out += '\n';
}

void note(std::string& out, std::string_view what, std::uint64_t value) {
    out.append(what);
    out += '=';
    out += std::to_string(value);
    out += '\n';
}

/// Bytes the store keeps as they are, so the dump stays text.
void note_bytes(std::string& out, const char* what, std::string_view bytes) {
    static constexpr char HEX[] = "0123456789abcdef";
    out += what;
    out += '=';
    for (unsigned char byte : bytes) {
        out += HEX[byte >> 4];
        out += HEX[byte & 0xF];
    }
    out += '\n';
}

/// The keys of an unordered map, sorted.
template <typename Map>
std::vector<std::string_view> sorted_keys(const Map& map) {
    std::vector<std::string_view> keys;
    keys.reserve(map.size());
    for (const auto& entry : map) keys.emplace_back(entry.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

/// One file of the registry: its logical path and its id.
struct RegisteredFile {
    std::string path;
    int file_id;
};

/// The registry in logical path order. File ids are not compared.
std::vector<RegisteredFile> registered_files(std::string& out,
                                             const IndexDatabase& db) {
    std::vector<RegisteredFile> files;
    for (const auto& [registered, entry] : db.query_all_file_registry())
        files.push_back({std::string(registered), entry.file_id});
    std::sort(files.begin(), files.end(),
              [](const RegisteredFile& a, const RegisteredFile& b) {
                  return a.path < b.path;
              });
    note(out, "registry.count", static_cast<std::uint64_t>(files.size()));
    for (const auto& file : files) note(out, "registry.path", file.path);
    return files;
}

void dump_file(std::string& out, const IndexDatabase& db,
               const RegisteredFile& file) {
    const int file_id = file.file_id;
    const auto& registered = file.path;

    note(out, "file.hash", db.get_file_hash(registered).value_or(0));
    const auto stat = db.get_file_stat(registered);
    note(out, "file.mtime", stat ? stat->mtime : 0);
    note(out, "file.size", stat ? stat->size : 0);
    note(out, "file.found", db.find_file(registered) >= 0 ? 1 : 0);
    note(out, "file.id_matches", db.find_file(registered) == file_id ? 1 : 0);

    note(out, "ckpt", db.get_checkpoint_size(file_id));
    note(out, "lines", db.get_num_lines(file_id));
    note(out, "max_bytes", db.get_max_bytes(file_id));
    note(out, "schema", db.file_schema(file_id).value_or("-"));

    for (const auto& member : db.query_gzip_members(file_id)) {
        note(out, "member.idx", member.member_idx);
        note(out, "member.uc_offset", member.uc_offset);
        note(out, "member.uc_size", member.uc_size);
        note(out, "member.first_line", member.first_line_num);
        note(out, "member.last_line", member.last_line_num);
    }
    for (const auto& chunk : db.query_chunk_statistics(file_id)) {
        note(out, "chunk.idx", chunk.checkpoint_idx);
        note(out, "chunk.events", chunk.stats.total_events);
        note(out, "chunk.min_ts", chunk.stats.min_timestamp_us);
        note(out, "chunk.max_ts", chunk.stats.max_timestamp_us);
    }
    for (const auto& [catalog_path, catalog_stat] : db.catalog(file_id)) {
        note(out, "catalog.path", catalog_path);
        note(out, "catalog.seen",
             static_cast<std::uint64_t>(catalog_stat.seen));
    }
    for (IndexExtension ext : ALL_EXT) {
        note(out, "ext", std::to_string(static_cast<int>(ext)));
        note(out, "ext.current",
             static_cast<std::uint64_t>(db.extension_current(file_id, ext)));
        const auto state = db.extension_state(file_id, ext);
        note(out, "ext.state", state ? "1" : "0");
        if (state) {
            note(out, "ext.version", state->version);
            note(out, "ext.params", state->params_hash);
            note(out, "ext.ready", static_cast<std::uint64_t>(state->ready));
        }
    }
    for (const auto& [name, state] : db.plugin_extension_states(file_id)) {
        note(out, "plugin.entry", name);
        note(out, "plugin.version", state.version);
        note(out, "plugin.params", state.params_hash);
        note(out, "plugin.ready", static_cast<std::uint64_t>(state.ready));
    }
    const auto plugin = db.plugin_extension_state(file_id, PLUGIN);
    note(out, "plugin.state", plugin ? "1" : "0");
    if (plugin) {
        note(out, "plugin.state.version", plugin->version);
        note(out, "plugin.state.params", plugin->params_hash);
        note(out, "plugin.state.ready",
             static_cast<std::uint64_t>(plugin->ready));
    }
    for (IndexExtension ext : PATH_KEYED) {
        const auto paths = db.extension_paths(file_id, ext);
        note(out, "paths.count", static_cast<std::uint64_t>(paths.size()));
        for (const auto& ext_path : paths) {
            note(out, "paths.path", ext_path);
            for (const auto& [granule, payload] :
                 db.path_granules(file_id, ext, ext_path)) {
                note(out, "granule.idx", granule);
                note_bytes(out, "granule.payload", payload);
            }
            note(out, "path_file",
                 db.path_file_value(file_id, ext, ext_path).value_or("-"));
        }
    }
    std::vector<std::uint64_t> pids;
    for (std::uint64_t pid : db.query_file_pids(file_id)) pids.push_back(pid);
    std::sort(pids.begin(), pids.end());
    note(out, "pids.count", static_cast<std::uint64_t>(pids.size()));
    for (std::uint64_t pid : pids) note(out, "pids.pid", pid);
}

void dump_batches(std::string& out, const IndexDatabase& db,
                  const std::vector<RegisteredFile>& files) {
    std::vector<int> ids;
    ids.reserve(files.size());
    for (const auto& file : files) ids.push_back(file.file_id);

    const auto chunks = db.query_chunk_statistics_batch(ids);
    const auto scalars = db.query_file_scalar_stats_batch(ids);
    const auto metadata = db.query_file_metadata_batch(ids);
    const auto counts = db.query_file_pid_tid_counts_batch(ids);
    const auto all_pids = db.query_all_file_pids();
    for (const auto& file : files) {
        note(out, "batch.path", file.path);
        const auto chunk_at = chunks.find(file.file_id);
        note(out, "batch.chunks",
             static_cast<std::uint64_t>(
                 chunk_at == chunks.end() ? 0 : chunk_at->second.size()));
        if (chunk_at != chunks.end()) {
            for (const auto& chunk : chunk_at->second) {
                note(out, "batch.chunk.idx", chunk.checkpoint_idx);
                note(out, "batch.chunk.events", chunk.stats.total_events);
                note(out, "batch.chunk.min_ts", chunk.stats.min_timestamp_us);
                note(out, "batch.chunk.max_ts", chunk.stats.max_timestamp_us);
            }
        }
        const auto scalar_at = scalars.find(file.file_id);
        note(out, "batch.scalar", scalar_at == scalars.end() ? "0" : "1");
        if (scalar_at != scalars.end()) {
            note(out, "batch.scalar.events",
                 scalar_at->second.stats.total_events);
            note(out, "batch.scalar.num_chunks", scalar_at->second.num_chunks);
            note(out, "batch.scalar.min_nonzero_ts",
                 scalar_at->second.stats.min_nonzero_timestamp_us);
            note(out, "batch.scalar.duration_count",
                 scalar_at->second.stats.duration_count);
        }
        const auto metadata_at = metadata.find(file.file_id);
        note(out, "batch.metadata", metadata_at == metadata.end() ? "0" : "1");
        if (metadata_at != metadata.end()) {
            note(out, "batch.metadata.checkpoint",
                 metadata_at->second.checkpoint_size);
            note(out, "batch.metadata.lines", metadata_at->second.num_lines);
            note(out, "batch.metadata.max_bytes",
                 metadata_at->second.max_bytes);
            note(out, "batch.metadata.truncated",
                 static_cast<std::uint64_t>(metadata_at->second.truncated));
        }
        const auto counts_at = counts.find(file.file_id);
        if (counts_at != counts.end()) {
            for (std::string_view key : sorted_keys(counts_at->second)) {
                out += "batch.pid_tid=";
                out += key;
                out += '=';
                out += std::to_string(counts_at->second.find(key)->second);
                out += '\n';
            }
        }
        const auto pids_at = all_pids.find(file.file_id);
        if (pids_at != all_pids.end()) {
            std::vector<std::uint64_t> pids(pids_at->second.begin(),
                                            pids_at->second.end());
            std::sort(pids.begin(), pids.end());
            note(out, "batch.pids.count",
                 static_cast<std::uint64_t>(pids.size()));
            for (std::uint64_t pid : pids) note(out, "batch.pids.pid", pid);
        }
    }
    for (const auto& column : db.query_all_columns())
        note(out, "columns.name", column);
    for (const auto& [column, type] : db.query_all_column_types()) {
        note(out, "column_types.name", column);
        note(out, "column_types.type",
             static_cast<std::uint64_t>(static_cast<int>(type)));
    }
}

void dump_freshness(std::string& out, const IndexDatabase& db,
                    const std::vector<std::string>& traces) {
    for (const auto& trace : traces) {
        note(out, "fresh",
             std::to_string(static_cast<int>(db.check_freshness(trace))));
    }
    const auto stale = db.find_stale_files(traces);
    note(out, "stale.format", stale.format_outdated ? 1 : 0);
    for (const auto& path : stale.changed) note(out, "stale.changed", path);
    for (const auto& path : stale.added) note(out, "stale.added", path);
    for (const auto& path : stale.removed) note(out, "stale.removed", path);
}

/// Everything the typed read API of `index_path` answers, as one string.
std::string dump_index(const std::string& index_path,
                       const std::vector<std::string>& traces) {
    std::string out;
    IndexDatabase db(index_path, index_store::IndexOpenMode::ReadOnly);
    const auto files = registered_files(out, db);
    for (const auto& file : files) dump_file(out, db, file);
    dump_batches(out, db, files);
    dump_freshness(out, db, traces);
    return out;
}

/// Builds `traces` into the index root of `dir`, in the order given.
std::string build_into(const std::string& dir,
                       const std::vector<std::string>& traces) {
    const auto index_path = dir + "/.dftindex";
    for (const auto& trace : traces)
        REQUIRE(dftu_utils_test::build_index(trace, index_path, CHECKPOINT));
    return index_path;
}

/// Writes what a plugin build writes for the plugin extension of every file.
void put_plugin_extension(const std::string& index_path,
                          const std::vector<std::string>& traces) {
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

/// Three gzip traces under `dir` whose creation order, c then a then b, is not
/// their path order.
std::vector<std::string> create_unsorted_traces(
    dftu_utils_test::TestEnvironment& env, const std::filesystem::path& dir) {
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

}  // namespace

TEST_CASE("index_database answers fresh, then stale, from the store") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::filesystem::path root(env.get_dir());
    std::filesystem::create_directories(root);

    const auto traces = create_unsorted_traces(env, root);
    REQUIRE(traces.size() == 3);

    const auto index_path = build_into(root.string(), traces);
    put_plugin_extension(index_path, traces);

    const std::string fresh_line =
        std::string("fresh=") +
        std::to_string(static_cast<int>(IndexDatabase::Freshness::Fresh));
    const std::string stale_line =
        std::string("fresh=") +
        std::to_string(static_cast<int>(IndexDatabase::Freshness::Stale));

    const std::string fresh = dump_index(index_path, traces);
    INFO("store (fresh):\n" << fresh);
    CHECK(fresh.find(stale_line) == std::string::npos);
    CHECK(fresh.find(fresh_line) != std::string::npos);

    // One trace changes on disk: it alone is then stale.
    std::filesystem::last_write_time(
        traces[1],
        std::filesystem::last_write_time(traces[1]) + std::chrono::hours(48));

    const std::string stale = dump_index(index_path, traces);
    INFO("store (stale):\n" << stale);
    CHECK_FALSE(stale.empty());
    CHECK(stale.find(stale_line) != std::string::npos);
    CHECK(stale.find("stale.changed=" + traces[1] + "\n") != std::string::npos);
    CHECK(stale.find("stale.changed=" + traces[0] + "\n") == std::string::npos);
    CHECK(stale.find("stale.changed=" + traces[2] + "\n") == std::string::npos);
}

TEST_CASE("store index sees a commit after the write") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());
    const std::string trace = env.create_dft_test_gzip_file(50);
    REQUIRE_FALSE(trace.empty());

    const auto index_path = std::string(env.get_dir()) + "/.dftindex";
    {
        IndexDatabase db(index_path);
        CHECK(db.find_file(trace) < 0);
    }
    REQUIRE(dftu_utils_test::build_index(trace, index_path, CHECKPOINT));
    IndexDatabase db(index_path, index_store::IndexOpenMode::ReadOnly);
    CHECK(db.find_file(trace) >= 0);
    CHECK(db.get_num_lines(db.find_file(trace)) > 0);
    CHECK_FALSE(db.format_outdated());
}

TEST_CASE("a read-only index root without a store reads nothing") {
    dftu_utils_test::TestEnvironment env;
    REQUIRE(env.is_valid());

    const auto root = std::filesystem::path(env.get_dir()) / "empty";
    std::filesystem::create_directories(root);
    const auto index_path = (root / ".dftindex").string();
    const auto trace = (root / "a.pfw.gz").string();

    IndexDatabase db(index_path, index_store::IndexOpenMode::ReadOnly);
    CHECK(db.query_all_file_registry().empty());
    CHECK(db.find_file(trace) < 0);
    CHECK_FALSE(db.get_file_stat(trace).has_value());
    CHECK_FALSE(db.get_file_hash(trace).has_value());
    CHECK(db.query_all_columns().empty());
    CHECK(db.query_all_file_pids().empty());
    CHECK(db.query_chunk_statistics_batch({1}).empty());
    CHECK_FALSE(db.extension_state(1, IndexExtension::Members).has_value());
    CHECK(db.extension_paths(1, IndexExtension::Zonemap).empty());
    CHECK(db.check_freshness(trace) == IndexDatabase::Freshness::Stale);
    const auto stale = db.find_stale_files({trace});
    CHECK(stale.added == std::vector<std::string>{trace});
    CHECK(stale.changed.empty());
    CHECK(stale.removed.empty());
    CHECK_FALSE(stale.format_outdated);

    CHECK_FALSE(std::filesystem::exists(root / ".dftindex"));
}
