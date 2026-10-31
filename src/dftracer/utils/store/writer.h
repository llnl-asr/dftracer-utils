#ifndef DFTRACER_UTILS_STORE_WRITER_H
#define DFTRACER_UTILS_STORE_WRITER_H

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
#include <dftracer/utils/runtime/coro/task.h>
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
#include <memory>
#include <mutex>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::store {

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

class Writer;

/// The entries of one commit. One batch is atomic: it becomes visible whole or
/// not at all. Entries are copied into the batch, so the caller's buffers may
/// go away.
class Batch {
   public:
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) noexcept = default;
    Batch& operator=(Batch&&) noexcept = default;
    ~Batch() = default;

    /// InvalidArgument on a fold namespace, reported by commit.
    void put(dstd::ByteView key, dstd::ByteView value);
    /// put() without a copy: the bytes of `key` and `value` must stay valid
    /// and unchanged until the commit of this batch returns.
    void put_ref(dstd::ByteView key, dstd::ByteView value);
    /// InvalidArgument on a non-fold namespace, and when a numeric fold gets a
    /// value that is not 8 bytes. Reported by commit.
    void fold(dstd::ByteView key, dstd::ByteView value);

   private:
    friend class Writer;

    struct Entry {
        dstd::ByteView key;
        dstd::ByteView value;
    };

    Batch() = default;

    /// `bytes` copied into the batch's arena, which never moves them.
    dstd::ByteView keep(dstd::ByteView bytes);
    void add(dstd::ByteView key, dstd::ByteView value);
    void record_error(dstd::Condition condition);

    const Writer* writer_ = nullptr;
    NamespaceId namespace_id_;
    ScopeKey scope_;
    std::uint64_t params_hash_ = 0;
    dstd::Vector<SourceStat> sources_;
    FoldKind fold_ = FoldKind::None;
    dstd::Optional<dstd::Error> error_;
    bool building_ = false;
    dstd::Vector<Entry> entries_;
    /// Opened by the first put() that copies. put_ref() needs none.
    dstd::UniquePtr<dstd::StringArena> arena_;
};

/// One data file of one process. Entries become visible at the group commit
/// that records their extent. add_namespace, batch, commit, fail, drop and
/// erase are safe from several threads (commit shares one lock with them).
/// flush() and close() are coroutines of the writer's executor.
class Writer {
   public:
    /// InvalidArgument when the executor has no I/O backend, and when
    /// `options.extent_size` is set but out of range.
    static dstd::Expected<std::unique_ptr<Writer>, dstd::Error> create(
        dstd::StringView dir, Executor& executor,
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
    /// Makes the building copy of `scope` the copy readers see, with
    /// `params_hash` and `sources` (one per path, or none). Call flush() first,
    /// so the record that publishes it never comes before one of its segments.
    /// InvalidArgument when the scope has no building copy.
    dstd::Expected<void, dstd::Error> publish(
        NamespaceId namespace_id, const ScopeKey& scope,
        std::uint64_t params_hash, const dstd::Vector<SourceStat>* sources);

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
    /// a batch committed after it shows its keys again. Without a current copy
    /// of the scope in this writer the call does nothing.
    dstd::Expected<void, dstd::Error> erase(NamespaceId namespace_id,
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
           Executor& executor, const WriterOptions& options);

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

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_WRITER_H
