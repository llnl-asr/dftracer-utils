#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/timer.h>
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
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

namespace {

/// A copy with no entries announces none, so a Ready copy with no key carries
/// this entry (inner key [0xFF], empty value).
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

/// A copy names a key by its kind byte and then the tail of the key. Empty for
/// a bound at the prefix.
std::string inner_bound_of(std::string_view key) {
    if (key.size() <= layout::KEY_PREFIX_BYTES) return {};
    std::string inner(1, key[2]);
    inner.append(key.substr(layout::KEY_PREFIX_BYTES));
    return inner;
}

/// Retries while the writer reports Busy.
coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_with_space(
    kv::Writer& writer, kv::Batch batch) {
    auto result = writer.commit(std::move(batch));
    while (!result && result.error().is_code(kv::WriterErrc::Busy)) {
        co_await writer.space();
        result = writer.commit(std::move(batch));
    }
    co_return result;
}

/// Runs `task` to its end on the process runtime and rethrows what it threw.
void wait_for(coro::CoroTask<void> task) {
    default_runtime().run_blocking("store index write",
                                   [&task](CoroScope&) -> coro::CoroTask<void> {
                                       co_await std::move(task);
                                   });
}

kv::NamespaceSpec make_spec(const std::string& name,
                            std::uint32_t format_version) {
    kv::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = format_version;
    return spec;
}

/// Every file has data in this extension, so it lists the files of the store.
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

/// Absolute and without a trailing separator, so two spellings of one directory
/// share one StoreRoot.
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

std::string full_key_of(layout::Ext ext, std::uint32_t file_id,
                        std::string_view inner_key) {
    std::string key = layout::file_prefix(
        ext, static_cast<std::uint8_t>(inner_key[0]), file_id);
    key.append(inner_key, 1, std::string::npos);
    return key;
}

}  // namespace

StoreRoot::StoreRoot(std::string index_root)
    : pool_(memory::Manager::get().root().child("index.store",
                                                memory::PoolKind::Results)),
      index_root_(std::move(index_root)) {
    if (!pool_) {
        throw std::runtime_error("store index: the pool table is full");
    }
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
    // A root outlives the query that opened it, so it is not charged to that
    // query's pool.
    memory::Scope general(memory::Manager::get().general());
    auto created = std::shared_ptr<StoreRoot>(new StoreRoot(key));
    roots.insert_or_assign(key, created);
    return created;
}

std::optional<std::string> StoreRoot::close_all() {
    std::vector<std::shared_ptr<StoreRoot>> live;
    {
        std::lock_guard<std::mutex> lock(root_registry_mutex());
        for (const auto& entry : root_registry()) {
            if (auto root = entry.second.lock())
                live.push_back(std::move(root));
        }
    }
    std::optional<std::string> first;
    for (const auto& root : live) {
        try {
            root->close();
        } catch (const std::exception& error) {
            if (!first) first = error.what();
        }
    }
    return first;
}

StoreRoot::~StoreRoot() {
    memory::Scope in_pool(*pool_);
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
    // A snapshot taken before this writer holds none of its copies.
    snapshot_taken_ = false;
}

dstd::Expected<void, dstd::Error> StoreRoot::commit(kv::Writer& writer,
                                                    kv::Batch batch) {
    return run(commit_with_space(writer, std::move(batch)));
}

dstd::Expected<void, dstd::Error> StoreRoot::run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    if (!runtime_)
        throw std::runtime_error("store index: no runtime to wait on");
    // A commit can come from a worker of this runtime, so the wait hands off
    // that worker's slot.
    const bool reentrant = Executor::current() == runtime_->executor();
    BlockingRegion block(reentrant ? runtime_->executor() : nullptr);
    return runtime_->submit(std::move(task)).get();
}

std::shared_ptr<const kv::Snapshot> StoreRoot::open_snapshot_locked() {
    // A root that was never written has no store directory and reads as empty.
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
    if (steady_now_ns() < next_look_.load(std::memory_order_acquire)) {
        std::shared_lock<std::shared_mutex> shared(published_mutex_);
        if (published_ != nullptr &&
            published_version_ == version_.load(std::memory_order_acquire))
            return published_;
    }
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_if_changed_locked();
    auto held = snapshot_locked();
    std::unique_lock<std::shared_mutex> publish(published_mutex_);
    published_ = held;
    published_version_ = snapshot_version_;
    return held;
}

