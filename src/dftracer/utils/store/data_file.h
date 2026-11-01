#ifndef DFTRACER_UTILS_STORE_DATA_FILE_H
#define DFTRACER_UTILS_STORE_DATA_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/raw_buffer.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/io_fault.h>
#include <dftracer/utils/store/record.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace dftracer::utils::store::engine {

inline constexpr std::size_t FILE_HEADER_BYTES = 4096;
inline constexpr std::size_t FILE_TRAILER_BYTES = 32;
/// Every this many sequences a commit record is a checkpoint: it lists every
/// item so far, with the earlier commits by summary, and a reader starts there.
inline constexpr std::uint64_t CHECKPOINT_INTERVAL = 64;
/// The most bytes one read run covers: adjacent extent refs are read together
/// with one preadv. A longer ref is a run of its own.
inline constexpr std::size_t READ_RUN_BYTES = std::size_t{16} << 20;
/// Every extent starts at a multiple of this many bytes and its stored form is
/// padded to one, so extents lie end to end whatever their compressed length.
inline constexpr std::size_t EXTENT_ALIGN_BYTES = 4096;
/// The most bytes the top table of a scope directory may use. Above it the
/// directory gains a level of middle blocks.
inline constexpr std::size_t DIRECTORY_TOP_BYTES = 32768;
inline constexpr std::size_t DIRECTORY_BLOCK_ENTRIES = 256;
/// The most bytes one segment block holds: the runs of one copy of many
/// commits are packed up to this size, so a cold read costs few block reads.
inline constexpr std::size_t SEGMENT_BLOCK_BYTES = std::size_t{256} << 10;
/// What a light open reads from the end of the file: the trailer, the footer
/// and, with them, the whole directory top table.
inline constexpr std::size_t TAIL_READ_BYTES = 65536;
/// The chunk verify_block reads with, so its memory stays bounded for a big
/// value run.
inline constexpr std::size_t VERIFY_CHUNK_BYTES = std::size_t{1} << 20;

struct ExtentRef {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
};

/// Where one extent of a run reads: the buffer it fills, and the flag it sets
/// when the extent does not verify. Both are owned by the caller.
struct ExtentTarget {
    dstd::Vector<std::uint8_t>* bytes = nullptr;
    std::uint8_t* bad = nullptr;
};

struct BlockReads {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

struct IndexedExtentRef {
    std::uint64_t sequence = 0;
    ExtentRefItem ref;
};

struct FileIndex {
    std::uint32_t extent_size = 0;
    std::uint64_t writer_random = 0;
    std::uint32_t pid = 0;
    dstd::String host;
    dstd::String label;
    std::uint64_t creation_time = 0;
    std::uint64_t last_sequence = 0;
    // The newest applied record is a footer. A close syncs every extent before
    // it writes the footer, so every extent ref of a closed file is on disk.
    bool closed = false;
    dstd::Vector<NamespaceItem> namespaces;
    dstd::Vector<ScopeItem> scopes;
    dstd::Vector<CopyItem> copies;
    dstd::HashMap<std::uint32_t, std::size_t> copy_at;
    dstd::Vector<DropItem> drops;
    dstd::Vector<EraseItem> erases;
    /// The extent refs the chain lists itself: those of the commits after the
    /// newest checkpoint, and the checkpoint's own.
    dstd::Vector<IndexedExtentRef> extent_refs;
    /// The commits a checkpoint or the footer lists by summary. Their extent
    /// refs stay in their records. read_commit reads them.
    dstd::Vector<CommitRefItem> commit_refs;
    /// The footer of a closed file, which names its items block, its extent
    /// table and its scope directory. Absent for a file that is not closed.
    dstd::Optional<FileTail> tail;
};

dstd::String data_file_name(dstd::StringView host, std::uint32_t pid,
                            std::uint64_t random, dstd::StringView label);

/// The suffix every data file's name ends with, so a store directory's data
/// files are recognized by name alone.
inline constexpr std::string_view DATA_FILE_SUFFIX = ".dfs";

/// Moves every data file of the store directory `from` into `to`, which is
/// created when it is missing, and returns how many moved. Each move is a
/// rename, so both directories must share a filesystem. On an Io error the
/// files already moved stay moved and `from` keeps the rest, so a later call
/// continues.
dstd::Expected<std::size_t, dstd::Error> move_data_files(dstd::StringView from,
                                                         dstd::StringView to);

/// The file of a store directory that lists the footer of every closed data
/// file, so a snapshot learns a file's directory without opening it. A writer
/// appends an entry when its close succeeds. The summary is a cache: a file it
/// does not list is read as if there were no summary.
inline constexpr std::string_view SUMMARY_FILE_NAME = "summary.dfsum";

struct SummaryEntry {
    dstd::String name;
    /// The bytes the file holds at least: every block its tail names.
    std::uint64_t needed_bytes = 0;
    std::uint64_t sequence = 0;
    RecordItems footer;
};

/// Every entry of the summary of `dir`, in the order written, up to the first
/// that does not verify. Empty when the directory has no summary. Io when it
/// cannot be read.
dstd::Expected<dstd::Vector<SummaryEntry>, dstd::Error> read_summary(
    dstd::StringView dir);

class DataFileWriter {
   public:
    static dstd::Expected<DataFileWriter, dstd::Error> create(
        dstd::StringView path, std::uint32_t extent_size, dstd::StringView host,
        dstd::StringView label);

