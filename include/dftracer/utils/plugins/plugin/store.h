// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_STORE_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_STORE_H

#include <dftracer/utils/dataframe/plugin_error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::plugins {

/// The fold a namespace applies to the values of a key; see @ref Store::ns.
/// Append only, and the same values as the store C ABI.
enum class StoreFold : std::int32_t {
    None = DFTU_STORE_FOLD_NONE,
    SumI64 = DFTU_STORE_FOLD_SUM_I64,
    SumF64 = DFTU_STORE_FOLD_SUM_F64,
    MinI64 = DFTU_STORE_FOLD_MIN_I64,
    MaxI64 = DFTU_STORE_FOLD_MAX_I64,
    MinF64 = DFTU_STORE_FOLD_MIN_F64,
    MaxF64 = DFTU_STORE_FOLD_MAX_F64,
    Count = DFTU_STORE_FOLD_COUNT,
    Last = DFTU_STORE_FOLD_LAST,
    Concat = DFTU_STORE_FOLD_CONCAT,
};

/// The signature of one trace file of a scope, in the path order of the
/// scope. A copy of a scope keeps the signature it was written under, so a
/// batch that reports another signature for that scope is refused.
struct StoreSource {
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
};

/// What a batch or a read is keyed by: the whole index root, or one or more
/// trace files. Paths are copied.
class StoreScope {
   public:
    StoreScope() = default;

    /// The whole index root.
    static StoreScope root() { return StoreScope{}; }

    /// One trace file, which pins a copy by its @ref StoreSource.
    static StoreScope file(dstd::StringView path) {
        StoreScope scope;
        scope.kind_ = DFTU_STORE_SCOPE_FILE;
        scope.paths_.push_back(dstd::String(path.data(), path.size()));
        return scope;
    }

    /// The given files, which the store sorts and deduplicates.
    static StoreScope files(dstd::Span<const dstd::StringView> paths) {
        StoreScope scope;
        scope.kind_ = DFTU_STORE_SCOPE_FILES;
        for (const dstd::StringView path : paths) {
            scope.paths_.push_back(dstd::String(path.data(), path.size()));
        }
        return scope;
    }

   private:
    friend class Store;
    friend class StoreBatch;
    friend class StoreSnapshot;

    /// The C form, with pointers into `c_paths_`; valid until the next call.
    dftu_store_scope raw() const {
        c_paths_.clear();
        for (const dstd::String& path : paths_)
            c_paths_.push_back(path.c_str());
        dftu_store_scope out{};
        out.kind = kind_;
        out.paths = c_paths_.empty() ? nullptr : c_paths_.data();
        out.path_count = c_paths_.size();
        return out;
    }

    dstd::Vector<dstd::String> paths_;
    mutable dstd::Vector<const char*> c_paths_;
    std::int32_t kind_ = DFTU_STORE_SCOPE_ROOT;
};

class StoreBatch;
class StoreSnapshot;
class StoreValue;
class StoreValues;
class StoreCursor;

namespace detail {

inline dstd::Error store_service_missing() noexcept {
    return make_error(dstd::Condition::Unsupported,
                      "host lacks dftu.svc.store");
}

}  // namespace detail

/// One namespace of the store, in the plugin's own names: the host registers
/// `name` as `plugin.<plugin name>.<name>`. Move only; frees the handle.
class StoreNamespace {
   public:
    StoreNamespace() = default;
    StoreNamespace(StoreNamespace&& other) noexcept
        : e_(other.e_), ns_(other.ns_) {
        other.ns_ = nullptr;
    }
    StoreNamespace& operator=(StoreNamespace&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            ns_ = other.ns_;
            other.ns_ = nullptr;
        }
        return *this;
    }
    StoreNamespace(const StoreNamespace&) = delete;
    StoreNamespace& operator=(const StoreNamespace&) = delete;
    ~StoreNamespace() { reset(); }

    explicit operator bool() const noexcept { return ns_ != nullptr; }

   private:
    friend class Store;
    friend class StoreSnapshot;

    StoreNamespace(const dftu_svc_store* e, dftu_store_ns_t* ns) noexcept
        : e_(e), ns_(ns) {}

    void reset() {
        if (ns_ != nullptr && e_ != nullptr) e_->ns_free(ns_);
        ns_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_store_ns_t* ns_ = nullptr;
};

