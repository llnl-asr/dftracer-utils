// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_WRITER_H
#define DFTRACER_UTILS_STORE_WRITER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/store/committer.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/open_extent.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer_extents.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils {

class TaskExecutor;

}  // namespace dftracer::utils

namespace dftracer::utils::store::engine {

inline constexpr dstd::ErrorDomain ERROR_DOMAIN =
    dstd::make_error_domain("dftracer.utils.index.store");

/// Store error codes. Portable conditions come from error_info.
enum class WriterErrc : std::int32_t {
    Busy,  ///< the bytes of earlier commits are not written yet; retry
};

inline constexpr dstd::ErrorCodeInfo WRITER_ERROR_INFOS[] = {
    {&ERROR_DOMAIN, static_cast<std::int32_t>(WriterErrc::Busy),
     dstd::Condition::Unknown, "BUSY"},
};

constexpr const dstd::ErrorCodeInfo& error_info(WriterErrc code) noexcept {
    const auto at = static_cast<std::size_t>(code);
    return at < std::size(WRITER_ERROR_INFOS) ? WRITER_ERROR_INFOS[at]
                                              : WRITER_ERROR_INFOS[0];
}

struct WriterOptions {
    /// The extent size of the data file. Unset means the extent size of `dir`,
    /// from extent_size_for().
    dstd::Optional<std::uint32_t> extent_size;
    /// The most extent bytes the writer holds while it holds any: the bytes
    /// appended to an extent and not yet written. Unset means max(16, 4 * the
    /// number of executor threads) extents. Zero is InvalidArgument.
    dstd::Optional<std::uint64_t> extent_quota_bytes;
    /// Pending segment bytes, raw, at which a group commit starts without
    /// waiting for commit_interval.
    std::uint64_t commit_bytes = std::uint64_t{64} << 20;
    /// The longest a queued extent or copy state change waits for its record.
    std::chrono::milliseconds commit_interval{100};
    dstd::String host;
    dstd::String label;
};

struct NamespaceId {
    std::uint32_t value = 0;

    friend bool operator==(NamespaceId a, NamespaceId b) {
        return a.value == b.value;
    }
};

enum class ChangeKind : std::uint8_t { Ready, Failed, Dropped };

/// One copy state change for Writer::apply. Ready publishes the building copy
/// of `scope` with `params_hash` and `sources`. Failed records a Failed copy
/// with `params_hash`. Dropped hides the whole scope. Only Ready reads
/// `sources`.
struct CopyChange {
    NamespaceId namespace_id;
    const ScopeKey* scope = nullptr;
    std::uint64_t params_hash = 0;
    const dstd::Vector<SourceStat>* sources = nullptr;
    ChangeKind kind = ChangeKind::Ready;
};

class Writer;

/// One data file of one process. Entries become visible at the group commit
/// that records their extent. add_namespace, batch, commit, fail, drop and
/// erase are safe from several threads (commit shares one lock with them).
/// flush() and close() are coroutines of the writer's executor.
class Writer {
   public:
    /// InvalidArgument when the executor has no I/O backend, and when
    /// `options.extent_size` is set but out of range.
    static dstd::Expected<dstd::UniquePtr<Writer>, dstd::Error> create(
        dstd::StringView dir, TaskExecutor& executor,
        const WriterOptions& options = WriterOptions{});

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    /// Without a preceding close(), seals the open extents and blocks until
    /// their writes completed, so no completion outlives this object. Blocking
    /// means the destructor must not run on a thread of the writer's executor.
    ~Writer();

    dstd::Expected<NamespaceId, dstd::Error> add_namespace(
        const NamespaceSpec& spec);

    /// `sources`, when given, holds one signature per path of `scope`, in its
    /// path order. It is stored with the copy this batch is the first to
    /// announce. Without it the copy carries none.
    Batch batch(NamespaceId namespace_id, const ScopeKey& scope,
                std::uint64_t params_hash,
                const dstd::Vector<SourceStat>* sources = nullptr);
    /// A batch of a building copy of `scope`: the first one starts a new copy
    /// under the next generation, which no reader sees, and later ones add to
    /// it, until publish() makes it the copy readers see. A plain batch of a
    /// scope whose copy is building is InvalidArgument at commit.
    Batch building_batch(NamespaceId namespace_id, const ScopeKey& scope);
    /// Applies every change in one record, so a reader sees all of them or
    /// none. A Ready change makes the building copy of its scope the copy
    /// readers see, with its `params_hash` and `sources` (one per path, or
    /// none). Call flush() first, so that record never comes before one of
    /// their segments. InvalidArgument, and nothing applied, when a change has
    /// a null scope, an unknown namespace or wrong sources, when a Ready change
    /// has no building copy, or when two changes name one copy.
    dstd::Expected<void, dstd::Error> apply(
        dstd::Span<const CopyChange> changes);

