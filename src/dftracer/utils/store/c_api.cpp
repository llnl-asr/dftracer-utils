// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/common/c_api_error.h>
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
#include <dftracer/utils/store/c_api.h>
#include <dftracer/utils/store/store.h>

#include <cstddef>
#include <cstdint>
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

dftu_status invalid_argument(dftu_error* err, const char* message) {
    return common::fill_error(err, Error(Condition::InvalidArgument, message));
}

template <class T>
bool clear_out(T** out) {
    if (!out) return false;
    *out = nullptr;
    return true;
}

template <class T>
coro::TaskResult<T> block(coro::CoroTask<T> task) {
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
    if (scope->path_count > paths.max_size()) {
        return dstd::Unexpected<Error>(
            Error(Condition::InvalidArgument, "scope path count is too large"));
    }
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
    if (count > out.max_size()) {
        return dstd::Unexpected<Error>(
            Error(Condition::InvalidArgument, "source count is too large"));
    }
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        out.push_back(
            SourceStat{sources[i].mtime, sources[i].size, sources[i].hash});
    }
    return out;
}

dftu_status make_batch(dftu_store_t* store, const dftu_store_ns_t* ns,
                       const dftu_store_scope* scope, std::uint64_t params_hash,
                       const dftu_store_source_stat* sources,
                       std::uint64_t source_count, bool building,
                       dftu_store_batch_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!store || !ns) return invalid_argument(err, "store or ns is NULL");
    auto key = scope_of(scope);
    if (!key) return common::fill_error(err, key.error());
    auto stats = sources_of(sources, source_count);
    if (!stats) return common::fill_error(err, stats.error());
    Batch batch =
        building ? store->store.building_batch(ns->ns, *key)
                 : store->store.batch(ns->ns, *key, params_hash,
                                      source_count > 0 ? &*stats : nullptr);
    *out = new dftu_store_batch_s(store->store, std::move(batch));
    return DFTU_OK;
}

dftu_status write_entry(dftu_store_batch_t* batch, const std::uint8_t* key,
                        std::uint64_t key_size, const std::uint8_t* value,
                        std::uint64_t value_size, bool fold, dftu_error* err) {
    if (!batch || !bytes_ok(key, key_size) || !bytes_ok(value, value_size)) {
        return invalid_argument(
            err, "batch is NULL, or a pointer is NULL with a size");
    }
    if (fold) {
        batch->batch.fold(view_of(key, key_size), view_of(value, value_size));
    } else {
        batch->batch.put(view_of(key, key_size), view_of(value, value_size));
    }
    return DFTU_OK;
}

dftu_status make_cursor(Cursor cursor, dftu_store_cursor_t** out) {
    *out = new dftu_store_cursor_s(std::move(cursor));
    return DFTU_OK;
}

}  // namespace

extern "C" {

dftu_status dftu_store_open(const char* dir, dftu_store_t** out,
                            dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!dir) return invalid_argument(err, "dir is NULL");
    auto opened = Store::open(dir);
    if (!opened) return common::fill_error(err, opened.error());
    *out = new dftu_store_s(std::move(*opened));
    return DFTU_OK;
}

void dftu_store_free(dftu_store_t* store) { delete store; }

dftu_status dftu_store_ns(dftu_store_t* store, const char* name,
                          std::uint32_t format_version, std::int32_t fold,
                          std::int32_t durable, std::int32_t shared,
                          dftu_store_ns_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!store || !name || !*name) {
        return invalid_argument(err, "store or name is NULL or empty");
    }
    if (fold < DFTU_STORE_FOLD_NONE || fold > DFTU_STORE_FOLD_CONCAT) {
        return invalid_argument(err, "unknown fold kind");
    }
    NamespaceSpec spec;
    spec.name = name;
    spec.format_version = format_version;
    spec.fold = static_cast<FoldKind>(fold);
    spec.durable = durable != 0;
    spec.shared = shared != 0;
    auto added = store->store.ns(spec);
    if (!added) return common::fill_error(err, added.error());
    *out = new dftu_store_ns_s(std::move(*added));
    return DFTU_OK;
}

void dftu_store_ns_free(dftu_store_ns_t* ns) { delete ns; }

dftu_status dftu_store_batch(dftu_store_t* store, const dftu_store_ns_t* ns,
                             const dftu_store_scope* scope,
                             std::uint64_t params_hash,
                             const dftu_store_source_stat* sources,
                             std::uint64_t source_count,
                             dftu_store_batch_t** out, dftu_error* err) {
    return make_batch(store, ns, scope, params_hash, sources, source_count,
                      false, out, err);
}