std::vector<std::pair<std::string, std::uint64_t>>
StoreRoot::foreign_files_locked() const {
    std::vector<std::pair<std::string, std::uint64_t>> files;
    std::error_code error;
    std::filesystem::directory_iterator it(store_dir_, error);
    if (error) return files;
    const std::string own =
        writer_ ? std::filesystem::path(std::string(writer_->path().data(),
                                                    writer_->path().size()))
                      .filename()
                      .string()
                : std::string();
    for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
        if (error) break;
        const auto& path = it->path();
        if (path.extension() != ".dfs") continue;
        std::string name = path.filename().string();
        if (name == own) continue;
        std::error_code size_error;
        const auto size = it->file_size(size_error);
        files.emplace_back(std::move(name),
                           size_error ? 0 : static_cast<std::uint64_t>(size));
    }
    std::sort(files.begin(), files.end());
    return files;
}

void StoreRoot::refresh_if_changed_locked() {
    static const std::uint64_t interval_ms =
        Env::get<std::uint64_t>(constants::indexer::INDEX_STORE_REFRESH_MS_ENV)
            .value_or(constants::indexer::INDEX_STORE_REFRESH_MS);
    const auto now = std::chrono::steady_clock::now();
    if (interval_ms > 0 && foreign_checked_.time_since_epoch().count() != 0 &&
        now - foreign_checked_ < std::chrono::milliseconds(interval_ms)) {
        return;
    }
    foreign_checked_ = now;
    if (interval_ms > 0) {
        next_look_.store((now + std::chrono::milliseconds(interval_ms))
                             .time_since_epoch()
                             .count(),
                         std::memory_order_release);
    }
    auto files = foreign_files_locked();
    if (files == foreign_seen_) return;
    foreign_seen_ = std::move(files);
    refresh_locked();
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
        if (ids_by_path_.find(path) != ids_by_path_.end()) continue;
        const int id = next_file_id_++;
        ids_by_path_.emplace(path, id);
        paths_by_id_.emplace(id, path);
    }
    seeded_ = true;
}

void StoreRoot::refresh() {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    foreign_seen_ = foreign_files_locked();
    foreign_checked_ = std::chrono::steady_clock::now();
    refresh_locked();
}

void StoreRoot::refresh_locked() {
    ++version_;
    refreshed_version_ = version_;
    if (seeded_) {
        seeded_ = false;
        seed_file_ids_locked();
    }
}

int StoreRoot::file_id_for(std::string_view logical_path) {
    memory::Scope in_pool(*pool_);
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
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    auto at = ids_by_path_.find(std::string(logical_path));
    if (at == ids_by_path_.end()) return std::nullopt;
    return at->second;
}

int StoreRoot::reserve_file_id_range(std::size_t count) {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    const int first = next_file_id_;
    next_file_id_ += static_cast<int>(count);
    return first;
}

int StoreRoot::next_file_id() {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    return next_file_id_;
}

std::optional<std::string> StoreRoot::path_of(int file_id) {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    seed_file_ids_locked();
    auto at = paths_by_id_.find(file_id);
    if (at == paths_by_id_.end()) return std::nullopt;
    return at->second;
}

void StoreRoot::bind_file_id(std::string_view logical_path, int file_id) {
    memory::Scope in_pool(*pool_);
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
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_writer_locked();
    return *writer_;
}

