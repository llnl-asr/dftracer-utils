#ifndef DFTRACER_UTILS_STORE_STORE_H
#define DFTRACER_UTILS_STORE_STORE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::store {

namespace engine {

class Snapshot;
class ScanCursor;
class Writer;

}  // namespace engine

/// NamespaceItem::fold_kind stores these numbers, so they are append only.
enum class FoldKind : std::uint8_t {
    None = 0,
    SumI64 = 1,
    SumF64 = 2,
    MinI64 = 3,
    MaxI64 = 4,
    MinF64 = 5,
    MaxF64 = 6,
    Count = 7,
    Last = 8,
    Concat = 9
};

/// ZstdDict is a zstd frame made with the dictionary the extent names.
enum class PageCodec : std::uint8_t { None = 0, Zstd = 1, ZstdDict = 2 };

enum class ScopeKind : std::uint8_t { Root = 0, File = 1, Files = 2 };

enum class ScopeState : std::uint8_t {
    Absent = 0,
    Ready = 1,
    Failed = 2,
    Bad = 3
};

/// The signature of one trace file of a scope, so a later check can tell
/// whether a stored copy still matches the trace files it was read from.
struct SourceStat {
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
};

/// Who owns the data, and therefore which copies of a scope a reader sees.
struct NamespaceSpec {
    dstd::String name;
    std::uint32_t format_version = 0;
    FoldKind fold = FoldKind::None;
    bool durable = false;
    /// Writers only. Reads and namespace matching ignore it.
    PageCodec codec = PageCodec::Zstd;
};

bool operator==(const NamespaceSpec& a, const NamespaceSpec& b);
bool operator!=(const NamespaceSpec& a, const NamespaceSpec& b);

/// Matched across files by kind and paths, since each file numbers scopes
/// itself.
struct ScopeKey {
    ScopeKind kind = ScopeKind::Root;
    dstd::Vector<dstd::String> paths;

    static ScopeKey root();
    static ScopeKey file(dstd::StringView path);
    /// Sorts and removes duplicate paths.
    static ScopeKey files(dstd::Span<const dstd::StringView> paths);
};

/// Kind first, then the paths in order. This is also the scan_all order.
int compare_scopes(const ScopeKey& a, const ScopeKey& b);
bool operator==(const ScopeKey& a, const ScopeKey& b);
bool operator!=(const ScopeKey& a, const ScopeKey& b);
bool operator<(const ScopeKey& a, const ScopeKey& b);

/// The current copy of one scope, read from the scope directories alone.
struct CopyInfo {
    ScopeState state = ScopeState::Absent;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    /// Empty, or one per path of the scope in its path order.
    dstd::Vector<SourceStat> sources;
};

struct ScopeEntry {
    ScopeKey scope;
    CopyInfo copy;
};

struct NamespaceDecl {
    dstd::String name;
    std::uint32_t format_version = 0;
};

/// A namespace this process registered with Store::ns.
struct Namespace {
    NamespaceSpec spec;
    std::uint32_t id = 0;
};

class Store;

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
    /// put() without a copy. The bytes of `key` and `value` must stay valid
    /// and unchanged until the commit of this batch returns.
    void put_ref(dstd::ByteView key, dstd::ByteView value);
    /// InvalidArgument on a non-fold namespace, and when a numeric fold gets a
    /// value that is not 8 bytes. Reported by commit.
    void fold(dstd::ByteView key, dstd::ByteView value);

   private:
    friend class engine::Writer;
    friend class Store;

    struct Entry {
        dstd::ByteView key;
        dstd::ByteView value;
    };

    Batch() = default;

    /// `bytes` copied into the batch's arena, which never moves them.
    dstd::ByteView keep(dstd::ByteView bytes);
    void add(dstd::ByteView key, dstd::ByteView value);
    void record_error(dstd::Condition condition);

    const engine::Writer* writer_ = nullptr;
    std::uint32_t namespace_id_ = 0;
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

/// A value the snapshot returned, in a buffer it owns. It is move-only. A copy
/// would leave the view pointing at the first value's buffer.
class Value {
   public:
    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;
    ~Value() = default;

    dstd::ByteView bytes() const { return view_; }

   private:
    friend class engine::Snapshot;

    dstd::ByteView view_;
    dstd::Vector<std::uint8_t> owned_;
};

/// A scan over one snapshot. key() and value() stay valid until next().
class Cursor {
   public:
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    Cursor(Cursor&&) noexcept;
    Cursor& operator=(Cursor&&) noexcept;
    ~Cursor();

    bool at_end() const;
    dstd::ByteView key() const;
    const ScopeKey& scope() const;
    dstd::Expected<dstd::ByteView, dstd::Error> value();
    void next();
    /// Set when a block could not be read. A cursor that stopped on one is at
    /// its end.
    const dstd::Optional<dstd::Error>& error() const;

