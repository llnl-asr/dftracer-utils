#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/no_destructor.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/open_count.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/store/writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>

namespace dftracer::utils::store {

namespace {

using dstd::ByteView;

/// Most batches hold few entries, and an arena grows by full blocks after this.
constexpr std::size_t FIRST_ARENA_BLOCK = 4096;

const std::array<std::uint8_t, 8> COUNT_VALUE = {1, 0, 0, 0, 0, 0, 0, 0};

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

/// Absolute and without a trailing separator, so two spellings of one directory
/// share one State.
std::string directory_key(dstd::StringView dir) {
    const std::string text(dir.data(), dir.size());
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(text, error);
    if (error) return text;
    path = path.lexically_normal();
    if (!path.has_filename() && path.has_parent_path()) {
        path = path.parent_path();
    }
    return path.string();
}

}  // namespace

bool operator==(const NamespaceSpec& a, const NamespaceSpec& b) {
    return a.name == b.name && a.format_version == b.format_version &&
           a.fold == b.fold && a.durable == b.durable;
}

bool operator!=(const NamespaceSpec& a, const NamespaceSpec& b) {
    return !(a == b);
}

ScopeKey ScopeKey::root() { return ScopeKey{}; }

ScopeKey ScopeKey::file(dstd::StringView path) {
    ScopeKey key;
    key.kind = ScopeKind::File;
    key.paths.push_back(dstd::String(path.data(), path.size()));
    return key;
}

ScopeKey ScopeKey::files(dstd::Span<const dstd::StringView> paths) {
    ScopeKey key;
    key.kind = ScopeKind::Files;
    key.paths.reserve(paths.size());
    for (const dstd::StringView path : paths)
        key.paths.push_back(dstd::String(path.data(), path.size()));
    std::sort(key.paths.begin(), key.paths.end());
    key.paths.erase(std::unique(key.paths.begin(), key.paths.end()),
                    key.paths.end());
    return key;
}

int compare_scopes(const ScopeKey& a, const ScopeKey& b) {
    if (a.kind != b.kind) {
        return static_cast<std::uint8_t>(a.kind) <
                       static_cast<std::uint8_t>(b.kind)
                   ? -1
                   : 1;
    }
    const std::size_t common =
        a.paths.size() < b.paths.size() ? a.paths.size() : b.paths.size();
    for (std::size_t i = 0; i < common; ++i) {
        const int cmp = a.paths[i].compare(b.paths[i]);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (a.paths.size() < b.paths.size()) return -1;
    if (a.paths.size() > b.paths.size()) return 1;
    return 0;
}

bool operator==(const ScopeKey& a, const ScopeKey& b) {
    return compare_scopes(a, b) == 0;
}

bool operator!=(const ScopeKey& a, const ScopeKey& b) { return !(a == b); }

bool operator<(const ScopeKey& a, const ScopeKey& b) {
    return compare_scopes(a, b) < 0;
}

ByteView Batch::keep(ByteView bytes) {
    if (bytes.empty()) return {};
    if (!arena_)
        arena_ = dstd::make_unique<dstd::StringArena>(FIRST_ARENA_BLOCK);
    const std::string_view kept =
        arena_->push(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return {reinterpret_cast<const std::byte*>(kept.data()), kept.size()};
}

void Batch::record_error(dstd::Condition condition) {
    if (!error_) error_ = dstd::Error(condition);
}

void Batch::add(ByteView key, ByteView value) {
    const ByteView kept_key = keep(key);
    entries_.push_back(Entry{kept_key, keep(value)});
}

void Batch::put(ByteView key, ByteView value) {
    if (error_) return;
    if (fold_ != FoldKind::None) {
        record_error(dstd::Condition::InvalidArgument);
        return;
    }
    add(key, value);
}

void Batch::put_ref(ByteView key, ByteView value) {
    if (error_) return;
    if (fold_ != FoldKind::None) {
        record_error(dstd::Condition::InvalidArgument);
        return;
    }
    entries_.push_back(Entry{key, value});
}

void Batch::fold(ByteView key, ByteView value) {
    if (error_) return;
    if (fold_ == FoldKind::None) {
        record_error(dstd::Condition::InvalidArgument);
        return;
    }
    if (fold_ == FoldKind::Count) {
        add(key, ByteView{COUNT_VALUE.data(), COUNT_VALUE.size()});
        return;
    }
    auto checked = engine::check_fold_value(fold_, value);
    if (!checked) {
        error_ = checked.error();
        return;
    }
    add(key, value);
}

Cursor::Cursor(Cursor&&) noexcept = default;

Cursor& Cursor::operator=(Cursor&& other) noexcept {
    impl_ = std::move(other.impl_);
    owner_ = std::move(other.owner_);
    return *this;
}

Cursor::~Cursor() = default;

bool Cursor::at_end() const { return impl_->at_end(); }

dstd::ByteView Cursor::key() const { return impl_->key(); }

const ScopeKey& Cursor::scope() const { return impl_->scope(); }

dstd::Expected<dstd::ByteView, dstd::Error> Cursor::value() {
    return impl_->value();
}

void Cursor::next() { impl_->next(); }

const dstd::Optional<dstd::Error>& Cursor::error() const {
    return impl_->error();
}

CopyInfo Snapshot::copy_info(const Namespace& ns, const ScopeKey& scope) const {
    return impl_->copy_info(ns.spec, scope);
}

dstd::Expected<dstd::Optional<Value>, dstd::Error> Snapshot::get(
    const Namespace& ns, const ScopeKey& scope, dstd::ByteView key) const {
    return impl_->get(ns.spec, scope, key);
}

dstd::Expected<dstd::Vector<dstd::Optional<Value>>, dstd::Error>
Snapshot::multi_get(const Namespace& ns, const ScopeKey& scope,
                    dstd::Span<const dstd::ByteView> keys) const {
    dstd::Vector<dstd::Optional<Value>> values;
    values.reserve(keys.size());
    for (const dstd::ByteView key : keys) {
        auto found = impl_->get(ns.spec, scope, key);
        if (!found) return dstd::Unexpected<dstd::Error>(found.error());
        values.push_back(std::move(*found));
    }
    return values;
}

Cursor Snapshot::scan(const Namespace& ns, const ScopeKey& scope,
                      dstd::ByteView prefix) const {
    return wrap(impl_->scan(ns.spec, scope, prefix));
}

Cursor Snapshot::scan(const Namespace& ns, const ScopeKey& scope,
                      dstd::ByteView begin, dstd::ByteView end) const {
    return wrap(impl_->scan(ns.spec, scope, begin, end));
}

Cursor Snapshot::scan(const Namespace& ns, dstd::Span<const ScopeKey> scopes,
                      dstd::ByteView begin, dstd::ByteView end) const {
    return wrap(impl_->scan(ns.spec, scopes, begin, end));
}

Cursor Snapshot::scan_all(const Namespace& ns, dstd::ByteView begin,
                          dstd::ByteView end) const {
    return wrap(impl_->scan_all(ns.spec, begin, end));
}

dstd::Expected<dstd::Vector<ScopeEntry>, dstd::Error> Snapshot::scopes(
    const Namespace& ns) const {
    return impl_->scopes(ns.spec);
}

dstd::Expected<dstd::Vector<NamespaceDecl>, dstd::Error> Snapshot::namespaces()
    const {
    return impl_->namespaces();
}

Cursor Snapshot::wrap(engine::ScanCursor&& scan) const {
    Cursor cursor;
    cursor.owner_ = impl_;
    cursor.impl_ = dstd::make_unique<engine::ScanCursor>(std::move(scan));
    return cursor;
}

namespace {

std::atomic<std::uint64_t> open_count{0};

std::mutex& registry_mutex() {
    static dstd::NoDestructor<std::mutex> value;
    return *value;
}

}  // namespace

struct Store::State {
    /// Holds what the store keeps across queries, never the pool of the query
    /// that made the call. First, so it closes after every other member is
    /// gone.
    memory::PoolHandle pool;
    std::string dir;
    mutable std::mutex mutex;
    dstd::SharedPtr<Runtime> runtime;
    dstd::SharedPtr<engine::Writer> writer;
    /// The id of a namespace is its index plus one, the same for every writer
    /// this store opens.
    dstd::Vector<Namespace> namespaces;
    dstd::SharedPtr<const engine::Snapshot> held;
    dstd::Vector<std::pair<std::string, std::uint64_t>> foreign_seen;
    std::chrono::steady_clock::time_point foreign_checked{};
    /// Changed under the mutex, read without it by snapshot()'s shortcut.
    std::atomic<std::uint64_t> version{0};
    std::uint64_t held_version = 0;
    std::uint64_t held_records = 0;
    bool held_taken = false;
    /// snapshot() hands it out under a shared lock, so readers never wait on
    /// the state mutex. Written under the mutex, then this lock.
    mutable std::shared_mutex published_mutex;
    dstd::SharedPtr<const engine::Snapshot> published;
    std::uint64_t published_version = 0;
    /// steady_clock ticks before which no look at other processes is due. 0
    /// makes every snapshot() look.
    std::atomic<std::int64_t> next_look{0};

    using Registry = dstd::Map<std::string, std::weak_ptr<State>>;

    static Registry& registry() {
        static dstd::NoDestructor<Registry> value;
        return *value;
    }

    explicit State(std::string directory);
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State();

    static dstd::Expected<dstd::SharedPtr<State>, dstd::Error> get_or_open(
        dstd::StringView dir);

    dstd::Expected<void, dstd::Error> ensure_dir_locked();
    dstd::Expected<dstd::SharedPtr<engine::Writer>, dstd::Error>
    writer_for_use();
    dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
    open_snapshot_locked();
    dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
    snapshot_locked();
    dstd::Vector<std::pair<std::string, std::uint64_t>> foreign_files_locked()
        const;
    void refresh_if_changed_locked();
    dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
    snapshot();
    struct Taken {
        dstd::SharedPtr<engine::Writer> writer;
        dstd::SharedPtr<Runtime> runtime;
    };

    Taken take_writer();
    void bump_version();
    /// Frees the cached snapshot and its page cache, so a close does not hold
    /// them while it writes its final record.
    void drop_published();
    dstd::Expected<void, dstd::Error> close_blocking();

    static coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_task(
        dstd::SharedPtr<State> self, Batch batch);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> publish_task(
        dstd::SharedPtr<State> self, dstd::Vector<engine::CopyChange> changes);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> failed_task(
        dstd::Error error);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> flush_task(
        dstd::SharedPtr<State> self);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> close_task(
        dstd::SharedPtr<State> self);
};

Store::State::State(std::string directory)
    : pool(memory::Manager::get().root().child("store",
                                               memory::PoolKind::Results)),
      dir(std::move(directory)) {}

Store::State::~State() {
    if (!pool) return;
    {
        memory::Scope in_pool(*pool);
        drop_published();
        (void)close_blocking();
    }
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& states = registry();
    auto found = states.find(dir);
    if (found != states.end() && found->second.expired()) states.erase(found);
}

dstd::Expected<dstd::SharedPtr<Store::State>, dstd::Error>
Store::State::get_or_open(dstd::StringView dir) {
    const std::string key = directory_key(dir);
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& states = registry();
    auto found = states.find(key);
    if (found != states.end()) {
        if (auto live = found->second.lock()) return live;
    }
    std::error_code error;
    std::filesystem::create_directories(key, error);
    if (error) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Io, "store: cannot create the directory " + key +
                                     ": " + error.message()));
    }
    // A state outlives the call that opened it, so it is not charged to that
    // call's pool.
    memory::Scope general(memory::Manager::get().general());
    auto created = dstd::make_shared<State>(key);
    if (!created->pool) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Internal, "store: the pool table is full"));
    }
    states.insert_or_assign(key, created);
    open_count.fetch_add(1, std::memory_order_relaxed);
    return created;
}