kv::NamespaceId StoreRoot::namespace_id(const std::string& name,
                                        std::uint32_t version) {
    memory::Scope in_pool(*pool_);
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
    memory::Scope in_pool(*pool_);
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
    memory::Scope in_pool(*pool_);
    if (scope.kind != kv::ScopeKind::File) return false;
    const std::string members =
        store_layout::namespace_name(layout::Ext::Members);
    std::shared_ptr<const kv::Snapshot> held;
    dstd::Vector<kv::SourceStat> found;
    for (const dstd::String& path : scope.paths) {
        const std::string key(path.data(), path.size());
        auto at = stats.find(key);
        if (at != stats.end()) {
            found.push_back(at->second);
            continue;
        }
        const kv::ScopeKey file = kv::ScopeKey::file(key);
        if (const auto own = committed_copy(members, file);
            own && own->sources.size() == 1) {
            found.push_back(own->sources[0]);
            continue;
        }
        if (!held) held = snapshot();
        if (!held) return false;
        const kv::CopyInfo info = held->copy_info(members_spec(), file);
        if (info.sources.size() != 1) return false;
        found.push_back(info.sources[0]);
    }
    if (found.empty()) return false;
    sources = std::move(found);
    return true;
}

void StoreRoot::forget_scope_entry_locked(const std::string& name,
                                          const kv::ScopeKey& scope) {
    if (committed_.empty() && dropped_.empty()) return;
    const std::pair<std::string, kv::ScopeKey> key{name, scope};
    committed_.erase(key);
    dropped_.erase(key);
}

std::optional<std::uint64_t> StoreRoot::file_copy_key_locked(
    const std::string& name, const kv::ScopeKey& scope) {
    if (scope.kind != kv::ScopeKind::File || scope.paths.size() != 1)
        return std::nullopt;
    const auto id = ids_by_path_.find(
        std::string(scope.paths[0].data(), scope.paths[0].size()));
    if (id == ids_by_path_.end() || id->second < 0) return std::nullopt;
    std::size_t name_index = 0;
    while (name_index < committed_names_.size() &&
           committed_names_[name_index] != name)
        ++name_index;
    if (name_index == committed_names_.size()) committed_names_.push_back(name);
    return (static_cast<std::uint64_t>(name_index) << 32U) |
           static_cast<std::uint32_t>(id->second);
}

void StoreRoot::mark_committed(const std::string& name,
                               const kv::ScopeKey& scope,
                               std::uint64_t params_hash,
                               const dstd::Vector<kv::SourceStat>* sources) {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (sources == nullptr || sources->size() <= 1) {
        if (const auto key = file_copy_key_locked(name, scope)) {
            forget_scope_entry_locked(name, scope);
            FileCopy& copy = file_copies_[*key];
            copy.params_hash = params_hash;
            copy.has_source = sources != nullptr && !sources->empty();
            copy.source = copy.has_source ? (*sources)[0] : kv::SourceStat{};
            copy.dropped = false;
            return;
        }
    }
    CommittedCopy committed;
    committed.params_hash = params_hash;
    if (sources != nullptr) committed.sources = *sources;
    dropped_.erase({name, scope});
    committed_.insert_or_assign({name, scope}, std::move(committed));
}

void StoreRoot::forget_committed(const std::string& name,
                                 const kv::ScopeKey& scope) {
    memory::Scope in_pool(*pool_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto key = file_copy_key_locked(name, scope)) {
        forget_scope_entry_locked(name, scope);
        file_copies_[*key] = FileCopy{.dropped = true};
        return;
    }
    committed_.erase({name, scope});
    dropped_.insert({name, scope});
}

bool StoreRoot::was_committed(const std::string& name,
                              const kv::ScopeKey& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto key = file_copy_key_locked(name, scope)) {
        const auto at = file_copies_.find(*key);
        if (at != file_copies_.end()) return !at->second.dropped;
    }
    return !committed_.empty() &&
           committed_.find({name, scope}) != committed_.end();
}

std::optional<StoreRoot::CommittedCopy> StoreRoot::committed_copy(
    const std::string& name, const kv::ScopeKey& scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto key = file_copy_key_locked(name, scope);
        key && file_copies_.contains(*key)) {
        const auto at = file_copies_.find(*key);
        if (at->second.dropped) return std::nullopt;
        CommittedCopy copy;
        copy.params_hash = at->second.params_hash;
        if (at->second.has_source) copy.sources.push_back(at->second.source);
        return copy;
    }
    if (committed_.empty()) return std::nullopt;
    auto at = committed_.find({name, scope});
    if (at == committed_.end()) return std::nullopt;
    return at->second;
}

