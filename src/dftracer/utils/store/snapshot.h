// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_SNAPSHOT_H
#define DFTRACER_UTILS_STORE_SNAPSHOT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/page_cache.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/scope_directory.h>
#include <dftracer/utils/store/segment.h>
#include <dftracer/utils/store/store.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::store::engine {

class Snapshot;

/// The highest generation a scope has in one directory, over copies and drops.
struct ScopeGeneration {
    dstd::String namespace_name;
    std::uint32_t format_version = 0;
    ScopeKey scope;
    std::uint64_t generation = 0;
};

/// One scope that Store::pack rewrites. A state of Absent is a whole-scope
/// drop.
struct PackTarget {
    NamespaceSpec spec;
    ScopeKey scope;
    ScopeState state = ScopeState::Ready;
    std::uint64_t params_hash = 0;
    dstd::Vector<SourceStat> sources;
};

/// A closed file with no current data, and the files that replaced it.
struct ReplacedFile {
    dstd::String name;
    std::uint64_t size = 0;
    dstd::Vector<dstd::String> replaced_by;
};

struct PackPlan {
    std::size_t files_packed = 0;
    dstd::Vector<PackTarget> targets;
    dstd::Vector<ReplacedFile> replaced;
};

/// Cumulative block reads, readable at any time from any thread. An open
/// leaves them at zero.
struct ReadStats {
    /// The commits that are not synced and were read and checked whole.
    std::uint64_t commits_verified = 0;
    /// The commit records read for the extent refs a summary leaves out.
    std::uint64_t indexes_read = 0;
    /// One per extent ref: the page tables of synced commits and every block
    /// of a commit checked whole.
    std::uint64_t extents_read = 0;
    /// The pages read and decoded because the page cache lacked them.
    std::uint64_t pages_decoded = 0;
    std::uint64_t page_hits = 0;
    std::uint64_t page_reads = 0;
    /// The windows of READ_RUN_BYTES read whole to warm the operating system
    /// cache, once a query loads the second extent in one.
    std::uint64_t windows_warmed = 0;
    /// The files a scope lookup skipped because their scope filter rules the
    /// scope out.
    std::uint64_t files_filtered = 0;
    /// The directory blocks and segment blocks of closed files.
    std::uint64_t metadata_blocks_read = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t bytes_read = 0;
};

/// One data file the snapshot could not open. The snapshot never reads it.
struct SkippedFile {
    dstd::String name;
    dstd::Error error;
};

namespace detail {

enum class LoadState : std::uint8_t {
    /// Not read yet.
    Unknown = 0,
    /// Read, its crc checked and every segment of it opened.
    Good = 1,
    /// Read, but a crc or a segment does not verify.
    Bad = 2,
    /// The read itself failed. The error is kept, so a later query reports it
    /// instead of reading again.
    Failed = 3
};

struct CommitExtent {
    ExtentRef ref;
    /// Null for a big value run. Points into the reader of the commit's file.
    const ExtentRefItem* listed = nullptr;
};

/// The page table of one extent, read once: acquire load, lock, recheck, read,
/// release store. The snapshot keeps no extent bytes.
struct ExtentSlot {
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    /// Written before the release store that publishes Good.
    ExtentRef ref;
    /// The page table, empty until the slot is Good.
    dstd::Optional<ExtentPages> pages;
    dstd::Error error;
};

struct Commit;
struct LazyBlock;
class GroupSlots;

struct SegmentSlot {
    /// Closed file only.
    ExtentRef extent;
    /// The commit whose copy holds the segment. A block of a closed file holds
    /// the runs of one copy of many commits, in this order.
    std::uint64_t sequence = 0;
    /// The block of `owner` holding the extent, for a file that is not closed.
    std::uint32_t block = 0;
    std::uint32_t extent_index = 0;
    std::uint32_t offset_in_extent = 0;
    std::uint32_t length = 0;
    std::uint64_t entry_count = 0;
    /// A larger ordinal is a later batch of the file.
    std::uint64_t ordinal = 0;
    /// Point into the file index the snapshot keeps.
    dstd::ByteView first_key;
    dstd::ByteView last_key;
};

/// What a query read of an extent: the one cached page a range lies in, or the
/// pages it spans joined into a buffer of its own.
struct PageHold {
    CachedPage page;
    dstd::RawBuffer<std::uint8_t> joined;
};

/// The pages one segment spans and the view of the segment over them, owned by
/// the cursor that reads the segment.
struct SegmentBody {
    SegmentBody(PageHold held, SegmentView opened, PageHold head_held = {})
        : hold(std::move(held)),
          head(std::move(head_held)),
          view(std::move(opened)) {}