/// A batch of puts, folds and erases for one scope. Move only; frees the
/// handle, so a batch left uncommitted writes nothing.
class StoreBatch {
   public:
    StoreBatch() = default;
    StoreBatch(StoreBatch&& other) noexcept
        : e_(other.e_), ctx_(other.ctx_), h_(other.h_), b_(other.b_) {
        other.b_ = nullptr;
    }
    StoreBatch& operator=(StoreBatch&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            ctx_ = other.ctx_;
            h_ = other.h_;
            b_ = other.b_;
            other.b_ = nullptr;
        }
        return *this;
    }
    StoreBatch(const StoreBatch&) = delete;
    StoreBatch& operator=(const StoreBatch&) = delete;
    ~StoreBatch() { reset(); }

    explicit operator bool() const noexcept { return b_ != nullptr; }

    /// Add a key and value; the bytes are copied.
    dstd::Result<void> put(dstd::ByteView key, dstd::ByteView value) const {
        return detail::reported(e_->put(ctx_, b_, bytes(key), size(key),
                                        bytes(value), size(value)));
    }
    /// Add an input to `key` of a fold namespace; a numeric fold needs 8
    /// bytes.
    dstd::Result<void> fold(dstd::ByteView key, dstd::ByteView value) const {
        return detail::reported(e_->fold(ctx_, b_, bytes(key), size(key),
                                         bytes(value), size(value)));
    }
    /// Hide `key` in the copy of this batch's scope.
    dstd::Result<void> erase(dstd::ByteView key) const {
        return detail::reported(e_->erase(ctx_, b_, bytes(key), size(key)));
    }
    /// Hide every key in [begin, end); an empty `end` means to the last key.
    dstd::Result<void> erase_range(dstd::ByteView begin,
                                   dstd::ByteView end) const {
        return detail::reported(e_->erase_range(
            ctx_, b_, bytes(begin), size(begin), bytes(end), size(end)));
    }
    /// Commit and consume the batch, whether the commit succeeds or the host
    /// refuses it. This process reads it from its next snapshot, and another
    /// process once the host flushes its writer at the end of the fold, or
    /// earlier if the batch's extent fills. Only a durable namespace is synced
    /// to disk. A snapshot can be up to a second old, so read a value back in a
    /// later fold.
    dstd::Result<void> commit() {
        if (b_ == nullptr)
            return make_error(dstd::Condition::InvalidArgument,
                              "the batch holds nothing to commit");
        dftu_store_batch_t* const b = b_;
        b_ = nullptr;
        return detail::reported(e_->commit(ctx_, h_, b));
    }
    /// Discard without writing.
    void discard() { reset(); }

   private:
    friend class Store;

    StoreBatch(const dftu_svc_store* e, dftu_ctx* ctx, void* h,
               dftu_store_batch_t* b) noexcept
        : e_(e), ctx_(ctx), h_(h), b_(b) {}

    static const std::uint8_t* bytes(dstd::ByteView v) noexcept {
        return reinterpret_cast<const std::uint8_t*>(v.data());
    }
    static std::uint64_t size(dstd::ByteView v) noexcept {
        return static_cast<std::uint64_t>(v.size());
    }
    void reset() {
        if (b_ != nullptr && e_ != nullptr) e_->batch_free(b_);
        b_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    void* h_ = nullptr;
    dftu_store_batch_t* b_ = nullptr;
};