    /// InvalidArgument on a closed writer, an unknown namespace, an invalid
    /// batch, or a copy with other params hash or sources than its first. The
    /// committer's error once it failed. Busy leaves `batch` whole, so retry:
    /// `while (commit(std::move(b)) is Busy) co_await space();`
    dstd::Expected<void, dstd::Error> commit(Batch&& batch);
    /// A copy in state Failed with no entries, under the next generation.
    dstd::Expected<void, dstd::Error> fail(NamespaceId namespace_id,
                                           const ScopeKey& scope,
                                           std::uint64_t params_hash);
    /// Hides every entry of the scope, including later copies with a lower
    /// generation. Ends the writer's current copy of that scope.
    dstd::Expected<void, dstd::Error> drop(NamespaceId namespace_id,
                                           const ScopeKey& scope);
    /// Hides the keys of [begin, end) of the current copy, with an empty
    /// `end` meaning to the last key. Both empty is a whole-scope drop.
    dstd::Expected<void, dstd::Error> drop(NamespaceId namespace_id,
                                           const ScopeKey& scope,
                                           dstd::ByteView begin,
                                           dstd::ByteView end);
    /// Hides the keys of [begin, end) that the current copy holds from earlier
    /// batches (an empty `end` means the last key). The copy stays current and
    /// a batch committed after it shows its keys again. False, with nothing
    /// queued, when this writer holds no current copy of the scope.
    dstd::Expected<bool, dstd::Error> erase(NamespaceId namespace_id,
                                            const ScopeKey& scope,
                                            dstd::ByteView begin,
                                            dstd::ByteView end);

    /// Covers every batch whose commit returned before this call: seals the
    /// open extents, waits for their writes and runs one group commit, syncing
    /// when any of those batches is durable.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> flush();
    /// flush(), then stops the committer task, then closes the data file.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> close();

    /// Waits until a write that started after this call completed, the writer
    /// holds no extent bytes, or it latched an error. A retry after a Busy
    /// commit waits on it and reports the outcome itself. Seals the open
    /// extents first. Runs on a thread of the writer's executor.
    coro::CoroTask<void> space();

    /// The group commit records this writer has written. It grows when a
    /// commit, flush or the commit interval makes entries readable.
    std::uint64_t committed_records() const noexcept {
        return committer_.records();
    }

    const dstd::String& name() const { return name_; }
    const dstd::String& path() const { return path_; }
    std::uint32_t extent_size() const { return extent_size_; }

   private:
    struct NamespaceEntry {
        NamespaceId id;
        NamespaceSpec spec;
    };

    struct ScopeEntry {
        std::uint32_t id = 0;
    };

    struct CopyEntry {
        std::uint64_t generation = 0;
        std::uint64_t highest_generation = 0;
        std::uint64_t params_hash = 0;
        dstd::Vector<SourceStat> sources;
        std::uint32_t copy_id = 0;
        bool live = false;
        bool announced = false;
        bool building = false;
    };

    struct ScopeKeyHash {
        using is_avalanching = void;

        std::size_t operator()(const ScopeKey& scope) const noexcept {
            std::size_t seed = static_cast<std::uint8_t>(scope.kind);
            for (const dstd::String& path : scope.paths) {
                seed = static_cast<std::size_t>(
                    dstd::hash::splitmix64(seed ^ dstd::StringHash{}(path)));
            }
            return seed;
        }
    };

    static std::uint64_t copy_key(std::uint32_t namespace_id,
                                  std::uint32_t scope_id) {
        return (static_cast<std::uint64_t>(namespace_id) << 32) |
               static_cast<std::uint64_t>(scope_id);
    }

    struct SegmentBytes {
        SegmentBuilder builder;
        std::size_t length = 0;
        std::uint64_t entry_count = 0;
        dstd::Vector<std::uint8_t> first_key;
        dstd::Vector<std::uint8_t> last_key;
        dstd::Vector<ExtentRef> big_runs;
    };

    Writer(dstd::String name, dstd::String path, std::uint32_t extent_size,
           dstd::Vector<ScopeGeneration> generations, DataFileWriter file,
           memory::PoolHandle extent_pool, std::uint64_t extent_quota,
           TaskExecutor& executor, const WriterOptions& options);

