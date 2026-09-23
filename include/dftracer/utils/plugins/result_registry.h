#ifndef DFTRACER_UTILS_PLUGINS_RESULT_REGISTRY_H
#define DFTRACER_UTILS_PLUGINS_RESULT_REGISTRY_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/plugins/abi/ops.h>
#include <dftracer/utils/plugins/owned_arrow.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Host-side named result channel backing dft.ext.result. Policy-free: it moves
// opaque bytes or a user-schema Arrow array and never interprets either.
namespace dftracer::utils::plugins {

/// A native columnar result carried across the ABI as a dftu_dataframe handle
/// (the dataframe engine's own boundary type), freed via the dataframe C ABI.
/// Lets an engine-backed result (the aggregation accumulator) cross as our own
/// DataFrame with no Arrow round-trip.
struct OwnedDataFrame {
    dftu_dataframe* handle = nullptr;
    OwnedDataFrame() = default;
    explicit OwnedDataFrame(dftu_dataframe* h) noexcept : handle(h) {}
    OwnedDataFrame(OwnedDataFrame&& o) noexcept : handle(o.handle) {
        o.handle = nullptr;
    }
    OwnedDataFrame& operator=(OwnedDataFrame&& o) noexcept {
        if (this != &o) {
            if (handle) dftu_dataframe_free(handle);
            handle = o.handle;
            o.handle = nullptr;
        }
        return *this;
    }
    OwnedDataFrame(const OwnedDataFrame&) = delete;
    OwnedDataFrame& operator=(const OwnedDataFrame&) = delete;
    ~OwnedDataFrame() {
        if (handle) dftu_dataframe_free(handle);
    }

    /// Releases ownership to the caller, who must free it (or hand it to
    /// something that will); the destructor no longer will.
    dftu_dataframe* release() noexcept {
        dftu_dataframe* h = handle;
        handle = nullptr;
        return h;
    }
};

/// A deferred columnar result carried across the ABI as a dftu_lazyframe
/// handle, freed via the dataframe C ABI. Lets a plugin return a lazy plan the
/// host collects at the Python edge. The plan MUST be self-contained (its
/// source an in-memory frame or a re-openable source, e.g. dftu_dataframe_lazy
/// of a materialized frame); referencing the plugin's per-scan/executor
/// context, which dies at finalize, is a plugin bug.
///
/// Also a fluent plan builder over the registered "dftu.lazy.*" ops (see
/// exported_lazy_ops.def): every method below reaches the host through the op
/// registry (dftu_svc_ops::run_lazy) rather than linking the dataframe
/// dftu_lazyframe_* ABI directly, the same indirection dftu_svc_ops already
/// uses for column/frame ops. Every method BORROWS `*this` and returns a new
/// plan, leaving this one usable - the same contract as the layers beneath,
/// where dftu_lazyframe_* takes a const handle and LazyFrame's own methods are
/// const. A method that fails for any reason (no ops extension wired up, an
/// unregistered name, or a kind/arity/shape/argument rejection) returns an
/// EMPTY OwnedLazyFrame (`!out.handle`); every method short-circuits on an
/// already-empty OwnedLazyFrame, so a failure partway through a chain
/// propagates to the end without corrupting or leaking anything, and the
/// caller checks `handle` once at the end of the chain rather than after
/// every step.
struct OwnedLazyFrame {
    OwnedLazyFrame() = default;
    explicit OwnedLazyFrame(dftu_lazyframe* h) noexcept : handle(h) {}
    OwnedLazyFrame(dftu_lazyframe* h, const dftu_svc_ops* ops,
                   void* host) noexcept
        : handle(h), ops_(ops), host_(host) {}
    OwnedLazyFrame(OwnedLazyFrame&& o) noexcept
        : handle(o.handle), ops_(o.ops_), host_(o.host_) {
        o.handle = nullptr;
    }
    OwnedLazyFrame& operator=(OwnedLazyFrame&& o) noexcept {
        if (this != &o) {
            if (handle) dftu_lazyframe_free(handle);
            handle = o.handle;
            ops_ = o.ops_;
            host_ = o.host_;
            o.handle = nullptr;
        }
        return *this;
    }
    OwnedLazyFrame(const OwnedLazyFrame&) = delete;
    OwnedLazyFrame& operator=(const OwnedLazyFrame&) = delete;
    ~OwnedLazyFrame() {
        if (handle) dftu_lazyframe_free(handle);
    }