std::optional<StoreRoot::ForeignCopy> StoreRoot::foreign_copy(
    const std::string& name, std::uint32_t version, const kv::ScopeKey& scope) {
    memory::Scope in_pool(*pool_);
    std::shared_ptr<const kv::Snapshot> held;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto file_key = file_copy_key_locked(name, scope);
        if (file_key && file_copies_.contains(*file_key)) return std::nullopt;
        if (!committed_.empty() || !dropped_.empty()) {
            const std::pair<std::string, kv::ScopeKey> key{name, scope};
            if (committed_.count(key) != 0 || dropped_.count(key) != 0)
                return std::nullopt;
        }
        refresh_if_changed_locked();
        held = snapshot_taken_ && snapshot_version_ >= refreshed_version_
                   ? snapshot_
                   : snapshot_locked();
    }
    if (!held) return std::nullopt;
    kv::CopyInfo info = held->copy_info(make_spec(name, version), scope);
    if (info.state != kv::ScopeState::Ready) return std::nullopt;
    return ForeignCopy{std::move(held), std::move(info)};
}

void StoreRoot::bump_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++version_;
}

void StoreRoot::close_locked() {
    if (!writer_) return;
    const auto closed = run(writer_->close());
    writer_.reset();
    namespaces_.clear();
    committed_names_.clear();
    file_copies_.clear();
    committed_.clear();
    dropped_.clear();
    ++version_;
    if (!closed)
        throw_store_error("store index: close the store", closed.error());
}

StoreRoot::WriterUse::WriterUse(StoreRoot& root) : root_(root) {
    std::unique_lock<std::mutex> lock(root_.use_mutex_);
    root_.use_cv_.wait(lock, [this] { return !root_.closing_; });
    ++root_.uses_;
}

StoreRoot::WriterUse::~WriterUse() {
    std::lock_guard<std::mutex> lock(root_.use_mutex_);
    if (--root_.uses_ == 0) root_.use_cv_.notify_all();
}

void StoreRoot::close() {
    memory::Scope in_pool(*pool_);
    {
        std::unique_lock<std::mutex> lock(use_mutex_);
        use_cv_.wait(lock, [this] { return !closing_; });
        closing_ = true;
        use_cv_.wait(lock, [this] { return uses_ == 0; });
    }
    struct Reopen {
        StoreRoot& root;
        ~Reopen() {
            std::lock_guard<std::mutex> lock(root.use_mutex_);
            root.closing_ = false;
            root.use_cv_.notify_all();
        }
    } reopen{*this};
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

std::unique_ptr<StoreIndexWrite> StoreRoot::begin_write(bool staged) {
    return std::unique_ptr<StoreIndexWrite>(
        new StoreIndexWrite(shared_from_this(), staged));
}

int StoreIndexWrite::file_id_for(std::string_view logical_path) {
    return root_->file_id_for(logical_path);
}

void StoreIndexWrite::bind_file(std::string_view logical_path, int file_id) {
    root_->bind_file_id(logical_path, file_id);
}

StoreIndexWrite::StoreIndexWrite(std::shared_ptr<StoreRoot> root, bool staged)
    : root_(std::move(root)), staged_(staged) {}

bool StoreIndexWrite::building(const std::string& name,
                               const kv::ScopeKey& scope) const {
    const auto at = staged_copies_.find({name, scope});
    return at != staged_copies_.end() && at->second.building;
}

const char* StoreIndexWrite::keep(std::string_view key,
                                  std::string_view value) {
    return arena_.push(key, value).data();
}

void StoreIndexWrite::put_key(PendingCopy& copy, std::string_view key,
                              std::string_view value) {
    if (key.size() >= TOMBSTONE || value.size() >= TOMBSTONE) {
        throw std::runtime_error("store index: a key or value is too large");
    }
    copy.keys.push_back({keep(key, value),
                         static_cast<std::uint32_t>(key.size()),
                         static_cast<std::uint32_t>(value.size())});
}

void StoreIndexWrite::delete_key(PendingCopy& copy, std::string_view key) {
    if (key.size() >= TOMBSTONE) {
        throw std::runtime_error("store index: a key is too large");
    }
    copy.keys.push_back(
        {keep(key, {}), static_cast<std::uint32_t>(key.size()), TOMBSTONE});
}

std::vector<StoreIndexWrite::PendingKey> StoreIndexWrite::resolved_keys(
    const PendingCopy& copy) {
    const auto key_of = [](const PendingKey& entry) {
        return std::string_view(entry.bytes, entry.key_length);
    };
    std::vector<std::uint32_t> order(copy.keys.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    // Stable, so the writes of one key keep their order.
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) {
                         return key_of(copy.keys[a]) < key_of(copy.keys[b]);
                     });
    std::vector<PendingKey> resolved;
    resolved.reserve(order.size());
    for (std::size_t at = 0; at < order.size();) {
        std::size_t last = at;
        while (last + 1 < order.size() && key_of(copy.keys[order[last + 1]]) ==
                                              key_of(copy.keys[order[at]])) {
            ++last;
        }
        const PendingKey& winner = copy.keys[order[last]];
        if (winner.value_length != TOMBSTONE) resolved.push_back(winner);
        at = last + 1;
    }
    return resolved;
}

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
    if (!target.meta.empty())
        return store_layout::agg_meta_namespace_name(target.meta);
    if (target.ext == layout::Ext::Plugin)
        return store_layout::plugin_namespace_name(target.plugin);
    return store_layout::namespace_name(target.ext);
}

