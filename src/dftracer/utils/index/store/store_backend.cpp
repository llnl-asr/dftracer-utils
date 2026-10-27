#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_backend.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

namespace {

/// A copy with no entries announces none, so a Ready copy that holds no key
/// carries this one entry instead: the inner key [0xFF], an empty value.
constexpr std::string_view EMPTY_COPY_MARKER = "\xFF";

std::string error_text(const dstd::Error& error) {
    const dstd::String text = error.format();
    return std::string(text.data(), text.size());
}

[[noreturn]] void throw_store_error(std::string_view what,
                                    const dstd::Error& error) {
    throw std::runtime_error(std::string(what) + ": " + error_text(error));
}

void check(std::string_view what,
           const dstd::Expected<void, dstd::Error>& result) {
    if (!result) throw_store_error(what, result.error());
}

/// Commits one batch, waiting for extent bytes and retrying while the writer
/// reports Busy.
coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_with_space(
    kv::Writer& writer, kv::Batch batch) {
    auto result = writer.commit(std::move(batch));
    while (!result && result.error().is_code(kv::WriterErrc::Busy)) {
        co_await writer.space();
        result = writer.commit(std::move(batch));
    }
    co_return result;
}

kv::NamespaceSpec make_spec(const std::string& name,
                            std::uint32_t format_version) {
    kv::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = format_version;
    return spec;
}

/// The namespace of the extension every file has data in, which lists the
/// files the store holds.
kv::NamespaceSpec members_spec() {
    return make_spec(store_layout::namespace_name(layout::Ext::Members),
                     store_layout::namespace_version(layout::Ext::Members));
}

bool same_sources(const dstd::Vector<kv::SourceStat>& previous,
                  const dstd::Vector<kv::SourceStat>& next) {
    if (previous.size() != next.size()) return false;
    for (std::size_t i = 0; i < previous.size(); ++i) {
        if (previous[i].mtime != next[i].mtime ||
            previous[i].size != next[i].size ||
            previous[i].hash != next[i].hash) {
            return false;
        }
    }
    return true;
}

std::mutex& root_registry_mutex() {
    static std::mutex value;
    return value;
}

std::map<std::string, std::weak_ptr<StoreRoot>>& root_registry() {
    static std::map<std::string, std::weak_ptr<StoreRoot>> value;
    return value;
}

/// The key of one index root: absolute, without a trailing separator, so two
/// spellings of one directory share one StoreRoot.
std::string root_key(const std::string& index_root) {
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(index_root, error);
    if (error) return index_root;
    path = path.lexically_normal();
    if (!path.has_filename() && path.has_parent_path()) {
        path = path.parent_path();
    }
    return path.string();
}

/// The full key of a buffered inner key of (ext, kind, file_id).
std::string full_key_of(layout::Ext ext, std::uint32_t file_id,
                        const std::string& inner_key) {
    std::string key = layout::file_prefix(
        ext, static_cast<std::uint8_t>(inner_key[0]), file_id);
    key.append(inner_key, 1, std::string::npos);
    return key;
}

}  // namespace

StoreRoot::StoreRoot(std::string index_root)
    : index_root_(std::move(index_root)) {
    store_dir_ = (std::filesystem::path(index_root_) / "store").string();
}

std::shared_ptr<StoreRoot> StoreRoot::get_or_open(
    const std::string& index_root) {
    const std::string key = root_key(index_root);
    std::lock_guard<std::mutex> lock(root_registry_mutex());
    auto& roots = root_registry();
    auto found = roots.find(key);
    if (found != roots.end()) {
        if (auto live = found->second.lock()) return live;
    }
    auto created = std::shared_ptr<StoreRoot>(new StoreRoot(key));
    roots.insert_or_assign(key, created);
    return created;
}

StoreRoot::~StoreRoot() {
    // A destructor cannot report the store error close() throws.
    try {
        close();
    } catch (const std::exception&) {
    }
}