    template <class T, class... Args>
    friend dstd::UniquePtr<T> dstd::make_unique(Args&&... args);

    NamespaceEntry* find_namespace(std::uint32_t id);
    NamespaceEntry* find_namespace(dstd::StringView name);
    ScopeEntry* find_scope(const ScopeKey& scope);
    ScopeEntry* add_scope(const ScopeKey& scope);
    CopyEntry* find_copy(std::uint32_t namespace_id, std::uint32_t scope_id);
    CopyEntry* add_copy(NamespaceId id, std::uint32_t scope_id,
                        std::uint64_t generation, bool live);
    CopyEntry* find_or_add_copy(const NamespaceSpec& spec, NamespaceId id,
                                const ScopeKey& scope, std::uint32_t scope_id);
    std::uint64_t generation_of(const NamespaceSpec& spec,
                                const ScopeKey& scope) const;

    /// InvalidArgument when the namespace is unknown. Fills the announce items
    /// and ids of `pending`. Takes the writer lock.
    dstd::Expected<void, dstd::Error> prepare(const Batch& batch,
                                              PendingBatch& pending);
    /// Sizes the segment, charges its exact length to the extent count and only
    /// then seals it, so a Busy result (WriterErrc::Busy) comes before any big
    /// value is written. On every other failure the charge is given back.
    dstd::Expected<SegmentBytes, dstd::Error> build_segment(const Batch& batch);
    /// Seals the open extents, so the bytes a Busy commit waits for are in
    /// flight, then reports WriterErrc::Busy, or the seal error instead.
    dstd::Unexpected<dstd::Error> busy_error();
    void latch(const dstd::Error& error);
    dstd::Optional<dstd::Error> latched() const;

    /// The three below create the PendingBatch for one change and update the
    /// copy bookkeeping. They hold the writer lock and cannot fail.
    PendingBatch* publish_locked(const NamespaceEntry& namespace_entry,
                                 const ScopeEntry& scope_entry,
                                 CopyEntry& copy_entry, const ScopeKey& scope,
                                 const CopyChange& change);
    PendingBatch* fail_locked(const NamespaceEntry& namespace_entry,
                              const ScopeEntry& scope_entry,
                              const ScopeKey& scope, std::uint64_t params_hash);
    PendingBatch* drop_locked(const NamespaceEntry& namespace_entry,
                              const ScopeEntry& scope_entry,
                              const ScopeKey& scope);

    friend struct WriterTestHook;

    struct BigPlacement {
        Writer* writer = nullptr;
        dstd::Vector<ExtentRef>* runs = nullptr;
    };
    static BigRef place_big(void* context, dstd::ByteView value);
    /// Runs on an I/O backend thread.
    static void wrote_extent(void* context, WrittenExtent&& extent);
    static void extents_idle(void* context);

    DataFileWriter file_;
    Committer committer_;
    /// The pool the extent bytes are charged to. Declared before the pieces
    /// that reserve from it and the completions that release them.
    memory::PoolHandle extent_pool_;
    WriterExtents extent_bytes_;
    // Declared after the committer, so an extent completion can never outlive
    // it.
    ExtentWriter extents_;
    dstd::String name_;
    dstd::String path_;
    std::uint32_t extent_size_ = 0;
    std::uint32_t next_namespace_id_ = 1;
    std::uint32_t next_scope_id_ = 1;
    std::uint32_t next_copy_id_ = 1;
    mutable std::mutex mutex_;
    dstd::Vector<NamespaceEntry> namespaces_;
    /// Behind UniquePtr, so a rehash leaves a pointer a caller holds valid.
    dstd::HashMap<ScopeKey, dstd::UniquePtr<ScopeEntry>, ScopeKeyHash> scopes_;
    dstd::HashMap<std::uint64_t, dstd::UniquePtr<CopyEntry>> copies_;
    dstd::Vector<ScopeGeneration> generations_;
    /// The batch ordinal of the next commit, so a reader orders the versions of
    /// a key by commit order and not by record order.
    std::atomic<std::uint64_t> next_ordinal_{1};
    dstd::Optional<dstd::Error> latched_error_;
    std::atomic<bool> closed_{false};
};

struct WriterTestHook {
    static DataFileWriter& file(Writer& writer);
    static WriterExtentsCounters extents(const Writer& writer);

    using SpaceSealedFn = void (*)(Writer& writer);
    /// Runs inside every space() after it sealed the open extents and before it
    /// parks. nullptr turns it off. Process-wide.
    static void set_space_sealed(SpaceSealedFn fn);
};

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_WRITER_H