StoreIndexWrite::PendingCopy& StoreIndexWrite::pending_copy(
    layout::Ext ext, std::uint32_t file_id, std::string_view plugin,
    std::string_view meta) {
    if (last_copy_ != nullptr && last_ext_ == ext && last_file_id_ == file_id &&
        last_plugin_ == plugin && last_meta_ == meta) {
        return *last_copy_;
    }
    PendingCopy pending;
    pending.plugin = ext == layout::Ext::Plugin;
    pending.own_namespace = pending.plugin || !meta.empty();
    if (!meta.empty())
        pending.name = store_layout::agg_meta_namespace_name(meta);
    else if (pending.plugin)
        pending.name = store_layout::plugin_namespace_name(plugin);
    else
        pending.name = store_layout::namespace_name(ext);
    pending.version = pending.own_namespace
                          ? root_->resolved_namespace_version(pending.name)
                          : store_layout::namespace_version(ext);
    pending.scope = scope_of(file_id);
    std::pair<std::string, kv::ScopeKey> key{pending.name, pending.scope};
    auto inserted = pending_.try_emplace(std::move(key), std::move(pending));
    last_copy_ = &inserted.first->second;
    last_ext_ = ext;
    last_file_id_ = file_id;
    last_plugin_.assign(plugin.data(), plugin.size());
    last_meta_.assign(meta.data(), meta.size());
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
                                 std::string_view plugin,
                                 std::string_view meta) {
    PendingCopy& copy = pending_copy(ext, file_id, plugin, meta);
    clear_pending(copy);
    if (staged_ && building(copy.name, copy.scope)) {
        erases_.push_back(
            PendingErase{copy.name, copy.version, copy.scope, {}, {}});
    }
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
            // A staged write publishes whole copies.
            if (!staged_)
                pending_copy(layout::Ext::Members, record->file_id, {}, {})
                    .file_record = true;
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
            PendingCopy& copy =
                pending_copy(*target.ext, target.file_id, {}, {});
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
                                             target.file_id, target.plugin, {});
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

void StoreIndexWrite::erase_keys(const store_layout::KeyTarget& target,
                                 std::string_view begin, std::string_view end) {
    const std::string name = namespace_of(target);
    const kv::ScopeKey scope = scope_of(target.file_id);
    const bool staged_keys = staged_ && building(name, scope);
    auto at = pending_.find({name, scope});
    if (!staged_keys) {
        if (at != pending_.end() && at->second.cleared) return;
        if (!root_->was_committed(name, scope)) {
            PendingCopy& copy = pending_copy(target.ext, target.file_id,
                                             target.plugin, target.meta);
            copy.skips.emplace_back(begin, end);
            return;
        }
    }
    const bool own_namespace =
        target.ext == layout::Ext::Plugin || !target.meta.empty();
    const std::uint32_t version =
        own_namespace ? root_->resolved_namespace_version(name)
                      : store_layout::namespace_version(target.ext);
    erases_.push_back(PendingErase{name, version, scope, std::string(begin),
                                   std::string(end)});
}