void StoreRoot::ensure_dir_locked() {
    std::error_code error;
    std::filesystem::create_directories(store_dir_, error);
    if (error) {
        throw std::runtime_error(
            "store index: cannot create the store "
            "directory " +
            store_dir_ + ": " + error.message());
    }
}

void StoreRoot::ensure_writer_locked() {
    if (writer_) return;
    ensure_dir_locked();
    runtime_ = default_runtime_shared();
    auto created = kv::Writer::create(store_dir_, *runtime_->executor());
    if (!created)
        throw_store_error("store index: open the writer", created.error());
    writer_ = std::move(*created);
    // The view a read took before this writer holds none of its copies.
    snapshot_taken_ = false;
}

dstd::Expected<void, dstd::Error> StoreRoot::run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    if (!runtime_)
        throw std::runtime_error("store index: no runtime to wait on");
    return runtime_->submit(std::move(task)).get();
}

std::shared_ptr<const kv::Snapshot> StoreRoot::open_snapshot_locked() {
    // A read-only root that was never written has no store directory; only a
    // write creates it, so such a root reads as an empty store.
    if (writer_) {
        ensure_dir_locked();
    } else {
        std::error_code missing;
        if (!std::filesystem::exists(store_dir_, missing)) return nullptr;
    }
    auto opened = kv::Snapshot::open(store_dir_);
    if (!opened)
        throw_store_error("store index: open the store", opened.error());
    return std::make_shared<const kv::Snapshot>(std::move(*opened));
}

std::shared_ptr<const kv::Snapshot> StoreRoot::snapshot_locked() {
    if (snapshot_taken_ && snapshot_version_ == version_) return snapshot_;
    snapshot_ = open_snapshot_locked();
    snapshot_version_ = version_;
    snapshot_taken_ = true;
    return snapshot_;
}

std::shared_ptr<const kv::Snapshot> StoreRoot::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_locked();
}

void StoreRoot::seed_file_ids_locked() {
    if (seeded_) return;
    std::error_code missing;
    if (!std::filesystem::exists(store_dir_, missing)) {
        seeded_ = true;
        return;
    }
    const auto held = snapshot_locked();
    if (!held) {
        seeded_ = true;
        return;
    }
    const auto listed = held->scopes(members_spec());
    if (!listed) {
        throw_store_error("store index: list the files of the store",
                          listed.error());
    }
    std::vector<std::string> paths;
    paths.reserve(listed->size());
    for (const kv::ScopeEntry& entry : *listed) {
        if (entry.scope.kind != kv::ScopeKind::File) continue;
        for (const dstd::String& path : entry.scope.paths) {
            paths.emplace_back(path.data(), path.size());
        }
    }
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const std::string& path : paths) {
        const int id = next_file_id_++;
        ids_by_path_.emplace(path, id);
        paths_by_id_.emplace(id, path);
    }
    seeded_ = true;
}

int StoreRoot::file_id_for(std::string_view logical_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    std::string path(logical_path);
    auto at = ids_by_path_.find(path);
    if (at != ids_by_path_.end()) return at->second;
    const int id = next_file_id_++;
    ids_by_path_.emplace(path, id);
    paths_by_id_.emplace(id, path);
    return id;
}

std::optional<int> StoreRoot::find_file_id(std::string_view logical_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    auto at = ids_by_path_.find(std::string(logical_path));
    if (at == ids_by_path_.end()) return std::nullopt;
    return at->second;
}

int StoreRoot::reserve_file_id_range(std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    const int first = next_file_id_;
    next_file_id_ += static_cast<int>(count);
    return first;
}

int StoreRoot::next_file_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    return next_file_id_;
}

std::optional<std::string> StoreRoot::path_of(int file_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    auto at = paths_by_id_.find(file_id);
    if (at == paths_by_id_.end()) return std::nullopt;
    return at->second;
}

