#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_backend.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>

namespace dftracer::utils::index {

namespace store_layout = store::store_layout;

using Ext = store::IndexExtension;

namespace {

/// A copy with no record would not publish, so it holds this one.
constexpr std::string_view EMPTY_COPY_KEY = "\xFF";

bool copy_exists(kv::ScopeState state) {
    return state == kv::ScopeState::Ready || state == kv::ScopeState::Failed;
}

bool same_stat(const SourceStat& a, const SourceStat& b) {
    return a.mtime == b.mtime && a.size == b.size;
}

kv::Namespace read_ns(Ext ext) {
    kv::Namespace ns;
    ns.spec = ext_spec(ext);
    return ns;
}

bool builtin(Ext ext) { return ext != Ext::Plugin && ext != Ext::Host; }

bool has_name(const dstd::Vector<kv::Namespace>& list,
              const kv::Namespace& ns) {
    for (const kv::Namespace& known : list)
        if (known.spec.name == ns.spec.name) return true;
    return false;
}

/// The newest declared version of every index namespace that holds a copy of
/// `scope`.
dstd::Expected<dstd::Vector<kv::Namespace>, dstd::Error> held_copies(
    kv::Store& store, const kv::Snapshot& taken, const kv::ScopeKey& scope) {
    auto declared = taken.namespaces();
    if (!declared) return dstd::Unexpected<dstd::Error>(declared.error());
    const std::string_view prefix(store_layout::NAMESPACE_PREFIX);
    const std::string_view meta(store_layout::AGG_META_NAMESPACE_PREFIX);
    std::map<std::string, kv::NamespaceSpec> latest;
    for (const kv::NamespaceDecl& decl : *declared) {
        const std::string_view name(decl.name.data(), decl.name.size());
        if (!name.starts_with(prefix) || name.starts_with(meta)) continue;
        kv::NamespaceSpec& known = latest[std::string(name)];
        if (known.name.empty() || decl.format_version > known.format_version) {
            known.name = decl.name;
            known.format_version = decl.format_version;
        }
    }
    dstd::Vector<kv::Namespace> held;
    held.reserve(latest.size());
    for (const auto& [name, spec] : latest) {
        kv::Namespace ns;
        ns.spec = spec;
        if (taken.copy_info(ns, scope).state == kv::ScopeState::Absent)
            continue;
        auto registered = store.ns(spec);
        if (!registered)
            return dstd::Unexpected<dstd::Error>(registered.error());
        held.push_back(*registered);
    }
    return held;
}

dstd::Unexpected<dstd::Error> invalid(std::string_view message) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument, message));
}

}  // namespace

dstd::StringView FileRef::path() const {
    return dstd::StringView(data_->path.data(), data_->path.size());
}

const SourceStat& FileRef::stat() const { return data_->stat; }

dstd::Expected<kv::Store, dstd::Error> Index::Impl::open_store() {
    std::lock_guard<std::mutex> lock(mutex);
    if (store) return *store;
    auto opened = kv::Store::open(store_dir);
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    store = std::move(*opened);
    return *store;
}

dstd::Expected<dstd::Optional<kv::Store>, dstd::Error>
Index::Impl::find_store() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (store) return store;
    }
    std::error_code missing;
    if (!std::filesystem::exists(store_dir, missing))
        return dstd::Optional<kv::Store>{};
    auto opened = open_store();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    return dstd::Optional<kv::Store>(std::move(*opened));
}

FileRef IndexAccess::make_file(dstd::String path, kv::ScopeKey scope,
                               const SourceStat& stat) {
    auto data = dstd::make_shared<FileRef::Data>();
    data->path = std::move(path);
    data->scope = std::move(scope);
    data->stat = stat;
    return FileRef(std::move(data));
}

Snapshot IndexAccess::make_snapshot(dstd::Optional<kv::Snapshot> snap) {
    auto state = dstd::make_shared<Snapshot::Impl>();
    state->snap = std::move(snap);
    return Snapshot(std::move(state));
}