void StoreIndexWrite::put(std::string_view key, std::string_view value) {
    note_bytes(key.size() + value.size());
    if (auto host = store_layout::host_target_of(key)) {
        host_put(*host, value);
        return;
    }
    auto target = store_layout::target_of(key);
    if (!target) {
        throw std::runtime_error("store index: a put key is not an index key");
    }
    PendingCopy& copy = pending_copy(target->ext, target->file_id,
                                     target->plugin, target->meta);
    put_key(copy, target->inner_key, value);
}

void StoreIndexWrite::delete_range(std::string_view begin,
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
    if (!target->meta.empty()) {
        clear_copy(target->ext, target->file_id, target->plugin, target->meta);
        return;
    }
    if (begin.size() == layout::KEY_PREFIX_BYTES &&
        end == layout::prefix_end(begin)) {
        clear_copy(target->ext, target->file_id, target->plugin);
        return;
    }
    const std::string name = namespace_of(*target);
    const kv::ScopeKey scope = scope_of(target->file_id);
    // A key is longer than its prefix, so a range ending at or below it holds
    // none.
    if (end.size() <= layout::KEY_PREFIX_BYTES) return;
    erase_keys(*target, inner_bound_of(begin), inner_bound_of(end));
    auto at = pending_.find({name, scope});
    if (at == pending_.end()) return;
    PendingCopy& copy = at->second;
    // A later put of a deleted key wins again.
    const std::size_t written = copy.keys.size();
    for (std::size_t i = 0; i < written; ++i) {
        const PendingKey entry = copy.keys[i];
        if (entry.value_length == TOMBSTONE) continue;
        const std::string_view key(entry.bytes, entry.key_length);
        const std::string full = full_key_of(target->ext, target->file_id, key);
        if (full.compare(begin) >= 0 && full.compare(end) < 0) {
            delete_key(copy, key);
        }
    }
}

void StoreIndexWrite::del(std::string_view key) {
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
    if (!target->meta.empty()) {
        clear_copy(target->ext, target->file_id, target->plugin, target->meta);
        return;
    }
    const std::string name = namespace_of(*target);
    const kv::ScopeKey scope = scope_of(target->file_id);
    erase_keys(*target, target->inner_key, target->inner_key + '\0');
    auto at = pending_.find({name, scope});
    if (at == pending_.end()) return;
    delete_key(at->second, target->inner_key);
}

void StoreIndexWrite::stage() {
    if (!staged_) {
        throw std::logic_error("store index: only a staged write stages");
    }
    if (committed_) return;
    StoreRoot::WriterUse use(*root_);
    memory::Scope in_pool(root_->pool());
    wait_for(stage_body(root_->writer()));
}

coro::CoroTask<void> StoreIndexWrite::stage_async() {
    if (!staged_) {
        throw std::logic_error("store index: only a staged write stages");
    }
    if (Executor::current() != default_runtime().executor())
        return IndexDatabaseWrite::stage_async();
    memory::Scope in_pool(root_->pool());
    return stage_in_use();
}

coro::CoroTask<void> StoreIndexWrite::stage_in_use() {
    if (committed_) co_return;
    StoreRoot::WriterUse use(*root_);
    co_await stage_body(root_->writer());
}