void StoreRoot::bind_file_id(std::string_view logical_path, int file_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    std::string path(logical_path);
    auto at = ids_by_path_.find(path);
    if (at != ids_by_path_.end()) {
        if (at->second == file_id) return;
        throw std::runtime_error("store index: " + path + " has file id " +
                                 std::to_string(at->second) + ", not " +
                                 std::to_string(file_id));
    }
    auto other = paths_by_id_.find(file_id);
    if (other != paths_by_id_.end()) {
        throw std::runtime_error("store index: file id " +
                                 std::to_string(file_id) + " names " +
                                 other->second + ", not " + path);
    }
    ids_by_path_.emplace(path, file_id);
    paths_by_id_.emplace(file_id, path);
    if (file_id >= next_file_id_) next_file_id_ = file_id + 1;
}

kv::Writer& StoreRoot::writer() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_writer_locked();
    return *writer_;
}

kv::NamespaceId StoreRoot::namespace_id(const std::string& name,
                                        std::uint32_t version) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_writer_locked();
    auto at = namespaces_.find(name);
    if (at != namespaces_.end()) {
        if (at->second.version != version) {
            throw std::runtime_error("store index: namespace " + name +
                                     " is at version " +
                                     std::to_string(at->second.version) +
                                     ", not " + std::to_string(version));
        }
        return at->second.id;
    }
    auto added = writer_->add_namespace(make_spec(name, version));
    if (!added)
        throw_store_error("store index: add a namespace", added.error());
    NamespaceEntry entry;
    entry.id = *added;
    entry.version = version;
    namespaces_.emplace(name, entry);
    return entry.id;
}

std::uint32_t StoreRoot::resolved_namespace_version(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto at = namespaces_.find(name);
        if (at != namespaces_.end()) return at->second.version;
    }
    const auto held = snapshot();
    if (!held) return 0;
    const auto declared = held->namespaces();
    if (!declared) return 0;
    std::uint32_t highest = 0;
    for (const kv::NamespaceDecl& entry : *declared) {
        const std::string_view known(entry.name.data(), entry.name.size());
        if (known == name) highest = std::max(highest, entry.format_version);
    }
    return highest;
}

bool StoreRoot::sources_of(const kv::ScopeKey& scope,
                           const std::map<std::string, kv::SourceStat>& stats,
                           dstd::Vector<kv::SourceStat>& sources) {
    if (scope.kind != kv::ScopeKind::File) return false;
    const auto held = snapshot();
    if (!held) return false;
    dstd::Vector<kv::SourceStat> found;
    for (const dstd::String& path : scope.paths) {
        const std::string key(path.data(), path.size());
        auto at = stats.find(key);
        if (at != stats.end()) {
            found.push_back(at->second);
            continue;
        }
        const kv::CopyInfo info =
            held->copy_info(members_spec(), kv::ScopeKey::file(key));
        if (info.sources.size() != 1) return false;
        found.push_back(info.sources[0]);
    }
    if (found.empty()) return false;
    sources = std::move(found);
    return true;
}

void StoreRoot::mark_committed(const std::string& name,
                               const kv::ScopeKey& scope,
                               std::uint64_t params_hash,
                               const dstd::Vector<kv::SourceStat>* sources) {
    CommittedCopy committed;
    committed.params_hash = params_hash;
    if (sources != nullptr) committed.sources = *sources;
    std::lock_guard<std::mutex> lock(mutex_);
    committed_.insert_or_assign({name, scope}, std::move(committed));
}

void StoreRoot::forget_committed(const std::string& name,
                                 const kv::ScopeKey& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    committed_.erase({name, scope});
}

bool StoreRoot::was_committed(const std::string& name,
                              const kv::ScopeKey& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_.find({name, scope}) != committed_.end();
}

std::optional<StoreRoot::CommittedCopy> StoreRoot::committed_copy(
    const std::string& name, const kv::ScopeKey& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto at = committed_.find({name, scope});
    if (at == committed_.end()) return std::nullopt;
    return at->second;
}

void StoreRoot::bump_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++version_;
}

void StoreRoot::close_locked() {
    if (!writer_) return;
    const auto closed = run(writer_->close());
    writer_.reset();
    // Namespace ids and committed copies belong to the closed writer.
    namespaces_.clear();
    committed_.clear();
    ++version_;
    if (!closed)
        throw_store_error("store index: close the store", closed.error());
}