FileBuild IndexAccess::make_build(std::string root, kv::Store store,
                                  kv::ScopeKey scope, const SourceStat& stat) {
    return FileBuild(dstd::make_shared<FileBuild::Impl>(
        std::move(root), std::move(store), std::move(scope), stat));
}

const std::string& IndexAccess::root(const Index& index) {
    return index.impl_->root;
}

dstd::Expected<kv::Store, dstd::Error> IndexAccess::open_store(
    const Index& index) {
    return index.impl_->open_store();
}

dstd::Expected<dstd::Optional<kv::Store>, dstd::Error> IndexAccess::find_store(
    const Index& index) {
    return index.impl_->find_store();
}

void note_applied(const std::string& root,
                  dstd::Span<const kv::Publish> applied) {
    const auto held = store::StoreRoot::find(root);
    if (!held) return;
    for (const kv::Publish& change : applied) {
        const std::string name(change.ns->spec.name.data(),
                               change.ns->spec.name.size());
        switch (change.change) {
            case kv::Publish::Change::Ready:
                held->note_published(name, *change.scope, change.params_hash,
                                     change.sources);
                break;
            case kv::Publish::Change::Failed:
                held->note_published(name, *change.scope, change.params_hash,
                                     nullptr);
                break;
            case kv::Publish::Change::Dropped:
                held->note_dropped(name, *change.scope);
                break;
        }
    }
}

const kv::Snapshot* IndexAccess::store_snapshot(const Snapshot& snapshot) {
    const auto& held = snapshot.impl_->snap;
    return held ? &*held : nullptr;
}

const kv::ScopeKey& IndexAccess::scope(const FileRef& file) {
    return file.data_->scope;
}

void IndexAccess::record_error(FileBuild& build, dstd::Error error) {
    FileBuild::Impl& impl = *build.impl_;
    if (!impl.error) impl.error = std::move(error);
}

void IndexAccess::drop_unwritten(FileBuild& build,
                                 dstd::Span<const kv::NamespaceSpec> kept) {
    FileBuild::Impl& impl = *build.impl_;
    impl.drop_unwritten = true;
    for (const kv::NamespaceSpec& spec : kept) impl.kept.push_back(spec.name);
}

void IndexAccess::put(FileBuild& build, const kv::NamespaceSpec& spec,
                      dstd::ByteView key, dstd::ByteView value) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    FileBuild::Impl::Copy* copy = impl.copy_of(spec);
    if (copy == nullptr) return;
    copy->batch.put(key, value);
    ++copy->puts;
}

void IndexAccess::erase(FileBuild& build, const kv::NamespaceSpec& spec,
                        dstd::ByteView begin, dstd::ByteView end) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error || impl.committed) return;
    for (FileBuild::Impl::Copy& copy : impl.copies) {
        if (copy.ns.spec.name != spec.name) continue;
        auto erased = impl.store.erase(copy.ns, impl.scope, begin, end);
        if (!erased) impl.error = erased.error();
        return;
    }
}

void IndexAccess::finish(FileBuild& build, const kv::NamespaceSpec& spec,
                         std::uint64_t params_hash) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    FileBuild::Impl::Copy* copy = impl.copy_of(spec);
    if (copy == nullptr) return;
    copy->params_hash = params_hash;
    copy->finished = true;
    copy->failed = false;
}

void IndexAccess::fail(FileBuild& build, const kv::NamespaceSpec& spec,
                       std::uint64_t params_hash) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    FileBuild::Impl::Copy* copy = impl.copy_of(spec);
    if (copy == nullptr) return;
    copy->params_hash = params_hash;
    copy->failed = true;
    copy->finished = false;
}