    /// Releases ownership to the caller, who must free it (or hand it to
    /// something that will); the destructor no longer will.
    dftu_lazyframe* release() noexcept {
        dftu_lazyframe* h = handle;
        handle = nullptr;
        return h;
    }

    dftu_lazyframe* handle = nullptr;

    OwnedLazyFrame auto_spill() const {
        return apply("dftu.lazy.auto_spill", nullptr);
    }
    OwnedLazyFrame describe() const {
        return apply("dftu.lazy.describe", nullptr);
    }
    OwnedLazyFrame drop_duplicates() const {
        return apply("dftu.lazy.drop_duplicates", nullptr);
    }
    OwnedLazyFrame drop_nulls() const {
        return apply("dftu.lazy.drop_nulls", nullptr);
    }
    OwnedLazyFrame is_duplicated() const {
        return apply("dftu.lazy.is_duplicated", nullptr);
    }
    OwnedLazyFrame is_unique() const {
        return apply("dftu.lazy.is_unique", nullptr);
    }
    OwnedLazyFrame null_count() const {
        return apply("dftu.lazy.null_count", nullptr);
    }
    OwnedLazyFrame unique() const { return apply("dftu.lazy.unique", nullptr); }
    OwnedLazyFrame reverse() const {
        return apply("dftu.lazy.reverse", nullptr);
    }

    OwnedLazyFrame explode(const char* column) const {
        return apply("dftu.lazy.explode",
                     dataframe::OpArgs().str(1, column ? column : ""));
    }
    OwnedLazyFrame to_dummies(const char* column) const {
        return apply("dftu.lazy.to_dummies",
                     dataframe::OpArgs().str(1, column ? column : ""));
    }
    OwnedLazyFrame with_row_index(const char* name) const {
        return apply("dftu.lazy.with_row_index",
                     dataframe::OpArgs().str(1, name ? name : ""));
    }

    OwnedLazyFrame filter_mask(const dftu_series* mask) const {
        return apply("dftu.lazy.filter_mask",
                     dataframe::OpArgs().series(1, mask));
    }

    OwnedLazyFrame fill_null(dftu_scalar value) const {
        return apply("dftu.lazy.fill_null",
                     dataframe::OpArgs().scalar(1, value));
    }
    OwnedLazyFrame filter(const dftu_expr* pred) const {
        return apply("dftu.lazy.filter", dataframe::OpArgs().expr(1, pred));
    }
    OwnedLazyFrame with_column(const char* name, const dftu_expr* expr) const {
        return apply(
            "dftu.lazy.with_column",
            dataframe::OpArgs().str(1, name ? name : "").expr(2, expr));
    }

    OwnedLazyFrame head(std::int64_t n) const {
        return apply("dftu.lazy.head", dataframe::OpArgs().i64(1, n));
    }
    OwnedLazyFrame tail(std::int64_t n) const {
        return apply("dftu.lazy.tail", dataframe::OpArgs().i64(1, n));
    }
    OwnedLazyFrame memory_budget(std::uint64_t bytes) const {
        return apply("dftu.lazy.memory_budget",
                     dataframe::OpArgs().u64(1, bytes));
    }
    OwnedLazyFrame slice(std::int64_t offset, std::int64_t len) const {
        return apply("dftu.lazy.slice",
                     dataframe::OpArgs().i64(1, offset).i64(2, len));
    }
    OwnedLazyFrame sample(std::int64_t n, std::uint64_t seed) const {
        return apply("dftu.lazy.sample",
                     dataframe::OpArgs().i64(1, n).u64(2, seed));
    }
    OwnedLazyFrame take(std::span<const std::int64_t> idx) const {
        return apply("dftu.lazy.take", dataframe::OpArgs().i64list(1, idx));
    }

