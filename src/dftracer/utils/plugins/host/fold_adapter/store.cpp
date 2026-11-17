// SPDX-License-Identifier: MIT
// dftu.svc.store: the index root's store of the scan's trace folder, lent to
// the plugin in namespaces the host names `plugin.<plugin name>.<name>`. Every
// call forwards one function of the store C ABI and turns its error into the
// callback's ctx message.
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/store/c_api.h>
#include <dftracer/utils/trace/index/store/root_layout.h>

#include <atomic>
#include <cstdint>

namespace dftracer::utils::plugins::host {

namespace {

// The prefix of every namespace name a plugin reaches: its own.
constexpr dstd::StringView PLUGIN_PREFIX = "plugin.";

// The names the host keeps for itself; a plugin may not name a namespace so.
constexpr dstd::StringView HOST_NAMES[] = {"dftu.", "server."};

// Why `name` is not a namespace name a plugin may pass, or null.
const char* bad_name_byte(dstd::StringView name) {
    for (const char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '_' || c == '.' || c == '-';
        if (!allowed) return "holds a byte outside a-z, 0-9, '_', '.' and '-'";
    }
    return nullptr;
}

dftu_status name_refused(dftu_ctx* ctx, dstd::StringView name,
                         const char* why) {
    return host_fail(ctx, DFTU_E_INVALID,
                     dstd::String("dftu.svc.store: namespace '") +
                         dstd::String(name.data(), name.size()) + "' " + why);
}

// The full name the store registers for the plugin's `name`: plugin.<plugin
// name>.<name>. Refuses an escape out of the plugin's own namespaces.
dftu_status namespace_name(dftu_ctx* ctx, dstd::StringView plugin,
                           const char* raw, dstd::String* out) {
    out->clear();
    if (raw == nullptr || *raw == '\0')
        return host_fail(ctx, DFTU_E_INVALID,
                         "dftu.svc.store: the namespace name is empty");
    const dstd::StringView name(raw);
    if (const char* why = bad_name_byte(name); why != nullptr)
        return name_refused(ctx, name, why);
    if (name.find("..") != dstd::StringView::npos)
        return name_refused(ctx, name, "contains '..'");
    if (name.front() == '.') return name_refused(ctx, name, "starts with '.'");
    for (const dstd::StringView kept : HOST_NAMES) {
        if (name.substr(0, kept.size()) == kept)
            return name_refused(ctx, name,
                                "starts with a name the host keeps for itself");
    }
    for (const char c : plugin) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed)
            return host_fail(
                ctx, DFTU_E_INVALID,
                dstd::String("dftu.svc.store: the plugin name '") +
                    dstd::String(plugin.data(), plugin.size()) +
                    "' holds a byte outside a-z, A-Z, 0-9, '_' and "
                    "'-'");
    }
    *out = dstd::String(PLUGIN_PREFIX);
    *out += dstd::String(plugin.data(), plugin.size());
    *out += '.';
    *out += name;
    return DFTU_OK;
}

// The status of a store C ABI call, with its error moved onto the ctx.
dftu_status reported(dftu_ctx* ctx, dftu_status status, dftu_error& err) {
    dftu_status out = status;
    if (status < 0) {
        out = host_fail(ctx, status,
                        err.message != nullptr
                            ? dstd::StringView(err.message)
                            : dstd::StringView("the store refused the call"));
    }
    dftu_error_release(&err);
    return out;
}

template <class Fn>
dftu_status call(dftu_ctx* ctx, Fn&& fn) {
    dftu_error err = DFTU_ERROR_INIT;
    return reported(ctx, fn(&err), err);
}

PluginFold* fold_of(void* h) { return static_cast<PluginFold*>(h); }

// The fold's store handle, opened on the first call and never reopened.
dftu_status fold_store(dftu_ctx* ctx, void* h, dftu_store_t** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "dftu.svc.store: out is NULL");
    *out = nullptr;
    if (h == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "dftu.svc.store: h is NULL");
    return fold_of(h)->store_for(ctx, out);
}