dftu_status dftu_store_building_batch(dftu_store_t* store,
                                      const dftu_store_ns_t* ns,
                                      const dftu_store_scope* scope,
                                      dftu_store_batch_t** out,
                                      dftu_error* err) {
    return make_batch(store, ns, scope, 0, nullptr, 0, true, out, err);
}

dftu_status dftu_store_batch_put(dftu_store_batch_t* batch,
                                 const std::uint8_t* key,
                                 std::uint64_t key_size,
                                 const std::uint8_t* value,
                                 std::uint64_t value_size, dftu_error* err) {
    return write_entry(batch, key, key_size, value, value_size, false, err);
}

dftu_status dftu_store_batch_fold(dftu_store_batch_t* batch,
                                  const std::uint8_t* key,
                                  std::uint64_t key_size,
                                  const std::uint8_t* value,
                                  std::uint64_t value_size, dftu_error* err) {
    return write_entry(batch, key, key_size, value, value_size, true, err);
}

dftu_status dftu_store_batch_erase(dftu_store_batch_t* batch,
                                   const std::uint8_t* key,
                                   std::uint64_t key_size, dftu_error* err) {
    if (!batch || !bytes_ok(key, key_size)) {
        return invalid_argument(err,
                                "batch is NULL, or key is NULL with a size");
    }
    batch->batch.erase(view_of(key, key_size));
    return DFTU_OK;
}

dftu_status dftu_store_batch_erase_range(dftu_store_batch_t* batch,
                                         const std::uint8_t* begin,
                                         std::uint64_t begin_size,
                                         const std::uint8_t* end,
                                         std::uint64_t end_size,
                                         dftu_error* err) {
    if (!batch || !bytes_ok(begin, begin_size) || !bytes_ok(end, end_size)) {
        return invalid_argument(
            err, "batch is NULL, or a bound is NULL with a size");
    }
    batch->batch.erase_range(view_of(begin, begin_size),
                             view_of(end, end_size));
    return DFTU_OK;
}

dftu_status dftu_store_batch_commit(dftu_store_batch_t* batch,
                                    dftu_error* err) {
    if (!batch) return invalid_argument(err, "batch is NULL");
    Store store = batch->store;
    Batch owned = std::move(batch->batch);
    delete batch;
    auto committed = block(store.commit(std::move(owned)));
    if (!committed) return common::fill_error(err, committed.error());
    return DFTU_OK;
}

void dftu_store_batch_free(dftu_store_batch_t* batch) { delete batch; }

dftu_status dftu_store_publish(dftu_store_t* store,
                               const dftu_store_publish_target* targets,
                               std::uint64_t n, dftu_error* err) {
    if (!store || (n > 0 && !targets)) {
        return invalid_argument(
            err, "store is NULL, or targets is NULL with a count");
    }
    const auto count = static_cast<std::size_t>(n);
    dstd::Vector<ScopeKey> scopes;
    dstd::Vector<dstd::Vector<SourceStat>> stats;
    dstd::Vector<Publish> changes;
    if (n > scopes.max_size() || n > stats.max_size() ||
        n > changes.max_size()) {
        return invalid_argument(err, "target count is too large");
    }
    scopes.reserve(count);
    stats.reserve(count);
    changes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const dftu_store_publish_target& target = targets[i];
        if (!target.ns) return invalid_argument(err, "a target ns is NULL");
        if (target.change < DFTU_STORE_PUBLISH_READY ||
            target.change > DFTU_STORE_PUBLISH_DROPPED) {
            return invalid_argument(err, "unknown publish change");
        }
        auto key = scope_of(target.scope);
        if (!key) return common::fill_error(err, key.error());
        auto sources = sources_of(target.sources, target.source_count);
        if (!sources) return common::fill_error(err, sources.error());
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
    if (!published) return common::fill_error(err, published.error());
    return DFTU_OK;
}

dftu_status dftu_store_fail(dftu_store_t* store, const dftu_store_ns_t* ns,
                            const dftu_store_scope* scope,
                            std::uint64_t params_hash, dftu_error* err) {
    if (!store || !ns) return invalid_argument(err, "store or ns is NULL");
    auto key = scope_of(scope);
    if (!key) return common::fill_error(err, key.error());
    auto marked = store->store.fail(ns->ns, *key, params_hash);
    if (!marked) return common::fill_error(err, marked.error());
    return DFTU_OK;
}

dftu_status dftu_store_drop(dftu_store_t* store, const dftu_store_ns_t* ns,
                            const dftu_store_scope* scope, dftu_error* err) {
    if (!store || !ns) return invalid_argument(err, "store or ns is NULL");
    auto key = scope_of(scope);
    if (!key) return common::fill_error(err, key.error());
    auto dropped = store->store.drop(ns->ns, *key);
    if (!dropped) return common::fill_error(err, dropped.error());
    return DFTU_OK;
}