coro::CoroTask<void> StoreIndexWrite::stage_body(kv::Writer& writer) {
    for (const PendingErase& erase : erases_) {
        const kv::NamespaceId id =
            root_->namespace_id(erase.name, erase.version);
        check(
            "store index: erase the keys of a copy",
            writer.erase(id, erase.scope,
                         dstd::ByteView{erase.begin.data(), erase.begin.size()},
                         dstd::ByteView{erase.end.data(), erase.end.size()}));
    }
    erases_.clear();
    for (auto& entry : pending_) {
        PendingCopy& copy = entry.second;
        StagedCopy& known = staged_copies_[entry.first];
        known.name = copy.name;
        known.version = copy.version;
        known.scope = copy.scope;
        if (copy.cleared) known.cleared = true;
        if (copy.has_manifest) {
            known.params_hash = copy.params_hash;
            known.has_manifest = true;
            known.failed = copy.failed;
        }
        const std::vector<PendingKey> keys = resolved_keys(copy);
        if (keys.empty() && !copy.has_manifest) continue;
        const kv::NamespaceId id = root_->namespace_id(copy.name, copy.version);
        kv::Batch batch = writer.building_batch(id, copy.scope);
        for (const PendingKey& key : keys) {
            batch.put_ref(
                dstd::ByteView(key.bytes, key.key_length),
                dstd::ByteView(key.bytes + key.key_length, key.value_length));
        }
        if (keys.empty()) {
            batch.put(dstd::ByteView(EMPTY_COPY_MARKER.data(),
                                     EMPTY_COPY_MARKER.size()),
                      dstd::ByteView());
        }
        const auto staged =
            co_await commit_with_space(writer, std::move(batch));
        check("store index: stage a copy", staged);
        known.building = true;
    }
    pending_.clear();
    last_copy_ = nullptr;
    arena_.clear();
}

std::size_t StoreIndexWrite::carry_keys(const PendingCopy& copy,
                                        const StoreRoot::ForeignCopy& foreign,
                                        kv::Batch& batch) {
    std::vector<std::string_view> touched;
    touched.reserve(copy.keys.size());
    for (const PendingKey& key : copy.keys)
        touched.emplace_back(key.bytes, key.key_length);
    std::sort(touched.begin(), touched.end());
    const auto skipped = [&](std::string_view key) {
        if (key == EMPTY_COPY_MARKER) return true;
        if (std::binary_search(touched.begin(), touched.end(), key))
            return true;
        for (const auto& [begin, end] : copy.skips)
            if (key >= begin && (end.empty() || key < end)) return true;
        return false;
    };
    std::size_t carried = 0;
    auto cursor =
        foreign.snapshot->scan(make_spec(copy.name, copy.version), copy.scope,
                               dstd::ByteView{}, dstd::ByteView{});
    for (; !cursor.at_end(); cursor.next()) {
        const dstd::ByteView key = cursor.key();
        const std::string_view text(reinterpret_cast<const char*>(key.data()),
                                    key.size());
        if (skipped(text)) continue;
        auto value = cursor.value();
        if (!value)
            throw_store_error("store index: carry a copy", value.error());
        batch.put(key, *value);
        ++carried;
    }
    if (cursor.error())
        throw_store_error("store index: carry a copy", *cursor.error());
    return carried;
}

void StoreIndexWrite::commit() {
    memory::Scope in_pool(root_->pool());
    wait_for(commit_body());
}

coro::CoroTask<void> StoreIndexWrite::commit_async() {
    // The writer finishes on the process runtime. Off a worker of it the
    // awaiting chain would see nothing to wait for, so the commit blocks.
    if (Executor::current() != default_runtime().executor())
        return IndexDatabaseWrite::commit_async();
    memory::Scope in_pool(root_->pool());
    return commit_body();
}