// The test-only count of store flushes that fail on purpose.
std::atomic<int> g_flush_failures_for_test{0};

// A handle the host made for a plugin: the store directory it belongs to and
// the handle inside it. The plugin sees only the address of this struct.
// Namespace ids are sequential per writer, so an id alone would reach another
// store's namespace id; the directory names the store, since every fold over
// one index root opens the same per-directory state.
struct Owned : dstd::HeapObject {
    Owned(dstd::StringView owner, void* handle)
        : store(owner.data(), owner.size()), inner(handle) {}
    dstd::String store;
    void* inner;
};

template <class T>
T* own(dstd::StringView store, T* inner) {
    return reinterpret_cast<T*>(new Owned(store, static_cast<void*>(inner)));
}

dstd::StringView owner_of(const void* handle) {
    return static_cast<const Owned*>(handle)->store;
}

template <class T>
T* inner_of(const void* handle) {
    return static_cast<T*>(static_cast<const Owned*>(handle)->inner);
}

void free_owned(void* handle) { delete static_cast<Owned*>(handle); }

// The error for a NULL handle of `what`.
dftu_status handle_null(dftu_ctx* ctx, const char* what) {
    return host_fail(
        ctx, DFTU_E_INVALID,
        dstd::String("dftu.svc.store: the ") + what + " handle is NULL");
}

// The handle inside a host-made handle. Every call that takes a namespace,
// batch, snapshot or cursor goes through it, so a NULL never reaches the store
// C ABI through a null struct.
template <class T>
dftu_status handle_inner(dftu_ctx* ctx, const void* handle, const char* what,
                         T** out) {
    *out = nullptr;
    if (handle == nullptr) return handle_null(ctx, what);
    *out = inner_of<T>(handle);
    return DFTU_OK;
}

// The store of `h`, plus the check that `handle` is one this store made.
dftu_status checked_store(dftu_ctx* ctx, void* h, const void* handle,
                          const char* what, dftu_store_t** out) {
    if (const dftu_status st = fold_store(ctx, h, out); st < 0) return st;
    if (handle == nullptr) return handle_null(ctx, what);
    if (owner_of(handle) != fold_of(h)->store_identity())
        return host_fail(ctx, DFTU_E_INVALID,
                         dstd::String("dftu.svc.store: the ") + what +
                             " handle was made by another host's store");
    return DFTU_OK;
}

// A snapshot and a namespace handle of one store: get and the scans take no
// host, so the snapshot's owner is the store they must agree on.
dftu_status checked_pair(dftu_ctx* ctx, const dftu_store_snapshot_t* snapshot,
                         const dftu_store_ns_t* ns) {
    if (snapshot == nullptr)
        return host_fail(ctx, DFTU_E_INVALID,
                         "dftu.svc.store: the snapshot handle is NULL");
    if (ns == nullptr) return handle_null(ctx, "namespace");
    if (owner_of(snapshot) != owner_of(ns))
        return host_fail(ctx, DFTU_E_INVALID,
                         "dftu.svc.store: the namespace handle was made by "
                         "another host's store");
    return DFTU_OK;
}

dftu_status store_ns(dftu_ctx* ctx, void* h, const char* name,
                     std::uint32_t format_version, dftu_store_fold_kind fold,
                     std::int32_t durable, std::int32_t shared,
                     dftu_store_ns_t** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "dftu.svc.store: ns out is NULL");
    *out = nullptr;
    dftu_store_t* store = nullptr;
    if (const dftu_status st = fold_store(ctx, h, &store); st < 0) return st;
    dstd::String full;
    if (const dftu_status st =
            namespace_name(ctx, fold_of(h)->plugin_name(), name, &full);
        st < 0)
        return st;
    dftu_store_ns_t* made = nullptr;
    const dftu_status status = call(ctx, [&](dftu_error* err) {
        return ::dftu_store_ns(store, full.c_str(), format_version, fold,
                               durable, shared, &made, err);
    });
    if (status < 0) return status;
    *out = own(fold_of(h)->store_identity(), made);
    return DFTU_OK;
}