    OwnedLazyFrame select(std::initializer_list<const char*> names) const {
        std::vector<const char*> v(names);
        return apply("dftu.lazy.select",
                     dataframe::OpArgs().strlist(
                         1, v.data(), static_cast<std::int32_t>(v.size())));
    }
    OwnedLazyFrame rename(std::initializer_list<const char*> names) const {
        std::vector<const char*> v(names);
        return apply("dftu.lazy.rename",
                     dataframe::OpArgs().strlist(
                         1, v.data(), static_cast<std::int32_t>(v.size())));
    }
    OwnedLazyFrame unpivot(
        std::initializer_list<const char*> id_vars,
        std::initializer_list<const char*> value_vars) const {
        std::vector<const char*> ids(id_vars), vals(value_vars);
        return apply(
            "dftu.lazy.unpivot",
            dataframe::OpArgs()
                .strlist(1, ids.data(), static_cast<std::int32_t>(ids.size()))
                .strlist(2, vals.data(),
                         static_cast<std::int32_t>(vals.size())));
    }
    OwnedLazyFrame melt(std::initializer_list<const char*> id_vars,
                        std::initializer_list<const char*> value_vars) const {
        std::vector<const char*> ids(id_vars), vals(value_vars);
        return apply(
            "dftu.lazy.melt",
            dataframe::OpArgs()
                .strlist(1, ids.data(), static_cast<std::int32_t>(ids.size()))
                .strlist(2, vals.data(),
                         static_cast<std::int32_t>(vals.size())));
    }
    OwnedLazyFrame group_by(std::initializer_list<const char*> keys,
                            std::initializer_list<dftu_group_agg> aggs) const {
        std::vector<const char*> k(keys);
        std::vector<dftu_group_agg> a(aggs);
        return apply(
            "dftu.lazy.group_by",
            dataframe::OpArgs()
                .strlist(1, k.data(), static_cast<std::int32_t>(k.size()))
                .agglist(2, a));
    }
    OwnedLazyFrame sort_by_multi(std::initializer_list<const char*> by,
                                 bool descending) const {
        std::vector<const char*> v(by);
        return apply(
            "dftu.lazy.sort_by_multi",
            dataframe::OpArgs()
                .strlist(1, v.data(), static_cast<std::int32_t>(v.size()))
                .i32(2, static_cast<std::int32_t>(descending)));
    }

    OwnedLazyFrame sort_by(const char* name, bool descending) const {
        return apply("dftu.lazy.sort_by",
                     dataframe::OpArgs()
                         .str(1, name ? name : "")
                         .i32(2, static_cast<std::int32_t>(descending)));
    }
    OwnedLazyFrame topk(const char* name, std::int64_t k, bool largest) const {
        return apply("dftu.lazy.topk",
                     dataframe::OpArgs()
                         .str(1, name ? name : "")
                         .i64(2, k)
                         .i32(3, static_cast<std::int32_t>(largest)));
    }

    OwnedLazyFrame pivot(const char* index, const char* on, const char* values,
                         const char* agg) const {
        return apply("dftu.lazy.pivot", dataframe::OpArgs()
                                            .str(1, index ? index : "")
                                            .str(2, on ? on : "")
                                            .str(3, values ? values : "")
                                            .str(4, agg ? agg : ""));
    }

    /// Join with `other` on `left_on[i]` = `right_on[i]` (dftu_dataframe_join
    /// semantics); `other` is borrowed. Empty if the key lists differ in
    /// length.
    OwnedLazyFrame join(const OwnedLazyFrame& other,
                        std::initializer_list<const char*> left_on,
                        std::initializer_list<const char*> right_on,
                        dftu_join_how how = DFTU_JOIN_INNER,
                        const char* suffix = nullptr) const {
        if (left_on.size() != right_on.size()) return OwnedLazyFrame{};
        std::vector<const char*> l(left_on);
        std::vector<const char*> r(right_on);
        const dftu_lazyframe* in[2] = {handle, other.handle};
        return apply(
            "dftu.lazy.join", in, 2,
            dataframe::OpArgs()
                .strlist(2, l.data(), static_cast<std::int32_t>(l.size()))
                .strlist(3, r.data(), static_cast<std::int32_t>(r.size()))
                .i32(4, static_cast<std::int32_t>(how))
                .str(5, suffix ? suffix : ""));
    }
    /// Join on the same-named key columns `on`.
    OwnedLazyFrame join(const OwnedLazyFrame& other,
                        std::initializer_list<const char*> on,
                        dftu_join_how how = DFTU_JOIN_INNER,
                        const char* suffix = nullptr) const {
        return join(other, on, on, how, suffix);
    }

    OwnedLazyFrame group_by_dynamic(const char* time_col, std::int64_t every,
                                    std::int64_t period,
                                    std::span<const dftu_group_agg> aggs,
                                    std::int64_t origin,
                                    bool origin_min) const {
        return apply("dftu.lazy.group_by_dynamic",
                     dataframe::OpArgs()
                         .str(1, time_col ? time_col : "")
                         .i64(2, every)
                         .i64(3, period)
                         .agglist(4, aggs)
                         .i64(5, origin)
                         .i32(6, static_cast<std::int32_t>(origin_min)));
    }

