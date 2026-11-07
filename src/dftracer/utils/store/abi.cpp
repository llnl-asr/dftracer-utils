// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/abi.h>
#include <dftracer/utils/store/store.h>

#include <cstdint>
#include <exception>
#include <utility>

using namespace dftracer::utils;

using dftracer::utils::dstd::Condition;
using dftracer::utils::dstd::Error;
using dftracer::utils::store::Batch;
using dftracer::utils::store::CopyInfo;
using dftracer::utils::store::Cursor;
using dftracer::utils::store::FoldKind;
using dftracer::utils::store::Namespace;
using dftracer::utils::store::NamespaceSpec;
using dftracer::utils::store::PackOptions;
using dftracer::utils::store::Publish;
using dftracer::utils::store::ScopeKey;
using dftracer::utils::store::ScopeKind;
using dftracer::utils::store::ScopeState;
using dftracer::utils::store::Snapshot;
using dftracer::utils::store::SourceStat;
using dftracer::utils::store::Store;
using dftracer::utils::store::Value;

static_assert(static_cast<int>(Condition::Unknown) == DFTU_COND_UNKNOWN);
static_assert(static_cast<int>(Condition::Internal) == DFTU_COND_INTERNAL);
static_assert(static_cast<int>(Condition::InvalidArgument) ==
              DFTU_COND_INVALID_ARGUMENT);
static_assert(static_cast<int>(Condition::NotFound) == DFTU_COND_NOT_FOUND);
static_assert(static_cast<int>(Condition::Io) == DFTU_COND_IO);
static_assert(static_cast<int>(Condition::Parse) == DFTU_COND_PARSE);
static_assert(static_cast<int>(Condition::Compression) ==
              DFTU_COND_COMPRESSION);
static_assert(static_cast<int>(Condition::Timeout) == DFTU_COND_TIMEOUT);
static_assert(static_cast<int>(Condition::Unsupported) ==
              DFTU_COND_UNSUPPORTED);
static_assert(static_cast<int>(Condition::Cancelled) == DFTU_COND_CANCELLED);
static_assert(static_cast<int>(Condition::ForkedHandle) ==
              DFTU_COND_FORKED_HANDLE);

static_assert(static_cast<int>(FoldKind::None) == DFTU_STORE_FOLD_NONE);
static_assert(static_cast<int>(FoldKind::SumI64) == DFTU_STORE_FOLD_SUM_I64);
static_assert(static_cast<int>(FoldKind::SumF64) == DFTU_STORE_FOLD_SUM_F64);
static_assert(static_cast<int>(FoldKind::MinI64) == DFTU_STORE_FOLD_MIN_I64);
static_assert(static_cast<int>(FoldKind::MaxI64) == DFTU_STORE_FOLD_MAX_I64);
static_assert(static_cast<int>(FoldKind::MinF64) == DFTU_STORE_FOLD_MIN_F64);
static_assert(static_cast<int>(FoldKind::MaxF64) == DFTU_STORE_FOLD_MAX_F64);
static_assert(static_cast<int>(FoldKind::Count) == DFTU_STORE_FOLD_COUNT);
static_assert(static_cast<int>(FoldKind::Last) == DFTU_STORE_FOLD_LAST);
static_assert(static_cast<int>(FoldKind::Concat) == DFTU_STORE_FOLD_CONCAT);

static_assert(static_cast<int>(ScopeKind::Root) == DFTU_STORE_SCOPE_ROOT);
static_assert(static_cast<int>(ScopeKind::File) == DFTU_STORE_SCOPE_FILE);
static_assert(static_cast<int>(ScopeKind::Files) == DFTU_STORE_SCOPE_FILES);

static_assert(static_cast<int>(ScopeState::Absent) == DFTU_STORE_STATE_ABSENT);
static_assert(static_cast<int>(ScopeState::Ready) == DFTU_STORE_STATE_READY);
static_assert(static_cast<int>(ScopeState::Failed) == DFTU_STORE_STATE_FAILED);
static_assert(static_cast<int>(ScopeState::Bad) == DFTU_STORE_STATE_BAD);