dftu_status dftu_store_flush(dftu_store_t* store, dftu_error* err) {
    if (!store) return invalid_argument(err, "store is NULL");
    auto flushed = block(store->store.flush());
    if (!flushed) return common::fill_error(err, flushed.error());
    return DFTU_OK;
}

dftu_status dftu_store_refresh(dftu_store_t* store, dftu_error* err) {
    if (!store) return invalid_argument(err, "store is NULL");
    auto refreshed = store->store.refresh();
    if (!refreshed) return common::fill_error(err, refreshed.error());
    return DFTU_OK;
}

dftu_status dftu_store_close(dftu_store_t* store, dftu_error* err) {
    if (!store) return invalid_argument(err, "store is NULL");
    auto closed = block(store->store.close());
    if (!closed) return common::fill_error(err, closed.error());
    return DFTU_OK;
}

void dftu_store_pack_options_init(dftu_store_pack_options* out) {
    if (!out) return;
    const PackOptions defaults;
    *out = dftu_store_pack_options{
        static_cast<std::uint32_t>(sizeof(dftu_store_pack_options)),
        defaults.small_file_bytes, defaults.dead_ratio, defaults.keep_seconds};
}

dftu_status dftu_store_pack(dftu_store_t* store,
                            const dftu_store_pack_options* options,
                            dftu_store_pack_report* out, dftu_error* err) {
    if (!store || !out) return invalid_argument(err, "store or out is NULL");
    PackOptions chosen;
    if (options) {
        dftu_store_pack_options copy;
        dftu_store_pack_options_init(&copy);
        if (!common::copy_struct(
                &copy, sizeof(copy), options, options->struct_size,
                offsetof(dftu_store_pack_options, small_file_bytes))) {
            return invalid_argument(
                err, "options struct_size does not reach the first field");
        }
        chosen.small_file_bytes = copy.small_file_bytes;
        chosen.dead_ratio = copy.dead_ratio;
        chosen.keep_seconds = copy.keep_seconds;
    }
    auto packed = block(store->store.pack(chosen));
    if (!packed) return common::fill_error(err, packed.error());
    *out = dftu_store_pack_report{packed->files_packed, packed->copies_written,
                                  packed->copies_skipped, packed->files_deleted,
                                  packed->bytes_deleted};
    return DFTU_OK;
}

dftu_status dftu_store_snapshot(dftu_store_t* store,
                                dftu_store_snapshot_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!store) return invalid_argument(err, "store is NULL");
    auto view = store->store.snapshot();
    if (!view) return common::fill_error(err, view.error());
    *out = new dftu_store_snapshot_s(std::move(*view));
    return DFTU_OK;
}

void dftu_store_snapshot_free(dftu_store_snapshot_t* snapshot) {
    delete snapshot;
}

dftu_status dftu_store_snapshot_copy_info(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, dftu_store_source_stat* sources,
    std::uint64_t capacity, dftu_store_copy* out, dftu_error* err) {
    if (!snapshot || !ns || !out || (capacity > 0 && !sources)) {
        return invalid_argument(
            err,
            "snapshot, ns or out is NULL, or sources is NULL with a capacity");
    }
    auto key = scope_of(scope);
    if (!key) return common::fill_error(err, key.error());
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
    *out =
        dftu_store_copy{static_cast<std::int32_t>(info.state), info.generation,
                        info.params_hash, info.sources.size()};
    return DFTU_OK;
}

dftu_status dftu_store_snapshot_get(const dftu_store_snapshot_t* snapshot,
                                    const dftu_store_ns_t* ns,
                                    const dftu_store_scope* scope,
                                    const std::uint8_t* key,
                                    std::uint64_t key_size,
                                    dftu_store_value_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!snapshot || !ns || !bytes_ok(key, key_size)) {
        return invalid_argument(
            err, "snapshot or ns is NULL, or key is NULL with a size");
    }
    auto where = scope_of(scope);
    if (!where) return common::fill_error(err, where.error());
    auto found = snapshot->snapshot.get(ns->ns, *where, view_of(key, key_size));
    if (!found) return common::fill_error(err, found.error());
    if (found->has_value()) {
        *out = new dftu_store_value_s(std::move(**found));
    }
    return DFTU_OK;
}

