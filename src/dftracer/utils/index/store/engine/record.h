#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_RECORD_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_RECORD_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <variant>

namespace dftracer::utils::index::store::engine {

inline constexpr std::size_t RECORD_BYTES = 4096;

enum class RecordKind : std::uint32_t {
    Commit = 1,
    Checkpoint = 2,
    Footer = 3
};
enum class ScopeKind : std::uint8_t { Root = 0, File = 1, Files = 2 };
enum class CopyState : std::uint8_t { Ready = 0, Failed = 1 };

struct NamespaceItem {
    std::uint32_t local_id = 0;
    dstd::String name;
    std::uint32_t format_version = 0;
    std::uint8_t fold_kind = 0;
    std::uint8_t codec = 0;
    std::uint8_t durable = 0;
};

struct ScopePath {
    dstd::String logical_path;
    std::uint64_t mtime = 0;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
};

struct ScopeItem {
    std::uint32_t local_id = 0;
    ScopeKind kind = ScopeKind::Root;
    dstd::Vector<ScopePath> paths;
};

struct CopyItem {
    std::uint32_t local_id = 0;
    std::uint32_t namespace_id = 0;
    std::uint32_t scope_id = 0;
    std::uint64_t params_hash = 0;
    std::uint64_t generation = 0;
    CopyState state = CopyState::Ready;
};

struct DropItem {
    std::uint32_t namespace_id = 0;
    std::uint32_t scope_id = 0;
    std::uint64_t generation = 0;
    dstd::Vector<std::uint8_t> begin_key;
    dstd::Vector<std::uint8_t> end_key;
};

struct PageSection {
    std::uint32_t copy_id = 0;
    std::uint32_t offset_in_page = 0;
    std::uint32_t length = 0;
    std::uint64_t entry_count = 0;
    dstd::Vector<std::uint8_t> first_key;
    dstd::Vector<std::uint8_t> last_key;
};

struct PageRefItem {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
    std::uint64_t sequence = 0;
    /// 1 when the bytes of the ref reached the disk before the record that
    /// first listed it: a durable commit syncs its pages before its record.
    std::uint8_t synced = 0;
    dstd::Vector<PageSection> sections;
};

/// The key range one commit holds for one copy.
struct CommitCopy {
    std::uint32_t copy_id = 0;
    dstd::Vector<std::uint8_t> first_key;
    dstd::Vector<std::uint8_t> last_key;
};

/// One commit as a checkpoint or a footer lists it: the record that holds its
/// page refs, and the copies it holds a section of with their key ranges. The
/// page refs stay in that record, so a reader reads them only when it first
/// needs a copy of the commit.
struct CommitRefItem {
    std::uint64_t sequence = 0;
    std::uint64_t record_offset = 0;
    std::uint32_t record_crc = 0;
    /// 1 when every page ref of the commit is synced.
    std::uint8_t synced = 0;
    dstd::Vector<CommitCopy> copies;
};

/// The offset, length and crc of one block written after the commit records:
/// one pread reads it whole, and the crc covers its bytes as
/// compress::gzip_crc32 computes them.
struct BlockHandle {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;

    bool empty() const { return length == 0; }
};

/// The footer item that names the blocks of a closed file: the header fields
/// a light open needs, the items block, the number of pages the section blocks
/// name, and the directory the first query of a scope reads.
struct FileTail {
    /// The format version of the file, as the header holds it.
    std::uint32_t format_version = 0;
    std::uint32_t page_size = 0;
    std::uint64_t writer_random = 0;
    std::uint32_t pid = 0;
    dstd::String host;
    dstd::String label;
    std::uint64_t creation_time = 0;
    BlockHandle items;
    /// The distinct pages that hold a section; a section block names each by
    /// its index below this count.
    std::uint32_t page_count = 0;
    /// The levels below the top table; 1 when it points at leaves.
    std::uint32_t directory_levels = 0;
    dstd::Vector<std::uint8_t> top_table;
};

using RecordItem = std::variant<NamespaceItem, ScopeItem, CopyItem, DropItem,
                                PageRefItem, CommitRefItem, FileTail>;

using RecordItems = dstd::Vector<RecordItem>;

struct Record {
    RecordKind kind = RecordKind::Commit;
    std::uint64_t sequence = 0;
    std::uint64_t prev_offset = 0;
    std::uint64_t offset = 0;
    std::uint32_t crc = 0;
    RecordItems items;
};

/// The bytes of one block handle: offset, length, crc.
void append_block_handle(dstd::Vector<std::uint8_t>& out,
                         const BlockHandle& handle);

/// Reads the bytes of one block handle, leaving `at` past it.
bool read_block_handle(ByteView bytes, std::size_t& at, BlockHandle& out);

/// The body of a record: every item, in the order given. The blocks a footer
/// names hold their items this way.
dstd::Vector<std::uint8_t> encode_items(const RecordItems& items);

/// The items of one body, as encode_items wrote them.
dstd::Expected<RecordItems, dstd::Error> decode_items(ByteView body);

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, const RecordItems& items);

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, std::initializer_list<RecordItem> items);

dstd::Expected<Record, dstd::Error> decode_record(ByteView bytes);

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_RECORD_H
