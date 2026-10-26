#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H

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
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/committer.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/open_page.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/writer_pages.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::index::store::engine {

/// The store library's error domain.
inline constexpr dstd::ErrorDomain ERROR_DOMAIN =
    dstd::make_error_domain("dftracer.utils.index.store");

/// Store error codes; portable conditions come from error_info.
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
    /// The page size of the data file. Unset means the page size of `dir`, from
    /// page_size_for().
    dstd::Optional<std::uint32_t> page_size;
    /// The most page bytes the writer holds while it holds any: the bytes
    /// appended to a page and not yet written. Unset means max(16, 4 * the
    /// number of executor threads) pages. Zero is InvalidArgument.
    dstd::Optional<std::uint64_t> page_quota_bytes;
    /// Pending page bytes at which a group commit starts without waiting for
    /// commit_interval.
    std::uint64_t commit_bytes = std::uint64_t{64} << 20;
    /// The longest a queued page or copy state change waits for its record.
    std::chrono::milliseconds commit_interval{100};
    dstd::String host;
    dstd::String label;
};

/// A namespace inside one writer. Names are unique per writer.
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
    void put(ByteView key, ByteView value);
    /// InvalidArgument on a non-fold namespace, and when a numeric fold gets a
    /// value that is not 8 bytes. Reported by commit.
    void fold(ByteView key, ByteView value);

   private:
    friend class Writer;

    struct Entry {
        std::size_t key_offset = 0;
        std::size_t key_length = 0;
        std::size_t value_offset = 0;
        std::size_t value_length = 0;
    };

    Batch() = default;

    ByteView view_at(std::size_t offset, std::size_t length) const;
    void add(ByteView key, ByteView value);
    void record_error(dstd::Condition condition);

    const Writer* writer_ = nullptr;
    NamespaceId namespace_id_;
    ScopeKey scope_;
    std::uint64_t params_hash_ = 0;
    dstd::Vector<ScopePath> signature_;
    FoldKind fold_ = FoldKind::None;
    dstd::Optional<dstd::Error> error_;
    dstd::Vector<Entry> entries_;
    dstd::Vector<std::uint8_t> arena_;
};

