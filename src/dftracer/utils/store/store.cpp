// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/no_destructor.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/open_count.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/store/writer.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace dftracer::utils::store {

namespace {

using dstd::ByteView;

/// Store::pack commits a rewritten copy in batches of about this size, which
/// bounds what it holds in memory.
constexpr std::size_t PACK_BATCH_BYTES = std::size_t{16} * 1024 * 1024;

/// Most batches hold few entries, and an arena grows by full blocks after this.
constexpr std::size_t FIRST_ARENA_BLOCK = 4096;

const std::array<std::uint8_t, 8> COUNT_VALUE = {1, 0, 0, 0, 0, 0, 0, 0};

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

/// Counts the forks this process came through. Each store call reads it, and
/// getpid would be one more system call for every snapshot.
std::atomic<std::uint64_t> g_forks{0};

void count_fork_in_child() noexcept {
    g_forks.fetch_add(1, std::memory_order_relaxed);
}

[[maybe_unused]] const bool FORK_COUNTER_REGISTERED =
    ::pthread_atfork(nullptr, nullptr, count_fork_in_child) == 0;

dstd::Error forked_handle_error() {
    return dstd::Error(dstd::Condition::ForkedHandle,
                       "store: this handle belongs to the parent of a fork, "
                       "call Store::open in the child");
}

dstd::Error foreign_copy_error() {
    return dstd::Error(dstd::Condition::Unsupported,
                       "erase needs the copy this writer wrote, and another "
                       "data file holds the current copy of the scope");
}

/// Absolute and without a trailing separator, so two spellings of one directory
/// share one State.
dstd::Path directory_key(dstd::StringView dir) {
    const auto absolute = dstd::fs::absolute(dstd::PathView(dir));
    if (!absolute) return dstd::Path(dir);
    return dstd::normalize(absolute->view());
}

}  // namespace

