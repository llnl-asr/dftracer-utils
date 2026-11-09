// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/list.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace dftracer::utils::index {

using Ext = store::IndexExtension;

namespace {

/// A copy with no record would not publish, so it holds this one.
constexpr dstd::StringView EMPTY_COPY_KEY = "\xFF";

bool copy_exists(dftracer::utils::store::ScopeState state) {
    return state == dftracer::utils::store::ScopeState::Ready ||
           state == dftracer::utils::store::ScopeState::Failed;
}

bool same_stat(const SourceStat& a, const SourceStat& b) {
    return a.mtime == b.mtime && a.size == b.size;
}

dftracer::utils::store::Namespace read_ns(Ext ext) {
    dftracer::utils::store::Namespace ns;
    ns.spec = ext_spec(ext);
    return ns;
}

bool builtin(Ext ext) { return ext != Ext::Plugin; }

bool has_name(const dstd::Vector<dftracer::utils::store::Namespace>& list,
              const dftracer::utils::store::Namespace& ns) {
    for (const dftracer::utils::store::Namespace& known : list)
        if (known.spec.name == ns.spec.name) return true;
    return false;
}

/// The newest declared version of every index namespace that holds a copy of
/// `scope`.
dstd::Expected<dstd::Vector<dftracer::utils::store::Namespace>, dstd::Error>
held_copies(dftracer::utils::store::Store& store,
            const dftracer::utils::store::Snapshot& taken,
            const dftracer::utils::store::ScopeKey& scope) {
    auto declared = taken.namespaces();
    if (!declared) return dstd::Unexpected<dstd::Error>(declared.error());
    const dstd::StringView prefix(store::store_layout::NAMESPACE_PREFIX);
    const dstd::StringView meta(store::store_layout::AGG_META_NAMESPACE_PREFIX);
    dstd::Map<dstd::String, dftracer::utils::store::NamespaceSpec> latest;
    for (const dftracer::utils::store::NamespaceDecl& decl : *declared) {
        const dstd::StringView name(decl.name.data(), decl.name.size());
        if (!name.starts_with(prefix) || name.starts_with(meta)) continue;
        dftracer::utils::store::NamespaceSpec& known =
            latest[dstd::String(name)];
        if (known.name.empty() || decl.format_version > known.format_version) {
            known.name = decl.name;
            known.format_version = decl.format_version;
            known.shared = decl.shared;
        }
    }
    dstd::Vector<dftracer::utils::store::Namespace> held;
    held.reserve(latest.size());
    for (const auto& [name, spec] : latest) {
        dftracer::utils::store::Namespace ns;
        ns.spec = spec;
        if (taken.copy_info(ns, scope).state ==
            dftracer::utils::store::ScopeState::Absent)
            continue;
        auto registered = store.ns(spec);
        if (!registered)
            return dstd::Unexpected<dstd::Error>(registered.error());
        held.push_back(*registered);
    }
    return held;
}

dstd::Unexpected<dstd::Error> invalid(dstd::StringView message) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument, message));
}

}  // namespace

dstd::StringView FileRef::path() const {
    return dstd::StringView(data_->path.data(), data_->path.size());
}

const SourceStat& FileRef::stat() const { return data_->stat; }

dstd::Expected<dftracer::utils::store::Store, dstd::Error>
Index::Impl::open_store() {
    std::lock_guard<std::mutex> lock(mutex);
    if (store) return *store;
    auto opened = dftracer::utils::store::Store::open(store_dir);
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    store = std::move(*opened);
    return *store;
}

dstd::Expected<dstd::Optional<dftracer::utils::store::Store>, dstd::Error>
Index::Impl::find_store() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (store) return store;
    }
    const auto present = dstd::fs::exists(dstd::PathView(store_dir));
    if (!present.has_value() || !*present)
        return dstd::Optional<dftracer::utils::store::Store>{};
    auto opened = open_store();
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    return dstd::Optional<dftracer::utils::store::Store>(std::move(*opened));
}

FileRef IndexAccess::make_file(dstd::String path,
                               dftracer::utils::store::ScopeKey scope,
                               const SourceStat& stat) {
    auto data = dstd::make_shared<FileRef::Data>();
    data->path = std::move(path);
    data->scope = std::move(scope);
    data->stat = stat;
    return FileRef(std::move(data));
}

