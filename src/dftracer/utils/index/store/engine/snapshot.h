#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/record.h>
#include <dftracer/utils/index/store/engine/section.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine {

class Snapshot;

/// Who owns the data, and therefore which copies of a scope a reader sees.
struct NamespaceSpec {
    dstd::String name;
    std::uint32_t format_version = 0;
    FoldKind fold = FoldKind::None;
    bool durable = false;
};

bool operator==(const NamespaceSpec& a, const NamespaceSpec& b);
bool operator!=(const NamespaceSpec& a, const NamespaceSpec& b);

/// When the data of a namespace becomes stale. Data files number scopes with
/// their own u32 ids, so a scope is matched across files by kind and paths.
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

enum class ScopeState : std::uint8_t {
    Absent = 0,
    Ready = 1,
    Failed = 2,
    Bad = 3
};

/// The highest generation a scope has in one directory, over copies and drops.
struct ScopeGeneration {
    dstd::String namespace_name;
    std::uint32_t format_version = 0;
    ScopeKey scope;
    std::uint64_t generation = 0;
};

/// A data file the snapshot could not open. The snapshot never reads it.
struct SkippedFile {
    dstd::String name;
    dstd::Error error;
};

/// A value the snapshot returned: a view into a page, or a buffer it owns for
/// a big value or a folded value. Move-only: a copy would leave the view
/// pointing at the first value's buffer.
class Value {
   public:
    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;
    ~Value() = default;

    ByteView bytes() const { return view_; }

   private:
    friend class Snapshot;

    ByteView view_;
    dstd::Vector<std::uint8_t> owned_;
};

namespace detail {

struct SectionRef {
    SectionView view;
    std::uint64_t sequence = 0;
    std::uint64_t page_offset = 0;
    std::uint32_t offset_in_page = 0;
    /// The section's key range from its commit record. Both views point into
    /// the file index the snapshot keeps, so a lookup skips a section that
    /// cannot hold the key without reading it.
    ByteView first_key;
    ByteView last_key;
};

struct CopyData {
    std::size_t file_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    FoldKind fold = FoldKind::None;
    CopyState state = CopyState::Ready;
    bool bad = false;
    // Ascending commit order: sequence, then page offset, then offset in page.
    dstd::Vector<SectionRef> sections;
};

struct HiddenRange {
    dstd::Vector<std::uint8_t> begin;
    dstd::Vector<std::uint8_t> end;
};

struct ScopeGroup {
    dstd::String namespace_name;
    std::uint32_t format_version = 0;
    ScopeKey scope;
    dstd::Vector<CopyData> copies;
    dstd::Vector<HiddenRange> hidden;
    std::uint64_t highest_generation = 0;
    bool dropped = false;
    std::size_t current = 0;
};

struct NamespaceGroup {
    dstd::String name;
    std::uint32_t format_version = 0;
    dstd::Vector<std::size_t> scopes;
};

struct LoadedPage {
    dstd::Vector<std::uint8_t> bytes;
    // A page whose crc or section bounds do not verify.
    bool bad = false;
};

struct FileData {
    dstd::String name;
    std::uint64_t writer_random = 0;
    DataFileReader reader;
    // One entry per page ref of the file, in page ref order.
    dstd::Vector<LoadedPage> pages;
};

struct ValueSource {
    std::size_t file_index = 0;
    SectionView::Entry entry;
};

}  // namespace detail

/// The keys of one copy of one scope, in key order, with the sections holding
/// each key in commit order. A cursor is used by one thread.
class ScopeCursor {
   public:
    ScopeCursor() = default;

    bool at_end() const { return at_end_; }
    ByteView key() const { return key_; }
    /// Section indices of the copy holding the current key, oldest first.
    const dstd::Vector<std::size_t>& matching() const { return matching_; }
    const detail::CopyData* copy() const { return copy_; }

    void next();

   private:
    friend class Snapshot;
    friend class ScanCursor;

    void open(const detail::CopyData& copy,
              const dstd::Vector<detail::HiddenRange>& hidden, ByteView begin,
              ByteView end);
    void settle();
    void advance_matching();

    void push(std::size_t index);

    dstd::Vector<SectionView::Cursor> cursors_;
    /// The current key of each cursor, so the heap compares without parsing.
    dstd::Vector<ByteView> keys_;
    /// Min-heap of the cursors not at their end, by key then section index.
    dstd::Vector<std::size_t> heap_;
    dstd::Vector<std::size_t> matching_;
    const dstd::Vector<detail::HiddenRange>* hidden_ = nullptr;
    const detail::CopyData* copy_ = nullptr;
    ByteView key_;
    ByteView end_;
    bool at_end_ = true;
};

/// A forward-only scan. It reads the snapshot it was made from, so it stays
/// valid only while that snapshot is alive and unmoved.
class ScanCursor {
   public:
    ScanCursor() = default;

