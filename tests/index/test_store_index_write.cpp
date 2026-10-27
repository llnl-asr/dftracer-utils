#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_backend.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/store/snapshot.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace dftu_test = dftu_utils_test;
namespace store = dftracer::utils::store;
namespace layout = dftracer::utils::index::store::layout;
namespace store_layout = dftracer::utils::index::store::store_layout;
namespace records = dftracer::utils::index::store::records;
namespace gzip = dftracer::utils::index::gzip;
namespace schemas = dftracer::utils::index::schemas::dft;

using dftracer::utils::append_u32_be;
using dftracer::utils::ByteView;
using dftracer::utils::index::store::ChunkMetadata;
using dftracer::utils::index::store::IndexWrite;
using dftracer::utils::index::store::PathStat;
using dftracer::utils::index::store::PathType;
using dftracer::utils::index::store::StoreIndexWrite;
using dftracer::utils::index::store::StoreRoot;

constexpr std::uint64_t PARAMS = 0xABCDEF01ULL;
constexpr std::string_view TRACE = "/traces/app.pfw.gz";

std::string to_std(ByteView bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

store::NamespaceSpec spec_of(const std::string& name,
                             std::uint32_t format_version) {
    store::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = format_version;
    return spec;
}

/// The namespace of one extension, at its own version.
store::NamespaceSpec spec_of(layout::Ext ext) {
    return spec_of(store_layout::namespace_name(ext),
                   store_layout::namespace_version(ext));
}

store::ScopeKey file_scope(std::string_view path) {
    return store::ScopeKey::file(path);
}

/// The key inside a copy of `key`.
std::string inner_of(const std::string& key) {
    std::string inner(1, key[2]);
    inner.append(key, layout::KEY_PREFIX_BYTES, std::string::npos);
    return inner;
}

/// Every row of the current copy of one scope of a namespace.
std::map<std::string, std::string> rows_of(StoreRoot& root,
                                           const store::NamespaceSpec& spec,
                                           const store::ScopeKey& scope) {
    std::map<std::string, std::string> rows;
    auto snapshot = root.snapshot();
    auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        rows.emplace(to_std(cursor.key()), to_std(*value));
        cursor.next();
    }
    return rows;
}

std::map<std::string, std::string> rows_of(StoreRoot& root, layout::Ext ext,
                                           const store::ScopeKey& scope) {
    return rows_of(root, spec_of(ext), scope);
}

/// One write through the seam that keeps every key and value it receives.
class RecordingWrite final : public IndexWrite {
   public:
    explicit RecordingWrite(IndexWrite& inner) : inner_(inner) {}

    void put(layout::Family family, std::string_view key,
             std::string_view value) override {
        puts.emplace_back(std::string(key), std::string(value));
        inner_.put(family, key, value);
    }
    void merge(layout::Family family, std::string_view key,
               std::string_view operand) override {
        inner_.merge(family, key, operand);
    }
    void delete_range(layout::Family family, std::string_view begin,
                      std::string_view end) override {
        inner_.delete_range(family, begin, end);
    }
    void del(layout::Family family, std::string_view key) override {
        inner_.del(family, key);
    }

    std::vector<std::pair<std::string, std::string>> puts;

   private:
    IndexWrite& inner_;
};

layout::FileRecord file_record(int file_id) {
    layout::FileRecord record;
    record.file_id = static_cast<std::uint32_t>(file_id);
    record.mtime = 111;
    record.hash = 222;
    record.size = 333;
    return record;
}

/// [ext][kind][file_id][path][0x00][granule]
std::string granule_key_of(layout::Ext ext, std::uint8_t kind, int file_id,
                           std::string_view path, std::uint32_t granule) {
    std::string key = layout::path_prefix(
        ext, kind, static_cast<std::uint32_t>(file_id), path);
    append_u32_be(key, granule);
    return key;
}

}  // namespace