    DataFileWriter(const DataFileWriter&) = delete;
    DataFileWriter& operator=(const DataFileWriter&) = delete;
    DataFileWriter(DataFileWriter&& other) noexcept;
    DataFileWriter& operator=(DataFileWriter&& other) noexcept;
    ~DataFileWriter();

    /// Writes the stored form of one extent of raw bytes (extent_codec.h) at
    /// an offset aligned to EXTENT_ALIGN_BYTES and returns its ref. Segment
    /// offsets inside the extent count from the raw bytes.
    dstd::Expected<ExtentRef, dstd::Error> append_extent(dstd::ByteView bytes);
    /// Writes one value too big for an extent in the stored form encode_meta
    /// gives it, compressed at BIG_ZSTD_LEVEL when that saves an eighth, and
    /// returns the ref of the stored bytes.
    dstd::Expected<ExtentRef, dstd::Error> append_big(dstd::ByteView bytes);
    /// Writes the bytes of a page dictionary at an offset aligned to
    /// EXTENT_ALIGN_BYTES, padded to a multiple of it, and returns where they
    /// lie. The write is done when this returns, so an extent that names the
    /// dictionary is written after it. Safe from many threads at once.
    dstd::Expected<DictRef, dstd::Error> append_dict(dstd::ByteView bytes);

    /// Takes `length` bytes at the first multiple of `align` (not zero) at or
    /// after the file end, and moves the end past them. Safe from any thread.
    /// No range is ever reused. `kind` lets a reserved extent region reach the
    /// fault recorder there. Any other kind is recorded where written.
    std::uint64_t reserve_range(
        std::uint64_t length, std::uint64_t align,
        io_fault::WriteKind kind = io_fault::WriteKind::Extent);

    /// The descriptor this writer appends to, -1 once closed.
    int fd() const noexcept { return fd_; }
    void add(RecordItem item);
    dstd::Expected<void, dstd::Error> commit();
    dstd::Expected<void, dstd::Error> close();

   private:
    friend struct DataFileWriterTestHook;

    DataFileWriter() = default;

    dstd::Expected<void, dstd::Error> write_header();
    dstd::Expected<void, dstd::Error> sync_data();
    RecordItems items_with_pending_extents() const;
    void add_commit_refs();
    /// Builds the segment blocks, directory blocks and items block of a closed
    /// file into `out` as they land when `out` is written at `base`, and
    /// returns the footer item that names them.
    dstd::Expected<FileTail, dstd::Error> build_blocks(
        const RecordItems& items, std::uint64_t base,
        dstd::Vector<std::uint8_t>& out) const;
    dstd::Expected<void, dstd::Error> write_record(RecordKind kind,
                                                   const RecordItems& items);
    void latch(const dstd::Error& error);
    bool latched() const noexcept {
        return latched_.load(std::memory_order_acquire);
    }
    dstd::Error latched_result() const;