   private:
    /// Runs the registered lazy op `name` on the current handle, consuming it
    /// either way. Empty (`!handle`) on a missing ops extension, an
    /// unregistered name, or a kind/arity/shape/argument rejection.
    // Borrows, like every layer beneath it: dftu_lazyframe_* takes a const
    // handle and LazyFrame's own methods are const, both returning a new plan
    // and leaving the input usable. Consuming here would let
    // `auto a = lf.head(1); auto b = lf.tail(1);` silently hand back an empty
    // `b`, with no error and nothing to see at the call site.
    OwnedLazyFrame apply(const char* name, const dftu_op_arg* args) const {
        const dftu_lazyframe* src = handle;
        return apply(name, &src, 1, args);
    }
    /// The multi-input form: `in[0]` is this handle, the rest are borrowed
    /// operand plans; empty if any of them is empty.
    OwnedLazyFrame apply(const char* name, const dftu_lazyframe* const* in,
                         std::uint32_t n, const dftu_op_arg* args) const {
        if (!handle) return OwnedLazyFrame{};
        for (std::uint32_t i = 0; i < n; ++i)
            if (!in[i]) return OwnedLazyFrame{};
        if (!ops_ || !ops_->run_lazy) return OwnedLazyFrame{};
        dftu_result_lazyframe res = ops_->run_lazy(host_, name, in, n, args);
        if (!DFTU_RESULT_OK(res)) return OwnedLazyFrame{};
        return OwnedLazyFrame{DFTU_RESULT_VALUE(res), ops_, host_};
    }

    const dftu_svc_ops* ops_ = nullptr;
    void* host_ = nullptr;
};

using NamedResult = std::variant<std::vector<std::byte>, OwnedArrow,
                                 OwnedDataFrame, OwnedLazyFrame>;

class NamedResultRegistry {
   public:
    NamedResultRegistry() = default;

    /// Movable so a run can hand its results back by value. Only safe once the
    /// scan that writes into it has finished; the mutex is not carried over.
    NamedResultRegistry(NamedResultRegistry&& other) noexcept
        : results_(std::move(other.results_)) {}
    NamedResultRegistry& operator=(NamedResultRegistry&& other) noexcept {
        if (this != &other) results_ = std::move(other.results_);
        return *this;
    }

    void emit_blob(const char* name, const void* data, std::uint64_t len) {
        if (!name) return;
        std::vector<std::byte> bytes(static_cast<std::size_t>(len));
        if (len && data)
            std::memcpy(bytes.data(), data, static_cast<std::size_t>(len));
        std::lock_guard<std::mutex> lock(mutex_);
        results_[name] = std::move(bytes);
    }

    /// Steals *a and *s (zeroing them). -1 on a null argument.
    int emit_arrow(const char* name, ArrowArray* a, ArrowSchema* s) {
        if (!name || !a || !s) return -1;
        OwnedArrow owned;
        owned.array = *a;
        owned.schema = *s;
        *a = ArrowArray{};
        *s = ArrowSchema{};
        std::lock_guard<std::mutex> lock(mutex_);
        results_[name] = std::move(owned);
        return 0;
    }

    /// Take ownership of a native dftu_dataframe result. A null handle is a
    /// no-op.
    void emit_frame(const char* name, dftu_dataframe* handle) {
        if (!name || !handle) return;
        std::lock_guard<std::mutex> lock(mutex_);
        results_[name] = OwnedDataFrame{handle};
    }

    /// Take ownership of a native dftu_lazyframe (deferred) result; the host
    /// collects it at the Python edge. The plan must be self-contained (see
    /// OwnedLazyFrame). A null handle is a no-op.
    void emit_lazyframe(const char* name, dftu_lazyframe* handle) {
        if (!name || !handle) return;
        std::lock_guard<std::mutex> lock(mutex_);
        results_[name] = OwnedLazyFrame{handle};
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.clear();
    }

    /// Drained after the scan completes, so no lock is needed by the reader.
    std::unordered_map<std::string, NamedResult>& results() { return results_; }

   private:
    std::mutex mutex_;
    std::unordered_map<std::string, NamedResult> results_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_RESULT_REGISTRY_H
