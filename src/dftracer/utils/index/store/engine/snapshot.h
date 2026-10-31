#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/record.h>
#include <dftracer/utils/index/store/engine/scope_directory.h>
#include <dftracer/utils/index/store/engine/section.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

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

/// Every block read the snapshot has done, cumulative and readable at any time
/// from any thread. A page is read once, the first time a query needs it, so
/// an open leaves these at zero.
struct ReadStats {
    /// The commits that are not synced and were read and checked whole.
    std::uint64_t commits_verified = 0;
    /// The commit records read for the page refs a summary leaves out.
    std::uint64_t indexes_read = 0;
    /// The blocks read, one per page ref: the pages of synced commits one at a
    /// time, and every block of a commit checked whole.
    std::uint64_t blocks_read = 0;
    /// The blocks of metadata read on first use: the directory blocks and the
    /// section blocks of a closed file.
    std::uint64_t metadata_blocks_read = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t bytes_read = 0;
};

/// One data file the snapshot could not open. The snapshot never reads it.
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

enum class LoadState : std::uint8_t {
    /// Not read yet.
    Unknown = 0,
    /// Read, its crc checked and every section of it opened.
    Good = 1,
    /// Read, but a crc or a section does not verify.
    Bad = 2,
    /// The read itself failed. The error is kept, so a later query reports it
    /// instead of reading again.
    Failed = 3
};

/// One block of a commit: a page holding sections, or the run of a big value,
/// which holds none.
struct CommitBlock {
    /// The page ref the commit record listed.
    PageRef ref;
    /// What that ref names in the file index: its page sections, or nothing for
    /// a big value run. Points into the file's reader, which the commit's own
    /// file owns.
    const PageRefItem* listed = nullptr;
};

/// The bytes of one page, read once. A closed file holds one slot per page
/// its section blocks name; a file that is not closed holds one per block of
/// its commits.
struct PageSlot {
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    /// Filled before the release store that publishes Good, and read only by a
    /// thread whose acquire load saw Good.
    dstd::Vector<std::uint8_t> bytes;
    /// The error of a Failed slot, written before the release store.
    dstd::Error error;
};

struct Commit;
struct LazyBlock;
class GroupSlots;

/// One section of the sections of one copy of one commit: the page holding it,
/// its place in that page, its key range, and the view opened from it once.
struct SectionSlot {
    /// The page holding the section, for a closed file.
    PageRef page;
    /// The block of `owner` holding the page, for a file that is not closed.
    std::uint32_t block = 0;
    /// The index of the page among the pages of a closed file.
    std::uint32_t page_index = 0;
    std::uint32_t offset_in_page = 0;
    std::uint32_t length = 0;
    std::uint64_t entry_count = 0;
    /// Both views point into the file index the snapshot keeps, so a lookup
    /// skips a section that cannot hold the key without reading it.
    ByteView first_key;
    ByteView last_key;
    /// Opened by the first query that reads the section, owned by the block.
    std::atomic<SectionView*> view{nullptr};
};

/// What a block holds, so a load knows how to decode it.
enum class BlockKind : std::uint8_t {
    /// A leaf of the directory: one slot table of entries.
    Leaf = 0,
    /// A middle of the directory: one separator per child block.
    Middle = 1,
    /// The sections of one copy of one commit.
    Sections = 2
};

/// One block of metadata, read once on its first use: its bytes, or the error
/// that stopped the read. A block built in memory at open starts Good, and a
/// load of it reads nothing.
struct LazyBlock {
    BlockHandle handle;
    BlockKind kind = BlockKind::Leaf;
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    dstd::Vector<std::uint8_t> bytes;
    dstd::Error error;
    /// A directory block: its level, 1 for a leaf. The children of a middle
    /// block are created while it loads, under its mutex and before it turns
    /// Good, and it owns them.
    std::uint32_t levels = 1;
    /// A middle block: its separators over `bytes`, and one child per
    /// separator.
    SlotTable separators;
    dstd::Vector<dstd::UniquePtr<LazyBlock>> children;
    /// A leaf block: its entries, and one group slot per entry.
    dstd::UniquePtr<ScopeDirectory> directory;
    dstd::UniquePtr<GroupSlots> groups;
    /// A section block: the commit whose pages hold the sections, for a file
    /// that is not closed. Null for a closed file, whose sections name their
    /// pages.
    Commit* owner = nullptr;
    dstd::Vector<SectionSlot> sections;