coro::CoroTask<void> StoreIndexWrite::commit_body() {
    if (committed_) co_return;
    committed_ = true;
    StoreRoot::WriterUse use(*root_);
    kv::Writer& writer = root_->writer();
    if (staged_) {
        co_await stage_body(writer);
        // Every batch is recorded before the records that publish them.
        const auto recorded = co_await writer.flush();
        check("store index: flush the writer", recorded);
        for (auto& entry : staged_copies_) {
            StagedCopy& copy = entry.second;
            const kv::NamespaceId id =
                root_->namespace_id(copy.name, copy.version);
            if (copy.building) {
                dstd::Vector<kv::SourceStat> sources;
                const bool have_sources =
                    root_->sources_of(copy.scope, file_stats_, sources);
                check("store index: publish a copy",
                      writer.publish(id, copy.scope, copy.params_hash,
                                     have_sources ? &sources : nullptr));
                root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                                      have_sources ? &sources : nullptr);
                if (copy.failed) {
                    check("store index: fail a copy",
                          writer.fail(id, copy.scope, copy.params_hash));
                }
            } else if (copy.cleared) {
                check("store index: drop a copy", writer.drop(id, copy.scope));
                root_->forget_committed(copy.name, copy.scope);
            } else if (copy.failed) {
                check("store index: fail a copy",
                      writer.fail(id, copy.scope, copy.params_hash));
                root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                                      nullptr);
            }
        }
        const auto published = co_await writer.flush();
        check("store index: flush the writer", published);
        root_->bump_version();
        co_return;
    }
    for (const PendingErase& erase : erases_) {
        const kv::NamespaceId id =
            root_->namespace_id(erase.name, erase.version);
        check(
            "store index: erase the keys of a copy",
            writer.erase(id, erase.scope,
                         dstd::ByteView{erase.begin.data(), erase.begin.size()},
                         dstd::ByteView{erase.end.data(), erase.end.size()}));
    }
    for (auto& entry : pending_) {
        PendingCopy& copy = entry.second;
        const std::uint32_t version = copy.version;
        const kv::NamespaceId id = root_->namespace_id(copy.name, version);
        const std::vector<PendingKey> keys = resolved_keys(copy);
        const auto previous = root_->committed_copy(copy.name, copy.scope);
        // A write to part of a copy another writer holds rewrites that copy
        // with the keys it leaves alone.
        std::optional<StoreRoot::ForeignCopy> foreign;
        if (!copy.cleared && !copy.failed && !previous)
            foreign = root_->foreign_copy(copy.name, version, copy.scope);
        if (!copy.has_manifest) {
            if (previous)
                copy.params_hash = previous->params_hash;
            else if (foreign)
                copy.params_hash = foreign->info.params_hash;
        }
        const bool has_body = !keys.empty() || copy.has_manifest ||
                              (foreign && !copy.skips.empty()) ||
                              copy.file_record;
        dstd::Vector<kv::SourceStat> sources;
        bool have_sources =
            has_body && root_->sources_of(copy.scope, file_stats_, sources);
        if (has_body && !have_sources && foreign &&
            !foreign->info.sources.empty()) {
            sources = foreign->info.sources;
            have_sources = true;
        }
        const bool carry = has_body && foreign &&
                           foreign->info.params_hash == copy.params_hash &&
                           same_sources(foreign->info.sources, sources);
        if (keys.empty() && !copy.has_manifest && copy.skips.empty() &&
            !copy.cleared &&
            (carry || (previous && previous->params_hash == copy.params_hash &&
                       same_sources(previous->sources, sources)))) {
            continue;
        }
        // The writer keeps one copy per scope with its announced params hash
        // and sources, so a copy with another of either, or one this write
        // clears, is dropped first.
        bool dropped = false;
        if (copy.cleared &&
            (!has_body || root_->was_committed(copy.name, copy.scope))) {
            check("store index: drop a copy", writer.drop(id, copy.scope));
            dropped = true;
        } else if (previous) {
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
        // The arena holds the bytes until the commit returns, so the batch
        // takes them in place.
        for (const PendingKey& key : keys) {
            batch.put_ref(
                dstd::ByteView(key.bytes, key.key_length),
                dstd::ByteView(key.bytes + key.key_length, key.value_length));
        }
        const std::size_t carried =
            carry ? carry_keys(copy, *foreign, batch) : 0;
        if (keys.empty() && carried == 0) {
            batch.put(dstd::ByteView(EMPTY_COPY_MARKER.data(),
                                     EMPTY_COPY_MARKER.size()),
                      dstd::ByteView());
        }
        const auto done = co_await commit_with_space(writer, std::move(batch));
        check("store index: commit a copy", done);
        root_->mark_committed(copy.name, copy.scope, copy.params_hash,
                              have_sources ? &sources : nullptr);
    }
    const auto flushed = co_await writer.flush();
    check("store index: flush the writer", flushed);
    root_->bump_version();
}

}  // namespace dftracer::utils::index::store