Snapshot IndexAccess::make_snapshot(
    dstd::Optional<dftracer::utils::store::Snapshot> snap) {
    auto state = dstd::make_shared<Snapshot::Impl>();
    state->snap = std::move(snap);
    return Snapshot(std::move(state));
}

FileBuild IndexAccess::make_build(dstd::String root,
                                  dftracer::utils::store::Store store,
                                  dftracer::utils::store::ScopeKey scope,
                                  const SourceStat& stat) {
    return FileBuild(dstd::make_shared<FileBuild::Impl>(
        std::move(root), std::move(store), std::move(scope), stat));
}

const dstd::String& IndexAccess::root(const Index& index) {
    return index.impl_->root;
}

dstd::Expected<dftracer::utils::store::Store, dstd::Error>
IndexAccess::open_store(const Index& index) {
    return index.impl_->open_store();
}

dstd::Expected<dstd::Optional<dftracer::utils::store::Store>, dstd::Error>
IndexAccess::find_store(const Index& index) {
    return index.impl_->find_store();
}

const dftracer::utils::store::Snapshot* IndexAccess::store_snapshot(
    const Snapshot& snapshot) {
    const auto& held = snapshot.impl_->snap;
    return held ? &*held : nullptr;
}

const dftracer::utils::store::ScopeKey& IndexAccess::scope(
    const FileRef& file) {
    return file.data_->scope;
}

void IndexAccess::record_error(FileBuild& build, dstd::Error error) {
    FileBuild::Impl& impl = *build.impl_;
    if (!impl.error) impl.error = std::move(error);
}

void IndexAccess::drop_unwritten(
    FileBuild& build,
    dstd::Span<const dftracer::utils::store::NamespaceSpec> kept) {
    FileBuild::Impl& impl = *build.impl_;
    impl.drop_unwritten = true;
    for (const dftracer::utils::store::NamespaceSpec& spec : kept)
        impl.kept.push_back(spec.name);
}

void FileBuild::Impl::put_to(Copy* copy, dstd::ByteView key,
                             dstd::ByteView value) {
    if (copy == nullptr) return;
    if (!copy->batch)
        copy->batch.emplace(store.building_batch(copy->ns, scope));
    copy->batch->put(key, value);
    ++copy->puts;
}

void IndexAccess::put(FileBuild& build,
                      const dftracer::utils::store::NamespaceSpec& spec,
                      dstd::ByteView key, dstd::ByteView value) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    impl.put_to(impl.copy_of(spec), key, value);
}

void IndexAccess::put(FileBuild& build, store::IndexExtension ext,
                      dstd::ByteView key, dstd::ByteView value) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    impl.put_to(impl.copy_of(ext), key, value);
}

void IndexAccess::erase(FileBuild& build,
                        const dftracer::utils::store::NamespaceSpec& spec,
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

void IndexAccess::finish(FileBuild& build,
                         const dftracer::utils::store::NamespaceSpec& spec,
                         std::uint64_t params_hash) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    FileBuild::Impl::Copy* copy = impl.copy_of(spec);
    if (copy == nullptr) return;
    copy->params_hash = params_hash;
    copy->finished = true;
    copy->failed = false;
}

void IndexAccess::fail(FileBuild& build,
                       const dftracer::utils::store::NamespaceSpec& spec,
                       std::uint64_t params_hash) {
    FileBuild::Impl& impl = *build.impl_;
    if (impl.error) return;
    FileBuild::Impl::Copy* copy = impl.copy_of(spec);
    if (copy == nullptr) return;
    copy->params_hash = params_hash;
    copy->failed = true;
    copy->finished = false;
}

void IndexAccess::clear(FileBuild& build,
                        const dftracer::utils::store::NamespaceSpec& spec) {
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
    for (const dftracer::utils::store::Namespace& known : impl.cleared) {
        if (known.spec.name == spec.name) return;
    }
    auto registered = impl.store.ns(spec);
    if (!registered) {
        impl.error = registered.error();
        return;
    }
    impl.cleared.push_back(*registered);
}