static_assert(static_cast<int>(Publish::Change::Ready) ==
              DFTU_STORE_PUBLISH_READY);
static_assert(static_cast<int>(Publish::Change::Failed) ==
              DFTU_STORE_PUBLISH_FAILED);
static_assert(static_cast<int>(Publish::Change::Dropped) ==
              DFTU_STORE_PUBLISH_DROPPED);

struct dftu_store_s : dstd::HeapObject {
    explicit dftu_store_s(Store s) : store(std::move(s)) {}
    Store store;
};

struct dftu_store_ns_s : dstd::HeapObject {
    explicit dftu_store_ns_s(Namespace n) : ns(std::move(n)) {}
    Namespace ns;
};

struct dftu_store_batch_s : dstd::HeapObject {
    dftu_store_batch_s(Store s, Batch b)
        : store(std::move(s)), batch(std::move(b)) {}
    Store store;
    Batch batch;
};

struct dftu_store_snapshot_s : dstd::HeapObject {
    explicit dftu_store_snapshot_s(Snapshot s) : snapshot(std::move(s)) {}
    Snapshot snapshot;
};

struct dftu_store_cursor_s : dstd::HeapObject {
    explicit dftu_store_cursor_s(Cursor c) : cursor(std::move(c)) {}
    Cursor cursor;
    dstd::Optional<Error> failure;
    bool started = false;
};

struct dftu_store_value_s : dstd::HeapObject {
    explicit dftu_store_value_s(Value v) : value(std::move(v)) {}
    Value value;
};

struct dftu_store_value_list_s : dstd::HeapObject {
    explicit dftu_store_value_list_s(dstd::Vector<dstd::Optional<Value>> v)
        : values(std::move(v)) {}
    dstd::Vector<dstd::Optional<Value>> values;
};

namespace {

thread_local Error t_error;

dftu_error to_c(const Error& error) {
    return dftu_error{error.domain().id, error.code(),
                      static_cast<std::int32_t>(error.condition()),
                      error.c_str()};
}

dftu_error report(Error error) {
    t_error = std::move(error);
    return to_c(t_error);
}

Error current_error() {
    try {
        throw;
    } catch (const common::DFTUtilsException& e) {
        return e.error();
    } catch (const std::exception& e) {
        return Error(Condition::Internal, e.what());
    } catch (...) {
        return Error(Condition::Internal, "unknown error");
    }
}

dftu_error report_current() { return report(current_error()); }

template <class R>
R failed(Error error) {
    R r{};
    r.u.err = report(std::move(error));
    return r;
}

template <class R>
R failed_current() {
    R r{};
    r.u.err = report_current();
    return r;
}

template <class R>
R invalid(const char* message) {
    return failed<R>(Error(Condition::InvalidArgument, message));
}

dftu_store_void_result done() {
    dftu_store_void_result r{};
    r.ok = 1;
    return r;
}

template <class R, class V>
R succeeded(V value) {
    R r{};
    r.ok = 1;
    r.u.value = value;
    return r;
}

template <class T>
T block(coro::CoroTask<T> task) {
    return default_runtime().block_on(std::move(task));
}

bool bytes_ok(const std::uint8_t* data, std::uint64_t size) {
    return data != nullptr || size == 0;
}

dstd::ByteView view_of(const std::uint8_t* data, std::uint64_t size) {
    return dstd::ByteView(data, static_cast<std::size_t>(size));
}

dftu_store_bytes bytes_of(dstd::ByteView view) {
    return dftu_store_bytes{reinterpret_cast<const std::uint8_t*>(view.data()),
                            view.size()};
}

dstd::Expected<ScopeKey, Error> scope_of(const dftu_store_scope* scope) {
    if (!scope) {
        return dstd::Unexpected<Error>(
            Error(Condition::InvalidArgument, "scope is NULL"));
    }
    if (scope->path_count > 0 && !scope->paths) {
        return dstd::Unexpected<Error>(
            Error(Condition::InvalidArgument, "scope paths is NULL"));
    }
    dstd::Vector<dstd::StringView> paths;
    paths.reserve(static_cast<std::size_t>(scope->path_count));
    for (std::uint64_t i = 0; i < scope->path_count; ++i) {
        if (!scope->paths[i]) {
            return dstd::Unexpected<Error>(
                Error(Condition::InvalidArgument, "a scope path is NULL"));
        }
        paths.emplace_back(scope->paths[i]);
    }
    switch (scope->kind) {
        case DFTU_STORE_SCOPE_ROOT:
            if (!paths.empty()) break;
            return ScopeKey::root();
        case DFTU_STORE_SCOPE_FILE:
            if (paths.size() != 1) break;
            return ScopeKey::file(paths[0]);
        case DFTU_STORE_SCOPE_FILES:
            if (paths.empty()) break;
            return ScopeKey::files(
                dstd::Span<const dstd::StringView>(paths.data(), paths.size()));
        default:
            return dstd::Unexpected<Error>(
                Error(Condition::InvalidArgument, "unknown scope kind"));
    }
    return dstd::Unexpected<Error>(
        Error(Condition::InvalidArgument,
              "the path count does not fit the scope kind"));
}

dstd::Expected<dstd::Vector<SourceStat>, Error> sources_of(
    const dftu_store_source_stat* sources, std::uint64_t count) {
    if (count > 0 && !sources) {
        return dstd::Unexpected<Error>(
            Error(Condition::InvalidArgument, "sources is NULL with a count"));
    }
    dstd::Vector<SourceStat> out;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        out.push_back(
            SourceStat{sources[i].mtime, sources[i].size, sources[i].hash});
    }
    return out;
}