TEST_CASE("every index key maps to its namespace, scope and inner key") {
    const auto check_data = [](layout::Ext ext, std::uint8_t kind,
                               const std::string& key, std::uint32_t file_id,
                               const std::string& plugin) {
        const auto target = store_layout::target_of(key);
        REQUIRE(target.has_value());
        CHECK(target->ext == ext);
        CHECK(target->kind == kind);
        CHECK(target->file_id == file_id);
        CHECK(target->plugin == plugin);
        CHECK(target->inner_key == inner_of(key));
    };

    check_data(layout::Ext::Members, layout::members::MEMBER,
               layout::granule_key(layout::Ext::Members,
                                   layout::members::MEMBER, 7, 3),
               7, "");
    check_data(
        layout::Ext::Members, layout::members::METADATA,
        layout::file_prefix(layout::Ext::Members, layout::members::METADATA, 7),
        7, "");
    check_data(layout::Ext::Members, layout::members::RESTART,
               layout::granule_key(layout::Ext::Members,
                                   layout::members::RESTART, 7, 0),
               7, "");
    check_data(
        layout::Ext::Bloom, layout::path_kind::PATHS,
        layout::file_prefix(layout::Ext::Bloom, layout::path_kind::PATHS, 7) +
            "args.name",
        7, "");
    check_data(layout::Ext::Bloom, layout::path_kind::DATA,
               granule_key_of(layout::Ext::Bloom, layout::path_kind::DATA, 7,
                              "args.name", 4),
               7, "");
    check_data(
        layout::Ext::Bloom, layout::path_kind::FILE,
        layout::file_prefix(layout::Ext::Bloom, layout::path_kind::FILE, 7) +
            "args.name",
        7, "");
    check_data(layout::Ext::Postings, layout::path_kind::IN_FILE,
               layout::path_prefix(layout::Ext::Postings,
                                   layout::path_kind::IN_FILE, 7, "args.name"),
               7, "");
    check_data(
        layout::Ext::Postings, layout::path_kind::IN_GRANULE,
        layout::path_prefix(layout::Ext::Postings,
                            layout::path_kind::IN_GRANULE, 7, "args.name") +
            std::string(8, '\0'),
        7, "");
    check_data(layout::Ext::Stats, layout::stats_kind::CHUNK_STATS,
               layout::granule_key(layout::Ext::Stats,
                                   layout::stats_kind::CHUNK_STATS, 7, 1),
               7, "");
    check_data(
        layout::Ext::Agg, layout::agg_kind::PARTIAL,
        layout::granule_key(layout::Ext::Agg, layout::agg_kind::PARTIAL, 7, 2),
        7, "");

    check_data(
        layout::Ext::Plugin, layout::path_kind::PATHS,
        layout::file_prefix(layout::Ext::Plugin, layout::path_kind::PATHS, 7) +
            "alpha",
        7, "alpha");
    check_data(layout::Ext::Plugin, layout::path_kind::DATA,
               granule_key_of(layout::Ext::Plugin, layout::path_kind::DATA, 7,
                              "alpha", 4),
               7, "alpha");
    check_data(
        layout::Ext::Plugin, layout::path_kind::FILE,
        layout::file_prefix(layout::Ext::Plugin, layout::path_kind::FILE, 7) +
            "alpha",
        7, "alpha");

    check_data(layout::Ext::Agg, layout::agg_kind::META,
               layout::file_prefix(layout::Ext::Agg, layout::agg_kind::META,
                                   layout::INDEX_WIDE) +
                   "config",
               layout::INDEX_WIDE, "");

    SUBCASE("only the index keys of a known extension and kind are data") {
        CHECK_FALSE(store_layout::target_of(std::string{}).has_value());
        CHECK_FALSE(store_layout::target_of(layout::format_key()).has_value());
        CHECK_FALSE(store_layout::target_of(
                        layout::kind_prefix(layout::Ext::Members, 9))
                        .has_value());
        CHECK_FALSE(store_layout::target_of(
                        layout::file_prefix(layout::Ext::Members,
                                            layout::members::MEMBER, 7)
                            .substr(0, 6))
                        .has_value());
    }

    SUBCASE("the namespaces of the extensions and plugins") {
        CHECK(store_layout::namespace_name(layout::Ext::Members) ==
              "dftu.idx.core.members");
        CHECK(store_layout::namespace_name(layout::Ext::Stats) ==
              "dftu.idx.dft.stats");
        CHECK(store_layout::plugin_namespace_name("alpha") ==
              "dftu.idx.plugin.alpha");
        CHECK(store_layout::plugin_namespace_name("alpha") !=
              store_layout::plugin_namespace_name("beta"));
        for (layout::Ext ext : dftracer::utils::index::store::ALL_EXTENSIONS) {
            CHECK(store_layout::namespace_version(ext) ==
                  layout::ext_version(ext));
        }
    }

    SUBCASE("host keys decode to what they name") {
        const auto format = store_layout::host_target_of(layout::format_key());
        REQUIRE(format.has_value());
        CHECK(format->kind == store_layout::HostKind::Format);

        const auto registry = store_layout::host_target_of(
            layout::ext_registry_key(layout::Ext::Stats));
        REQUIRE(registry.has_value());
        CHECK(registry->kind == store_layout::HostKind::ExtRegistry);
        REQUIRE(registry->ext.has_value());
        CHECK(*registry->ext == layout::Ext::Stats);

        const auto by_path = store_layout::host_target_of(
            layout::file_by_path_key("/traces/app.pfw.gz"));
        REQUIRE(by_path.has_value());
        CHECK(by_path->kind == store_layout::HostKind::FileByPath);
        CHECK(by_path->path == "/traces/app.pfw.gz");

        const auto by_id =
            store_layout::host_target_of(layout::file_by_id_key(9));
        REQUIRE(by_id.has_value());
        CHECK(by_id->kind == store_layout::HostKind::FileById);
        CHECK(by_id->file_id == 9);

        const auto next =
            store_layout::host_target_of(layout::next_file_id_key());
        REQUIRE(next.has_value());
        CHECK(next->kind == store_layout::HostKind::NextFileId);

        const auto manifest = store_layout::host_target_of(
            layout::manifest_key(9, layout::Ext::Counts));
        REQUIRE(manifest.has_value());
        CHECK(manifest->kind == store_layout::HostKind::Manifest);
        CHECK(manifest->file_id == 9);
        REQUIRE(manifest->ext.has_value());
        CHECK(*manifest->ext == layout::Ext::Counts);
        CHECK(manifest->plugin.empty());

        const auto plugin = store_layout::host_target_of(
            layout::plugin_manifest_key(9, "alpha"));
        REQUIRE(plugin.has_value());
        CHECK(plugin->kind == store_layout::HostKind::PluginManifest);
        CHECK(plugin->file_id == 9);
        CHECK(plugin->plugin == "alpha");
        REQUIRE(plugin->ext.has_value());
        CHECK(*plugin->ext == layout::Ext::Plugin);

        CHECK_FALSE(store_layout::host_target_of(
                        layout::granule_key(layout::Ext::Members,
                                            layout::members::MEMBER, 7, 3))
                        .has_value());
        CHECK_FALSE(store_layout::host_target_of("").has_value());
    }
}