    ~LazyBlock();
};

/// One commit of one file: every page ref of one sequence, deduplicated by
/// offset. Owned through a pointer, because the copies of the snapshot point
/// at it.
///
/// A synced commit, one whose pages reached the disk before its record, loads
/// each page on its first read, and a bad page fails only the queries that
/// read it. A commit that is not synced can be half written after a crash, so
/// its first touch reads and checks all of it, under `mutex`, and any bad block
/// makes the whole commit Bad.
struct Commit {
    std::uint64_t sequence = 0;
    std::size_t file_index = 0;
    bool synced = false;
    dstd::Vector<CommitBlock> blocks;
    /// One per block, in block order.
    dstd::Vector<PageSlot> pages;
    /// The whole-commit state of a commit that is not synced.
    std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex mutex;
    dstd::Error error;
    /// The summary a checkpoint or the footer lists, for a commit whose page
    /// refs stay in their record until a query first needs them. Null when the
    /// chain listed the page refs itself.
    const CommitRefItem* summary = nullptr;
    /// Good once `blocks`, `pages` and `section_blocks` are there: from open
    /// for a commit the chain listed, after load_index for one a summary
    /// names. Written under `index_mutex` before the release store.
    std::atomic<std::uint8_t> index_state{
        static_cast<std::uint8_t>(LoadState::Unknown)};
    std::mutex index_mutex;
    dstd::Error index_error;
    /// The page refs read from the record of a summary commit; the blocks
    /// point into them.
    dstd::Vector<PageRefItem> listed_refs;
    /// One section block per copy the commit holds a section of, built in
    /// memory at open because the chain itself is in memory. `section_copies`
    /// is parallel and sorted by copy id.
    dstd::Vector<dstd::UniquePtr<LazyBlock>> section_blocks;
    dstd::Vector<std::uint32_t> section_copies;
};

/// One commit holding a section of a copy, with the copy's key range in it.
/// The range comes from the commit record or from the summary, so a lookup
/// skips a commit that cannot hold its key without reading anything.
struct CopyCommit {
    std::uint64_t sequence = 0;
    ByteView first_key;
    ByteView last_key;
    /// The section block of this copy in this commit of a closed file, owned
    /// by the group. Null for a file that is not closed, whose commit holds
    /// the block (Snapshot::copy_sections finds it).
    LazyBlock* sections = nullptr;
    /// The commit object, null for a closed file, every commit of which is
    /// synced.
    Commit* commit = nullptr;
};

/// What a load left, and the error of a Failed one.
struct LoadStatus {
    LoadState state = LoadState::Unknown;
    dstd::Error error;
};

struct CopyData {
    std::size_t file_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    FoldKind fold = FoldKind::None;
    CopyState state = CopyState::Ready;
    /// The id of the copy in its file, which the sections of its commits name.
    std::uint32_t local_id = 0;
    /// The commits holding a section of this copy, oldest first. The copy is
    /// readable only once every one of them that is not synced has verified.
    dstd::Vector<CopyCommit> commits;
};

struct HiddenRange {
    dstd::Vector<std::uint8_t> begin;
    dstd::Vector<std::uint8_t> end;
};

/// Every copy and drop of one scope over the files of the snapshot, built from
/// their scope directories the first time a query needs the scope. The section
/// blocks of its copies are owned here, since two groups never share a
/// (commit, copy).
struct ScopeGroup {
    dstd::Vector<CopyData> copies;
    dstd::Vector<HiddenRange> hidden;
    dstd::Vector<dstd::UniquePtr<LazyBlock>> sections;
    std::uint64_t highest_generation = 0;
    bool dropped = false;
    std::size_t current = 0;
};