    PageHold hold;
    /// The header and index pages, when `view` covers only some granules.
    PageHold head;
    SegmentView view;
};

enum class BlockKind : std::uint8_t {
    Leaf = 0,
    Middle = 1,
    /// The segments of the runs of one copy of many commits, in commit order.
    Segments = 2
};

/// One block of metadata, read once on first use. A block built in memory at
/// open starts Good.
struct LazyBlock {
    BlockHandle handle;
    BlockKind kind = BlockKind::Leaf;
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    dstd::Vector<std::uint8_t> bytes;
    dstd::Error error;
    /// A directory block: its level, 1 for a leaf. A middle block creates and
    /// owns its children while it loads, under its mutex.
    std::uint32_t levels = 1;
    SlotTable separators;
    dstd::Vector<dstd::UniquePtr<LazyBlock>> children;
    dstd::UniquePtr<ScopeDirectory> directory;
    dstd::UniquePtr<GroupSlots> groups;
    /// A segment block of a file that is not closed: the commit whose extents
    /// hold the segments. Null for a closed file.
    Commit* owner = nullptr;
    dstd::Vector<SegmentSlot> segments;
};

/// One commit of one file: every extent ref of one sequence, deduplicated by
/// offset. Owned through a pointer because snapshot copies point at it. A
/// synced commit (extents on disk before its record) loads each extent on first
/// read. Otherwise the first touch checks all of it, and any bad block makes it
/// Bad.
struct Commit {
    std::uint64_t sequence = 0;
    std::size_t file_index = 0;
    bool synced = false;
    dstd::Vector<CommitExtent> blocks;
    dstd::Vector<ExtentSlot> extent_slots;
    /// The whole-commit state of a commit that is not synced.
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    dstd::Error error;
    /// For a commit whose extent refs stay in their record until a query needs
    /// them. Null when the chain listed them itself.
    const CommitRefItem* summary = nullptr;
    /// Good once `blocks`, `extent_slots` and `segment_blocks` exist. Written
    /// under `index_mutex` before the release store.
    std::atomic<std::uint8_t> index_state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex index_mutex;
    dstd::Error index_error;
    /// The extent refs read from a summary commit's record, which `blocks`
    /// point into.
    dstd::Vector<ExtentRefItem> listed_refs;
    /// One segment block per copy the commit holds a segment of, sorted by copy
    /// id in the parallel `segment_copies`.
    dstd::Vector<dstd::UniquePtr<LazyBlock>> segment_blocks;
    dstd::Vector<std::uint32_t> segment_copies;
};

/// One commit holding a segment of a copy, with the copy's key range in it, so
/// a lookup skips a commit that cannot hold its key.
struct CopyCommit {
    std::uint64_t sequence = 0;
    dstd::ByteView first_key;
    dstd::ByteView last_key;
    /// Closed file: the segment block of this copy, owned by the group and
    /// shared by many commits. Snapshot::copy_segments returns this commit's
    /// run of it. Null for a file that is not closed, whose commit holds the
    /// block.
    LazyBlock* segments = nullptr;
    /// Null for a closed file, whose commits are all synced.
    Commit* commit = nullptr;
};

struct LoadStatus {
    LoadState state = LoadState::Unknown;
    dstd::Error error;
};

struct EraseRange {
    /// The batch ordinal the erase took: it hides the versions of the keys in
    /// [begin, end) whose version comes from a smaller ordinal of the copy.
    std::uint64_t ordinal = 0;
    dstd::ByteView begin;
    dstd::ByteView end;
};

struct CopyData {
    std::size_t file_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    FoldKind fold = FoldKind::None;
    CopyState state = CopyState::Ready;
    std::uint32_t local_id = 0;
    /// Empty, or one per path of its scope in scope path order.
    dstd::Vector<SourceStat> sources;
    /// The commits holding a segment of this copy, oldest first. The copy is
    /// readable only once every one of them that is not synced has verified.
    dstd::Vector<CopyCommit> commits;
    dstd::Vector<EraseRange> erases;
};

struct HiddenRange {
    dstd::Vector<std::uint8_t> begin;
    dstd::Vector<std::uint8_t> end;
};

/// Every copy and drop of one scope over the files of the snapshot, built on
/// the first query of the scope. Owns the segment blocks of its copies.
struct ScopeGroup {
    dstd::Vector<CopyData> copies;
    dstd::Vector<HiddenRange> hidden;
    dstd::Vector<dstd::UniquePtr<LazyBlock>> segments;
    std::uint64_t highest_generation = 0;
    bool dropped = false;
    /// In a shared namespace, reads use the copies from first_visible to
    /// current, the newest last. Otherwise only current.
    bool shared = false;
    std::size_t first_visible = 0;
    std::size_t current = 0;
};

/// The groups found by queries, by directory key. A key hashes to one of
/// SHARDS shards, each with its own mutex. The leaf blocks own the groups.
class GroupIndex {
   public:
    const ScopeGroup* find(const dstd::String& key) const;
    void add(const dstd::String& key, const ScopeGroup* group) const;

