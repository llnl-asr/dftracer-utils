#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
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
/// The most bytes one read run covers: page refs that are adjacent in the file
/// are read together up to this many bytes, so a run of them costs one preadv
/// instead of one pread per page. A ref longer than this is a run of its own.
inline constexpr std::size_t READ_RUN_BYTES = std::size_t{16} << 20;
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
    dstd::Vector<NamespaceItem> namespaces;
    dstd::Vector<ScopeItem> scopes;
    dstd::Vector<CopyItem> copies;
    dstd::Vector<DropItem> drops;
    dstd::Vector<IndexedPageRef> page_refs;
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
    /// The committer thread closes and latches while commit threads read, so
    /// both are flags. The error behind `latched_` is written first and read
    /// under `error_mutex_`.
    std::atomic<bool> closed_{false};
    std::atomic<bool> latched_{false};
    dstd::Error latched_error_;
    mutable std::mutex error_mutex_;
};

class DataFileReader {
   public:
    static dstd::Expected<DataFileReader, dstd::Error> open(
        dstd::StringView path);

    DataFileReader(const DataFileReader&) = delete;
    DataFileReader& operator=(const DataFileReader&) = delete;
    DataFileReader(DataFileReader&& other) noexcept;
    DataFileReader& operator=(DataFileReader&& other) noexcept;
    ~DataFileReader();

    const FileIndex& index() const noexcept { return index_; }
    std::size_t records_read() const noexcept { return records_read_; }

    dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> read_page(
        const PageRef& ref) const;
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

   private:
    DataFileReader() = default;

    int fd_ = -1;
    std::uint64_t file_size_ = 0;
    std::size_t records_read_ = 0;
    FileIndex index_;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