/// The groups of one leaf block's entries, one slot per entry, each set once
/// by the first query of its scope and owned here. The leaf whose entries hold
/// a scope first owns that scope's group.
class GroupSlots {
   public:
    GroupSlots() = default;
    GroupSlots(const GroupSlots&) = delete;
    GroupSlots& operator=(const GroupSlots&) = delete;
    GroupSlots(GroupSlots&&) noexcept = default;
    GroupSlots& operator=(GroupSlots&&) noexcept = delete;
    ~GroupSlots();

    /// Empty slots for `count` entries; called once, before any query.
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

/// The page slots of a closed file, one per page its section blocks name,
/// made a chunk at a time by the first query that reads a page of the chunk,
/// so an open allocates one pointer per chunk.
class PageSlots {
   public:
    PageSlots() = default;
    PageSlots(const PageSlots&) = delete;
    PageSlots& operator=(const PageSlots&) = delete;
    PageSlots(PageSlots&&) noexcept = default;
    PageSlots& operator=(PageSlots&&) noexcept = delete;
    ~PageSlots();

    /// Empty chunks for `count` pages; called once, before any query.
    void assign(std::size_t count);

    std::size_t size() const { return count_; }

    /// The slot of one page below size(), making its chunk on first use.
    PageSlot& slot(std::size_t index) const;

   private:
    static constexpr std::size_t CHUNK_PAGES = 256;
    struct Chunk {
        PageSlot slots[CHUNK_PAGES];
    };

    std::size_t count_ = 0;
    /// A cache the const queries of a snapshot fill, one compare-and-swap per
    /// chunk.
    mutable dstd::Vector<std::atomic<Chunk*>> chunks_;
};

struct FileData {
    explicit FileData(DataFileReader opened) : reader(std::move(opened)) {}