    bool at_end() const { return at_end_; }
    ByteView key() const { return key_; }
    const ScopeKey& scope() const;
    dstd::Expected<ByteView, dstd::Error> value();

    void next();

   private:
    friend class Snapshot;

    struct Arm {
        ScopeKey scope;
        ScopeCursor cursor;
    };

    struct ArmAfter;

    void settle();
    void push_arm(std::size_t index);

    const Snapshot* snapshot_ = nullptr;
    dstd::Vector<Arm> arms_;
    std::size_t arm_ = 0;
    /// Fold scans only: min-heap of the arms not at their end, by key then
    /// arm index, so a key costs O(log scopes) and not one step per scope.
    dstd::Vector<std::size_t> arm_heap_;
    dstd::Vector<std::size_t> at_key_;
    FoldKind fold_ = FoldKind::None;
    ByteView key_;
    bool at_end_ = true;
    dstd::Vector<std::uint8_t> end_storage_;
    dstd::Vector<detail::ValueSource> sources_;
    dstd::Vector<std::uint8_t> buffer_;
};

/// What one open read, and how long each of its phases took. The phases after
/// the listing run on the process-wide default runtime's threads, unless open
/// itself runs on one of them. `read_calls` counts the preadv and pread calls
/// the block phase made, which is the work coalescing the reads cuts.
struct OpenStats {
    /// The data files of the directory that were readable.
    std::size_t files = 0;
    /// The page refs of those files, each read or crc-checked at open.
    std::size_t pages = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t bytes_read = 0;
    double ms_list = 0;        ///< Listing the directory.
    double ms_open_files = 0;  ///< Opening and indexing every data file.
    double ms_read = 0;        ///< Reading and verifying every block.
    double ms_tables = 0;      ///< Opening sections and building the tables.
    double ms_total = 0;
};

/// The state of every data file in one directory, as of the moment it opened.
///
/// Immutable after open, so the const methods are safe from many threads: no
/// mutable members and no caches. A cursor and the views it returns stay valid
/// while the snapshot is alive and unmoved.
class Snapshot {
   public:
    /// Reads every data file of `dir`, then every block of each one: opening
    /// and reading run in parallel on the process-wide default runtime, which
    /// is created if none is installed; a caller wanting thread control
    /// installs its own runtime with set_default_runtime. The phases run
    /// serially on the calling thread when a fork-join on that runtime could
    /// not finish: on a worker of the runtime itself, inside one of its
    /// parallel regions, and in the child of a fork, whose copy of an inherited
    /// runtime has no workers.
    static dstd::Expected<Snapshot, dstd::Error> open(dstd::StringView dir);

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

    dstd::Span<const SkippedFile> skipped() const { return skipped_; }

    /// What the open of this snapshot read, its phases and their durations.
    const OpenStats& open_stats() const { return open_stats_; }

    /// The highest generation of every (namespace, scope) a writer has to add
    /// one to, over copies and drops.
    dstd::Vector<ScopeGeneration> generations() const;

    ScopeState state(const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// The value of `key`, or an empty Optional when the scope is absent,
    /// Failed or Bad, when the key is absent, or when a drop hides it.
    dstd::Expected<dstd::Optional<Value>, dstd::Error> get(
        const NamespaceSpec& spec, const ScopeKey& scope, ByteView key) const;

    ScanCursor scan(const NamespaceSpec& spec, const ScopeKey& scope,
                    ByteView prefix) const;
    ScanCursor scan(const NamespaceSpec& spec, const ScopeKey& scope,
                    ByteView begin, ByteView end) const;
    /// Scopes one after the other in the order given, or merged by key and
    /// folded in that order for a fold namespace.
    ScanCursor scan(const NamespaceSpec& spec,
                    dstd::Span<const ScopeKey> scopes, ByteView begin,
                    ByteView end) const;
    /// Every current scope of the namespace in ScopeKey order.
    ScanCursor scan_all(const NamespaceSpec& spec, ByteView begin,
                        ByteView end) const;

   private:
    friend class ScanCursor;

    Snapshot() = default;

    const detail::ScopeGroup* find_group(const NamespaceSpec& spec,
                                         const ScopeKey& scope) const;
    const detail::NamespaceGroup* find_namespace(
        const NamespaceSpec& spec) const;

    dstd::Expected<void, dstd::Error> materialize(
        FoldKind fold, dstd::Span<const detail::ValueSource> sources,
        ByteView& view, dstd::Vector<std::uint8_t>& owned) const;
    dstd::Expected<void, dstd::Error> read_big(
        std::size_t file_index, BigRef ref, ByteView& view,
        dstd::Vector<std::uint8_t>& owned) const;

    dstd::Expected<void, dstd::Error> load(dstd::StringView dir);

    dstd::Vector<detail::FileData> files_;
    dstd::Vector<SkippedFile> skipped_;
    dstd::Vector<detail::ScopeGroup> groups_;
    dstd::Vector<detail::NamespaceGroup> namespaces_;
    OpenStats open_stats_;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H