    int fd_ = -1;
    dstd::String path_;
    std::uint32_t extent_size_ = 0;
    std::atomic<std::uint64_t> next_offset_{FILE_HEADER_BYTES};
    std::uint64_t last_sequence_ = 0;
    std::uint64_t tail_offset_ = 0;
    std::uint32_t tail_kind_ = 0;
    std::uint32_t tail_crc_ = 0;
    std::uint64_t writer_random_ = 0;
    std::uint32_t pid_ = 0;
    std::uint64_t creation_time_ = 0;
    dstd::String host_;
    dstd::String label_;
    RecordItems pending_items_;
    RecordItems all_items_;
    dstd::HashMap<std::uint32_t, std::size_t> copy_item_at_;
    /// Every committed extent ref, in commit order, so close() builds the
    /// tables without reading the file again.
    dstd::Vector<ExtentRefItem> committed_extents_;
    /// The committer thread closes and latches while commit threads read, so
    /// both are flags. The error behind `latched_` is written first and read
    /// under `error_mutex_`.
    std::atomic<bool> closed_{false};
    std::atomic<bool> latched_{false};
    dstd::Error latched_error_;
    mutable std::mutex error_mutex_;
};

/// What DataFileReader::open reads of a closed file: everything, its items
/// block too, or only the end of the file, which a snapshot needs.
enum class FooterItems : std::uint8_t { All = 0, Directory = 1 };

class DataFileReader {
   public:
    /// Opens a data file and indexes its newest state. With
    /// FooterItems::Directory a closed file is read through one read of its
    /// end: the index holds its header fields, namespaces and tail, and its
    /// `scopes`, `copies`, `drops` and commit summaries stay empty.
    static dstd::Expected<DataFileReader, dstd::Error> open(
        dstd::StringView path, FooterItems footer = FooterItems::All);
    /// The reader of the closed file at `path` as its summary entry lists it,
    /// with nothing read. The file opens on the first read, which fails with
    /// Parse when the file is shorter than the entry's needed bytes. Parse when
    /// the entry holds no tail or one this reader cannot use.
    static dstd::Expected<DataFileReader, dstd::Error> from_summary(
        dstd::StringView path, const SummaryEntry& entry);

    DataFileReader(const DataFileReader&) = delete;
    DataFileReader& operator=(const DataFileReader&) = delete;
    DataFileReader(DataFileReader&& other) noexcept;
    DataFileReader& operator=(DataFileReader&& other) noexcept;
    ~DataFileReader();

    const FileIndex& index() const noexcept { return index_; }
    std::uint64_t file_size() const noexcept { return file_size_; }
    std::size_t records_read() const noexcept { return records_read_; }

    /// Reads one extent whole and checks its crc: the stored bytes, as an
    /// extent holds them. Parse when the block is out of the file or its crc
    /// fails, Io when the read fails.
    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> read_extent(
        const ExtentRef& ref) const;
    /// Reads `length` bytes at `offset` into `out`, which it resizes, with no
    /// crc: the caller checks what it reads, such as the pages of an extent.
    /// Parse when the range lies outside the file, Io when the read fails.
    /// `reads` counts the call and its bytes.
    dstd::Expected<void, dstd::Error> read_range(
        std::uint64_t offset, std::size_t length,
        dstd::RawBuffer<std::uint8_t>& out, BlockReads* reads) const;
    /// Reads `length` bytes at `offset` in VERIFY_CHUNK_BYTES chunks and keeps
    /// none, so the operating system caches the range. A range past the end of
    /// the file stops at the end. Io when a read fails. `reads` counts them.
    dstd::Expected<void, dstd::Error> warm_range(std::uint64_t offset,
                                                 std::uint64_t length,
                                                 BlockReads* reads) const;
    /// Reads one metadata block whole: one pread, then the crc of its bytes.
    /// Parse when the block lies outside the file or its crc fails, Io when
    /// the read itself fails. `reads` counts the call and its bytes.
    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> read_block(
        const BlockHandle& handle, BlockReads* reads) const;
    /// Checks the crc of a block in VERIFY_CHUNK_BYTES chunks, so memory stays
    /// bounded for a big value run. Parse when the block is out of the file or
    /// its crc fails. `reads` counts the calls and bytes.
    dstd::Expected<void, dstd::Error> verify_block(const ExtentRef& ref,
                                                   BlockReads* reads) const;
    /// Reads the extents of one run, one preadv per group of at most IOV_MAX
    /// refs, then checks each crc. `refs` and `targets` are parallel, and
    /// `refs` must ascend, each starting where the previous ends. A bad or
    /// truncated extent is left empty and marked bad. Io, the only error, ends
    /// the file.
    dstd::Expected<BlockReads, dstd::Error> read_extents(
        dstd::Span<const ExtentRef> refs,
        dstd::Span<const ExtentTarget> targets) const;
    /// The extent refs of one commit a summary lists, read from its record.
    /// Parse when the record does not decode, sits elsewhere or is not the one
    /// the summary names by crc. Io when the read fails. `reads` counts the
    /// calls and bytes.
    dstd::Expected<dstd::Vector<ExtentRefItem>, dstd::Error> read_commit(
        const CommitRefItem& ref, BlockReads* reads) const;
    /// Every extent ref of the file in commit order: those the chain lists, and
    /// those of every commit summary, read from their records.
    dstd::Expected<dstd::Vector<IndexedExtentRef>, dstd::Error>
    all_extent_refs() const;