FileBuild::Impl::Impl(dstd::String index_root, dftracer::utils::store::Store s,
                      dftracer::utils::store::ScopeKey key,
                      const SourceStat& stat)
    : root(std::move(index_root)), store(std::move(s)), scope(std::move(key)) {
    ext_copy.fill(-1);
    sources.push_back(stat);
}

FileBuild::Impl::Copy* FileBuild::Impl::copy_of(store::IndexExtension ext) {
    std::int16_t& known = ext_copy[static_cast<std::size_t>(ext)];
    if (known >= 0) return &copies[static_cast<std::size_t>(known)];
    Copy* found = copy_of(ext_spec(ext));
    if (found != nullptr)
        known = static_cast<std::int16_t>(found - copies.data());
    return found;
}

FileBuild::Impl::Copy* FileBuild::Impl::copy_of(
    const dftracer::utils::store::NamespaceSpec& spec) {
    for (const dftracer::utils::store::Namespace& known : cleared) {
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
    copies.push_back(Copy{*registered, dstd::nullopt, 0, 0, 0, false, false});
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
        auto written = co_await impl.store.commit(std::move(*copy.batch));
        copy.batch.reset();
        if (!written) {
            impl.error = written.error();
            co_return dstd::Unexpected<dstd::Error>(written.error());
        }
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

coro::CoroTask<dstd::Expected<void, dstd::Error>> FileBuild::Impl::prepare(
    Impl& build, dstd::Vector<dftracer::utils::store::Publish>& targets) {
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
        dstd::Vector<dftracer::utils::store::Namespace> written;
        for (const Copy& copy : build.copies) written.push_back(copy.ns);
        for (const dftracer::utils::store::Namespace& ns : *held) {
            const bool kept = std::find(build.kept.begin(), build.kept.end(),
                                        ns.spec.name) != build.kept.end();
            if (!kept && !has_name(written, ns) && !has_name(build.cleared, ns))
                build.cleared.push_back(ns);
        }
    }
    targets.reserve(targets.size() + build.copies.size() +
                    build.cleared.size());
    for (Copy& copy : build.copies) {
        if (copy.failed) {
            targets.push_back(dftracer::utils::store::Publish{
                &copy.ns, &build.scope, copy.params_hash, nullptr,
                dftracer::utils::store::Publish::Change::Failed});
            continue;
        }
        const bool empty = copy.puts == 0 && copy.staged_puts == 0;
        if (empty) {
            copy.batch.emplace(
                build.store.building_batch(copy.ns, build.scope));
            copy.batch->put(dstd::ByteView(EMPTY_COPY_KEY), dstd::ByteView());
        }
        if (empty || copy.puts != 0) {
            auto written = co_await build.store.commit(std::move(*copy.batch));
            if (!written)
                co_return dstd::Unexpected<dstd::Error>(written.error());
        }
        targets.push_back(dftracer::utils::store::Publish{
            &copy.ns, &build.scope, copy.params_hash, &build.sources,
            dftracer::utils::store::Publish::Change::Ready,
            build.replaces_older});
    }
    if (!build.cleared.empty()) {
        auto taken = build.store.snapshot();
        if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
        for (const dftracer::utils::store::Namespace& ns : build.cleared) {
            if (taken->copy_info(ns, build.scope).state ==
                dftracer::utils::store::ScopeState::Absent)
                continue;
            targets.push_back(dftracer::utils::store::Publish{
                &ns, &build.scope, 0, nullptr,
                dftracer::utils::store::Publish::Change::Dropped});
        }
    }
    co_return dstd::Expected<void, dstd::Error>{};
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> FileBuild::Impl::commit_task(
    dstd::SharedPtr<Impl> self) {
    Impl& build = *self;
    dstd::Vector<dftracer::utils::store::Publish> targets;
    auto prepared = co_await prepare(build, targets);
    if (!prepared) co_return prepared;
    if (!targets.empty()) {
        auto published = co_await build.store.publish(
            dstd::Span<const dftracer::utils::store::Publish>(targets.data(),
                                                              targets.size()));
        if (!published)
            co_return dstd::Unexpected<dstd::Error>(published.error());
    }
    co_return dstd::Expected<void, dstd::Error>{};
}

coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
FileBuild::Impl::commit_group_task(dstd::Vector<dstd::SharedPtr<Impl>> group) {
    const std::size_t n = group.size();
    dstd::Vector<dstd::Optional<dstd::Error>> errors(n);
    dstd::Vector<dftracer::utils::store::Publish> targets;
    dstd::Vector<std::size_t> begin(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        begin[i] = targets.size();
        auto prepared = co_await prepare(*group[i], targets);
        if (!prepared) {
            errors[i] = prepared.error();
            targets.resize(begin[i], dftracer::utils::store::Publish{});
        }
    }
    begin[n] = targets.size();
    if (targets.empty()) co_return errors;
    dftracer::utils::store::Store store = group[0]->store;
    auto published = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(targets.data(),
                                                          targets.size()));
    for (std::size_t i = 0; i < n; ++i) {
        if (errors[i] || begin[i] == begin[i + 1]) continue;
        if (!published) errors[i] = published.error();
    }
    co_return errors;
}

coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
IndexAccess::commit_group(dstd::Span<FileBuild* const> builds) {
    dstd::Vector<dstd::SharedPtr<FileBuild::Impl>> group;
    group.reserve(builds.size());
    for (FileBuild* build : builds) group.push_back(build->impl_);
    return FileBuild::Impl::commit_group_task(std::move(group));
}

coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>> commit_group(
    dstd::Span<FileBuild* const> builds) {
    return IndexAccess::commit_group(builds);
}

void FileBuild::keep_older_copies() { impl_->replaces_older = false; }

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
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return dstd::nullopt;
    dftracer::utils::store::ScopeKey scope =
        dftracer::utils::store::ScopeKey::file(path);
    const dftracer::utils::store::CopyInfo info =
        snap->copy_info(read_ns(Ext::Members), scope);
    if (!copy_exists(info.state)) return dstd::nullopt;
    return IndexAccess::make_file(
        dstd::String(path.data(), path.size()), std::move(scope),
        info.sources.empty() ? SourceStat{} : info.sources[0]);
}

