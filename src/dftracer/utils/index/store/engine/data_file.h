#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
#include <dftracer/utils/index/store/engine/record.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace dftracer::utils::index::store::engine {

inline constexpr std::size_t FILE_HEADER_BYTES = 4096;
inline constexpr std::size_t FILE_TRAILER_BYTES = 32;
/// Every this many sequences a commit record is a checkpoint: it lists every
/// item so far, with the earlier commits by summary, and a reader starts there.
inline constexpr std::uint64_t CHECKPOINT_INTERVAL = 64;
/// The most bytes one read run covers: page refs that are adjacent in the file
/// are read together up to this many bytes, so a run of them costs one preadv
/// instead of one pread per page. A ref longer than this is a run of its own.
inline constexpr std::size_t READ_RUN_BYTES = std::size_t{16} << 20;
/// The most bytes the top table of a scope directory may use; above it the
/// directory gains a level of middle blocks.
inline constexpr std::size_t DIRECTORY_TOP_BYTES = 32768;
/// Entries per directory leaf block and per middle block.
inline constexpr std::size_t DIRECTORY_BLOCK_ENTRIES = 256;
/// What a light open reads from the end of the file: the trailer, the footer
/// and, with them, the whole directory top table.
inline constexpr std::size_t TAIL_READ_BYTES = 65536;
/// The chunk verify_block reads with, so its memory stays bounded for a big
/// value run.
inline constexpr std::size_t VERIFY_CHUNK_BYTES = std::size_t{1} << 20;

struct PageRef {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
};

/// Where one page of a run reads: the buffer it fills, and the flag it sets
/// when the page does not verify. Both are owned by the caller.
struct PageTarget {
    dstd::Vector<std::uint8_t>* bytes = nullptr;
    std::uint8_t* bad = nullptr;
};

/// What a block read did: the pread/preadv calls it made, and the bytes they
/// returned.
struct BlockReads {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
};

struct IndexedPageRef {
    std::uint64_t sequence = 0;
    PageRefItem ref;
};

struct FileIndex {
    std::uint32_t page_size = 0;
    std::uint64_t writer_random = 0;
    std::uint32_t pid = 0;
    dstd::String host;
    dstd::String label;
    std::uint64_t creation_time = 0;
    // The sequence of the newest applied record; a footer has its own sequence.
    std::uint64_t last_sequence = 0;
    // The newest applied record is a footer. A close syncs every page before
    // it writes the footer, so every page ref of a closed file is on disk.
    bool closed = false;
    dstd::Vector<NamespaceItem> namespaces;
    dstd::Vector<ScopeItem> scopes;
    dstd::Vector<CopyItem> copies;
    dstd::Vector<DropItem> drops;
    /// The page refs the chain lists itself: those of the commits after the
    /// newest checkpoint, and the checkpoint's own.
    dstd::Vector<IndexedPageRef> page_refs;
    /// The commits a checkpoint or the footer lists by summary. Their page
    /// refs stay in their records; read_commit reads them.
    dstd::Vector<CommitRefItem> commit_refs;
    /// The footer of a closed file, which names its items block, its page
    /// table and its scope directory. Absent for a file that is not closed.
    dstd::Optional<FileTail> tail;
};

dstd::String data_file_name(dstd::StringView host, std::uint32_t pid,
                            std::uint64_t random, dstd::StringView label);

class DataFileWriter {
   public:
    static dstd::Expected<DataFileWriter, dstd::Error> create(
        dstd::StringView path, std::uint32_t page_size, dstd::StringView host,
        dstd::StringView label);

    DataFileWriter(const DataFileWriter&) = delete;
    DataFileWriter& operator=(const DataFileWriter&) = delete;
    DataFileWriter(DataFileWriter&& other) noexcept;
    DataFileWriter& operator=(DataFileWriter&& other) noexcept;
    ~DataFileWriter();

    dstd::Expected<PageRef, dstd::Error> append_page(ByteView bytes);
    dstd::Expected<PageRef, dstd::Error> append_big(ByteView bytes);