void IndexAccess::clear(FileBuild& build, const kv::NamespaceSpec& spec) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    for (const FileBuild::Impl::Copy& known : impl.copies) {
        if (known.ns.spec.name == spec.name) {
            impl.error = dstd::Error(dstd::Condition::InvalidArgument,
                                     "index: an extension is written and "
                                     "cleared");
            return;
        }
    }
    for (const kv::Namespace& known : impl.cleared) {
        if (known.spec.name == spec.name) return;
    }
    auto registered = impl.store.ns(spec);
    if (!registered) {
        impl.error = registered.error();
        return;
    }
    impl.cleared.push_back(*registered);
}

FileBuild::Impl::Impl(std::string index_root, kv::Store s, kv::ScopeKey key,
                      const SourceStat& stat)
    : root(std::move(index_root)), store(std::move(s)), scope(std::move(key)) {
    sources.push_back(stat);
}

FileBuild::Impl::Copy* FileBuild::Impl::copy_of(const kv::NamespaceSpec& spec) {
    for (const kv::Namespace& known : cleared) {
        if (known.spec.name != spec.name) continue;
        error = dstd::Error(dstd::Condition::InvalidArgument,
                            "index: an extension is written and cleared");
        return nullptr;
    }
    for (Copy& known : copies) {
        if (known.ns.spec.name != spec.name) continue;
        if (known.ns.spec != spec) {
            error = dstd::Error(dstd::Condition::InvalidArgument,
                                "index: one namespace at two versions");
            return nullptr;
        }
        return &known;
    }
    auto registered = store.ns(spec);
    if (!registered) {
        error = registered.error();
        return nullptr;
    }
    kv::Batch batch = store.building_batch(*registered, scope);
    copies.push_back(
        Copy{*registered, std::move(batch), 0, 0, 0, false, false});
    return &copies.back();
}

coro::AsyncMutex& IndexAccess::write_mutex(FileBuild& build) {
    return build.impl_->write_mutex;
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> IndexAccess::stage(
    FileBuild& build) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) co_return dstd::Unexpected<dstd::Error>(*impl.error);
    if (impl.committed) co_return invalid("index: the build is committed");
    for (FileBuild::Impl::Copy& copy : impl.copies) {
        if (copy.puts == 0 || copy.failed) continue;
        auto written = co_await impl.store.commit(std::move(copy.batch));
        if (!written) {
            impl.error = written.error();
            co_return dstd::Unexpected<dstd::Error>(written.error());
        }
        copy.batch = impl.store.building_batch(copy.ns, impl.scope);
        copy.staged_puts += copy.puts;
        copy.puts = 0;
    }
    co_return dstd::Expected<void, dstd::Error>{};
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Claim::stage() {
    return IndexAccess::stage(*build_);
}