bool operator==(const NamespaceSpec& a, const NamespaceSpec& b) {
    return a.name == b.name && a.format_version == b.format_version &&
           a.fold == b.fold && a.durable == b.durable && a.shared == b.shared;
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
    const dstd::StringView kept =
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

void Batch::erase(ByteView key) {
    if (error_) return;
    dstd::Vector<std::byte> end(key.size() + 1, std::byte{0});
    std::copy_n(key.data(), key.size(), end.begin());
    const ByteView kept_begin = keep(key);
    erases_.push_back(EraseEntry{kept_begin, keep({end.data(), end.size()})});
}

void Batch::erase_range(ByteView begin, ByteView end) {
    if (error_) return;
    const ByteView kept_begin = keep(begin);
    erases_.push_back(EraseEntry{kept_begin, keep(end)});
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

void close_all_at_shutdown() noexcept {
    if (auto closed = Store::close_all(); !closed) {
        DFTRACER_UTILS_LOG_ERROR("store: close at exit: {}",
                                 closed.error().format());
    }
}

}  // namespace

void Store::register_shutdown_close() {
    static std::atomic<bool> registered{false};
    if (registered.exchange(true, std::memory_order_acq_rel)) return;
    // The exit check registers here, so it runs after the hook that this adds
    // has stopped what holds memory.
    if (auto configured = common::configure_memory_from_env(); !configured) {
        DFTRACER_UTILS_LOG_ERROR("store: memory settings: {}",
                                 configured.error().format());
    }
    dstd::add_shutdown_hook(dstd::ShutdownStage::CloseStores,
                            &close_all_at_shutdown);
}

struct Store::State {
    /// Holds what the store keeps across queries, never the pool of the query
    /// that made the call. First, so it closes after every other member is
    /// gone.
    memory::PoolHandle pool;
    const std::uint64_t owner_fork = g_forks.load(std::memory_order_relaxed);
    dstd::Path dir;
    mutable std::mutex mutex;
    dstd::SharedPtr<Runtime> runtime;
    dstd::SharedPtr<engine::Writer> writer;
    /// The id of a namespace is its index plus one, the same for every writer
    /// this store opens.
    dstd::Vector<Namespace> namespaces;
    dstd::SharedPtr<const engine::Snapshot> held;
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> foreign_seen;
    std::chrono::steady_clock::time_point foreign_checked{};
    /// Changed under the mutex, read without it by snapshot()'s shortcut.
    std::atomic<std::uint64_t> version{0};
    std::uint64_t held_version = 0;
    std::uint64_t held_records = 0;
    bool held_taken = false;
    dstd::SharedPtr<engine::detail::VerifiedCommits> verified =
        dstd::make_shared<engine::detail::VerifiedCommits>();
    /// snapshot() hands it out under a shared lock, so readers never wait on
    /// the state mutex. Written under the mutex, then this lock.
    mutable std::shared_mutex published_mutex;
    dstd::SharedPtr<const engine::Snapshot> published;
    std::uint64_t published_version = 0;
    /// steady_clock ticks before which no look at other processes is due. 0
    /// makes every snapshot() look.
    std::atomic<std::int64_t> next_look{0};

    using Registry = dstd::Map<dstd::String, dstd::WeakPtr<State>>;

    /// True in the child of a fork. The mutexes may be held by a thread that
    /// does not exist here, so this comes before any lock.
    bool forked() const {
        return g_forks.load(std::memory_order_relaxed) != owner_fork;
    }

    static Registry& registry() {
        static dstd::NoDestructor<Registry> value;
        return *value;
    }

    explicit State(dstd::Path directory);
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
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> foreign_files_locked()
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
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> pack_rewrite(
        dstd::SharedPtr<State> self, const engine::Snapshot& view,
        const engine::PackPlan& plan, PackReport& report);
    static coro::CoroTask<dstd::Expected<PackReport, dstd::Error>> pack_task(
        dstd::SharedPtr<State> self, PackOptions options);
};

Store::State::State(dstd::Path directory)
    : pool(memory::detail::Manager::get().root().child(
          "store", memory::PoolKind::Results)),
      dir(std::move(directory)) {}

Store::State::~State() {
    if (!pool) return;
    if (forked()) {
        // The threads these wait for are gone, and the writer's file is the
        // parent's to close.
        struct Inherited {
            memory::PoolHandle pool;
            dstd::SharedPtr<Runtime> runtime;
            dstd::SharedPtr<engine::Writer> writer;
            dstd::SharedPtr<const engine::Snapshot> held;
            dstd::SharedPtr<const engine::Snapshot> published;
        };
        static dstd::NoDestructor<std::mutex> inherited_mutex;
        static dstd::NoDestructor<dstd::Vector<Inherited>> inherited;
        memory::detail::Scope general(memory::detail::Manager::get().general());
        std::lock_guard<std::mutex> lock(*inherited_mutex);
        inherited->push_back(Inherited{std::move(pool), std::move(runtime),
                                       std::move(writer), std::move(held),
                                       std::move(published)});
        return;
    }
    {
        memory::detail::Scope in_pool(*pool);
        drop_published();
        close_blocking().discard();
    }
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& states = registry();
    auto found = states.find(dir.str());
    if (found != states.end() && found->second.expired()) states.erase(found);
}

dstd::Expected<dstd::SharedPtr<Store::State>, dstd::Error>
Store::State::get_or_open(dstd::StringView dir) {
    Store::register_shutdown_close();
    const dstd::Path key = directory_key(dir);
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& states = registry();
    auto found = states.find(key.str());
    if (found != states.end()) {
        auto live = found->second.lock();
        if (live && !live->forked()) return live;
    }
    const auto made = dstd::fs::create_directories(key.view());
    if (!made.has_value()) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Io,
            common::str_cat("store: cannot create the directory ", key.str(),
                            ": ", made.error().message())));
    }
    // A state outlives the call that opened it, so it is not charged to that
    // call's pool.
    memory::detail::Scope general(memory::detail::Manager::get().general());
    auto created = dstd::make_shared<State>(key);
    if (!created->pool) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Internal, "store: the pool table is full"));
    }
    states.insert_or_assign(dstd::String(key.str()), created);
    open_count.fetch_add(1, std::memory_order_relaxed);
    return created;
}

dstd::Expected<void, dstd::Error> Store::State::ensure_dir_locked() {
    const auto made = dstd::fs::create_directories(dir.view());
    if (!made.has_value()) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Io,
            common::str_cat("store: cannot create the directory ", dir.str(),
                            ": ", made.error().message())));
    }
    return {};
}