dstd::Expected<dstd::Vector<FileRef>, dstd::Error> Snapshot::files() const {
    dstd::Vector<FileRef> out;
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return out;
    auto listed = snap->scopes(read_ns(Ext::Members));
    if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
    for (dftracer::utils::store::ScopeEntry& entry : *listed) {
        if (entry.scope.kind != dftracer::utils::store::ScopeKind::File ||
            entry.scope.paths.empty() || !copy_exists(entry.copy.state))
            continue;
        dstd::String path = entry.scope.paths[0];
        out.push_back(IndexAccess::make_file(
            std::move(path), std::move(entry.scope),
            entry.copy.sources.empty() ? SourceStat{} : entry.copy.sources[0]));
    }
    return out;
}

bool is_append_of(dstd::StringView path, std::uint64_t size,
                  const SourceStat& stored) {
    if (size <= stored.size) return false;
    auto file = dstd::File::open(dstd::PathView(path), {});
    if (!file) return false;
    return store::internal::calculate_file_hash(*file, stored.size) ==
           stored.hash;
}

Freshness Snapshot::freshness(dstd::StringView path,
                              const SourceStat& now) const {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(*this);
    if (snap == nullptr) return Freshness::Missing;
    const dftracer::utils::store::CopyInfo info = snap->copy_info(
        read_ns(Ext::Members), dftracer::utils::store::ScopeKey::file(path));
    if (!copy_exists(info.state)) return Freshness::Missing;
    if (!info.sources.empty()) {
        if (!same_stat(info.sources[0], now)) return Freshness::Stale;
    } else if (info.state == dftracer::utils::store::ScopeState::Ready) {
        return Freshness::Stale;
    }
    return info.state == dftracer::utils::store::ScopeState::Failed
               ? Freshness::Failed
               : Freshness::Current;
}

Freshness Snapshot::extension(const FileRef& file, store::IndexExtension ext,
                              std::uint64_t params_hash) const {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(*this);
    if (snap == nullptr || !builtin(ext)) return Freshness::Missing;
    const dftracer::utils::store::CopyInfo info =
        snap->copy_info(read_ns(ext), IndexAccess::scope(file));
    switch (info.state) {
        case dftracer::utils::store::ScopeState::Absent:
            return Freshness::Missing;
        case dftracer::utils::store::ScopeState::Failed:
        case dftracer::utils::store::ScopeState::Bad:
            return Freshness::Failed;
        case dftracer::utils::store::ScopeState::Ready:
            break;
    }
    if (!info.sources.empty() && !same_stat(info.sources[0], file.stat()))
        return Freshness::Stale;
    return info.params_hash == params_hash ? Freshness::Current
                                           : Freshness::Stale;
}