   private:
    static constexpr std::size_t SHARDS = 16;
    struct Shard {
        std::mutex mutex;
        dstd::HashMap<dstd::String, const ScopeGroup*> groups;
    };

    Shard& shard_of(const dstd::String& key) const;

    mutable Shard shards_[SHARDS];
};

/// The groups of one leaf block's entries, one slot per entry, set once by the
/// first query of the scope and owned here. The first leaf holding a scope owns
/// it.
class GroupSlots {
   public:
    GroupSlots() = default;
    GroupSlots(const GroupSlots&) = delete;
    GroupSlots& operator=(const GroupSlots&) = delete;
    GroupSlots(GroupSlots&&) noexcept = default;
    GroupSlots& operator=(GroupSlots&&) noexcept = delete;
    ~GroupSlots();

    /// Empty slots for `count` entries. Called once, before any query.
    void assign(std::size_t count) {
        slots_ = dstd::Vector<std::atomic<ScopeGroup*>>(count);
    }

    const ScopeGroup* get(std::size_t index) const {
        return slots_[index].load(std::memory_order_acquire);
    }
    /// Publishes `group` unless another thread did first, and returns the
    /// group the slot holds.
    const ScopeGroup* publish(std::size_t index,
                              dstd::UniquePtr<ScopeGroup> group) const;

   private:
    /// A cache the const queries of a snapshot fill, one compare-and-swap per
    /// slot.
    mutable dstd::Vector<std::atomic<ScopeGroup*>> slots_;
};

/// The extent slots of a closed file, made a chunk at a time by the first
/// query that reads an extent of the chunk.
class ExtentSlots {
   public:
    ExtentSlots() = default;
    ExtentSlots(const ExtentSlots&) = delete;
    ExtentSlots& operator=(const ExtentSlots&) = delete;
    ExtentSlots(ExtentSlots&&) noexcept = default;
    ExtentSlots& operator=(ExtentSlots&&) noexcept = delete;
    ~ExtentSlots();

    /// Empty chunks for `count` extent slots. Called once, before any query.
    void assign(std::size_t count);

    std::size_t size() const { return count_; }

    /// The extent slot of one extent below size(), making its chunk on first
    /// use.
    ExtentSlot& slot(std::size_t index) const;

   private:
    static constexpr std::size_t CHUNK_SLOTS = 256;
    struct Chunk {
        ExtentSlot slots[CHUNK_SLOTS];
    };

    std::size_t count_ = 0;
    /// A cache the const queries of a snapshot fill, one compare-and-swap per
    /// chunk.
    mutable dstd::Vector<std::atomic<Chunk*>> chunks_;
};

struct DictCache {
    std::mutex mutex;
    dstd::Vector<dstd::SharedPtr<const PageDict>> dicts;
};

struct FileData {
    explicit FileData(DataFileReader opened) : reader(std::move(opened)) {}

