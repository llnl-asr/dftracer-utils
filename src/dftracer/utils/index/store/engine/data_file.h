#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/record.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::index::store::engine {

inline constexpr std::size_t FILE_HEADER_BYTES = 4096;
inline constexpr std::size_t FILE_TRAILER_BYTES = 32;

struct PageRef {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
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

    int fd_ = -1;
    std::uint32_t page_size_ = 0;
    std::uint64_t next_offset_ = FILE_HEADER_BYTES;
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
    bool closed_ = false;
    bool latched_ = false;
    dstd::Error latched_error_;
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

   private:
    DataFileReader() = default;

    int fd_ = -1;
    std::uint64_t file_size_ = 0;
    std::size_t records_read_ = 0;
    FileIndex index_;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_DATA_FILE_H