TEST_CASE("a write through the records lands in one copy per extension") {
    dftu_test::ScopedTestDir dir("store_index_write");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    REQUIRE(file_id > 0);

    gzip::GzipMemberRecord member;
    member.member_idx = 0;
    member.c_offset = 40;
    member.c_size = 100;
    member.uc_size = 400;
    member.first_line_num = 1;
    member.last_line_num = 10;
    schemas::ChunkStatistics stats;
    stats.total_events = 5;

    std::vector<std::pair<std::string, std::string>> written;
    {
        auto inner = root->begin_write();
        RecordingWrite write(*inner);
        records::put_file_record(write, path, file_record(file_id));
        records::put_file_metadata(write, file_id, 4096, 100, 8192, false);
        records::put_gzip_member(write, file_id, member);
        records::put_restart_window(write, file_id, 0, std::string(64, 'w'));
        records::put_path(write, layout::Ext::Bloom, file_id, "args.name");
        records::put_path_granule(write, layout::Ext::Bloom, file_id,
                                  "args.name", 0, "granule");
        records::put_path_granule(write, layout::Ext::Bloom, file_id,
                                  "args.name", 1, "granule-1");
        records::put_path_file(write, layout::Ext::Bloom, file_id, "args.name",
                               "file");
        records::put_posting(write, file_id, "args.name", 0x1234);
        records::put_posting_granule(write, file_id, "args.name", 0x1234, 0);
        records::put_chunk_statistics(write, file_id, 0, stats);
        records::put_chunk_metadata(write, file_id, 0, ChunkMetadata{});
        records::put_profile(write, file_id, "profile-1", PARAMS);
        records::put_catalog_path(write, file_id, "args.name",
                                  PathStat{PathType::String, 1, 7});
        records::put_manifest(write, file_id, layout::Ext::Members, PARAMS);
        records::put_manifest(write, file_id, layout::Ext::Bloom, PARAMS);
        records::put_manifest(write, file_id, layout::Ext::Metadata, PARAMS);
        records::put_manifest(write, file_id, layout::Ext::Postings, PARAMS);
        records::put_manifest(write, file_id, layout::Ext::Stats, PARAMS);
        records::put_manifest(write, file_id, layout::Ext::Catalog, PARAMS);
        written = write.puts;
        inner->commit();
    }

    std::map<layout::Ext, std::map<std::string, std::string>> expected;
    for (const auto& [key, value] : written) {
        const auto target = store_layout::target_of(key);
        if (!target) continue;
        REQUIRE(target->plugin.empty());
        expected[target->ext].emplace(target->inner_key, value);
    }
    for (layout::Ext ext :
         {layout::Ext::Members, layout::Ext::Bloom, layout::Ext::Metadata,
          layout::Ext::Postings, layout::Ext::Stats, layout::Ext::Catalog,
          layout::Ext::Profile}) {
        CHECK(expected.count(ext) == 1);
    }

    const store::ScopeKey scope = file_scope(TRACE);
    for (const auto& [ext, rows] : expected) {
        const store::NamespaceSpec spec = spec_of(ext);
        CHECK(rows_of(*root, spec, scope) == rows);
        const store::CopyInfo info = root->snapshot()->copy_info(spec, scope);
        CHECK(info.state == store::ScopeState::Ready);
        CHECK(info.params_hash == PARAMS);
        CHECK(info.generation == 1);
        REQUIRE(info.sources.size() == 1);
        CHECK(info.sources[0].mtime == 111);
        CHECK(info.sources[0].hash == 222);
        CHECK(info.sources[0].size == 333);
    }

    SUBCASE("the file record and the manifests stay out of the data scopes") {
        const auto members =
            rows_of(*root, spec_of(layout::Ext::Members), scope);
        CHECK(members.count(inner_of(layout::file_by_path_key(path))) == 0U);
        CHECK(members.count(inner_of(
                  layout::manifest_key(static_cast<std::uint32_t>(file_id),
                                       layout::Ext::Members))) == 0U);
    }
}