   private:
    friend class Snapshot;

    Cursor() = default;

    /// Declared first, so the cursor is gone before the snapshot it reads.
    dstd::SharedPtr<const engine::Snapshot> owner_;
    dstd::UniquePtr<engine::ScanCursor> impl_;
};

/// A point-in-time view of every data file of a store. Cheap to copy. Values
/// and cursors read from it keep it alive.
class Snapshot {
   public:
    CopyInfo copy_info(const Namespace& ns, const ScopeKey& scope) const;
    dstd::Expected<dstd::Optional<Value>, dstd::Error> get(
        const Namespace& ns, const ScopeKey& scope, dstd::ByteView key) const;
    /// One result per key, in the order of `keys`.
    dstd::Expected<dstd::Vector<dstd::Optional<Value>>, dstd::Error> multi_get(
        const Namespace& ns, const ScopeKey& scope,
        dstd::Span<const dstd::ByteView> keys) const;
    Cursor scan(const Namespace& ns, const ScopeKey& scope,
                dstd::ByteView prefix) const;
    Cursor scan(const Namespace& ns, const ScopeKey& scope,
                dstd::ByteView begin, dstd::ByteView end) const;
    Cursor scan(const Namespace& ns, dstd::Span<const ScopeKey> scopes,
                dstd::ByteView begin, dstd::ByteView end) const;
    Cursor scan_all(const Namespace& ns, dstd::ByteView begin,
                    dstd::ByteView end) const;
    dstd::Expected<dstd::Vector<ScopeEntry>, dstd::Error> scopes(
        const Namespace& ns) const;
    dstd::Expected<dstd::Vector<NamespaceDecl>, dstd::Error> namespaces() const;

   private:
    friend class Store;

    Cursor wrap(engine::ScanCursor&& scan) const;

    dstd::SharedPtr<const engine::Snapshot> impl_;
};

/// One copy state change for Store::publish. Ready makes the building copy of
/// `scope` the copy readers see. Failed records a Failed copy with
/// `params_hash` and needs no building copy. Dropped hides the whole scope.
struct Publish {
    enum class Change : std::uint8_t { Ready, Failed, Dropped };

    const Namespace* ns = nullptr;
    const ScopeKey* scope = nullptr;
    std::uint64_t params_hash = 0;
    /// Ready only. Null, or one per path of the scope in its path order.
    const dstd::Vector<SourceStat>* sources = nullptr;
    Change change = Change::Ready;
};

/// One store directory, shared by every handle to it in this process, with one
/// writer that opens on the first write. Thread safe.
class Store {
   public:
    /// Creates the directory when it does not exist.
    static dstd::Expected<Store, dstd::Error> open(dstd::StringView dir);

    /// Registers the namespace with this process's writer. The same spec
    /// twice returns the same handle.
    dstd::Expected<Namespace, dstd::Error> ns(const NamespaceSpec& spec);

    /// A batch belongs to the writer that made it. Commit rejects it once that
    /// writer closed, with InvalidArgument.
    Batch batch(const Namespace& ns, const ScopeKey& scope,
                std::uint64_t params_hash = 0,
                const dstd::Vector<SourceStat>* sources = nullptr);
    /// A batch of a new copy of `scope` that no reader sees until publish().
    /// Like batch(), it belongs to the writer that made it.
    Batch building_batch(const Namespace& ns, const ScopeKey& scope);

    /// Waits for writer space when the writer is busy. Readers see the batch
    /// after flush(), publish() or close(), or once its extent fills.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> commit(Batch batch);
    /// Flushes, then applies every change of `targets` in one record: a reader
    /// sees all of them or none. A bad target changes nothing.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> publish(
        dstd::Span<const Publish> targets);

    dstd::Expected<void, dstd::Error> fail(const Namespace& ns,
                                           const ScopeKey& scope,
                                           std::uint64_t params_hash);
    dstd::Expected<void, dstd::Error> drop(const Namespace& ns,
                                           const ScopeKey& scope);
    dstd::Expected<void, dstd::Error> erase(const Namespace& ns,
                                            const ScopeKey& scope,
                                            dstd::ByteView begin,
                                            dstd::ByteView end);

    coro::CoroTask<dstd::Expected<void, dstd::Error>> flush();
    /// Flushes and closes this process's writer. A later write opens a new one.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> close();

    /// The newest view: reopened after a commit of this process and when
    /// another process added data files.
    dstd::Expected<Snapshot, dstd::Error> snapshot();

    /// Closes the writer of every store this process holds. For a CLI exit,
    /// while the runtime still runs. Returns the first error.
    static dstd::Expected<void, dstd::Error> close_all();

   private:
    struct State;

    explicit Store(dstd::SharedPtr<State> state) : state_(std::move(state)) {}

    dstd::SharedPtr<State> state_;
};

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_STORE_H