void store_ns_free(::dftu_store_ns_t* ns) {
    if (ns == nullptr) return;
    ::dftu_store_ns_free(inner_of<::dftu_store_ns_t>(ns));
    free_owned(ns);
}

dftu_status store_batch(dftu_ctx* ctx, void* h, const dftu_store_ns_t* ns,
                        const dftu_store_scope* scope,
                        std::uint64_t params_hash,
                        const dftu_store_source_stat* sources,
                        std::uint64_t source_count, dftu_store_batch_t** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID,
                         "dftu.svc.store: batch out is NULL");
    *out = nullptr;
    dftu_store_t* store = nullptr;
    if (const dftu_status st = checked_store(ctx, h, ns, "namespace", &store);
        st < 0)
        return st;
    dftu_store_batch_t* made = nullptr;
    const dftu_status status = call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch(store, inner_of<dftu_store_ns_t>(ns), scope,
                                  params_hash, sources, source_count, &made,
                                  err);
    });
    if (status < 0) return status;
    *out = own(fold_of(h)->store_identity(), made);
    return DFTU_OK;
}

void store_batch_free(::dftu_store_batch_t* batch) {
    if (batch == nullptr) return;
    ::dftu_store_batch_free(inner_of<::dftu_store_batch_t>(batch));
    free_owned(batch);
}

dftu_status store_put(dftu_ctx* ctx, dftu_store_batch_t* batch,
                      const std::uint8_t* key, std::uint64_t key_size,
                      const std::uint8_t* value, std::uint64_t value_size) {
    dftu_store_batch_t* inner = nullptr;
    if (const dftu_status st = handle_inner(ctx, batch, "batch", &inner);
        st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch_put(inner, key, key_size, value, value_size,
                                      err);
    });
}

dftu_status store_fold(dftu_ctx* ctx, dftu_store_batch_t* batch,
                       const std::uint8_t* key, std::uint64_t key_size,
                       const std::uint8_t* value, std::uint64_t value_size) {
    dftu_store_batch_t* inner = nullptr;
    if (const dftu_status st = handle_inner(ctx, batch, "batch", &inner);
        st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch_fold(inner, key, key_size, value, value_size,
                                       err);
    });
}

dftu_status store_erase(dftu_ctx* ctx, dftu_store_batch_t* batch,
                        const std::uint8_t* key, std::uint64_t key_size) {
    dftu_store_batch_t* inner = nullptr;
    if (const dftu_status st = handle_inner(ctx, batch, "batch", &inner);
        st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch_erase(inner, key, key_size, err);
    });
}

dftu_status store_erase_range(dftu_ctx* ctx, dftu_store_batch_t* batch,
                              const std::uint8_t* begin,
                              std::uint64_t begin_size, const std::uint8_t* end,
                              std::uint64_t end_size) {
    dftu_store_batch_t* inner = nullptr;
    if (const dftu_status st = handle_inner(ctx, batch, "batch", &inner);
        st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch_erase_range(inner, begin, begin_size, end,
                                              end_size, err);
    });
}

// Commit only. The plugin has no flush of its own, so the host flushes its one
// writer once, from the fold's finalize, whichever fold of the plugin
// committed; a reader in another process sees the batch from then on, or
// earlier once the batch's extent fills. Only a durable namespace is synced, so
// nothing here promises a machine crash keeps it.
dftu_status store_commit(dftu_ctx* ctx, void* h, dftu_store_batch_t* batch) {
    dftu_store_t* store = nullptr;
    if (const dftu_status st = checked_store(ctx, h, batch, "batch", &store);
        st < 0) {
        // The store C ABI frees a batch on commit whether it commits or fails,
        // so a refused commit frees it here too.
        if (batch != nullptr) {
            ::dftu_store_batch_free(inner_of<::dftu_store_batch_t>(batch));
            free_owned(batch);
        }
        return st;
    }
    dftu_store_batch_t* const inner = inner_of<dftu_store_batch_t>(batch);
    free_owned(batch);
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_batch_commit(inner, err);
    });
}