TEST_CASE("a rebuild in a new store root replaces one extension of one file") {
    dftu_test::ScopedTestDir dir("store_index_rebuild");
    const std::string path(TRACE);
    int file_id = 0;
    std::map<std::string, std::string> members_before;
    {
        auto root = StoreRoot::get_or_open(dir.str());
        file_id = root->file_id_for(path);
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_file_metadata(*write, file_id, 4096, 100, 8192, false);
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        records::put_path(*write, layout::Ext::Bloom, file_id, "args.name");
        records::put_path_granule(*write, layout::Ext::Bloom, file_id,
                                  "args.name", 0, "old");
        records::put_manifest(*write, file_id, layout::Ext::Bloom, 1);
        write->commit();
        members_before =
            rows_of(*root, layout::Ext::Members, file_scope(TRACE));
        root->close();
    }
    REQUIRE_FALSE(members_before.empty());

    auto root = StoreRoot::get_or_open(dir.str());
    CHECK(root->file_id_for(path) == file_id);
    auto write = root->begin_write();
    records::clear_file(*write, layout::Ext::Bloom, file_id);
    records::put_path(*write, layout::Ext::Bloom, file_id, "args.name");
    records::put_path_granule(*write, layout::Ext::Bloom, file_id, "args.name",
                              1, "new");
    records::put_manifest(*write, file_id, layout::Ext::Bloom, 2);
    write->commit();

    const store::ScopeKey scope = file_scope(TRACE);
    const auto bloom = rows_of(*root, layout::Ext::Bloom, scope);
    const std::string old_granule = inner_of(granule_key_of(
        layout::Ext::Bloom, layout::path_kind::DATA, file_id, "args.name", 0));
    const std::string new_granule = inner_of(granule_key_of(
        layout::Ext::Bloom, layout::path_kind::DATA, file_id, "args.name", 1));
    CHECK(bloom.count(old_granule) == 0);
    CHECK(bloom.count(new_granule) == 1);
    CHECK(bloom.size() == 2);
    for (const auto& [key, value] : bloom) {
        CHECK(value.find("old") == std::string::npos);
    }
    CHECK(rows_of(*root, layout::Ext::Members, scope) == members_before);

    const store::CopyInfo info =
        root->snapshot()->copy_info(spec_of(layout::Ext::Bloom), scope);
    CHECK(info.state == store::ScopeState::Ready);
    CHECK(info.generation == 2);
    CHECK(info.params_hash == 2);
}