void StoreRoot::close() {
    std::unique_lock<std::shared_mutex> gate(gate_);
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

std::unique_ptr<StoreIndexWrite> StoreRoot::begin_write() {
    return std::unique_ptr<StoreIndexWrite>(
        new StoreIndexWrite(shared_from_this()));
}

int StoreIndexWrite::file_id_for(std::string_view logical_path) {
    return root_->file_id_for(logical_path);
}

StoreIndexWrite::StoreIndexWrite(std::shared_ptr<StoreRoot> root)
    : root_(std::move(root)) {}

kv::ScopeKey StoreIndexWrite::scope_of(std::uint32_t file_id) {
    if (file_id == layout::INDEX_WIDE) return kv::ScopeKey::root();
    auto path = root_->path_of(static_cast<int>(file_id));
    if (!path) {
        throw std::runtime_error("store index: file id " +
                                 std::to_string(file_id) +
                                 " is not bound to a path");
    }
    return kv::ScopeKey::file(*path);
}

std::string StoreIndexWrite::namespace_of(
    const store_layout::KeyTarget& target) const {
    if (target.ext == layout::Ext::Plugin) {
        return store_layout::plugin_namespace_name(target.plugin);
    }
    return store_layout::namespace_name(target.ext);
}

StoreIndexWrite::PendingCopy& StoreIndexWrite::pending_copy(
    layout::Ext ext, std::uint32_t file_id, std::string_view plugin) {
    if (last_copy_ != nullptr && last_ext_ == ext && last_file_id_ == file_id &&
        last_plugin_ == plugin) {
        return *last_copy_;
    }
    PendingCopy pending;
    pending.plugin = ext == layout::Ext::Plugin;
    pending.name = pending.plugin ? store_layout::plugin_namespace_name(plugin)
                                  : store_layout::namespace_name(ext);
    pending.version = pending.plugin
                          ? root_->resolved_namespace_version(pending.name)
                          : store_layout::namespace_version(ext);
    pending.scope = scope_of(file_id);
    std::pair<std::string, kv::ScopeKey> key{pending.name, pending.scope};
    auto inserted = pending_.try_emplace(std::move(key), std::move(pending));
    last_copy_ = &inserted.first->second;
    last_ext_ = ext;
    last_file_id_ = file_id;
    last_plugin_.assign(plugin.data(), plugin.size());
    return *last_copy_;
}

void StoreIndexWrite::clear_pending(PendingCopy& copy) {
    copy.cleared = true;
    copy.has_manifest = false;
    copy.failed = false;
    copy.params_hash = 0;
    copy.keys.clear();
}

void StoreIndexWrite::clear_copy(layout::Ext ext, std::uint32_t file_id,
                                 std::string_view plugin) {
    clear_pending(pending_copy(ext, file_id, plugin));
}

void StoreIndexWrite::clear_plugin_copies(std::uint32_t file_id) {
    const kv::ScopeKey scope = scope_of(file_id);
    for (auto& entry : pending_)
        if (entry.second.plugin && entry.second.scope == scope)
            clear_pending(entry.second);
    const auto held = root_->snapshot();
    if (!held) return;
    const auto declared = held->namespaces();
    if (!declared)
        throw_store_error("store index: list namespaces", declared.error());
    for (const kv::NamespaceDecl& entry : *declared) {
        const std::string_view name(entry.name.data(), entry.name.size());
        if (!name.starts_with(store_layout::PLUGIN_NAMESPACE_PREFIX)) continue;
        const std::string plugin(
            name.substr(store_layout::PLUGIN_NAMESPACE_PREFIX.size()));
        if (pending_.find({store_layout::plugin_namespace_name(plugin),
                           scope}) != pending_.end())
            continue;
        clear_copy(layout::Ext::Plugin, file_id, plugin);
    }
}

void StoreIndexWrite::host_put(const store_layout::HostTarget& target,
                               std::string_view value) {
    switch (target.kind) {
        case store_layout::HostKind::FileByPath: {
            const auto record = layout::decode_file_record(value);
            if (!record) {
                throw std::runtime_error(
                    "store index: a file record does not decode");
            }
            file_stats_.insert_or_assign(
                target.path,
                kv::SourceStat{record->mtime, record->size, record->hash});
            root_->bind_file_id(target.path, static_cast<int>(record->file_id));
            return;
        }
        case store_layout::HostKind::Manifest: {
            if (!target.ext) {
                throw std::runtime_error(
                    "store index: a manifest names an unknown extension");
            }
            const auto entry = layout::decode_manifest(value);
            if (!entry) {
                throw std::runtime_error(
                    "store index: a manifest does not decode");
            }
            PendingCopy& copy = pending_copy(*target.ext, target.file_id, {});
            copy.params_hash = entry->params_hash;
            copy.has_manifest = true;
            copy.failed = entry->status == layout::ExtStatus::Failed;
            return;
        }
        case store_layout::HostKind::PluginManifest: {
            if (target.plugin.empty()) {
                throw std::runtime_error(
                    "store index: a plugin manifest names no plugin");
            }
            const auto entry = layout::decode_manifest(value);
            if (!entry) {
                throw std::runtime_error(
                    "store index: a manifest does not decode");
            }
            PendingCopy& copy = pending_copy(layout::Ext::Plugin,
                                             target.file_id, target.plugin);
            copy.params_hash = entry->params_hash;
            copy.version = entry->version;
            copy.has_manifest = true;
            copy.failed = entry->status == layout::ExtStatus::Failed;
            return;
        }
        case store_layout::HostKind::Format:
        case store_layout::HostKind::ExtRegistry:
        case store_layout::HostKind::FileById:
        case store_layout::HostKind::NextFileId:
            return;
    }
}

void StoreIndexWrite::host_delete(const store_layout::HostTarget& target) {
    switch (target.kind) {
        case store_layout::HostKind::Manifest:
            if (!target.ext) {
                throw std::runtime_error(
                    "store index: a manifest names an unknown extension");
            }
            clear_copy(*target.ext, target.file_id, {});
            return;
        case store_layout::HostKind::PluginManifest:
            if (target.plugin.empty()) {
                clear_plugin_copies(target.file_id);
                return;
            }
            clear_copy(layout::Ext::Plugin, target.file_id, target.plugin);
            return;
        case store_layout::HostKind::Format:
        case store_layout::HostKind::ExtRegistry:
        case store_layout::HostKind::FileByPath:
        case store_layout::HostKind::FileById:
        case store_layout::HostKind::NextFileId:
            return;
    }
}

void StoreIndexWrite::check_narrow_delete(const std::string& name,
                                          const kv::ScopeKey& scope) {
    auto at = pending_.find({name, scope});
    if (at != pending_.end() && at->second.cleared) return;
    if (root_->was_committed(name, scope)) {
        throw std::runtime_error(
            "store index: a delete inside a committed copy is not supported");
    }
}

void StoreIndexWrite::put(layout::Family, std::string_view key,
                          std::string_view value) {
    note_bytes(key.size() + value.size());
    if (auto host = store_layout::host_target_of(key)) {
        host_put(*host, value);
        return;
    }
    auto target = store_layout::target_of(key);
    if (!target) {
        throw std::runtime_error("store index: a put key is not an index key");
    }
    PendingCopy& copy =
        pending_copy(target->ext, target->file_id, target->plugin);
    copy.keys.insert_or_assign(std::move(target->inner_key),
                               std::string(value));
}

void StoreIndexWrite::merge(layout::Family, std::string_view,
                            std::string_view) {
    throw std::runtime_error("store index: merge is not supported");
}

void StoreIndexWrite::delete_range(layout::Family, std::string_view begin,
                                   std::string_view end) {
    note_bytes(begin.size() + end.size());
    if (auto host = store_layout::host_target_of(begin)) {
        host_delete(*host);
        return;
    }
    auto target = store_layout::target_of(begin);
    if (!target) {
        throw std::runtime_error(
            "store index: a delete range is not an index key");
    }
    if (target->plugin.empty() && target->ext == layout::Ext::Plugin) {
        clear_plugin_copies(target->file_id);
        return;
    }
    if (begin.size() == layout::KEY_PREFIX_BYTES &&
        end == layout::prefix_end(begin)) {
        clear_copy(target->ext, target->file_id, target->plugin);
        return;
    }
    const std::string name = namespace_of(*target);
    const kv::ScopeKey scope = scope_of(target->file_id);
    check_narrow_delete(name, scope);
    auto at = pending_.find({name, scope});
    if (at == pending_.end()) return;
    auto& keys = at->second.keys;
    for (auto key = keys.begin(); key != keys.end();) {
        const std::string full =
            full_key_of(target->ext, target->file_id, key->first);
        if (full.compare(begin) >= 0 && full.compare(end) < 0) {
            key = keys.erase(key);
        } else {
            ++key;
        }
    }
}

void StoreIndexWrite::del(layout::Family, std::string_view key) {
    note_bytes(key.size());
    if (auto host = store_layout::host_target_of(key)) {
        host_delete(*host);
        return;
    }
    auto target = store_layout::target_of(key);
    if (!target) {
        throw std::runtime_error(
            "store index: a delete key is not an index key");
    }
    if (target->plugin.empty() && target->ext == layout::Ext::Plugin) {
        clear_plugin_copies(target->file_id);
        return;
    }
    const std::string name = namespace_of(*target);
    const kv::ScopeKey scope = scope_of(target->file_id);
    check_narrow_delete(name, scope);
    auto at = pending_.find({name, scope});
    if (at == pending_.end()) return;
    at->second.keys.erase(target->inner_key);
}

void StoreIndexWrite::commit() {
    if (committed_) return;
    committed_ = true;
    std::shared_lock<std::shared_mutex> gate(root_->gate_);
    kv::Writer& writer = root_->writer();
    for (auto& entry : pending_) {
        PendingCopy& copy = entry.second;
        const std::uint32_t version = copy.version;
        const kv::NamespaceId id = root_->namespace_id(copy.name, version);
        const bool has_body = !copy.keys.empty() || copy.has_manifest;
        dstd::Vector<kv::SourceStat> sources;
        const bool have_sources =
            has_body && root_->sources_of(copy.scope, file_stats_, sources);
        // The writer keeps one copy of a scope and the params hash and sources
        // it announced for it, so a copy this root already holds with another
        // of either, and a copy this write clears, is replaced by a drop first.
        bool dropped = false;
        if (copy.cleared &&
            (!has_body || root_->was_committed(copy.name, copy.scope))) {
            check("store index: drop a copy", writer.drop(id, copy.scope));
            dropped = true;
        } else if (const auto previous =
                       root_->committed_copy(copy.name, copy.scope)) {
            if (previous->params_hash != copy.params_hash ||
                !same_sources(previous->sources, sources)) {
                check("store index: drop a copy", writer.drop(id, copy.scope));
                dropped = true;
            }
        }
        if (!has_body) {
            if (dropped) {
                root_->forget_committed(copy.name, copy.scope);
            } else {
                root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                                      nullptr);
            }
            continue;
        }
        if (copy.failed) {
            check("store index: fail a copy",
                  writer.fail(id, copy.scope, copy.params_hash));
            root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                                  nullptr);
            continue;
        }
        kv::Batch batch = writer.batch(id, copy.scope, copy.params_hash,
                                       have_sources ? &sources : nullptr);
        for (const auto& [key, value] : copy.keys) {
            batch.put(ByteView(key.data(), key.size()),
                      ByteView(value.data(), value.size()));
        }
        if (copy.keys.empty()) {
            batch.put(
                ByteView(EMPTY_COPY_MARKER.data(), EMPTY_COPY_MARKER.size()),
                ByteView());
        }
        check("store index: commit a copy",
              root_->run(commit_with_space(writer, std::move(batch))));
        root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                              have_sources ? &sources : nullptr);
    }
    check("store index: flush the writer", root_->run(writer.flush()));
    root_->bump_version();
}

}  // namespace dftracer::utils::index::store