/// The bytes of one stored value, valid until the value is freed. Move only.
class StoreValue {
   public:
    StoreValue() = default;
    StoreValue(StoreValue&& other) noexcept : e_(other.e_), v_(other.v_) {
        other.v_ = nullptr;
    }
    StoreValue& operator=(StoreValue&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            v_ = other.v_;
            other.v_ = nullptr;
        }
        return *this;
    }
    StoreValue(const StoreValue&) = delete;
    StoreValue& operator=(const StoreValue&) = delete;
    ~StoreValue() { reset(); }

    dstd::ByteView bytes() const noexcept {
        const dftu_store_bytes raw = e_->value_bytes(v_);
        return dstd::ByteView{reinterpret_cast<const char*>(raw.data),
                              static_cast<std::size_t>(raw.size)};
    }

   private:
    friend class StoreSnapshot;

    StoreValue(const dftu_svc_store* e, dftu_store_value_t* v) noexcept
        : e_(e), v_(v) {}
    void reset() {
        if (v_ != nullptr && e_ != nullptr) e_->value_free(v_);
        v_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_store_value_t* v_ = nullptr;
};

/// One result per key of a multi_get, in key order; a missing key has no
/// value. Move only.
class StoreValues {
   public:
    StoreValues() = default;
    StoreValues(StoreValues&& other) noexcept : e_(other.e_), v_(other.v_) {
        other.v_ = nullptr;
    }
    StoreValues& operator=(StoreValues&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            v_ = other.v_;
            other.v_ = nullptr;
        }
        return *this;
    }
    StoreValues(const StoreValues&) = delete;
    StoreValues& operator=(const StoreValues&) = delete;
    ~StoreValues() { reset(); }

    std::size_t size() const noexcept {
        return static_cast<std::size_t>(e_->value_list_count(v_));
    }

    /// The value at `i`, or nothing when that key was missing.
    dstd::Result<dstd::Optional<dstd::ByteView>> at(std::size_t i) const {
        dftu_store_bytes raw{};
        const std::int32_t found =
            e_->value_list_at(v_, static_cast<std::uint64_t>(i), &raw);
        if (found < 0)
            return make_error(dstd::Condition::InvalidArgument,
                              "the value index is past the count");
        if (found == 0) return dstd::Optional<dstd::ByteView>{};
        return dstd::Optional<dstd::ByteView>{
            dstd::ByteView{reinterpret_cast<const char*>(raw.data),
                           static_cast<std::size_t>(raw.size)}};
    }

   private:
    friend class StoreSnapshot;

    StoreValues(const dftu_svc_store* e, dftu_store_value_list_t* v) noexcept
        : e_(e), v_(v) {}
    void reset() {
        if (v_ != nullptr && e_ != nullptr) e_->value_list_free(v_);
        v_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_store_value_list_t* v_ = nullptr;
};

/// A scan over the keys of a copy, in key order. It starts before the first
/// entry: call @ref next first. Move only.
class StoreCursor {
   public:
    StoreCursor() = default;
    StoreCursor(StoreCursor&& other) noexcept
        : e_(other.e_), ctx_(other.ctx_), c_(other.c_) {
        other.c_ = nullptr;
    }
    StoreCursor& operator=(StoreCursor&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            ctx_ = other.ctx_;
            c_ = other.c_;
            other.c_ = nullptr;
        }
        return *this;
    }
    StoreCursor(const StoreCursor&) = delete;
    StoreCursor& operator=(const StoreCursor&) = delete;
    ~StoreCursor() { reset(); }

    /// Move to the next entry; false at a clean end.
    dstd::Result<bool> next() const {
        const dftu_status status = e_->cursor_next(ctx_, c_);
        if (status == DFTU_END) return false;
        DFTRACER_UTILS_TRY_VOID(detail::reported(status));
        return true;
    }

    /// The key of the current entry, valid until the next @ref next.
    dstd::ByteView key() const noexcept {
        const dftu_store_bytes raw = e_->cursor_key(c_);
        return dstd::ByteView{reinterpret_cast<const char*>(raw.data),
                              static_cast<std::size_t>(raw.size)};
    }

    /// The value of the current entry, valid as its key is.
    dstd::Result<dstd::ByteView> value() const {
        dftu_store_bytes raw{};
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->cursor_value(ctx_, c_, &raw)));
        return dstd::ByteView{reinterpret_cast<const char*>(raw.data),
                              static_cast<std::size_t>(raw.size)};
    }

   private:
    friend class StoreSnapshot;

    StoreCursor(const dftu_svc_store* e, dftu_ctx* ctx,
                dftu_store_cursor_t* c) noexcept
        : e_(e), ctx_(ctx), c_(c) {}
    void reset() {
        if (c_ != nullptr && e_ != nullptr) e_->cursor_free(c_);
        c_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    dftu_store_cursor_t* c_ = nullptr;
};