TEST_CASE("a clear and a rewrite in one store root replace the copy") {
    dftu_test::ScopedTestDir dir("store_index_rewrite");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    {
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        records::put_path(*write, layout::Ext::Bloom, file_id, "args.name");
        records::put_path_granule(*write, layout::Ext::Bloom, file_id,
                                  "args.name", 0, "old");
        records::put_manifest(*write, file_id, layout::Ext::Bloom, 1);
        write->commit();
    }

    auto write = root->begin_write();
    records::clear_file(*write, layout::Ext::Bloom, file_id);
    records::put_path(*write, layout::Ext::Bloom, file_id, "args.name");
    records::put_path_granule(*write, layout::Ext::Bloom, file_id, "args.name",
                              1, "new");
    records::put_manifest(*write, file_id, layout::Ext::Bloom, 2);
    write->commit();

    const store::ScopeKey scope = file_scope(TRACE);
    const auto bloom = rows_of(*root, layout::Ext::Bloom, scope);
    const std::string old_granule = inner_of(granule_key_of(
        layout::Ext::Bloom, layout::path_kind::DATA, file_id, "args.name", 0));
    const std::string new_granule = inner_of(granule_key_of(
        layout::Ext::Bloom, layout::path_kind::DATA, file_id, "args.name", 1));
    CHECK(bloom.count(old_granule) == 0U);
    CHECK(bloom.count(new_granule) == 1U);
    CHECK(bloom.size() == 2);

    const store::CopyInfo info =
        root->snapshot()->copy_info(spec_of(layout::Ext::Bloom), scope);
    CHECK(info.state == store::ScopeState::Ready);
    CHECK(info.params_hash == 2);
}

TEST_CASE("rebuilding one plugin leaves the other plugin of the file whole") {
    dftu_test::ScopedTestDir dir("store_index_plugins");
    const std::string path(TRACE);
    int file_id = 0;
    const store::NamespaceSpec beta_spec =
        spec_of(store_layout::plugin_namespace_name("beta"), 7);
    std::map<std::string, std::string> beta_before;
    {
        auto root = StoreRoot::get_or_open(dir.str());
        file_id = root->file_id_for(path);
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        for (const char* name : {"alpha", "beta"}) {
            records::put_path(*write, layout::Ext::Plugin, file_id, name);
            records::put_path_granule(*write, layout::Ext::Plugin, file_id,
                                      name, 0, "old");
        }
        records::put_plugin_manifest(*write, file_id, "alpha", 3, 10);
        records::put_plugin_manifest(*write, file_id, "beta", 7, 20);
        write->commit();
        beta_before = rows_of(*root, beta_spec, file_scope(TRACE));
        root->close();
    }
    REQUIRE(beta_before.size() == 2);

    auto root = StoreRoot::get_or_open(dir.str());
    CHECK(root->file_id_for(path) == file_id);
    auto write = root->begin_write();
    records::clear_plugin(*write, file_id, "alpha");
    records::put_path(*write, layout::Ext::Plugin, file_id, "alpha");
    records::put_path_granule(*write, layout::Ext::Plugin, file_id, "alpha", 1,
                              "new");
    records::put_plugin_manifest(*write, file_id, "alpha", 3, 30);
    write->commit();

    const store::ScopeKey scope = file_scope(TRACE);
    const store::NamespaceSpec alpha_spec =
        spec_of(store_layout::plugin_namespace_name("alpha"), 3);
    const auto alpha = rows_of(*root, alpha_spec, scope);
    CHECK(alpha.size() == 2);
    for (const auto& [key, value] : alpha) {
        CHECK(value.find("old") == std::string::npos);
    }
    const store::CopyInfo alpha_info =
        root->snapshot()->copy_info(alpha_spec, scope);
    CHECK(alpha_info.state == store::ScopeState::Ready);
    CHECK(alpha_info.params_hash == 30);
    CHECK(alpha_info.generation == 2);

    CHECK(rows_of(*root, beta_spec, scope) == beta_before);
    const store::CopyInfo beta_info =
        root->snapshot()->copy_info(beta_spec, scope);
    CHECK(beta_info.state == store::ScopeState::Ready);
    CHECK(beta_info.params_hash == 20);
    CHECK(beta_info.generation == 1);
}