    dstd::String name;
    std::uint64_t writer_random = 0;
    DataFileReader reader;
    /// One per commit of the file, in sequence order. A closed file holds no
    /// commit object: every commit of it is synced.
    dstd::Vector<dstd::UniquePtr<Commit>> commits;
    dstd::Vector<std::uint8_t> top_table;
    /// The separators of `top_table`. The vector keeps its buffer when the
    /// file data moves, so the view stays valid.
    SlotTable top;
    /// One block per separator of the top table. A file that is not closed has
    /// one leaf block here, built at open.
    dstd::Vector<dstd::UniquePtr<LazyBlock>> roots;
    std::uint32_t directory_levels = 0;
    ExtentSlots extent_slots;
    /// Per window of READ_RUN_BYTES of the file, the extents loaded in it.
    mutable dstd::Vector<std::atomic<std::uint32_t>> windows;
    /// Behind a pointer, so the file data moves.
    dstd::UniquePtr<DictCache> dicts = dstd::make_unique<DictCache>();
};

class ReadCounters {
   public:
    void add_index(const BlockReads& reads) {
        indexes_read_.fetch_add(1, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    void add(std::uint64_t commits, std::uint64_t extents, std::uint64_t pages,
             const BlockReads& reads) {
        commits_verified_.fetch_add(commits, std::memory_order_relaxed);
        extents_read_.fetch_add(extents, std::memory_order_relaxed);
        pages_decoded_.fetch_add(pages, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    void add_pages(std::uint64_t decoded, std::uint64_t hits,
                   const BlockReads& reads) {
        pages_decoded_.fetch_add(decoded, std::memory_order_relaxed);
        page_hits_.fetch_add(hits, std::memory_order_relaxed);
        page_reads_.fetch_add(reads.calls, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    void add_filtered() {
        files_filtered_.fetch_add(1, std::memory_order_relaxed);
    }

    void add_window(const BlockReads& reads) {
        windows_warmed_.fetch_add(1, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    void add_metadata(const BlockReads& reads) {
        metadata_blocks_read_.fetch_add(1, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    ReadStats read() const {
        return ReadStats{commits_verified_.load(std::memory_order_relaxed),
                         indexes_read_.load(std::memory_order_relaxed),
                         extents_read_.load(std::memory_order_relaxed),
                         pages_decoded_.load(std::memory_order_relaxed),
                         page_hits_.load(std::memory_order_relaxed),
                         page_reads_.load(std::memory_order_relaxed),
                         windows_warmed_.load(std::memory_order_relaxed),
                         files_filtered_.load(std::memory_order_relaxed),
                         metadata_blocks_read_.load(std::memory_order_relaxed),
                         read_calls_.load(std::memory_order_relaxed),
                         bytes_read_.load(std::memory_order_relaxed)};
    }

   private:
    std::atomic<std::uint64_t> commits_verified_{0};
    std::atomic<std::uint64_t> indexes_read_{0};
    std::atomic<std::uint64_t> extents_read_{0};
    std::atomic<std::uint64_t> pages_decoded_{0};
    std::atomic<std::uint64_t> page_hits_{0};
    std::atomic<std::uint64_t> page_reads_{0};
    std::atomic<std::uint64_t> windows_warmed_{0};
    std::atomic<std::uint64_t> files_filtered_{0};
    std::atomic<std::uint64_t> metadata_blocks_read_{0};
    std::atomic<std::uint64_t> read_calls_{0};
    std::atomic<std::uint64_t> bytes_read_{0};
};

struct ValueSource {
    std::size_t file_index = 0;
    std::uint64_t ordinal = 0;
    SegmentView::Entry entry;
};

struct SegmentLocation {
    LazyBlock* block = nullptr;
    std::size_t index = 0;
};

/// The entries [begin, end) of a block that holds the runs of one copy of many
/// commits, sorted by commit sequence. A block of a file that is not closed
/// holds one commit, so its run is the whole block.
struct SegmentRun {
    LazyBlock* block = nullptr;
    std::size_t begin = 0;
    std::size_t end = 0;
};

}  // namespace detail

/// The keys of one copy of one scope, in key order, with the segments holding
/// each key in the order of their batch ordinals, oldest first. A cursor is
/// used by one thread.
class ScopeCursor {
   public:
    ScopeCursor() = default;
    ScopeCursor(const ScopeCursor&) = delete;
    ScopeCursor& operator=(const ScopeCursor&) = delete;
    ScopeCursor(ScopeCursor&&) noexcept = default;
    ScopeCursor& operator=(ScopeCursor&&) noexcept = default;

    bool at_end() const { return at_end_; }
    dstd::ByteView key() const { return key_; }
    /// Segment indices of the copy holding the current key, oldest batch first.
    const dstd::Vector<std::size_t>& matching() const { return matching_; }
    const detail::CopyData* copy() const { return copy_; }

    void next();

   private:
    friend class Snapshot;
    friend class ScanCursor;

    /// Puts every segment of the copy that can hold a key in [begin, end) on
    /// the heap by its first key from the record, so no extent is read until
    /// the segment reaches the top. Reads the extent refs of summary commits
    /// whose range meets [begin, end). Every commit of the copy not synced must
    /// be Good.
    void open(const Snapshot& snapshot, const detail::CopyData& copy,
              const dstd::Vector<detail::HiddenRange>& hidden,
              dstd::ByteView begin, dstd::ByteView end);
    void settle();
    void advance_matching();
    /// Reads the pages of one segment and puts its cursor back on the heap.
    /// False, with `error_` set, when the extent cannot be used.
    bool load(std::size_t index);

    void push(std::size_t index);

    const Snapshot* snapshot_ = nullptr;
    /// The segments of every commit of the copy that can hold a key in range,
    /// oldest commit first.
    dstd::Vector<detail::SegmentLocation> segments_;
    dstd::Vector<SegmentView::Cursor> cursors_;
    /// The pages of each loaded segment, until its cursor ends.
    dstd::Vector<dstd::UniquePtr<detail::SegmentBody>> bodies_;
    /// The batch ordinal of each segment, so the heap orders the versions of
    /// one key by batch.
    dstd::Vector<std::uint64_t> ordinals_;
    /// The current key of each cursor. For a segment not loaded yet, the lower
    /// bound its record gives.
    dstd::Vector<dstd::ByteView> keys_;
    dstd::Vector<std::uint8_t> loaded_;
    /// Min-heap of the cursors not at their end, by key, then ordinal, then
    /// segment index.
    dstd::Vector<std::size_t> heap_;
    dstd::Vector<std::size_t> matching_;
    const dstd::Vector<detail::HiddenRange>* hidden_ = nullptr;
    const detail::CopyData* copy_ = nullptr;
    dstd::ByteView key_;
    dstd::ByteView begin_;
    dstd::ByteView end_;
    bool at_end_ = true;
    dstd::Optional<dstd::Error> error_;
};

/// A forward-only scan. It reads the snapshot it was made from, so it stays
/// valid only while that snapshot is alive and unmoved. The views key() and
/// value() return stay valid until the next call of next().
class ScanCursor {
   public:
    ScanCursor() = default;
    ScanCursor(const ScanCursor&) = delete;
    ScanCursor& operator=(const ScanCursor&) = delete;
    ScanCursor(ScanCursor&&) noexcept = default;
    ScanCursor& operator=(ScanCursor&&) noexcept = default;

    bool at_end() const { return at_end_; }
    dstd::ByteView key() const { return key_; }
    const ScopeKey& scope() const;
    dstd::Expected<dstd::ByteView, dstd::Error> value();

    /// Empty unless an extent of one of the scopes could not be read, or an
    /// extent of a synced commit failed its crc: a cursor that stopped on one
    /// is at its end. A copy with a bad commit that is not synced is skipped
    /// and leaves this empty.
    const dstd::Optional<dstd::Error>& error() const { return error_; }

    void next();

   private:
    friend class Snapshot;

    struct Arm {
        ScopeKey scope;
        const detail::ScopeGroup* group = nullptr;
        /// The copy this arm reads, in group->copies.
        std::size_t copy_index = 0;
        /// A merge by key stays inside one rank. A fold merges the rows of all
        /// its scopes, so they share a rank. A shared namespace merges only
        /// the copies of one scope, so each scope has its own.
        std::size_t merge_rank = 0;
        bool opened = false;
        ScopeCursor cursor;
    };

    struct ArmAfter;

    void settle();
    void push_arm(std::size_t index);
    /// True, with the cursor stopped, when the arm stopped on an error.
    bool take_error(const ScopeCursor& cursor);
    /// Checks the commits of one arm's copy that are not synced and opens its
    /// cursor. False, with the scan stopped, on an error.
    bool open_arm(std::size_t index);

    const Snapshot* snapshot_ = nullptr;
    dstd::Vector<Arm> arms_;
    std::size_t arm_ = 0;
    /// Fold scans only: min-heap of the arms not at their end, by key then
    /// arm index, so a key costs O(log scopes) and not one step per scope.
    dstd::Vector<std::size_t> arm_heap_;
    dstd::Vector<std::size_t> at_key_;
    FoldKind fold_ = FoldKind::None;
    dstd::ByteView key_;
    bool at_end_ = true;
    /// Owned copies of the bounds, which the arms read until the scan ends.
    dstd::Vector<std::uint8_t> begin_storage_;
    dstd::Vector<std::uint8_t> end_storage_;
    dstd::Vector<detail::ValueSource> sources_;
    dstd::Vector<std::uint8_t> buffer_;
    dstd::Optional<dstd::Error> error_;
};

/// What one open read, and how long each phase took. The phases after the
/// listing run on the process-wide default runtime's threads, unless open
/// itself runs on one of them. No block is read at open.
struct OpenStats {
    std::size_t files = 0;
    /// Closed files indexed from the directory summary without opening them.
    std::size_t files_summarized = 0;
    std::size_t commits = 0;
    /// The extent refs the record chains list. A summary commit's refs are
    /// read with its first query.
    std::size_t extents = 0;
    double ms_list = 0;  ///< Listing the directory and reading its summary.
    double ms_open_files = 0;  ///< Opening and indexing every data file.
    double ms_tables = 0;  ///< Grouping the commits and building the tables.
    double ms_total = 0;
};

/// One commit the full check found unreadable: a block whose crc or bounds do
/// not verify, or a segment of the commit that does not open.
struct BadCommit {
    dstd::String file;
    std::uint64_t sequence = 0;
};

/// One segment of a commit the full check could not walk: its bytes are not
/// there, its entries do not parse, or its keys do not strictly ascend.
struct BadSegment {
    dstd::String file;
    /// The offset of the extent ref, and of the segment inside it.
    std::uint64_t extent_offset = 0;
    std::uint32_t offset_in_extent = 0;
};

/// What one full check read and what it found.
struct CheckReport {
    std::size_t files = 0;
    std::size_t commits = 0;
    /// The extents and big value runs checked, each once.
    std::size_t blocks = 0;
    /// The directory and segment blocks of closed files checked.
    std::size_t metadata_blocks = 0;
    std::uint64_t bytes_read = 0;
    dstd::Vector<BadCommit> bad_commits;
    std::size_t segments = 0;
    dstd::Vector<BadSegment> bad_segments;
};

/// The state of every data file in one directory, as of the moment it opened.
/// The const methods are safe from many threads, and each block a query needs
/// is read and checked once. A cursor and its views stay valid while the
/// snapshot is alive and unmoved, and the views of a cursor until it moves on.
class Snapshot {
   public:
    /// Opens every data file of `dir`. A closed file costs one read of its end,
    /// and no block of it is read here. Phases run in parallel on the process
    /// runtime (configure_default_runtime sets its threads) and serially on the
    /// calling thread when a fork-join could not finish there (worker, fork).
    static dstd::Expected<Snapshot, dstd::Error> open(dstd::StringView dir);

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

    dstd::Span<const SkippedFile> skipped() const { return skipped_; }

    const OpenStats& open_stats() const { return open_stats_; }

    /// Every block read so far, cumulative and readable at any time. An open
    /// adds nothing to it, and a snapshot a move left empty has no counters.
    ReadStats read_stats() const {
        if (reads_ == nullptr) return ReadStats{};
        return reads_->read();
    }

    /// Reads and checks every block of every commit, then walks every entry of
    /// every segment, reporting what is wrong. Runs in parallel under the rules
    /// of `open` and changes no commit state. An error is returned only when a
    /// block could not be read at all.
    dstd::Expected<CheckReport, dstd::Error> check() const;

    /// The highest generation of every (namespace, scope) a writer has to add
    /// one to, over copies and drops. Reads every directory block. The error
    /// of the first one that does not load, since a scope left out would get
    /// a generation that is too low.
    dstd::Expected<dstd::Vector<ScopeGeneration>, dstd::Error> generations()
        const;

    /// What Store::pack does on this view. Reads every directory block, and
    /// the segment blocks of every file not below `small_file_bytes`.
    dstd::Expected<PackPlan, dstd::Error> pack_plan(
        std::uint64_t small_file_bytes, double dead_ratio) const;

    ScopeState state(const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// The current copy of `scope` from the scope directories, reading no data
    /// page. For a file that is not closed, the state verifies the blocks of
    /// the copy's commits. No copy, or a dropped one, reads Absent.
    CopyInfo copy_info(const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// The value of `key`, or an empty Optional when the scope is absent,
    /// Failed or Bad, when the key is absent, or when a drop hides it. The
    /// error is the one a block of a commit of the copy failed to read with.
    dstd::Expected<dstd::Optional<Value>, dstd::Error> get(
        const NamespaceSpec& spec, const ScopeKey& scope,
        dstd::ByteView key) const;

    ScanCursor scan(const NamespaceSpec& spec, const ScopeKey& scope,
                    dstd::ByteView prefix) const;
    ScanCursor scan(const NamespaceSpec& spec, const ScopeKey& scope,
                    dstd::ByteView begin, dstd::ByteView end) const;
    /// Scopes one after the other in the order given, or merged by key and
    /// folded in that order for a fold namespace.
    ScanCursor scan(const NamespaceSpec& spec,
                    dstd::Span<const ScopeKey> scopes, dstd::ByteView begin,
                    dstd::ByteView end) const;
    /// Every current scope of the namespace in ScopeKey order. A directory
    /// block that does not load gives a cursor at its end with the error.
    ScanCursor scan_all(const NamespaceSpec& spec, dstd::ByteView begin,
                        dstd::ByteView end) const;

    /// Every scope of the namespace and version that has a current copy, in
    /// ScopeKey order, read from the scope directories alone. The error is the
    /// one a directory block failed to read with.
    dstd::Expected<dstd::Vector<ScopeEntry>, dstd::Error> scopes(
        const NamespaceSpec& spec) const;

    /// Every namespace the data files of this snapshot declare, one entry per
    /// (name, version) in name then version order. A file that declares the
    /// same namespace at two versions appears twice.
    dstd::Expected<dstd::Vector<NamespaceDecl>, dstd::Error> namespaces() const;

   private:
    friend class ScanCursor;
    friend class ScopeCursor;

    Snapshot() = default;

    /// Calls `visit(directory, index)` for each directory entry of `file`.
    template <typename Visit>
    dstd::Expected<void, dstd::Error> walk_directory(
        const detail::FileData& file, Visit&& visit) const;

    dstd::Expected<dstd::Optional<Value>, dstd::Error> get_in_copy(
        const detail::CopyData& copy, const detail::ScopeGroup& group,
        dstd::ByteView key) const;
    /// The group of a scope: from the leaf block of the first file whose
    /// directory holds it, built and published there on its first query. Null
    /// when no file holds the scope, and the error of a block that had to be
    /// read to decide.
    dstd::Expected<const detail::ScopeGroup*, dstd::Error> find_group(
        const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// Reads and decodes one block of metadata once, under its mutex. The fast
    /// path is one acquire load. A block built in memory at open is Good.
    detail::LoadStatus load_block(const detail::FileData& file,
                                  detail::LazyBlock& block) const;
    /// The dictionary at `ref` in `file`, read and checked on first use and
    /// then shared. Null for an empty `ref`. Io when the read fails, Parse
    /// when the bytes do not match `ref`.
    dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error> page_dict(
        const detail::FileData& file, const DictRef& ref) const;
    /// Reads the page table of one extent of a closed file once. An index at or
    /// past the extent count is Bad.
    detail::LoadStatus load_extent(const detail::FileData& file,
                                   std::size_t extent_index,
                                   const ExtentRef& ref) const;
    /// Reads and checks every block of one commit that is not synced, once,
    /// under its mutex.
    detail::LoadStatus verify_commit(detail::Commit& commit) const;
    /// Verifies every commit of one copy that is not synced, oldest first.
    /// Good only when all of them are. A Bad commit decides over a Failed one,
    /// since a copy whose data does not verify is unreadable whatever else
    /// could not be read.
    detail::LoadStatus ensure_copy(const detail::CopyData& copy) const;
    /// Reads the page table of the extent of one block of a synced commit
    /// once, under the slot's mutex.
    detail::LoadStatus load_extent(detail::Commit& commit,
                                   std::size_t block) const;
    /// Counts one more extent loaded at `offset` of `file`, and reads its
    /// window whole when it is the second of the window, so a dense workload
    /// reads from the operating system cache.
    void warm_window(const detail::FileData& file, std::uint64_t offset) const;
    /// The raw bytes [offset_in_extent, offset_in_extent + length) of one
    /// extent whose page table is loaded, and a view into `hold`: the pages
    /// they span from the page cache, and those it lacks read from the file in
    /// one call, checked, decoded and added to it.
    dstd::Expected<dstd::ByteView, dstd::Error> read_raw(
        const detail::FileData& file, const detail::ExtentSlot& extent,
        std::uint32_t offset_in_extent, std::uint32_t length,
        detail::PageHold& hold) const;
    /// The header and index of a segment over pages [first_page, end_page) of
    /// `extent`, read page by page until they parse. Empty when they never do.
    dstd::Expected<dstd::ByteView, dstd::Error> read_head(
        const detail::FileData& file, const detail::ExtentSlot& extent,
        const detail::SegmentSlot& slot, std::size_t first_page,
        std::size_t end_page, detail::PageHold& hold) const;
    struct FileCheck;
    /// Reads the extent refs of a summary commit once, under its index mutex,
    /// and builds its blocks, extents and segments.
    detail::LoadStatus load_index(detail::Commit& commit) const;
    /// The pages of one segment that can hold a key in [begin, end) and its
    /// view over them, for a cursor to own. An empty `end` is unbounded. An
    /// extent that failed its crc is a Parse error.
    dstd::Expected<dstd::UniquePtr<detail::SegmentBody>, dstd::Error>
    open_segment(const detail::FileData& file, detail::LazyBlock& block,
                 std::size_t index, dstd::ByteView begin,
                 dstd::ByteView end) const;
    /// The extent holding one segment, loaded once: the extent of a closed file
    /// or the block of the commit that holds it. An extent that failed its crc
    /// is a Parse error.
    dstd::Expected<const detail::ExtentSlot*, dstd::Error> loaded_extent(
        const detail::FileData& file, detail::LazyBlock& block,
        std::size_t index) const;
    /// The entry of `key` in one segment, read through the page of the header
    /// and the index and the page of the granule that holds it when those are
    /// two pages of the extent, so a point read decodes those two and no more.
    /// The entry points into `holder`.
    dstd::Expected<dstd::Optional<SegmentView::Entry>, dstd::Error>
    find_in_segment(const detail::FileData& file, detail::LazyBlock& block,
                    std::size_t index, dstd::ByteView key,
                    detail::PageHold& holder) const;
    /// The run of one commit of one copy inside its segment block, with what it
    /// needs read first: the commit index of a file that is not closed, or the
    /// segment block of a closed one. `block` is null when the commit holds no
    /// segment of the copy. A block that does not verify is a Parse error.
    dstd::Expected<detail::SegmentRun, dstd::Error> copy_segments(
        const detail::CopyData& copy, const detail::CopyCommit& held) const;
    dstd::Expected<void, dstd::Error> materialize(
        FoldKind fold, dstd::Span<const detail::ValueSource> sources,
        dstd::ByteView& view, dstd::Vector<std::uint8_t>& owned) const;
    dstd::Expected<void, dstd::Error> read_big(
        std::size_t file_index, BigRef ref, dstd::ByteView& view,
        dstd::Vector<std::uint8_t>& owned) const;

    dstd::Expected<void, dstd::Error> load(dstd::StringView dir);

    /// The scopes of the namespace and version in any file's directory, sorted
    /// and without duplicates. The error is the one a block failed to read
    /// with.
    dstd::Expected<dstd::Vector<ScopeKey>, dstd::Error> namespace_scopes(
        const NamespaceSpec& spec) const;

    dstd::Vector<detail::FileData> files_;
    dstd::Vector<SkippedFile> skipped_;
    OpenStats open_stats_;
    /// Behind pointers so the snapshot moves while threads read these.
    dstd::UniquePtr<detail::ReadCounters> reads_;
    dstd::UniquePtr<PageCache> pages_;
    dstd::UniquePtr<detail::GroupIndex> groups_;
};

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_SNAPSHOT_H