/// One data file of one process. Entries become visible at the group commit
/// that records their page.
///
/// add_namespace, batch, commit, fail and drop are safe from several threads at
/// once; commit takes one lock of this writer, and that is the only one it
/// shares with the others. A batch is sorted and sealed into one section and
/// appended to its namespace's open page, so a page of the file holds the
/// sections of several batches. flush() and close() are coroutines of the
/// executor the writer was created with, and close() must not run while another
/// thread is still calling the writer.
class Writer {
   public:
    /// InvalidArgument when the executor has no I/O backend, and when
    /// `options.page_size` is set but out of range.
    static dstd::Expected<std::unique_ptr<Writer>, dstd::Error> create(
        dstd::StringView dir, Executor& executor,
        const WriterOptions& options = WriterOptions{});

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) = delete;
    Writer& operator=(Writer&&) = delete;
    /// Without a preceding close(), seals the open pages and blocks until their
    /// writes completed, so no completion outlives this object. Blocking means
    /// the destructor must not run on a thread of the writer's executor.
    ~Writer();

    dstd::Expected<NamespaceId, dstd::Error> add_namespace(
        const NamespaceSpec& spec);

    /// `scope_paths`, when given, holds one signature per path of `scope`; its
    /// mtime, size and hash are stored with the scope. Without it they are
    /// zero.
    Batch batch(NamespaceId namespace_id, const ScopeKey& scope,
                std::uint64_t params_hash,
                const dstd::Vector<ScopePath>* scope_paths = nullptr);

    /// InvalidArgument on a closed writer, an unknown namespace, an invalid
    /// batch, or a closed pool; the committer's error once it failed; and
    /// WriterErrc::Busy while the page bytes of earlier commits are not
    /// written. A Busy commit assigns no scope, copy or generation and leaves
    /// `batch` whole, so the caller retries it, waiting on space():
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
                                           ByteView begin, ByteView end);

    /// Covers every batch whose commit returned before this call: seals the
    /// open pages, waits for their writes and runs one group commit, syncing
    /// when any of those batches is durable.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> flush();
    /// flush(), then stops the committer task, then closes the data file.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> close();

    /// Waits until a write that started after this call completed, the writer
    /// holds no page bytes, or it latched an error: what a retry after a Busy
    /// commit waits for. It reports nothing; the retry reports the outcome.
    /// Seals the open pages first, so the bytes it waits for are in flight.
    /// Runs on a thread of the writer's executor.
    coro::CoroTask<void> space();

    /// The file name and the full path of the data file this writer owns.
    const dstd::String& name() const { return name_; }
    const dstd::String& path() const { return path_; }
    std::uint32_t page_size() const { return page_size_; }

   private:
    struct NamespaceEntry {
        NamespaceId id;
        NamespaceSpec spec;
    };

    /// A scope id. The scope itself is the key of scopes_.
    struct ScopeEntry {
        std::uint32_t id = 0;
    };

    /// The state of one copy of one scope. The namespace id and the scope id
    /// are the key of copies_.
    struct CopyEntry {
        std::uint32_t copy_id = 0;
        std::uint64_t generation = 0;
        std::uint64_t highest_generation = 0;
        bool live = false;
    };

    /// Hashes a scope by kind and paths, so a lookup over any number of scopes
    /// costs the same.
    struct ScopeKeyHash {
        using is_avalanching = void;

        std::size_t operator()(const ScopeKey& scope) const noexcept {
            std::size_t seed = static_cast<std::uint8_t>(scope.kind);
            for (const dstd::String& path : scope.paths) {
                seed = static_cast<std::size_t>(
                    hash::splitmix64(seed ^ dstd::StringHash{}(path)));
            }
            return seed;
        }
    };

    /// The key of copies_: the namespace id high, the scope id low.
    static std::uint64_t copy_key(std::uint32_t namespace_id,
                                  std::uint32_t scope_id) {
        return (static_cast<std::uint64_t>(namespace_id) << 32) |
               static_cast<std::uint64_t>(scope_id);
    }

    struct SectionBytes {
        dstd::Vector<std::uint8_t> bytes;
        std::uint64_t entry_count = 0;
        dstd::Vector<std::uint8_t> first_key;
        dstd::Vector<std::uint8_t> last_key;
        dstd::Vector<PageRef> big_runs;
    };

    Writer(dstd::String name, dstd::String path, std::uint32_t page_size,
           dstd::Vector<ScopeGeneration> generations, DataFileWriter file,
           memory::PoolHandle page_pool, std::uint64_t page_quota,
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
    /// and ids of `pending`; takes the writer lock.
    dstd::Expected<void, dstd::Error> prepare(const Batch& batch,
                                              PendingBatch& pending);
    /// Sizes the section, charges its exact length to the page count and only
    /// then seals it, so a Busy result (WriterErrc::Busy) comes before any big
    /// value is written. On every other failure the charge is given back.
    dstd::Expected<SectionBytes, dstd::Error> build_section(const Batch& batch);
    /// Seals the open pages, so the bytes a Busy commit waits for are in
    /// flight, then reports WriterErrc::Busy, or the seal error instead.
    dstd::Unexpected<dstd::Error> busy_error();
    void latch(const dstd::Error& error);
    dstd::Optional<dstd::Error> latched() const;

    friend struct WriterTestHook;

    /// The placer context of one section seal: the big value runs it wrote
    /// are collected for the commit record.
    struct BigPlacement {
        Writer* writer = nullptr;
        dstd::Vector<PageRef>* runs = nullptr;
    };
    static BigRef place_big(void* context, ByteView value);
    /// The WrittenPageFn of the page writer: releases the bytes the page held
    /// and hands it to the committer. Runs on an I/O backend thread.
    static void wrote_page(void* context, WrittenPage&& page);
    /// The IdleFn of the page writer: wakes back-pressure waiters once no page
    /// write is in flight, so one of them seals the open pages.
    static void pages_idle(void* context);

    DataFileWriter file_;
    Committer committer_;
    /// The pool the page bytes are charged to. Declared before the pieces that
    /// reserve from it and the completions that release them.
    memory::PoolHandle page_pool_;
    WriterPages page_bytes_;
    // Declared after the committer, so a page completion can never outlive it.
    PageWriter pages_;
    dstd::String name_;
    dstd::String path_;
    std::uint32_t page_size_ = 0;
    std::uint32_t next_namespace_id_ = 1;
    std::uint32_t next_scope_id_ = 1;
    std::uint32_t next_copy_id_ = 1;
    mutable std::mutex mutex_;
    dstd::Vector<NamespaceEntry> namespaces_;
    /// A commit looks up its scope and its copy, so a hash answers in place of
    /// a search that grows with the count, and an add is a hash insert in place
    /// of a middle insert that moves every entry after it. The entries sit
    /// behind UniquePtr, so a rehash that moves the table leaves a pointer a
    /// caller holds valid.
    dstd::HashMap<ScopeKey, dstd::UniquePtr<ScopeEntry>, ScopeKeyHash> scopes_;
    dstd::HashMap<std::uint64_t, dstd::UniquePtr<CopyEntry>> copies_;
    dstd::Vector<ScopeGeneration> generations_;
    dstd::Optional<dstd::Error> latched_error_;
    std::atomic<bool> closed_{false};
};

/// Test-only access to the parts of a writer.
struct WriterTestHook {
    /// The data file the writer appends pages and records to.
    static DataFileWriter& file(Writer& writer);
    /// The page bytes the writer holds, the most it held, and its counts.
    static WriterPagesCounters pages(const Writer& writer);

    using SpaceSealedFn = void (*)(Writer& writer);
    /// Runs inside every space() after it sealed the open pages and before it
    /// parks; nullptr turns it off. Process-wide.
    static void set_space_sealed(SpaceSealedFn fn);
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H