/// A point-in-time view of the store, for reads. Move only; values and
/// cursors taken from it keep it alive at the C ABI, so take them before the
/// snapshot goes.
class StoreSnapshot {
   public:
    StoreSnapshot() = default;
    StoreSnapshot(StoreSnapshot&& other) noexcept
        : e_(other.e_), ctx_(other.ctx_), s_(other.s_) {
        other.s_ = nullptr;
    }
    StoreSnapshot& operator=(StoreSnapshot&& other) noexcept {
        if (this != &other) {
            reset();
            e_ = other.e_;
            ctx_ = other.ctx_;
            s_ = other.s_;
            other.s_ = nullptr;
        }
        return *this;
    }
    StoreSnapshot(const StoreSnapshot&) = delete;
    StoreSnapshot& operator=(const StoreSnapshot&) = delete;
    ~StoreSnapshot() { reset(); }

    /// The value of `key`, or nothing when it has none.
    dstd::Result<dstd::Optional<StoreValue>> get(const StoreNamespace& ns,
                                                 const StoreScope& scope,
                                                 dstd::ByteView key) const {
        const dftu_store_scope raw_scope = scope.raw();
        dftu_store_value_t* v = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->get(ctx_, s_, ns.ns_, &raw_scope, cast(key),
                    static_cast<std::uint64_t>(key.size()), &v)));
        if (v == nullptr) return dstd::Optional<StoreValue>{};
        return dstd::Optional<StoreValue>{StoreValue{e_, v}};
    }

    /// One result per key, in order.
    dstd::Result<StoreValues> multi_get(
        const StoreNamespace& ns, const StoreScope& scope,
        dstd::Span<const dstd::ByteView> keys) const {
        dstd::Vector<dftu_store_bytes> raw_keys;
        raw_keys.reserve(keys.size());
        for (const dstd::ByteView key : keys) {
            raw_keys.push_back(dftu_store_bytes{
                cast(key), static_cast<std::uint64_t>(key.size())});
        }
        const dftu_store_scope raw_scope = scope.raw();
        dftu_store_value_list_t* v = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->multi_get(ctx_, s_, ns.ns_, &raw_scope, raw_keys.data(),
                          static_cast<std::uint64_t>(raw_keys.size()), &v)));
        return StoreValues{e_, v};
    }

    /// A scan of the keys that start with `prefix`, in key order.
    dstd::Result<StoreCursor> scan_prefix(const StoreNamespace& ns,
                                          const StoreScope& scope,
                                          dstd::ByteView prefix) const {
        const dftu_store_scope raw_scope = scope.raw();
        dftu_store_cursor_t* c = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->scan_prefix(ctx_, s_, ns.ns_, &raw_scope, cast(prefix),
                            static_cast<std::uint64_t>(prefix.size()), &c)));
        return StoreCursor{e_, ctx_, c};
    }

    /// A scan of the keys in [begin, end); an empty `end` means to the last
    /// key.
    dstd::Result<StoreCursor> scan_range(const StoreNamespace& ns,
                                         const StoreScope& scope,
                                         dstd::ByteView begin,
                                         dstd::ByteView end) const {
        const dftu_store_scope raw_scope = scope.raw();
        dftu_store_cursor_t* c = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(
            e_->scan_range(ctx_, s_, ns.ns_, &raw_scope, cast(begin),
                           static_cast<std::uint64_t>(begin.size()), cast(end),
                           static_cast<std::uint64_t>(end.size()), &c)));
        return StoreCursor{e_, ctx_, c};
    }

   private:
    friend class Store;

    StoreSnapshot(const dftu_svc_store* e, dftu_ctx* ctx,
                  dftu_store_snapshot_t* s) noexcept
        : e_(e), ctx_(ctx), s_(s) {}
    static const std::uint8_t* cast(dstd::ByteView v) noexcept {
        return reinterpret_cast<const std::uint8_t*>(v.data());
    }
    void reset() {
        if (s_ != nullptr && e_ != nullptr) e_->snapshot_free(s_);
        s_ = nullptr;
    }

    const dftu_svc_store* e_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    dftu_store_snapshot_t* s_ = nullptr;
};