coro::CoroTask<dstd::UniquePtr<Claim>> claim(FileBuild& build) {
    coro::AsyncMutex& mutex = IndexAccess::write_mutex(build);
    co_await mutex.lock();
    co_return dstd::make_unique<Claim>(build, coro::AsyncMutexGuard(mutex));
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> FileBuild::Impl::commit_task(
    dstd::SharedPtr<Impl> self) {
    Impl& build = *self;
    if (build.error) co_return dstd::Unexpected<dstd::Error>(*build.error);
    if (build.committed) co_return invalid("index: the build is committed");
    build.committed = true;
    for (const Copy& copy : build.copies) {
        if (!copy.finished && !copy.failed)
            co_return invalid("index: an extension is not finished");
    }
    if (build.drop_unwritten) {
        auto taken = build.store.snapshot();
        if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
        auto held = held_copies(build.store, *taken, build.scope);
        if (!held) co_return dstd::Unexpected<dstd::Error>(held.error());
        dstd::Vector<kv::Namespace> written;
        for (const Copy& copy : build.copies) written.push_back(copy.ns);
        for (const kv::Namespace& ns : *held) {
            const bool kept = std::find(build.kept.begin(), build.kept.end(),
                                        ns.spec.name) != build.kept.end();
            if (!kept && !has_name(written, ns) && !has_name(build.cleared, ns))
                build.cleared.push_back(ns);
        }
    }
    dstd::Vector<kv::Publish> targets;
    targets.reserve(build.copies.size() + build.cleared.size());
    for (Copy& copy : build.copies) {
        if (copy.failed) {
            targets.push_back(kv::Publish{&copy.ns, &build.scope,
                                          copy.params_hash, nullptr,
                                          kv::Publish::Change::Failed});
            continue;
        }
        const bool empty = copy.puts == 0 && copy.staged_puts == 0;
        if (empty)
            copy.batch.put(dstd::ByteView(EMPTY_COPY_KEY), dstd::ByteView());
        if (empty || copy.puts != 0) {
            auto written = co_await build.store.commit(std::move(copy.batch));
            if (!written)
                co_return dstd::Unexpected<dstd::Error>(written.error());
        }
        targets.push_back(kv::Publish{&copy.ns, &build.scope, copy.params_hash,
                                      &build.sources});
    }
    if (!build.cleared.empty()) {
        auto taken = build.store.snapshot();
        if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
        for (const kv::Namespace& ns : build.cleared) {
            if (taken->copy_info(ns, build.scope).state ==
                kv::ScopeState::Absent)
                continue;
            targets.push_back(kv::Publish{&ns, &build.scope, 0, nullptr,
                                          kv::Publish::Change::Dropped});
        }
    }
    if (!targets.empty()) {
        auto published = co_await build.store.publish(
            dstd::Span<const kv::Publish>(targets.data(), targets.size()));
        if (!published)
            co_return dstd::Unexpected<dstd::Error>(published.error());
        note_applied(build.root, dstd::Span<const kv::Publish>(targets.data(),
                                                               targets.size()));
    }
    co_return dstd::Expected<void, dstd::Error>{};
}

void FileBuild::clear(store::IndexExtension ext) {
    if (!builtin(ext)) {
        IndexAccess::record_error(
            *this, dstd::Error(dstd::Condition::InvalidArgument,
                               "index: not a built-in extension"));
        return;
    }
    IndexAccess::clear(*this, ext_spec(ext));
}

void FileBuild::finish(store::IndexExtension ext, std::uint64_t params_hash) {
    if (!builtin(ext)) {
        IndexAccess::record_error(
            *this, dstd::Error(dstd::Condition::InvalidArgument,
                               "index: not a built-in extension"));
        return;
    }
    IndexAccess::finish(*this, ext_spec(ext), params_hash);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> FileBuild::commit() {
    return Impl::commit_task(impl_);
}

dstd::Optional<FileRef> Snapshot::file(dstd::StringView path) const {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return std::nullopt;
    kv::ScopeKey scope = kv::ScopeKey::file(path);
    const kv::CopyInfo info = snap->copy_info(read_ns(Ext::Members), scope);
    if (!copy_exists(info.state)) return std::nullopt;
    return IndexAccess::make_file(
        dstd::String(path.data(), path.size()), std::move(scope),
        info.sources.empty() ? SourceStat{} : info.sources[0]);
}

dstd::Expected<dstd::Vector<FileRef>, dstd::Error> Snapshot::files() const {
    dstd::Vector<FileRef> out;
    const kv::Snapshot* snap = IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return out;
    auto listed = snap->scopes(read_ns(Ext::Members));
    if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
    for (kv::ScopeEntry& entry : *listed) {
        if (entry.scope.kind != kv::ScopeKind::File ||
            entry.scope.paths.empty() || !copy_exists(entry.copy.state))
            continue;
        dstd::String path = entry.scope.paths[0];
        out.push_back(IndexAccess::make_file(
            std::move(path), std::move(entry.scope),
            entry.copy.sources.empty() ? SourceStat{} : entry.copy.sources[0]));
    }
    return out;
}

Freshness Snapshot::freshness(dstd::StringView path,
                              const SourceStat& now) const {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return Freshness::Missing;
    const kv::CopyInfo info =
        snap->copy_info(read_ns(Ext::Members), kv::ScopeKey::file(path));
    if (!copy_exists(info.state)) return Freshness::Missing;
    if (!info.sources.empty()) {
        if (!same_stat(info.sources[0], now)) return Freshness::Stale;
    } else if (info.state == kv::ScopeState::Ready) {
        return Freshness::Stale;
    }
    return info.state == kv::ScopeState::Failed ? Freshness::Failed
                                                : Freshness::Current;
}

Freshness Snapshot::extension(const FileRef& file, store::IndexExtension ext,
                              std::uint64_t params_hash) const {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(*this);
    if (snap == nullptr || !builtin(ext)) return Freshness::Missing;
    const kv::CopyInfo info =
        snap->copy_info(read_ns(ext), IndexAccess::scope(file));
    switch (info.state) {
        case kv::ScopeState::Absent:
            return Freshness::Missing;
        case kv::ScopeState::Failed:
        case kv::ScopeState::Bad:
            return Freshness::Failed;
        case kv::ScopeState::Ready:
            break;
    }
    if (!info.sources.empty() && !same_stat(info.sources[0], file.stat()))
        return Freshness::Stale;
    return info.params_hash == params_hash ? Freshness::Current
                                           : Freshness::Stale;
}

dstd::Expected<Index, dstd::Error> Index::open(dstd::StringView root) {
    if (root.empty()) return invalid("index: the root is empty");
    auto impl = dstd::make_shared<Impl>();
    impl->root = std::string(root);
    impl->store_dir = (std::filesystem::path(impl->root) / "store").string();
    return Index(std::move(impl));
}

dstd::Expected<Snapshot, dstd::Error> Index::snapshot() const {
    auto found = impl_->find_store();
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return IndexAccess::make_snapshot(std::nullopt);
    auto taken = (*found)->snapshot();
    if (!taken) return dstd::Unexpected<dstd::Error>(taken.error());
    return IndexAccess::make_snapshot(std::move(*taken));
}

dstd::Expected<FileBuild, dstd::Error> Index::build(
    dstd::StringView path, const SourceStat& stat) const {
    if (path.empty()) return invalid("index: the path is empty");
    auto opened = impl_->open_store();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    return IndexAccess::make_build(impl_->root, std::move(*opened),
                                   kv::ScopeKey::file(path), stat);
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Index::remove(
    dstd::StringView path) const {
    return IndexAccess::remove_task(impl_,
                                    dstd::String(path.data(), path.size()));
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> IndexAccess::remove_task(
    dstd::SharedPtr<Index::Impl> impl, dstd::String path) {
    auto found = impl->find_store();
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return dstd::Expected<void, dstd::Error>{};
    kv::Store store = **found;
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    const kv::ScopeKey scope =
        kv::ScopeKey::file(dstd::StringView(path.data(), path.size()));
    auto namespaces = held_copies(store, *taken, scope);
    if (!namespaces)
        co_return dstd::Unexpected<dstd::Error>(namespaces.error());
    dstd::Vector<kv::Publish> targets;
    for (const kv::Namespace& ns : *namespaces) {
        targets.push_back(
            kv::Publish{&ns, &scope, 0, nullptr, kv::Publish::Change::Dropped});
    }
    if (targets.empty()) co_return dstd::Expected<void, dstd::Error>{};
    auto applied = co_await store.publish(
        dstd::Span<const kv::Publish>(targets.data(), targets.size()));
    if (!applied) co_return dstd::Unexpected<dstd::Error>(applied.error());
    note_applied(impl->root,
                 dstd::Span<const kv::Publish>(targets.data(), targets.size()));
    co_return dstd::Expected<void, dstd::Error>{};
}

}  // namespace dftracer::utils::index