dstd::Expected<void, dstd::Error> Store::State::ensure_dir_locked() {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Io, "store: cannot create the directory " + dir +
                                     ": " + error.message()));
    }
    return {};
}

dstd::Expected<dstd::SharedPtr<engine::Writer>, dstd::Error>
Store::State::writer_for_use() {
    memory::Scope in_pool(*pool);
    std::lock_guard<std::mutex> lock(mutex);
    if (writer) return writer;
    auto made = ensure_dir_locked();
    if (!made) return dstd::Unexpected<dstd::Error>(made.error());
    auto shared = default_runtime_shared();
    auto created = engine::Writer::create(
        dstd::StringView(dir.data(), dir.size()), *shared->executor());
    if (!created) return dstd::Unexpected<dstd::Error>(created.error());
    dstd::SharedPtr<engine::Writer> opened(std::move(*created));
    for (const Namespace& known : namespaces) {
        auto added = opened->add_namespace(known.spec);
        if (!added) return dstd::Unexpected<dstd::Error>(added.error());
        if (added->value != known.id) {
            return dstd::Unexpected<dstd::Error>(dstd::Error(
                dstd::Condition::Internal, "store: namespace ids diverged"));
        }
    }
    runtime = std::move(shared);
    writer = std::move(opened);
    // A snapshot taken before this writer holds none of its copies.
    held_taken = false;
    return writer;
}

dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
Store::State::open_snapshot_locked() {
    auto made = ensure_dir_locked();
    if (!made) return dstd::Unexpected<dstd::Error>(made.error());
    auto opened =
        engine::Snapshot::open(dstd::StringView(dir.data(), dir.size()));
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    return dstd::make_shared<const engine::Snapshot>(std::move(*opened));
}

dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
Store::State::snapshot_locked() {
    const std::uint64_t records = writer ? writer->committed_records() : 0;
    const std::uint64_t current = version.load(std::memory_order_relaxed);
    if (held_taken && held_version == current && held_records == records) {
        return held;
    }
    auto opened = open_snapshot_locked();
    if (!opened) return opened;
    held = std::move(*opened);
    held_version = current;
    held_records = records;
    held_taken = true;
    return held;
}

dstd::Vector<std::pair<std::string, std::uint64_t>>
Store::State::foreign_files_locked() const {
    dstd::Vector<std::pair<std::string, std::uint64_t>> files;
    std::error_code error;
    std::filesystem::directory_iterator it(dir, error);
    if (error) return files;
    const std::string own =
        writer ? std::filesystem::path(
                     std::string(writer->path().data(), writer->path().size()))
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

void Store::State::refresh_if_changed_locked() {
    static const std::uint64_t interval_ms =
        Env::get<std::uint64_t>(constants::indexer::INDEX_STORE_REFRESH_MS_ENV)
            .value_or(constants::indexer::INDEX_STORE_REFRESH_MS);
    const auto now = std::chrono::steady_clock::now();
    if (interval_ms > 0 && foreign_checked.time_since_epoch().count() != 0 &&
        now - foreign_checked < std::chrono::milliseconds(interval_ms)) {
        return;
    }
    foreign_checked = now;
    if (interval_ms > 0) {
        next_look.store((now + std::chrono::milliseconds(interval_ms))
                            .time_since_epoch()
                            .count(),
                        std::memory_order_release);
    }
    auto files = foreign_files_locked();
    if (files == foreign_seen) return;
    foreign_seen = std::move(files);
    ++version;
}

dstd::Expected<dstd::SharedPtr<const engine::Snapshot>, dstd::Error>
Store::State::snapshot() {
    if (steady_now_ns() < next_look.load(std::memory_order_acquire)) {
        std::shared_lock<std::shared_mutex> shared(published_mutex);
        if (published != nullptr &&
            published_version == version.load(std::memory_order_acquire)) {
            return published;
        }
    }
    memory::Scope in_pool(*pool);
    std::lock_guard<std::mutex> lock(mutex);
    refresh_if_changed_locked();
    auto opened = snapshot_locked();
    if (!opened) return opened;
    std::unique_lock<std::shared_mutex> publish(published_mutex);
    published = *opened;
    published_version = held_version;
    return opened;
}

Store::State::Taken Store::State::take_writer() {
    std::lock_guard<std::mutex> lock(mutex);
    return Taken{std::move(writer), runtime};
}

void Store::State::drop_published() {
    dstd::SharedPtr<const engine::Snapshot> dropped;
    {
        std::unique_lock<std::shared_mutex> lock(published_mutex);
        dropped = std::move(published);
        published_version = 0;
    }
}

void Store::State::bump_version() {
    std::lock_guard<std::mutex> lock(mutex);
    ++version;
}

dstd::Expected<void, dstd::Error> Store::State::close_blocking() {
    Taken taken = take_writer();
    if (!taken.writer) return {};
    const bool reentrant = Executor::current() == taken.runtime->executor();
    BlockingRegion block(reentrant ? taken.runtime->executor() : nullptr);
    auto closed = taken.runtime->submit(taken.writer->close()).get();
    bump_version();
    return closed;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::commit_task(
    dstd::SharedPtr<State> self, Batch batch) {
    auto opened = self->writer_for_use();
    if (!opened) co_return dstd::Unexpected<dstd::Error>(opened.error());
    const dstd::SharedPtr<engine::Writer> writer = *opened;
    auto result = writer->commit(std::move(batch));
    while (!result && result.error().is_code(engine::WriterErrc::Busy)) {
        co_await writer->space();
        result = writer->commit(std::move(batch));
    }
    self->bump_version();
    co_return result;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::publish_task(
    dstd::SharedPtr<State> self, dstd::Vector<engine::CopyChange> changes) {
    auto opened = self->writer_for_use();
    if (!opened) co_return dstd::Unexpected<dstd::Error>(opened.error());
    const dstd::SharedPtr<engine::Writer> writer = *opened;
    auto before = co_await writer->flush();
    self->bump_version();
    if (!before) co_return before;
    auto published = writer->apply(
        dstd::Span<const engine::CopyChange>(changes.data(), changes.size()));
    if (!published) co_return published;
    auto after = co_await writer->flush();
    self->bump_version();
    co_return after;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::failed_task(
    dstd::Error error) {
    co_return dstd::Unexpected<dstd::Error>(std::move(error));
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::flush_task(
    dstd::SharedPtr<State> self) {
    dstd::SharedPtr<engine::Writer> writer;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        writer = self->writer;
    }
    if (!writer) co_return dstd::Expected<void, dstd::Error>{};
    auto flushed = co_await writer->flush();
    self->bump_version();
    co_return flushed;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::close_task(
    dstd::SharedPtr<State> self) {
    dstd::SharedPtr<engine::Writer> writer = self->take_writer().writer;
    if (!writer) co_return dstd::Expected<void, dstd::Error>{};
    auto closed = co_await writer->close();
    self->bump_version();
    co_return closed;
}

std::uint64_t internal::opens() noexcept {
    return open_count.load(std::memory_order_relaxed);
}

dstd::Expected<Store, dstd::Error> Store::open(dstd::StringView dir) {
    auto state = State::get_or_open(dir);
    if (!state) return dstd::Unexpected<dstd::Error>(state.error());
    return Store(std::move(*state));
}

dstd::Expected<Namespace, dstd::Error> Store::ns(const NamespaceSpec& spec) {
    if (spec.name.empty()) return invalid_error();
    memory::Scope in_pool(*state_->pool);
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (const Namespace& known : state_->namespaces) {
        if (known.spec.name != spec.name) continue;
        if (known.spec != spec) return invalid_error();
        return known;
    }
    Namespace added;
    added.spec = spec;
    added.id = static_cast<std::uint32_t>(state_->namespaces.size() + 1);
    if (state_->writer) {
        auto registered = state_->writer->add_namespace(spec);
        if (!registered) {
            return dstd::Unexpected<dstd::Error>(registered.error());
        }
        if (registered->value != added.id) {
            return dstd::Unexpected<dstd::Error>(dstd::Error(
                dstd::Condition::Internal, "store: namespace ids diverged"));
        }
    }
    state_->namespaces.push_back(added);
    return added;
}

Batch Store::batch(const Namespace& ns, const ScopeKey& scope,
                   std::uint64_t params_hash,
                   const dstd::Vector<SourceStat>* sources) {
    auto opened = state_->writer_for_use();
    if (!opened) {
        Batch failed;
        failed.error_ = opened.error();
        return failed;
    }
    return (*opened)->batch(engine::NamespaceId{ns.id}, scope, params_hash,
                            sources);
}

Batch Store::building_batch(const Namespace& ns, const ScopeKey& scope) {
    auto opened = state_->writer_for_use();
    if (!opened) {
        Batch failed;
        failed.error_ = opened.error();
        return failed;
    }
    return (*opened)->building_batch(engine::NamespaceId{ns.id}, scope);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::commit(Batch batch) {
    return State::commit_task(state_, std::move(batch));
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::publish(
    dstd::Span<const Publish> targets) {
    dstd::Vector<engine::CopyChange> converted;
    converted.reserve(targets.size());
    for (const Publish& target : targets) {
        if (target.ns == nullptr || target.scope == nullptr) {
            return State::failed_task(
                dstd::Error(dstd::Condition::InvalidArgument));
        }
        engine::ChangeKind kind = engine::ChangeKind::Ready;
        switch (target.change) {
            case Publish::Change::Ready:
                break;
            case Publish::Change::Failed:
                kind = engine::ChangeKind::Failed;
                break;
            case Publish::Change::Dropped:
                kind = engine::ChangeKind::Dropped;
                break;
        }
        converted.push_back(
            engine::CopyChange{engine::NamespaceId{target.ns->id}, target.scope,
                               target.params_hash, target.sources, kind});
    }
    return State::publish_task(state_, std::move(converted));
}

dstd::Expected<void, dstd::Error> Store::fail(const Namespace& ns,
                                              const ScopeKey& scope,
                                              std::uint64_t params_hash) {
    auto opened = state_->writer_for_use();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    auto result =
        (*opened)->fail(engine::NamespaceId{ns.id}, scope, params_hash);
    state_->bump_version();
    return result;
}

dstd::Expected<void, dstd::Error> Store::drop(const Namespace& ns,
                                              const ScopeKey& scope) {
    auto opened = state_->writer_for_use();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    auto result = (*opened)->drop(engine::NamespaceId{ns.id}, scope);
    state_->bump_version();
    return result;
}

dstd::Expected<void, dstd::Error> Store::erase(const Namespace& ns,
                                               const ScopeKey& scope,
                                               dstd::ByteView begin,
                                               dstd::ByteView end) {
    auto opened = state_->writer_for_use();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    auto result =
        (*opened)->erase(engine::NamespaceId{ns.id}, scope, begin, end);
    state_->bump_version();
    return result;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::flush() {
    return State::flush_task(state_);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::close() {
    return State::close_task(state_);
}

dstd::Expected<Snapshot, dstd::Error> Store::snapshot() {
    auto held = state_->snapshot();
    if (!held) return dstd::Unexpected<dstd::Error>(held.error());
    Snapshot view;
    view.impl_ = std::move(*held);
    return view;
}

dstd::Expected<void, dstd::Error> Store::close_all() {
    dstd::Vector<dstd::SharedPtr<State>> live;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        for (const auto& entry : State::registry()) {
            if (auto state = entry.second.lock())
                live.push_back(std::move(state));
        }
    }
    dstd::Expected<void, dstd::Error> first;
    for (const auto& state : live) {
        memory::Scope in_pool(*state->pool);
        state->drop_published();
        auto closed = state->close_blocking();
        if (!closed && first) first = std::move(closed);
    }
    return first;
}

}  // namespace dftracer::utils::store