dftu_status dftu_store_snapshot_multi_get(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const dftu_store_bytes* keys,
    std::uint64_t n, dftu_store_value_list_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!snapshot || !ns || (n > 0 && !keys)) {
        return invalid_argument(
            err, "snapshot or ns is NULL, or keys is NULL with a count");
    }
    auto where = scope_of(scope);
    if (!where) return common::fill_error(err, where.error());
    dstd::Vector<dstd::ByteView> views;
    if (n > views.max_size()) {
        return invalid_argument(err, "key count is too large");
    }
    views.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        if (!bytes_ok(keys[i].data, keys[i].size)) {
            return invalid_argument(err, "a key is NULL with a size");
        }
        views.push_back(view_of(keys[i].data, keys[i].size));
    }
    auto found = snapshot->snapshot.multi_get(
        ns->ns, *where,
        dstd::Span<const dstd::ByteView>(views.data(), views.size()));
    if (!found) return common::fill_error(err, found.error());
    *out = new dftu_store_value_list_s(std::move(*found));
    return DFTU_OK;
}

dftu_store_bytes dftu_store_value_bytes(const dftu_store_value_t* v) {
    if (!v) return dftu_store_bytes{nullptr, 0};
    return bytes_of(v->value.bytes());
}

void dftu_store_value_free(dftu_store_value_t* v) { delete v; }

std::uint64_t dftu_store_value_list_count(const dftu_store_value_list_t* list) {
    return list ? list->values.size() : 0;
}

std::int32_t dftu_store_value_list_at(const dftu_store_value_list_t* list,
                                      std::uint64_t i, dftu_store_bytes* out) {
    if (!list || !out || i >= list->values.size()) return -1;
    const auto& entry = list->values[static_cast<std::size_t>(i)];
    if (!entry.has_value()) return 0;
    *out = bytes_of(entry->bytes());
    return 1;
}

void dftu_store_value_list_free(dftu_store_value_list_t* list) { delete list; }

dftu_status dftu_store_snapshot_scan_prefix(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const std::uint8_t* prefix,
    std::uint64_t prefix_size, dftu_store_cursor_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!snapshot || !ns || !bytes_ok(prefix, prefix_size)) {
        return invalid_argument(
            err, "snapshot or ns is NULL, or prefix is NULL with a size");
    }
    auto where = scope_of(scope);
    if (!where) return common::fill_error(err, where.error());
    return make_cursor(
        snapshot->snapshot.scan(ns->ns, *where, view_of(prefix, prefix_size)),
        out);
}

dftu_status dftu_store_snapshot_scan_range(
    const dftu_store_snapshot_t* snapshot, const dftu_store_ns_t* ns,
    const dftu_store_scope* scope, const std::uint8_t* begin,
    std::uint64_t begin_size, const std::uint8_t* end, std::uint64_t end_size,
    dftu_store_cursor_t** out, dftu_error* err) {
    if (!clear_out(out)) return invalid_argument(err, "out is NULL");
    if (!snapshot || !ns || !bytes_ok(begin, begin_size) ||
        !bytes_ok(end, end_size)) {
        return invalid_argument(
            err, "snapshot or ns is NULL, or a bound is NULL with a size");
    }
    auto where = scope_of(scope);
    if (!where) return common::fill_error(err, where.error());
    return make_cursor(
        snapshot->snapshot.scan(ns->ns, *where, view_of(begin, begin_size),
                                view_of(end, end_size)),
        out);
}

dftu_status dftu_store_cursor_next(dftu_store_cursor_t* cursor,
                                   dftu_error* err) {
    if (!cursor) return invalid_argument(err, "cursor is NULL");
    if (cursor->started) {
        if (!cursor->cursor.at_end()) cursor->cursor.next();
    } else {
        cursor->started = true;
    }
    if (!cursor->cursor.at_end()) return DFTU_OK;
    if (cursor->cursor.error().has_value()) {
        return common::fill_error(err, *cursor->cursor.error());
    }
    return DFTU_END;
}

dftu_store_bytes dftu_store_cursor_key(const dftu_store_cursor_t* cursor) {
    if (!cursor || !cursor->started || cursor->cursor.at_end()) {
        return dftu_store_bytes{nullptr, 0};
    }
    return bytes_of(cursor->cursor.key());
}

dftu_status dftu_store_cursor_value(dftu_store_cursor_t* cursor,
                                    dftu_store_bytes* out, dftu_error* err) {
    if (!cursor || !out || !cursor->started || cursor->cursor.at_end()) {
        return invalid_argument(
            err,
            "cursor or out is NULL, or the cursor is not moved or at its "
            "end");
    }
    auto value = cursor->cursor.value();
    if (!value) return common::fill_error(err, value.error());
    *out = bytes_of(*value);
    return DFTU_OK;
}

void dftu_store_cursor_free(dftu_store_cursor_t* cursor) { delete cursor; }

}  // extern "C"