    /// Takes `length` bytes at the first offset that is a multiple of `align`
    /// at or after the file end, and moves the end past them. Safe from any
    /// thread: one compare-and-swap hands out a range, no range is ever
    /// reused and the range belongs to the caller before it returns. `align`
    /// must not be zero. `kind` names the range only so a reserved page region
    /// reaches the fault recorder there; a range of any other kind is recorded
    /// with its bytes where they are written.
    std::uint64_t reserve_range(
        std::uint64_t length, std::uint64_t align,
        io_fault::WriteKind kind = io_fault::WriteKind::Page);

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
    /// Every item a checkpoint or the footer lists, with the page refs of the
    /// commit not yet written.
    RecordItems items_with_pending_pages() const;
    /// Summarizes the page refs of the record just written into one commit ref
    /// per sequence, which later checkpoints and the footer list.
    void add_commit_refs();
    /// Builds the section block of every copy of every commit, the directory
    /// blocks and the items block of a closed file into `out`, as they land
    /// when `out` is written at `base`, and returns the footer item that names
    /// them. Each block is read by one pread, and its crc covers its bytes.
    dstd::Expected<FileTail, dstd::Error> build_blocks(
        const RecordItems& items, std::uint64_t base,
        dstd::Vector<std::uint8_t>& out) const;
    dstd::Expected<void, dstd::Error> write_record(RecordKind kind,
                                                   const RecordItems& items);
    void latch(const dstd::Error& error);
    /// True once a write failed, which every later write must report.
    bool latched() const noexcept {
        return latched_.load(std::memory_order_acquire);
    }
    /// The first write error, meaningful once latched().
    dstd::Error latched_result() const;

    int fd_ = -1;
    std::uint32_t page_size_ = 0;
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
    /// Every committed page ref, in commit order: close() builds the page
    /// table, the section blocks and the directory from them without reading
    /// the file again.
    dstd::Vector<PageRefItem> committed_pages_;
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

    DataFileReader(const DataFileReader&) = delete;
    DataFileReader& operator=(const DataFileReader&) = delete;
    DataFileReader(DataFileReader&& other) noexcept;
    DataFileReader& operator=(DataFileReader&& other) noexcept;
    ~DataFileReader();

    const FileIndex& index() const noexcept { return index_; }
    std::size_t records_read() const noexcept { return records_read_; }

    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> read_page(
        const PageRef& ref) const;
    /// Reads one metadata block whole: one pread, then the crc of its bytes.
    /// Parse when the block lies outside the file or its crc fails, Io when
    /// the read itself fails. `reads` counts the call and its bytes.
    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> read_block(
        const BlockHandle& handle, BlockReads* reads) const;
    /// Checks the crc of a block without keeping its bytes: reads it in
    /// VERIFY_CHUNK_BYTES chunks, so memory stays bounded for a big value run.
    /// Parse when the block is out of the file or its crc fails, as read_page
    /// does. `reads` counts the pread calls it made and the bytes they
    /// returned.
    dstd::Expected<void, dstd::Error> verify_block(const PageRef& ref,
                                                   BlockReads* reads) const;
    /// Reads the pages of one run into their own buffers, one preadv per group
    /// of at most IOV_MAX refs with one iovec per page pointing straight into
    /// that page's buffer, then checks each page crc. `refs` and `targets` are
    /// parallel spans of equal length; `refs` must be in ascending offset order
    /// with each ref starting where the previous one ends, so one preadv covers
    /// a group. A ref of zero length or out of the file, and a page whose crc
    /// fails, is left empty and marked bad through its target. A short read
    /// past the end of the file (a truncated file) leaves the pages it did not
    /// fill empty and bad, as read_page reports them. The only error is Io,
    /// when a read call itself fails, and the caller must then treat the whole
    /// file as unreadable. The bytes of a bad page are dropped.
    dstd::Expected<BlockReads, dstd::Error> read_pages(
        dstd::Span<const PageRef> refs,
        dstd::Span<const PageTarget> targets) const;
    /// The page refs of one commit a summary lists, read from its record. Parse
    /// when the record does not decode, sits elsewhere or is not the one the
    /// summary names by crc; Io when the read fails. `reads` counts the calls
    /// and bytes.
    dstd::Expected<dstd::Vector<PageRefItem>, dstd::Error> read_commit(
        const CommitRefItem& ref, BlockReads* reads) const;
    /// Every page ref of the file in commit order: those the chain lists, and
    /// those of every commit summary, read from their records.
    dstd::Expected<dstd::Vector<IndexedPageRef>, dstd::Error> all_page_refs()
        const;

   private:
    DataFileReader() = default;

    /// True when one read of the file end gave a footer with a FileTail: the
    /// header fields, the namespaces and the tail of it, and nothing else.
    /// False falls back to the full open of the chain. Unsupported when the
    /// tail names another format version.
    dstd::Expected<bool, dstd::Error> open_light(std::uint64_t file_size);
    /// Applies the items block a footer names, as the footer's own items, so
    /// the index of a full open holds every scope, copy, drop and commit
    /// summary of the file.
    dstd::Expected<void, dstd::Error> apply_items_block();

    int fd_ = -1;
    std::uint64_t file_size_ = 0;
    std::size_t records_read_ = 0;
    FileIndex index_;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