dftu_status store_snapshot(dftu_ctx* ctx, void* h,
                           dftu_store_snapshot_t** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID,
                         "dftu.svc.store: snapshot out is NULL");
    *out = nullptr;
    dftu_store_t* store = nullptr;
    if (const dftu_status st = fold_store(ctx, h, &store); st < 0) return st;
    dftu_store_snapshot_t* made = nullptr;
    const dftu_status status = call(ctx, [&](dftu_error* err) {
        return ::dftu_store_snapshot(store, &made, err);
    });
    if (status < 0) return status;
    *out = own(fold_of(h)->store_identity(), made);
    return DFTU_OK;
}

void store_snapshot_free(::dftu_store_snapshot_t* snapshot) {
    if (snapshot == nullptr) return;
    ::dftu_store_snapshot_free(inner_of<::dftu_store_snapshot_t>(snapshot));
    free_owned(snapshot);
}

dftu_status store_get(dftu_ctx* ctx, const dftu_store_snapshot_t* snapshot,
                      const dftu_store_ns_t* ns, const dftu_store_scope* scope,
                      const std::uint8_t* key, std::uint64_t key_size,
                      dftu_store_value_t** out) {
    if (const dftu_status st = checked_pair(ctx, snapshot, ns); st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_snapshot_get(
            inner_of<dftu_store_snapshot_t>(snapshot),
            inner_of<dftu_store_ns_t>(ns), scope, key, key_size, out, err);
    });
}

dftu_status store_multi_get(dftu_ctx* ctx,
                            const dftu_store_snapshot_t* snapshot,
                            const dftu_store_ns_t* ns,
                            const dftu_store_scope* scope,
                            const dftu_store_bytes* keys, std::uint64_t n,
                            dftu_store_value_list_t** out) {
    if (const dftu_status st = checked_pair(ctx, snapshot, ns); st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_snapshot_multi_get(
            inner_of<dftu_store_snapshot_t>(snapshot),
            inner_of<dftu_store_ns_t>(ns), scope, keys, n, out, err);
    });
}

dftu_status store_scan_prefix(dftu_ctx* ctx,
                              const dftu_store_snapshot_t* snapshot,
                              const dftu_store_ns_t* ns,
                              const dftu_store_scope* scope,
                              const std::uint8_t* prefix,
                              std::uint64_t prefix_size,
                              dftu_store_cursor_t** out) {
    if (const dftu_status st = checked_pair(ctx, snapshot, ns); st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_snapshot_scan_prefix(
            inner_of<dftu_store_snapshot_t>(snapshot),
            inner_of<dftu_store_ns_t>(ns), scope, prefix, prefix_size, out,
            err);
    });
}

dftu_status store_scan_range(dftu_ctx* ctx,
                             const dftu_store_snapshot_t* snapshot,
                             const dftu_store_ns_t* ns,
                             const dftu_store_scope* scope,
                             const std::uint8_t* begin,
                             std::uint64_t begin_size, const std::uint8_t* end,
                             std::uint64_t end_size,
                             dftu_store_cursor_t** out) {
    if (const dftu_status st = checked_pair(ctx, snapshot, ns); st < 0)
        return st;
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_snapshot_scan_range(
            inner_of<dftu_store_snapshot_t>(snapshot),
            inner_of<dftu_store_ns_t>(ns), scope, begin, begin_size, end,
            end_size, out, err);
    });
}

// A cursor comes from a snapshot and is the store's own pointer, so it carries
// no owner: the store refuses it by its directory, through its snapshot.
dftu_status store_cursor_next(dftu_ctx* ctx, dftu_store_cursor_t* cursor) {
    if (cursor == nullptr) return handle_null(ctx, "cursor");
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_cursor_next(cursor, err);
    });
}

dftu_status store_cursor_value(dftu_ctx* ctx, dftu_store_cursor_t* cursor,
                               dftu_store_bytes* out) {
    if (cursor == nullptr) return handle_null(ctx, "cursor");
    return call(ctx, [&](dftu_error* err) {
        return ::dftu_store_cursor_value(cursor, out, err);
    });
}