/// The store of the index root the scan reads, in the plugin's own
/// namespaces. A host that lent this service writes `plugin.<plugin
/// name>.<name>` for the `name` a plugin passes, so a plugin reaches only its
/// own namespaces; a name the host refuses fails with DFTU_E_INVALID.
class Store {
   public:
    Store() = default;

    /// The namespace `name` of this plugin; the same name and settings twice
    /// return equal handles and other settings are refused. `fold` needs
    /// @ref StoreFold::None with `shared`.
    dstd::Result<StoreNamespace> ns(dstd::StringView name,
                                    std::uint32_t format_version = 0,
                                    StoreFold fold = StoreFold::None,
                                    bool durable = false,
                                    bool shared = false) const {
        if (e_ == nullptr) return detail::store_service_missing();
        const dstd::String text(name.data(), name.size());
        dftu_store_ns_t* out = nullptr;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->ns(ctx_, h_->h, text.c_str(), format_version,
                                    static_cast<dftu_store_fold_kind>(fold),
                                    durable ? 1 : 0, shared ? 1 : 0, &out)));
        return StoreNamespace{e_, out};
    }

    /// A batch of writes for `scope`; commit it or let it go to write
    /// nothing. `sources` is one entry per path of `scope` and pins the copy.
    dstd::Result<StoreBatch> batch(
        const StoreNamespace& ns, const StoreScope& scope,
        std::uint64_t params_hash = 0,
        dstd::Span<const StoreSource> sources = {}) const {
        if (e_ == nullptr) return detail::store_service_missing();
        dstd::Vector<dftu_store_source_stat> raw_sources;
        raw_sources.reserve(sources.size());
        for (const StoreSource& source : sources) {
            raw_sources.push_back(
                dftu_store_source_stat{source.mtime, source.size, source.hash});
        }
        const dftu_store_scope raw_scope = scope.raw();
        dftu_store_batch_t* out = nullptr;
        DFTRACER_UTILS_TRY_VOID(detail::reported(e_->batch(
            ctx_, h_->h, ns.ns_, &raw_scope, params_hash, raw_sources.data(),
            static_cast<std::uint64_t>(raw_sources.size()), &out)));
        return StoreBatch{e_, ctx_, h_->h, out};
    }

    /// A point-in-time view for reads.
    dstd::Result<StoreSnapshot> snapshot() const {
        if (e_ == nullptr) return detail::store_service_missing();
        dftu_store_snapshot_t* out = nullptr;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->snapshot(ctx_, h_->h, &out)));
        return StoreSnapshot{e_, ctx_, out};
    }

   private:
    friend class Host;

    Store(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          e_(h != nullptr && h->get_service != nullptr
                 ? static_cast<const dftu_svc_store*>(
                       h->get_service(h->h, DFTU_SVC_STORE))
                 : nullptr) {}

    const dftu_plugin_host* h_ = nullptr;
    dftu_ctx* ctx_ = nullptr;
    const dftu_svc_store* e_ = nullptr;
};

inline Store Host::store() const { return Store{h_, ctx_}; }

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_STORE_H */