namespace {

/// Roots kept open after their last user, so the steps of one query and a
/// reader that opens the index per file reuse one open store and its page
/// cache instead of reading the data file footers again.
constexpr std::size_t INDEX_ROOTS_RETAINED = 64;

class RetainedRoots {
   public:
    Index find_or_add(const dstd::String& root, Index made) {
        memory::detail::Scope general(memory::detail::Manager::get().general());
        std::lock_guard<std::mutex> lock(mutex_);
        const auto known =
            std::find_if(roots_.begin(), roots_.end(),
                         [&](const auto& held) { return held.first == root; });
        if (known != roots_.end()) {
            roots_.splice(roots_.begin(), roots_, known);
            return roots_.front().second;
        }
        roots_.emplace_front(root, made);
        if (roots_.size() > INDEX_ROOTS_RETAINED) roots_.pop_back();
        return made;
    }

    void clear() {
        Held dropped;
        std::lock_guard<std::mutex> lock(mutex_);
        dropped.swap(roots_);
    }

    void forget(const dstd::String& root) {
        Held dropped;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto known =
            std::find_if(roots_.begin(), roots_.end(),
                         [&](const auto& held) { return held.first == root; });
        if (known != roots_.end())
            dropped.splice(dropped.begin(), roots_, known);
    }

   private:
    using Held = dstd::List<dstd::Pair<dstd::String, Index>>;
    std::mutex mutex_;
    Held roots_;
};

RetainedRoots& retained_roots() {
    RetainedRoots& roots = dstd::Singleton<RetainedRoots>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dftracer::utils::store::Store::register_shutdown_close();
        dstd::add_shutdown_hook(
            dstd::ShutdownStage::CloseStores,
            []() noexcept { dstd::Singleton<RetainedRoots>::get().clear(); });
        return true;
    }();
    return roots;
}

}  // namespace

void release_root(dstd::StringView root) {
    retained_roots().forget(dstd::String(root));
}

void release_all_roots() {
    cache::clear_cache_roots();
    retained_roots().clear();
    if (auto closed = dftracer::utils::store::Store::close_all(); !closed) {
        DFTRACER_UTILS_LOG_ERROR("index: close at exit: %s",
                                 closed.error().format().c_str());
    }
}

dstd::Expected<Index, dstd::Error> Index::open(dstd::StringView root) {
    if (root.empty()) return invalid("index: the root is empty");
    auto impl = dstd::make_shared<Impl>();
    impl->root = dstd::String(root);
    impl->store_dir = index::store_dir(impl->root);
    const dstd::String key = impl->root;
    return retained_roots().find_or_add(key, Index(std::move(impl)));
}

dstd::Expected<Snapshot, dstd::Error> Index::snapshot() const {
    auto found = impl_->find_store();
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return IndexAccess::make_snapshot(dstd::nullopt);
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
                                   dftracer::utils::store::ScopeKey::file(path),
                                   stat);
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
    dftracer::utils::store::Store store = **found;
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    const dftracer::utils::store::ScopeKey scope =
        dftracer::utils::store::ScopeKey::file(
            dstd::StringView(path.data(), path.size()));
    auto namespaces = held_copies(store, *taken, scope);
    if (!namespaces)
        co_return dstd::Unexpected<dstd::Error>(namespaces.error());
    dstd::Vector<dftracer::utils::store::Publish> targets;
    for (const dftracer::utils::store::Namespace& ns : *namespaces) {
        targets.push_back(dftracer::utils::store::Publish{
            &ns, &scope, 0, nullptr,
            dftracer::utils::store::Publish::Change::Dropped});
    }
    if (targets.empty()) co_return dstd::Expected<void, dstd::Error>{};
    auto applied = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(targets.data(),
                                                          targets.size()));
    if (!applied) co_return dstd::Unexpected<dstd::Error>(applied.error());
    co_return dstd::Expected<void, dstd::Error>{};
}

}  // namespace dftracer::utils::index