    dstd::String name;
    std::uint64_t writer_random = 0;
    DataFileReader reader;
    /// One per commit of the file, in sequence order. A closed file holds no
    /// commit object: every commit of it is synced.
    dstd::Vector<dstd::UniquePtr<Commit>> commits;
    /// The top table of a closed file's directory; empty for a file that is
    /// not closed.
    dstd::Vector<std::uint8_t> top_table;
    /// The separators of `top_table`. The vector keeps its buffer when the
    /// file data moves, so the view stays valid.
    SlotTable top;
    /// One block per separator of the top table, in table order: the blocks
    /// just below it, which are leaves when `directory_levels` is 1. A file
    /// that is not closed has one leaf block here, built at open.
    dstd::Vector<dstd::UniquePtr<LazyBlock>> roots;
    /// The levels of the directory below its top table, 1 or more.
    std::uint32_t directory_levels = 0;
    /// The pages of a closed file.
    PageSlots pages;
};

/// The counters of Snapshot::read_stats, shared by every thread that loads.
class ReadCounters {
   public:
    void add_index(const BlockReads& reads) {
        indexes_read_.fetch_add(1, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    void add(std::uint64_t commits, std::uint64_t blocks,
             const BlockReads& reads) {
        commits_verified_.fetch_add(commits, std::memory_order_relaxed);
        blocks_read_.fetch_add(blocks, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    /// One block of metadata, read whole: the directory blocks and the
    /// section blocks of a closed file.
    void add_metadata(const BlockReads& reads) {
        metadata_blocks_read_.fetch_add(1, std::memory_order_relaxed);
        read_calls_.fetch_add(reads.calls, std::memory_order_relaxed);
        bytes_read_.fetch_add(reads.bytes, std::memory_order_relaxed);
    }

    ReadStats read() const {
        return ReadStats{commits_verified_.load(std::memory_order_relaxed),
                         indexes_read_.load(std::memory_order_relaxed),
                         blocks_read_.load(std::memory_order_relaxed),
                         metadata_blocks_read_.load(std::memory_order_relaxed),
                         read_calls_.load(std::memory_order_relaxed),
                         bytes_read_.load(std::memory_order_relaxed)};
    }

   private:
    std::atomic<std::uint64_t> commits_verified_{0};
    std::atomic<std::uint64_t> indexes_read_{0};
    std::atomic<std::uint64_t> blocks_read_{0};
    std::atomic<std::uint64_t> metadata_blocks_read_{0};
    std::atomic<std::uint64_t> read_calls_{0};
    std::atomic<std::uint64_t> bytes_read_{0};
};

struct ValueSource {
    std::size_t file_index = 0;
    SectionView::Entry entry;
};

/// One section a cursor holds: the block it is in and its index there.
struct SectionLocation {
    LazyBlock* block = nullptr;
    std::size_t index = 0;
};

}  // namespace detail

/// The keys of one copy of one scope, in key order, with the sections holding
/// each key in commit order. A cursor is used by one thread.
class ScopeCursor {
   public:
    ScopeCursor() = default;
    ScopeCursor(const ScopeCursor&) = delete;
    ScopeCursor& operator=(const ScopeCursor&) = delete;
    ScopeCursor(ScopeCursor&&) noexcept = default;
    ScopeCursor& operator=(ScopeCursor&&) noexcept = default;

    bool at_end() const { return at_end_; }
    ByteView key() const { return key_; }
    /// Section indices of the copy holding the current key, oldest first.
    const dstd::Vector<std::size_t>& matching() const { return matching_; }
    const detail::CopyData* copy() const { return copy_; }

    void next();

   private:
    friend class Snapshot;
    friend class ScanCursor;

    /// Puts every section of the copy that can hold a key in [begin, end) on
    /// the heap, keyed by its first key from the record: no page is read until
    /// the section reaches the top. The page refs of a summary commit are read
    /// here, for the commits whose range meets [begin, end). Every commit of
    /// the copy that is not synced must be Good.
    void open(const Snapshot& snapshot, const detail::CopyData& copy,
              const dstd::Vector<detail::HiddenRange>& hidden, ByteView begin,
              ByteView end);
    void settle();
    void advance_matching();
    /// Loads the page of one section, stands its cursor at `begin_` and puts it
    /// back on the heap. False, with `error_` set, when the page cannot be
    /// used.
    bool load(std::size_t index);

    void push(std::size_t index);

    const Snapshot* snapshot_ = nullptr;
    /// The sections of every commit of the copy that can hold a key in range,
    /// oldest commit first.
    dstd::Vector<detail::SectionLocation> sections_;
    dstd::Vector<SectionView::Cursor> cursors_;
    /// The current key of each cursor, so the heap compares without parsing.
    /// For a section not loaded yet, the lower bound its record gives.
    dstd::Vector<ByteView> keys_;
    dstd::Vector<std::uint8_t> loaded_;
    /// Min-heap of the cursors not at their end, by key then section index.
    dstd::Vector<std::size_t> heap_;
    dstd::Vector<std::size_t> matching_;
    const dstd::Vector<detail::HiddenRange>* hidden_ = nullptr;
    const detail::CopyData* copy_ = nullptr;
    ByteView key_;
    ByteView begin_;
    ByteView end_;
    bool at_end_ = true;
    dstd::Optional<dstd::Error> error_;
};

/// A forward-only scan. It reads the snapshot it was made from, so it stays
/// valid only while that snapshot is alive and unmoved.
class ScanCursor {
   public:
    ScanCursor() = default;
    ScanCursor(const ScanCursor&) = delete;
    ScanCursor& operator=(const ScanCursor&) = delete;
    ScanCursor(ScanCursor&&) noexcept = default;
    ScanCursor& operator=(ScanCursor&&) noexcept = default;

    bool at_end() const { return at_end_; }
    ByteView key() const { return key_; }
    const ScopeKey& scope() const;
    dstd::Expected<ByteView, dstd::Error> value();

    /// Empty unless a page of one of the scopes could not be read, or a page of
    /// a synced commit failed its crc: a cursor that stopped on one is at its
    /// end. A copy with a bad commit that is not synced is skipped and leaves
    /// this empty.
    const dstd::Optional<dstd::Error>& error() const { return error_; }

    void next();

   private:
    friend class Snapshot;

    struct Arm {
        ScopeKey scope;
        const detail::ScopeGroup* group = nullptr;
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
    ByteView key_;
    bool at_end_ = true;
    /// Owned copies of the bounds, which the arms read until the scan ends.
    dstd::Vector<std::uint8_t> begin_storage_;
    dstd::Vector<std::uint8_t> end_storage_;
    dstd::Vector<detail::ValueSource> sources_;
    dstd::Vector<std::uint8_t> buffer_;
    dstd::Optional<dstd::Error> error_;
};

/// What one open read, and how long each of its phases took. The phases after
/// the listing run on the process-wide default runtime's threads, unless open
/// itself runs on one of them. No block is read at open: the reads happen when
/// a query needs them, and `Snapshot::read_stats` counts them.
struct OpenStats {
    /// The data files of the directory that were readable.
    std::size_t files = 0;
    /// The commits of those files.
    std::size_t commits = 0;
    /// The page refs the record chains list themselves. A checkpoint or a
    /// footer lists a commit by summary, and its page refs are read with its
    /// first query.
    std::size_t pages = 0;
    double ms_list = 0;        ///< Listing the directory.
    double ms_open_files = 0;  ///< Opening and indexing every data file.
    double ms_tables = 0;  ///< Grouping the commits and building the tables.
    double ms_total = 0;
};

/// One commit the full check found unreadable: a block whose crc or bounds do
/// not verify, or a section of the commit that does not open.
struct BadCommit {
    dstd::String file;
    std::uint64_t sequence = 0;
};

/// One section of a commit the full check could not walk: its bytes are not
/// there, its entries do not parse, or its keys do not strictly ascend.
struct BadSection {
    dstd::String file;
    /// The offset of the page ref, and of the section inside it.
    std::uint64_t page_offset = 0;
    std::uint32_t offset_in_page = 0;
};

/// What one full check read and what it found.
struct CheckReport {
    std::size_t files = 0;
    std::size_t commits = 0;
    /// The pages and big value runs checked, each once.
    std::size_t blocks = 0;
    /// The directory and section blocks of closed files checked.
    std::size_t metadata_blocks = 0;
    std::uint64_t bytes_read = 0;
    dstd::Vector<BadCommit> bad_commits;
    std::size_t sections = 0;
    dstd::Vector<BadSection> bad_sections;
};

/// The state of every data file in one directory, as of the moment it opened.
///
/// Immutable to callers after open, so the const methods are safe from many
/// threads: the metadata never changes, and each block a query needs is read
/// and checked once behind the state of its block, page or commit. A cursor
/// and the views it returns stay valid while the snapshot is alive and unmoved.
class Snapshot {
   public:
    /// Opens every data file of `dir`. A closed file costs one read of its
    /// end: the footer and the top table of its directory. A file that is not
    /// closed is read through its header and record chain, and its directory
    /// is built in memory. No page, directory block or section block of a
    /// closed file is read here; a query reads and checks each block it needs
    /// the first time it needs it. Listing, opening and indexing run in
    /// parallel on the process-wide default runtime, which is created if none
    /// is installed; a caller wanting thread control installs its own runtime
    /// with set_default_runtime. The phases run serially on the calling thread
    /// when a fork-join on that runtime could not finish: on a worker of the
    /// runtime itself, inside one of its parallel regions, and in the child of
    /// a fork, whose copy of an inherited runtime has no workers.
    static dstd::Expected<Snapshot, dstd::Error> open(dstd::StringView dir);

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    ~Snapshot() = default;

    dstd::Span<const SkippedFile> skipped() const { return skipped_; }

    /// What the open of this snapshot did, its phases and their durations.
    const OpenStats& open_stats() const { return open_stats_; }

    /// Every block read so far, cumulative and readable at any time. An open
    /// adds nothing to it, and a snapshot a move left empty has no counters.
    ReadStats read_stats() const {
        if (reads_ == nullptr) return ReadStats{};
        return reads_->read();
    }

    /// Reads and checks every block of every commit, and then walks every entry
    /// of every section, reporting what is wrong. It runs its phases in
    /// parallel on the process-wide default runtime, under the rules `open`
    /// follows, and it reports a commit whose section fails the full walk
    /// without changing the state of that commit: a commit already known to be
    /// bad keeps the state it has, and one that a query read Good stays Good.
    /// Blocks already read are not read again. An error is returned only when a
    /// block could not be read at all.
    dstd::Expected<CheckReport, dstd::Error> check() const;

    /// The highest generation of every (namespace, scope) a writer has to add
    /// one to, over copies and drops. Reads every directory block; the error
    /// of the first one that does not load, since a scope left out would get
    /// a generation that is too low.
    dstd::Expected<dstd::Vector<ScopeGeneration>, dstd::Error> generations()
        const;

    ScopeState state(const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// The value of `key`, or an empty Optional when the scope is absent,
    /// Failed or Bad, when the key is absent, or when a drop hides it. The
    /// error is the one a block of a commit of the copy failed to read with.
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
    /// Every current scope of the namespace in ScopeKey order. A directory
    /// block that does not load gives a cursor at its end with the error.
    ScanCursor scan_all(const NamespaceSpec& spec, ByteView begin,
                        ByteView end) const;

   private:
    friend class ScanCursor;
    friend class ScopeCursor;

    Snapshot() = default;

    /// The group of a scope: from the leaf block of the first file whose
    /// directory holds it, built and published there on its first query. Null
    /// when no file holds the scope, and the error of a block that had to be
    /// read to decide.
    dstd::Expected<const detail::ScopeGroup*, dstd::Error> find_group(
        const NamespaceSpec& spec, const ScopeKey& scope) const;

    /// Reads and decodes one block of metadata once, under its mutex, and
    /// stores Good, Bad or Failed with a release store. The fast path is one
    /// acquire load. A block built in memory at open is Good already.
    detail::LoadStatus load_block(const detail::FileData& file,
                                  detail::LazyBlock& block) const;
    /// Reads one page of a closed file once, under the slot's mutex. An index
    /// at or past the page count is Bad.
    detail::LoadStatus load_page(const detail::FileData& file,
                                 std::size_t page_index,
                                 const PageRef& ref) const;
    /// Reads and checks every block of one commit that is not synced once,
    /// under its mutex, and stores Good, Bad or Failed with a release store.
    /// The fast path of a commit a query already read is one acquire load.
    detail::LoadStatus verify_commit(detail::Commit& commit) const;
    /// Verifies every commit of one copy that is not synced, oldest first.
    /// Good only when all of them are; a Bad commit decides over a Failed one,
    /// since a copy whose data does not verify is unreadable whatever else
    /// could not be read.
    detail::LoadStatus ensure_copy(const detail::CopyData& copy) const;
    /// Loads the page of one block of a synced commit once, under the slot's
    /// mutex.
    detail::LoadStatus load_page(detail::Commit& commit,
                                 std::size_t block) const;
    /// Reads the page refs of a summary commit once, under its index mutex,
    /// and builds its blocks, pages and sections. The fast path is one acquire
    /// load.
    detail::LoadStatus load_index(detail::Commit& commit) const;
    /// The view of one section: its page loaded, or the error of the page. A
    /// page that failed its crc is a Parse error.
    dstd::Expected<const SectionView*, dstd::Error> section_view(
        const detail::FileData& file, detail::LazyBlock& block,
        std::size_t index) const;
    /// The section block of one copy in one commit, with what it needs read
    /// first: the commit index of a file that is not closed, or the section
    /// block of a closed one. Null when the commit holds no
    /// section of the copy; a block that does not verify is a Parse error.
    dstd::Expected<detail::LazyBlock*, dstd::Error> copy_sections(
        const detail::CopyData& copy, const detail::CopyCommit& held) const;
    dstd::Expected<void, dstd::Error> materialize(
        FoldKind fold, dstd::Span<const detail::ValueSource> sources,
        ByteView& view, dstd::Vector<std::uint8_t>& owned) const;
    dstd::Expected<void, dstd::Error> read_big(
        std::size_t file_index, BigRef ref, ByteView& view,
        dstd::Vector<std::uint8_t>& owned) const;

    dstd::Expected<void, dstd::Error> load(dstd::StringView dir);

    dstd::Vector<detail::FileData> files_;
    dstd::Vector<SkippedFile> skipped_;
    OpenStats open_stats_;
    /// Behind a pointer: the counters move once as a Snapshot is returned, and
    /// the commits of the snapshot add to counters any thread can read.
    dstd::UniquePtr<detail::ReadCounters> reads_;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_SNAPSHOT_H