dstd::Expected<dstd::SharedPtr<engine::Writer>, dstd::Error>
Store::State::writer_for_use() {
    if (forked()) return dstd::Unexpected<dstd::Error>(forked_handle_error());
    memory::detail::Scope in_pool(*pool);
    std::lock_guard<std::mutex> lock(mutex);
    if (writer) return writer;
    auto made = ensure_dir_locked();
    if (!made) return dstd::Unexpected<dstd::Error>(made.error());
    auto shared = default_runtime_shared();
    auto created = engine::Writer::create(dir.str(), *shared->executor());
    if (!created) return dstd::Unexpected<dstd::Error>(created.error());
    dstd::SharedPtr<engine::Writer> opened =
        dstd::to_shared_ptr(std::move(*created));
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
    auto opened = engine::Snapshot::open(dir.str(), verified);
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

dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>
Store::State::foreign_files_locked() const {
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> files;
    const auto opened = dstd::fs::Dir::open(dir.view());
    if (!opened) return files;
    auto listing = opened->entries();
    if (!listing) return files;
    const dstd::StringView own =
        writer ? dstd::PathView(writer->path()).filename() : dstd::StringView();
    for (const dstd::fs::DirEntry& entry : *listing) {
        if (dstd::PathView(entry.name).extension() !=
            engine::DATA_FILE_SUFFIX) {
            continue;
        }
        if (entry.name == own) continue;
        const auto found = opened->stat(entry.name);
        files.emplace_back(dstd::String(entry.name), found ? found->size : 0);
    }
    std::sort(files.begin(), files.end());
    return files;
}

void Store::State::refresh_if_changed_locked() {
    static const std::uint64_t interval_ms =
        common::Env::get<std::uint64_t>(
            common::constants::indexer::INDEX_STORE_REFRESH_MS_ENV)
            .value_or(common::constants::indexer::INDEX_STORE_REFRESH_MS);
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
    if (common::steady_now_ns() < next_look.load(std::memory_order_acquire)) {
        std::shared_lock<std::shared_mutex> shared(published_mutex);
        if (published != nullptr &&
            published_version == version.load(std::memory_order_acquire)) {
            return published;
        }
    }
    memory::detail::Scope in_pool(*pool);
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
    const bool reentrant =
        coro::Executor::current() == taken.runtime->executor();
    coro::BlockingRegion block(reentrant ? taken.runtime->executor() : nullptr);
    auto closed = taken.runtime->submit(taken.writer->close()).get();
    bump_version();
    return closed;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::commit_task(
    dstd::SharedPtr<State> self, Batch batch) {
    auto opened = self->writer_for_use();
    if (!opened) co_return dstd::Unexpected<dstd::Error>(opened.error());
    const dstd::SharedPtr<engine::Writer> writer = *opened;
    if (!batch.erases_.empty()) {
        bool shared = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            for (const Namespace& known : self->namespaces) {
                if (known.id == batch.namespace_id_) shared = known.spec.shared;
            }
        }
        auto spec = writer->spec_when_no_copy(
            engine::NamespaceId{batch.namespace_id_}, batch.scope_);
        if (spec) {
            if (!shared) {
                auto view = Store(self).snapshot();
                if (!view)
                    co_return dstd::Unexpected<dstd::Error>(view.error());
                const Namespace ns{*spec, batch.namespace_id_};
                if (view->copy_info(ns, batch.scope_).state ==
                    ScopeState::Ready) {
                    co_return dstd::Unexpected<dstd::Error>(
                        foreign_copy_error());
                }
            }
            batch.erases_.clear();
        }
    }
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
    if (self->forked()) {
        co_return dstd::Unexpected<dstd::Error>(forked_handle_error());
    }
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
    if (self->forked()) {
        co_return dstd::Unexpected<dstd::Error>(forked_handle_error());
    }
    dstd::SharedPtr<engine::Writer> writer = self->take_writer().writer;
    if (!writer) co_return dstd::Expected<void, dstd::Error>{};
    auto closed = co_await writer->close();
    self->bump_version();
    co_return closed;
}

std::uint64_t internal::opens() noexcept {
    return open_count.load(std::memory_order_relaxed);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::State::pack_rewrite(
    dstd::SharedPtr<State> self, const engine::Snapshot& view,
    const engine::PackPlan& plan, PackReport& report) {
    using Failure = dstd::Unexpected<dstd::Error>;
    Store store(self);
    dstd::Vector<engine::CopyChange> changes;
    for (const engine::PackTarget& target : plan.targets) {
        auto handle = store.ns(target.spec);
        if (!handle) {
            if (!handle.error().is(dstd::Condition::InvalidArgument)) {
                co_return Failure(handle.error());
            }
            ++report.copies_skipped;
            continue;
        }
        engine::ChangeKind kind = engine::ChangeKind::Ready;
        if (target.state == ScopeState::Absent) {
            kind = engine::ChangeKind::Dropped;
        } else if (target.state == ScopeState::Failed) {
            kind = engine::ChangeKind::Failed;
        } else {
            engine::ScanCursor rows = view.scan(
                target.spec, target.scope, dstd::ByteView{}, dstd::ByteView{});
            std::size_t batch_bytes = 0;
            Batch batch = store.building_batch(*handle, target.scope);
            while (!rows.at_end()) {
                auto value = rows.value();
                if (!value) co_return Failure(value.error());
                batch.add(rows.key(), *value);
                batch_bytes += rows.key().size() + value->size();
                rows.next();
                if (batch_bytes < PACK_BATCH_BYTES) continue;
                auto committed = co_await commit_task(self, std::move(batch));
                if (!committed) co_return Failure(committed.error());
                batch = store.building_batch(*handle, target.scope);
                batch_bytes = 0;
            }
            if (rows.error()) co_return Failure(*rows.error());
            auto committed = co_await commit_task(self, std::move(batch));
            if (!committed) co_return Failure(committed.error());
        }
        changes.push_back(engine::CopyChange{
            engine::NamespaceId{handle->id}, &target.scope, target.params_hash,
            target.sources.empty() ? nullptr : &target.sources, kind,
            target.spec.shared && kind == engine::ChangeKind::Ready});
        ++report.copies_written;
    }
    if (changes.empty()) co_return dstd::Expected<void, dstd::Error>{};
    auto published = co_await publish_task(self, std::move(changes));
    if (!published) co_return Failure(published.error());
    co_return dstd::Expected<void, dstd::Error>{};
}

coro::CoroTask<dstd::Expected<PackReport, dstd::Error>> Store::State::pack_task(
    dstd::SharedPtr<State> self, PackOptions options) {
    using Failure = dstd::Unexpected<dstd::Error>;
    if (self->forked()) co_return Failure(forked_handle_error());
    auto closed_before = co_await close_task(self);
    if (!closed_before) co_return Failure(closed_before.error());

    auto view = [&] {
        coro::BlockingRegion blocking;
        return engine::Snapshot::open(self->dir.str());
    }();
    if (!view) co_return Failure(view.error());
    auto plan = view->pack_plan(options.small_file_bytes, options.dead_ratio);
    if (!plan) co_return Failure(plan.error());

    PackReport report;
    report.files_packed = plan->files_packed;
    auto rewritten = co_await pack_rewrite(self, *view, *plan, report);
    // Also on a failure: copies left building would refuse plain batches. They
    // stay hidden from readers, and the old copies stay current.
    auto closed_after = co_await close_task(self);
    if (!rewritten) co_return Failure(rewritten.error());
    if (!closed_after) co_return Failure(closed_after.error());

    auto after = [&] {
        coro::BlockingRegion blocking;
        return engine::Snapshot::open(self->dir.str());
    }();
    if (!after) co_return Failure(after.error());
    // Every file counts as small, so this reads no segment block: only the
    // list of replaced files is used.
    auto settled =
        after->pack_plan(std::numeric_limits<std::uint64_t>::max(), 2.0);
    if (!settled) co_return Failure(settled.error());
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const std::uint64_t keep_ns = options.keep_seconds * 1000000000ULL;
    coro::BlockingRegion blocking;
    for (const engine::ReplacedFile& file : settled->replaced) {
        bool old_enough = true;
        for (const dstd::String& name : file.replaced_by) {
            auto stamp =
                dstd::fs::mtime_ns((self->dir / dstd::PathView(name)).view());
            if (!stamp) {
                old_enough = false;
                break;
            }
            const std::uint64_t age =
                static_cast<std::uint64_t>(now) > *stamp
                    ? static_cast<std::uint64_t>(now) - *stamp
                    : 0;
            old_enough = old_enough && age >= keep_ns;
        }
        if (!old_enough) continue;
        auto removed =
            dstd::fs::remove((self->dir / dstd::PathView(file.name)).view());
        if (!removed.has_value()) {
            co_return Failure(
                dstd::Error(dstd::Condition::Io,
                            common::str_cat("store: cannot delete ", file.name,
                                            ": ", removed.error().message())));
        }
        ++report.files_deleted;
        report.bytes_deleted += file.size;
    }
    self->bump_version();
    co_return report;
}

dstd::Expected<Store, dstd::Error> Store::open(dstd::StringView dir) {
    auto state = State::get_or_open(dir);
    if (!state) return dstd::Unexpected<dstd::Error>(state.error());
    return Store(std::move(*state));
}

dstd::Expected<Namespace, dstd::Error> Store::ns(const NamespaceSpec& spec) {
    if (spec.name.empty()) return invalid_error();
    if (spec.shared && spec.fold != FoldKind::None) return invalid_error();
    if (state_->forked()) {
        return dstd::Unexpected<dstd::Error>(forked_handle_error());
    }
    memory::detail::Scope in_pool(*state_->pool);
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
        converted.push_back(engine::CopyChange{
            engine::NamespaceId{target.ns->id}, target.scope,
            target.params_hash, target.sources, kind,
            target.replaces_older && target.ns->spec.shared &&
                kind == engine::ChangeKind::Ready});
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
    auto erased =
        (*opened)->erase(engine::NamespaceId{ns.id}, scope, begin, end);
    state_->bump_version();
    if (!erased.has_value())
        return dstd::Unexpected<dstd::Error>(erased.error());
    if (*erased || ns.spec.shared) return {};
    auto view = snapshot();
    if (!view) return dstd::Unexpected<dstd::Error>(view.error());
    if (view->copy_info(ns, scope).state != ScopeState::Ready) return {};
    return dstd::Unexpected<dstd::Error>(foreign_copy_error());
}

coro::CoroTask<dstd::Expected<PackReport, dstd::Error>> Store::pack(
    PackOptions options) {
    return State::pack_task(state_, options);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::flush() {
    return State::flush_task(state_);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Store::close() {
    return State::close_task(state_);
}

dstd::Expected<void, dstd::Error> Store::refresh() {
    if (state_->forked()) {
        return dstd::Unexpected<dstd::Error>(forked_handle_error());
    }
    memory::detail::Scope in_pool(*state_->pool);
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->foreign_checked = {};
    state_->refresh_if_changed_locked();
    return {};
}

dstd::Expected<Snapshot, dstd::Error> Store::snapshot() {
    if (state_->forked()) {
        return dstd::Unexpected<dstd::Error>(forked_handle_error());
    }
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
            auto state = entry.second.lock();
            if (state && !state->forked()) live.push_back(std::move(state));
        }
    }
    dstd::Expected<void, dstd::Error> first;
    for (const auto& state : live) {
        memory::detail::Scope in_pool(*state->pool);
        state->drop_published();
        auto closed = state->close_blocking();
        if (!closed && first) first = std::move(closed);
    }
    return first;
}

dstd::Expected<std::size_t, dstd::Error> Store::adopt(dstd::StringView from_dir,
                                                      dstd::StringView to_dir) {
    auto moved = engine::move_data_files(from_dir, to_dir);
    const dstd::Path key = directory_key(to_dir);
    dstd::SharedPtr<State> live;
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        auto found = State::registry().find(key.str());
        if (found != State::registry().end()) live = found->second.lock();
        if (live && live->forked()) live = nullptr;
    }
    if (live) {
        std::lock_guard<std::mutex> lock(live->mutex);
        live->foreign_checked = {};
        live->next_look.store(0, std::memory_order_release);
    }
    return moved;
}

}  // namespace dftracer::utils::store