dftu_store_batch_result make_batch(dftu_store_t* store,
                                   const dftu_store_ns_t* ns,
                                   const dftu_store_scope* scope,
                                   std::uint64_t params_hash,
                                   const dftu_store_source_stat* sources,
                                   std::uint64_t source_count, bool building) {
    if (!store || !ns) {
        return invalid<dftu_store_batch_result>("store or ns is NULL");
    }
    try {
        auto key = scope_of(scope);
        if (!key) return failed<dftu_store_batch_result>(key.error());
        auto stats = sources_of(sources, source_count);
        if (!stats) return failed<dftu_store_batch_result>(stats.error());
        Batch batch =
            building ? store->store.building_batch(ns->ns, *key)
                     : store->store.batch(ns->ns, *key, params_hash,
                                          source_count > 0 ? &*stats : nullptr);
        return succeeded<dftu_store_batch_result>(
            new dftu_store_batch_s(store->store, std::move(batch)));
    } catch (...) {
        return failed_current<dftu_store_batch_result>();
    }
}

dftu_store_void_result write_entry(dftu_store_batch_t* batch,
                                   const std::uint8_t* key,
                                   std::uint64_t key_size,
                                   const std::uint8_t* value,
                                   std::uint64_t value_size, bool fold) {
    if (!batch || !bytes_ok(key, key_size) || !bytes_ok(value, value_size)) {
        return invalid<dftu_store_void_result>(
            "batch is NULL, or a pointer is NULL with a size");
    }
    try {
        if (fold) {
            batch->batch.fold(view_of(key, key_size),
                              view_of(value, value_size));
        } else {
            batch->batch.put(view_of(key, key_size),
                             view_of(value, value_size));
        }
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_cursor_result make_cursor(Cursor cursor) {
    return succeeded<dftu_store_cursor_result>(
        new dftu_store_cursor_s(std::move(cursor)));
}

}  // namespace

extern "C" {

dftu_store_open_result dftu_store_open(const char* dir) {
    if (!dir) return invalid<dftu_store_open_result>("dir is NULL");
    try {
        auto opened = Store::open(dir);
        if (!opened) return failed<dftu_store_open_result>(opened.error());
        return succeeded<dftu_store_open_result>(
            new dftu_store_s(std::move(*opened)));
    } catch (...) {
        return failed_current<dftu_store_open_result>();
    }
}

void dftu_store_free(dftu_store_t* store) { delete store; }

dftu_store_ns_result dftu_store_ns(dftu_store_t* store, const char* name,
                                   uint32_t format_version, int32_t fold,
                                   int32_t durable, int32_t shared) {
    if (!store || !name || !*name) {
        return invalid<dftu_store_ns_result>("store or name is NULL or empty");
    }
    if (fold < DFTU_STORE_FOLD_NONE || fold > DFTU_STORE_FOLD_CONCAT) {
        return invalid<dftu_store_ns_result>("unknown fold kind");
    }
    try {
        NamespaceSpec spec;
        spec.name = name;
        spec.format_version = format_version;
        spec.fold = static_cast<FoldKind>(fold);
        spec.durable = durable != 0;
        spec.shared = shared != 0;
        auto added = store->store.ns(spec);
        if (!added) return failed<dftu_store_ns_result>(added.error());
        return succeeded<dftu_store_ns_result>(
            new dftu_store_ns_s(std::move(*added)));
    } catch (...) {
        return failed_current<dftu_store_ns_result>();
    }
}

void dftu_store_ns_free(dftu_store_ns_t* ns) { delete ns; }

dftu_store_batch_result dftu_store_batch(dftu_store_t* store,
                                         const dftu_store_ns_t* ns,
                                         const dftu_store_scope* scope,
                                         uint64_t params_hash,
                                         const dftu_store_source_stat* sources,
                                         uint64_t source_count) {
    return make_batch(store, ns, scope, params_hash, sources, source_count,
                      false);
}

dftu_store_batch_result dftu_store_building_batch(
    dftu_store_t* store, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope) {
    return make_batch(store, ns, scope, 0, nullptr, 0, true);
}

dftu_store_void_result dftu_store_batch_put(dftu_store_batch_t* batch,
                                            const uint8_t* key,
                                            uint64_t key_size,
                                            const uint8_t* value,
                                            uint64_t value_size) {
    return write_entry(batch, key, key_size, value, value_size, false);
}

dftu_store_void_result dftu_store_batch_fold(dftu_store_batch_t* batch,
                                             const uint8_t* key,
                                             uint64_t key_size,
                                             const uint8_t* value,
                                             uint64_t value_size) {
    return write_entry(batch, key, key_size, value, value_size, true);
}

dftu_store_void_result dftu_store_batch_erase(dftu_store_batch_t* batch,
                                              const uint8_t* key,
                                              uint64_t key_size) {
    if (!batch || !bytes_ok(key, key_size)) {
        return invalid<dftu_store_void_result>(
            "batch is NULL, or key is NULL with a size");
    }
    try {
        batch->batch.erase(view_of(key, key_size));
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_batch_erase_range(dftu_store_batch_t* batch,
                                                    const uint8_t* begin,
                                                    uint64_t begin_size,
                                                    const uint8_t* end,
                                                    uint64_t end_size) {
    if (!batch || !bytes_ok(begin, begin_size) || !bytes_ok(end, end_size)) {
        return invalid<dftu_store_void_result>(
            "batch is NULL, or a bound is NULL with a size");
    }
    try {
        batch->batch.erase_range(view_of(begin, begin_size),
                                 view_of(end, end_size));
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_batch_commit(dftu_store_batch_t* batch) {
    if (!batch) return invalid<dftu_store_void_result>("batch is NULL");
    try {
        Store store = batch->store;
        Batch owned = std::move(batch->batch);
        delete batch;
        auto committed = block(store.commit(std::move(owned)));
        if (!committed)
            return failed<dftu_store_void_result>(committed.error());
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

void dftu_store_batch_free(dftu_store_batch_t* batch) { delete batch; }

dftu_store_void_result dftu_store_publish(
    dftu_store_t* store, const dftu_store_publish_target* targets, uint64_t n) {
    if (!store || (n > 0 && !targets)) {
        return invalid<dftu_store_void_result>(
            "store is NULL, or targets is NULL with a count");
    }
    try {
        const auto count = static_cast<std::size_t>(n);
        dstd::Vector<ScopeKey> scopes;
        dstd::Vector<dstd::Vector<SourceStat>> stats;
        dstd::Vector<Publish> changes;
        scopes.reserve(count);
        stats.reserve(count);
        changes.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const dftu_store_publish_target& target = targets[i];
            if (!target.ns) {
                return invalid<dftu_store_void_result>("a target ns is NULL");
            }
            if (target.change < DFTU_STORE_PUBLISH_READY ||
                target.change > DFTU_STORE_PUBLISH_DROPPED) {
                return invalid<dftu_store_void_result>(
                    "unknown publish change");
            }
            auto key = scope_of(target.scope);
            if (!key) return failed<dftu_store_void_result>(key.error());
            auto sources = sources_of(target.sources, target.source_count);
            if (!sources) {
                return failed<dftu_store_void_result>(sources.error());
            }
            scopes.push_back(std::move(*key));
            stats.push_back(std::move(*sources));
            Publish change;
            change.ns = &target.ns->ns;
            change.scope = &scopes.back();
            change.params_hash = target.params_hash;
            change.sources = stats.back().empty() ? nullptr : &stats.back();
            change.change = static_cast<Publish::Change>(target.change);
            change.replaces_older = target.replaces_older != 0;
            changes.push_back(change);
        }
        auto published = block(store->store.publish(
            dstd::Span<const Publish>(changes.data(), changes.size())));
        if (!published) {
            return failed<dftu_store_void_result>(published.error());
        }
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_fail(dftu_store_t* store,
                                       const dftu_store_ns_t* ns,
                                       const dftu_store_scope* scope,
                                       uint64_t params_hash) {
    if (!store || !ns) {
        return invalid<dftu_store_void_result>("store or ns is NULL");
    }
    try {
        auto key = scope_of(scope);
        if (!key) return failed<dftu_store_void_result>(key.error());
        auto marked = store->store.fail(ns->ns, *key, params_hash);
        if (!marked) return failed<dftu_store_void_result>(marked.error());
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_drop(dftu_store_t* store,
                                       const dftu_store_ns_t* ns,
                                       const dftu_store_scope* scope) {
    if (!store || !ns) {
        return invalid<dftu_store_void_result>("store or ns is NULL");
    }
    try {
        auto key = scope_of(scope);
        if (!key) return failed<dftu_store_void_result>(key.error());
        auto dropped = store->store.drop(ns->ns, *key);
        if (!dropped) return failed<dftu_store_void_result>(dropped.error());
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_flush(dftu_store_t* store) {
    if (!store) return invalid<dftu_store_void_result>("store is NULL");
    try {
        auto flushed = block(store->store.flush());
        if (!flushed) return failed<dftu_store_void_result>(flushed.error());
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_refresh(dftu_store_t* store) {
    if (!store) return invalid<dftu_store_void_result>("store is NULL");
    try {
        auto refreshed = store->store.refresh();
        if (!refreshed) {
            return failed<dftu_store_void_result>(refreshed.error());
        }
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

dftu_store_void_result dftu_store_close(dftu_store_t* store) {
    if (!store) return invalid<dftu_store_void_result>("store is NULL");
    try {
        auto closed = block(store->store.close());
        if (!closed) return failed<dftu_store_void_result>(closed.error());
        return done();
    } catch (...) {
        return failed_current<dftu_store_void_result>();
    }
}

void dftu_store_pack_options_init(dftu_store_pack_options* out) {
    if (!out) return;
    const PackOptions defaults;
    *out = dftu_store_pack_options{defaults.small_file_bytes,
                                   defaults.dead_ratio, defaults.keep_seconds};
}

dftu_store_pack_result dftu_store_pack(dftu_store_t* store,
                                       const dftu_store_pack_options* options) {
    if (!store) return invalid<dftu_store_pack_result>("store is NULL");
    try {
        PackOptions chosen;
        if (options) {
            chosen.small_file_bytes = options->small_file_bytes;
            chosen.dead_ratio = options->dead_ratio;
            chosen.keep_seconds = options->keep_seconds;
        }
        auto packed = block(store->store.pack(chosen));
        if (!packed) return failed<dftu_store_pack_result>(packed.error());
        return succeeded<dftu_store_pack_result>(dftu_store_pack_report{
            packed->files_packed, packed->copies_written,
            packed->copies_skipped, packed->files_deleted,
            packed->bytes_deleted});
    } catch (...) {
        return failed_current<dftu_store_pack_result>();
    }
}

dftu_store_snapshot_result dftu_store_snapshot(dftu_store_t* store) {
    if (!store) return invalid<dftu_store_snapshot_result>("store is NULL");
    try {
        auto view = store->store.snapshot();
        if (!view) return failed<dftu_store_snapshot_result>(view.error());
        return succeeded<dftu_store_snapshot_result>(
            new dftu_store_snapshot_s(std::move(*view)));
    } catch (...) {
        return failed_current<dftu_store_snapshot_result>();
    }
}

void dftu_store_snapshot_free(dftu_store_snapshot_t* snapshot) {
    delete snapshot;
}

dftu_store_copy_result dftu_store_snapshot_copy_info(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, dftu_store_source_stat* sources,
    uint64_t capacity) {
    if (!snapshot || !ns || (capacity > 0 && !sources)) {
        return invalid<dftu_store_copy_result>(
            "snapshot or ns is NULL, or sources is NULL with a capacity");
    }
    try {
        auto key = scope_of(scope);
        if (!key) return failed<dftu_store_copy_result>(key.error());
        const CopyInfo info = snapshot->snapshot.copy_info(ns->ns, *key);
        if (sources) {
            const std::size_t copied = info.sources.size() < capacity
                                           ? info.sources.size()
                                           : static_cast<std::size_t>(capacity);
            for (std::size_t i = 0; i < copied; ++i) {
                sources[i] = dftu_store_source_stat{info.sources[i].mtime,
                                                    info.sources[i].size,
                                                    info.sources[i].hash};
            }
        }
        return succeeded<dftu_store_copy_result>(
            dftu_store_copy{static_cast<int32_t>(info.state), info.generation,
                            info.params_hash, info.sources.size()});
    } catch (...) {
        return failed_current<dftu_store_copy_result>();
    }
}

dftu_store_value_result dftu_store_snapshot_get(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const uint8_t* key, uint64_t key_size) {
    if (!snapshot || !ns || !bytes_ok(key, key_size)) {
        return invalid<dftu_store_value_result>(
            "snapshot or ns is NULL, or key is NULL with a size");
    }
    try {
        auto where = scope_of(scope);
        if (!where) return failed<dftu_store_value_result>(where.error());
        auto found =
            snapshot->snapshot.get(ns->ns, *where, view_of(key, key_size));
        if (!found) return failed<dftu_store_value_result>(found.error());
        if (!found->has_value()) {
            return succeeded<dftu_store_value_result>(
                static_cast<dftu_store_value_t*>(nullptr));
        }
        return succeeded<dftu_store_value_result>(
            new dftu_store_value_s(std::move(**found)));
    } catch (...) {
        return failed_current<dftu_store_value_result>();
    }
}

dftu_store_value_list_result dftu_store_snapshot_multi_get(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const dftu_store_bytes* keys, uint64_t n) {
    if (!snapshot || !ns || (n > 0 && !keys)) {
        return invalid<dftu_store_value_list_result>(
            "snapshot or ns is NULL, or keys is NULL with a count");
    }
    try {
        auto where = scope_of(scope);
        if (!where) return failed<dftu_store_value_list_result>(where.error());
        dstd::Vector<dstd::ByteView> views;
        views.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) {
            if (!bytes_ok(keys[i].data, keys[i].size)) {
                return invalid<dftu_store_value_list_result>(
                    "a key is NULL with a size");
            }
            views.push_back(view_of(keys[i].data, keys[i].size));
        }
        auto found = snapshot->snapshot.multi_get(
            ns->ns, *where,
            dstd::Span<const dstd::ByteView>(views.data(), views.size()));
        if (!found) {
            return failed<dftu_store_value_list_result>(found.error());
        }
        return succeeded<dftu_store_value_list_result>(
            new dftu_store_value_list_s(std::move(*found)));
    } catch (...) {
        return failed_current<dftu_store_value_list_result>();
    }
}

dftu_store_bytes dftu_store_value_bytes(const dftu_store_value_t* v) {
    if (!v) return dftu_store_bytes{nullptr, 0};
    return bytes_of(v->value.bytes());
}

void dftu_store_value_free(dftu_store_value_t* v) { delete v; }

uint64_t dftu_store_value_list_count(const dftu_store_value_list_t* list) {
    return list ? list->values.size() : 0;
}

int32_t dftu_store_value_list_at(const dftu_store_value_list_t* list,
                                 uint64_t i, dftu_store_bytes* out) {
    if (!list || !out || i >= list->values.size()) return -1;
    const auto& entry = list->values[static_cast<std::size_t>(i)];
    if (!entry.has_value()) return 0;
    *out = bytes_of(entry->bytes());
    return 1;
}

void dftu_store_value_list_free(dftu_store_value_list_t* list) { delete list; }

dftu_store_cursor_result dftu_store_snapshot_scan_prefix(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const uint8_t* prefix,
    uint64_t prefix_size) {
    if (!snapshot || !ns || !bytes_ok(prefix, prefix_size)) {
        return invalid<dftu_store_cursor_result>(
            "snapshot or ns is NULL, or prefix is NULL with a size");
    }
    try {
        auto where = scope_of(scope);
        if (!where) return failed<dftu_store_cursor_result>(where.error());
        return make_cursor(snapshot->snapshot.scan(
            ns->ns, *where, view_of(prefix, prefix_size)));
    } catch (...) {
        return failed_current<dftu_store_cursor_result>();
    }
}

dftu_store_cursor_result dftu_store_snapshot_scan_range(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const uint8_t* begin, uint64_t begin_size,
    const uint8_t* end, uint64_t end_size) {
    if (!snapshot || !ns || !bytes_ok(begin, begin_size) ||
        !bytes_ok(end, end_size)) {
        return invalid<dftu_store_cursor_result>(
            "snapshot or ns is NULL, or a bound is NULL with a size");
    }
    try {
        auto where = scope_of(scope);
        if (!where) return failed<dftu_store_cursor_result>(where.error());
        return make_cursor(snapshot->snapshot.scan(ns->ns, *where,
                                                   view_of(begin, begin_size),
                                                   view_of(end, end_size)));
    } catch (...) {
        return failed_current<dftu_store_cursor_result>();
    }
}

int32_t dftu_store_cursor_next(dftu_store_cursor_t* cursor) {
    if (!cursor) return -1;
    if (cursor->failure) return 0;
    try {
        if (cursor->started) {
            if (!cursor->cursor.at_end()) cursor->cursor.next();
        } else {
            cursor->started = true;
        }
        return cursor->cursor.at_end() ? 0 : 1;
    } catch (...) {
        cursor->failure = current_error();
        return 0;
    }
}

dftu_store_bytes dftu_store_cursor_key(const dftu_store_cursor_t* cursor) {
    if (!cursor || !cursor->started || cursor->failure ||
        cursor->cursor.at_end()) {
        return dftu_store_bytes{nullptr, 0};
    }
    return bytes_of(cursor->cursor.key());
}

dftu_store_bytes_result dftu_store_cursor_value(dftu_store_cursor_t* cursor) {
    if (!cursor || !cursor->started || cursor->failure ||
        cursor->cursor.at_end()) {
        return invalid<dftu_store_bytes_result>(
            "cursor is NULL, not moved or at its end");
    }
    try {
        auto value = cursor->cursor.value();
        if (!value) return failed<dftu_store_bytes_result>(value.error());
        return succeeded<dftu_store_bytes_result>(bytes_of(*value));
    } catch (...) {
        return failed_current<dftu_store_bytes_result>();
    }
}

int32_t dftu_store_cursor_error(const dftu_store_cursor_t* cursor,
                                dftu_error* out) {
    if (!cursor) return 0;
    if (cursor->failure) {
        if (out) *out = to_c(*cursor->failure);
        return 1;
    }
    if (!cursor->cursor.error().has_value()) return 0;
    if (out) *out = to_c(*cursor->cursor.error());
    return 1;
}

void dftu_store_cursor_free(dftu_store_cursor_t* cursor) { delete cursor; }

}  // extern "C"