TEST_CASE("a failed manifest reads as a failed copy with its params hash") {
    dftu_test::ScopedTestDir dir("store_index_failed");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    auto write = root->begin_write();
    records::put_file_record(*write, path, file_record(file_id));
    records::put_manifest(*write, file_id, layout::Ext::Zonemap, 0x77,
                          layout::ExtStatus::Failed);
    write->commit();

    const store::CopyInfo info = root->snapshot()->copy_info(
        spec_of(layout::Ext::Zonemap), file_scope(TRACE));
    CHECK(info.state == store::ScopeState::Failed);
    CHECK(info.params_hash == 0x77);
    CHECK(info.generation == 1);
}

TEST_CASE("clearing an extension without data drops its copy") {
    dftu_test::ScopedTestDir dir("store_index_clear");
    const std::string path(TRACE);
    int file_id = 0;
    {
        auto root = StoreRoot::get_or_open(dir.str());
        file_id = root->file_id_for(path);
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        records::put_path(*write, layout::Ext::Zonemap, file_id, "args.name");
        records::put_manifest(*write, file_id, layout::Ext::Zonemap, 1);
        write->commit();
        CHECK(root->snapshot()
                  ->copy_info(spec_of(layout::Ext::Zonemap), file_scope(TRACE))
                  .state == store::ScopeState::Ready);
        root->close();
    }

    auto root = StoreRoot::get_or_open(dir.str());
    CHECK(root->file_id_for(path) == file_id);
    auto write = root->begin_write();
    records::clear_file(*write, layout::Ext::Zonemap, file_id);
    write->commit();

    const store::CopyInfo info = root->snapshot()->copy_info(
        spec_of(layout::Ext::Zonemap), file_scope(TRACE));
    CHECK(info.state == store::ScopeState::Absent);
    CHECK(info.generation == 0);
    CHECK(rows_of(*root, layout::Ext::Zonemap, file_scope(TRACE)).empty());
}

TEST_CASE("merge throws and a delete inside a committed copy throws") {
    dftu_test::ScopedTestDir dir("store_index_unsupported");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    {
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        write->commit();
    }

    auto write = root->begin_write();
    CHECK_THROWS_WITH_AS(
        write->merge(
            layout::Family::SystemMetrics,
            layout::file_prefix(layout::Ext::Zonemap, layout::path_kind::PATHS,
                                static_cast<std::uint32_t>(file_id)),
            std::string(8, '\0')),
        doctest::Contains("merge is not supported"), std::runtime_error);

    records::put_path(*write, layout::Ext::Zonemap, file_id, "args.name");
    records::put_manifest(*write, file_id, layout::Ext::Zonemap, 1);
    write->commit();
    CHECK(root->snapshot()
              ->copy_info(spec_of(layout::Ext::Zonemap), file_scope(TRACE))
              .state == store::ScopeState::Ready);

    auto later = root->begin_write();
    CHECK_THROWS_WITH_AS(
        records::clear_path(*later, layout::Ext::Zonemap, file_id, "args.name"),
        doctest::Contains("a delete inside a committed copy"),
        std::runtime_error);
}

TEST_CASE("file ids are stable and a new store root seeds them in path order") {
    dftu_test::ScopedTestDir dir("store_index_ids");
    const std::vector<std::string> paths = {
        "/traces/a.pfw.gz", "/traces/b.pfw.gz", "/traces/c.pfw.gz"};
    {
        auto root = StoreRoot::get_or_open(dir.str());
        // Written in reverse, so the ids of this root differ from the path
        // order a new root seeds.
        for (auto at = paths.rbegin(); at != paths.rend(); ++at) {
            const std::string& path = *at;
            const int file_id = root->file_id_for(path);
            auto write = root->begin_write();
            records::put_file_record(*write, path, file_record(file_id));
            records::put_file_metadata(*write, file_id, 4096, 100, 8192, false);
            records::put_manifest(*write, file_id, layout::Ext::Members,
                                  PARAMS);
            write->commit();
        }
        for (std::size_t index = 0; index < paths.size(); ++index) {
            CHECK(root->file_id_for(paths[index]) ==
                  static_cast<int>(paths.size() - index));
        }
        root->close();
    }

    auto root = StoreRoot::get_or_open(dir.str());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        CHECK(root->file_id_for(paths[index]) == static_cast<int>(index) + 1);
        CHECK(root->path_of(static_cast<int>(index) + 1) ==
              std::optional<std::string>(paths[index]));
    }
    CHECK(root->file_id_for("/traces/d.pfw.gz") == 4);
    CHECK_FALSE(root->path_of(99).has_value());
}