   private:
    DataFileReader() = default;

    /// The descriptor of the file, opened here on the first call of a reader
    /// built from a summary entry.
    dstd::Expected<int, dstd::Error> descriptor() const;
    /// Indexes a closed file from the namespaces and the tail of its footer.
    /// False when the footer holds no tail or one that `file_size` bytes
    /// cannot hold.
    dstd::Expected<bool, dstd::Error> apply_footer(const RecordItems& items,
                                                   std::uint64_t sequence,
                                                   std::uint64_t file_size);

    /// True when one read of the file end gave a footer with a FileTail. False
    /// falls back to the full open of the chain. Unsupported when the tail
    /// names another format version.
    dstd::Expected<bool, dstd::Error> open_light(std::uint64_t file_size);
    /// Applies the items block a footer names, so the index of a full open
    /// holds every scope, copy, drop and commit summary.
    dstd::Expected<void, dstd::Error> apply_items_block();

    mutable std::atomic<int> fd_{-1};
    std::uint64_t file_size_ = 0;
    std::size_t records_read_ = 0;
    FileIndex index_;
    dstd::String path_;
    dstd::UniquePtr<std::mutex> open_mutex_;
};

/// Where the bytes of one closed data file are. Each category counts bytes no
/// other category counts, except extent_padding_bytes, which extent_bytes
/// already holds. The sum is exact: header + extent + record + gap +
/// segment_block + items_block + directory_leaf + directory_separator + footer.
struct SpaceReport {
    std::uint64_t file_bytes = 0;
    std::uint64_t header_bytes = 0;
    std::uint64_t extents = 0;
    /// The stored bytes of every extent, the padding inside it included.
    std::uint64_t extent_bytes = 0;
    /// The page tables of the extents, their headers left out.
    std::uint64_t extent_page_table_bytes = 0;
    std::uint64_t page_bytes = 0;
    std::uint64_t page_raw_bytes = 0;
    std::uint64_t pages = 0;
    /// The pages stored with zstd, with a dictionary or without.
    std::uint64_t pages_compressed = 0;
    /// The pages stored with zstd and a dictionary.
    std::uint64_t pages_dict = 0;
    std::uint64_t dicts = 0;
    std::uint64_t dict_bytes = 0;
    /// The zero bytes an extent holds past its last page: part of extent_bytes.
    std::uint64_t extent_padding_bytes = 0;
    /// What no other line counts: the bytes that align a range, the runs that
    /// hold a big value outside an extent, and any byte after the trailer.
    std::uint64_t gap_bytes = 0;
    std::uint64_t record_bytes = 0;
    std::uint64_t segment_block_bytes = 0;
    std::uint64_t segment_entries = 0;
    /// The first and last keys the segment entries hold.
    std::uint64_t segment_key_bytes = 0;
    std::uint64_t items_block_bytes = 0;
    std::uint64_t directory_leaf_bytes = 0;
    std::uint64_t directory_separator_bytes = 0;
    std::uint64_t directory_entries = 0;
    /// The keys of the directory entries, as a block stores them.
    std::uint64_t directory_key_bytes = 0;
    /// The first and last keys of the per-commit ranges of the entries.
    std::uint64_t directory_commit_key_bytes = 0;
    std::uint64_t filter_bytes = 0;
    /// The footer record, the top table it holds and the trailer.
    std::uint64_t footer_bytes = 0;
    std::uint64_t copies = 0;
    std::uint64_t commits = 0;
};

/// The space report of one closed data file. It decodes no page. Parse when the
/// file is not closed, when a record, an extent or a block does not verify, or
/// when two categories claim the same bytes. Io when a read fails.
dstd::Expected<SpaceReport, dstd::Error> space_report(dstd::StringView path);

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_DATA_FILE_H