const dftu_svc_store STORE_VTABLE = {
    .struct_size = sizeof(dftu_svc_store),
    .ns = store_ns,
    .ns_free = store_ns_free,
    .batch = store_batch,
    .put = store_put,
    .fold = store_fold,
    .erase = store_erase,
    .erase_range = store_erase_range,
    .commit = store_commit,
    .batch_free = store_batch_free,
    .snapshot = store_snapshot,
    .snapshot_free = store_snapshot_free,
    .get = store_get,
    .multi_get = store_multi_get,
    .scan_prefix = store_scan_prefix,
    .scan_range = store_scan_range,
    .cursor_next = store_cursor_next,
    .cursor_key = ::dftu_store_cursor_key,
    .cursor_value = store_cursor_value,
    .cursor_free = ::dftu_store_cursor_free,
    .value_bytes = ::dftu_store_value_bytes,
    .value_free = ::dftu_store_value_free,
    .value_list_count = ::dftu_store_value_list_count,
    .value_list_at = ::dftu_store_value_list_at,
    .value_list_free = ::dftu_store_value_list_free,
};

}  // namespace

const dstd::String& PluginFold::store_identity() { return store_dir_; }

dftu_status PluginFold::store_for(::dftu_ctx* ctx, ::dftu_store_t** out) {
    *out = nullptr;
    if (!store_conflict_.empty())
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::String("dftu.svc.store: the scan covers two index roots ('") +
                store_root_ + "' and '" + store_conflict_ +
                "'); one store cannot serve both");
    if (store_root_.empty())
        return host_fail(
            ctx, DFTU_E_INVALID,
            "dftu.svc.store: the scan has named no trace file yet, so there "
            "is no index root; call it from a fold step or later");
    // A slice that hands the store out writes to the shared writer of the
    // directory, so the master has to flush it from finalize.
    store_used_ = true;
    if (store_ != nullptr) {
        *out = store_;
        return DFTU_OK;
    }
    dftu_error err = DFTU_ERROR_INIT;
    const dftu_status opened =
        ::dftu_store_open(store_identity().c_str(), &store_, &err);
    if (const dftu_status st = reported(ctx, opened, err); st < 0) {
        store_ = nullptr;
        return st;
    }
    *out = store_;
    return DFTU_OK;
}

dstd::Result<void> PluginFold::store_flush() {
    // A slice wrote through its own handle, so the master's is still closed; it
    // opens the same per-directory state to flush what the slices committed.
    if (store_ == nullptr && store_used_ && !store_root_.empty()) {
        AbiContext ctx;
        ::dftu_store_t* opened = nullptr;
        if (const ::dftu_status st = store_for(ctx.get(), &opened); st < 0)
            return callback_error(plugin_label(), "store flush", st,
                                  ctx.message());
    }
    if (store_ == nullptr) return {};
    if (g_flush_failures_for_test.load(std::memory_order_relaxed) > 0 &&
        g_flush_failures_for_test.fetch_sub(1) > 0) {
        return dstd::Unexpected<dstd::Error>(
            callback_error(plugin_label(), "store flush", DFTU_E_IO,
                           "the store flush failed"));
    }
    dftu_error err = DFTU_ERROR_INIT;
    const dftu_status flushed = ::dftu_store_flush(store_, &err);
    if (flushed >= 0) {
        dftu_error_release(&err);
        return {};
    }
    const dstd::Error error = callback_error(
        plugin_label(), "store flush", flushed,
        err.message != nullptr ? dstd::StringView(err.message)
                               : dstd::StringView("the store flush failed"));
    dftu_error_release(&err);
    return dstd::Unexpected<dstd::Error>(error);
}

void detail::fail_next_store_flush_for_test() {
    g_flush_failures_for_test.fetch_add(1);
}

const void* detail::store_ext_vtable() { return &STORE_VTABLE; }

}  // namespace dftracer::utils::plugins::host