TEST_CASE("a store root takes writes again after close") {
    dftu_test::ScopedTestDir dir("store_index_reopen");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    {
        auto write = root->begin_write();
        records::put_file_record(*write, path, file_record(file_id));
        records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS);
        write->commit();
    }
    root->close();

    auto write = root->begin_write();
    records::clear_file(*write, layout::Ext::Members, file_id);
    records::put_file_metadata(*write, file_id, 4096, 100, 8192, false);
    records::put_manifest(*write, file_id, layout::Ext::Members, PARAMS + 1);
    records::put_path(*write, layout::Ext::Zonemap, file_id, "args.name");
    records::put_manifest(*write, file_id, layout::Ext::Zonemap, 1);
    write->commit();

    const store::CopyInfo members = root->snapshot()->copy_info(
        spec_of(layout::Ext::Members), file_scope(TRACE));
    CHECK(members.state == store::ScopeState::Ready);
    CHECK(members.generation == 2);
    CHECK(members.params_hash == PARAMS + 1);
    CHECK(rows_of(*root, layout::Ext::Zonemap, file_scope(TRACE)).size() == 1);
}

TEST_CASE("a manifest with no data writes a copy of one marker key") {
    dftu_test::ScopedTestDir dir("store_index_marker");
    auto root = StoreRoot::get_or_open(dir.str());
    const std::string path(TRACE);
    const int file_id = root->file_id_for(path);
    auto write = root->begin_write();
    records::put_file_record(*write, path, file_record(file_id));
    records::put_manifest(*write, file_id, layout::Ext::Catalog, 5);
    write->commit();

    const store::CopyInfo info = root->snapshot()->copy_info(
        spec_of(layout::Ext::Catalog), file_scope(TRACE));
    CHECK(info.state == store::ScopeState::Ready);
    CHECK(info.params_hash == 5);
    const auto rows = rows_of(*root, layout::Ext::Catalog, file_scope(TRACE));
    REQUIRE(rows.size() == 1);
    CHECK(rows.begin()->first == std::string(1, '\xFF'));
    CHECK(rows.begin()->second.empty());
}

TEST_CASE("commits from many threads and a close in between all land") {
    dftu_test::ScopedTestDir dir("store_index_threads");
    auto root = StoreRoot::get_or_open(dir.str());
    constexpr int THREADS = 4;
    constexpr int ROUNDS = 10;
    std::vector<int> ids;
    for (int t = 0; t < THREADS; ++t) {
        ids.push_back(root->file_id_for("/traces/t" + std::to_string(t)));
    }
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&root, &ids, t] {
            const int file_id = ids[static_cast<std::size_t>(t)];
            for (int round = 0; round < ROUNDS; ++round) {
                auto write = root->begin_write();
                records::clear_file(*write, layout::Ext::Catalog, file_id);
                records::put_catalog_path(
                    *write, file_id, "r" + std::to_string(round), PathStat{});
                records::put_manifest(*write, file_id, layout::Ext::Catalog,
                                      static_cast<std::uint64_t>(round));
                write->commit();
            }
        });
    }
    for (int close = 0; close < 3; ++close) root->close();
    for (std::thread& thread : threads) thread.join();
    root->close();

    for (int t = 0; t < THREADS; ++t) {
        const auto scope = file_scope("/traces/t" + std::to_string(t));
        const store::CopyInfo info =
            root->snapshot()->copy_info(spec_of(layout::Ext::Catalog), scope);
        CHECK(info.state == store::ScopeState::Ready);
        CHECK(info.params_hash == ROUNDS - 1);
        const auto rows = rows_of(*root, layout::Ext::Catalog, scope);
        REQUIRE(rows.size() == 1);
        CHECK(rows.begin()->first.find("r" + std::to_string(ROUNDS - 1)) !=
              std::string::npos);
    }
}
