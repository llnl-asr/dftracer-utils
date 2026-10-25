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
    dstd::Vector<PageSection> sections;
};

using RecordItem =
    std::variant<NamespaceItem, ScopeItem, CopyItem, DropItem, PageRefItem>;
using RecordItems = dstd::Vector<RecordItem>;

struct Record {
    RecordKind kind = RecordKind::Commit;
    std::uint64_t sequence = 0;
    std::uint64_t prev_offset = 0;
    std::uint64_t offset = 0;
    std::uint32_t crc = 0;
    RecordItems items;
};

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, const RecordItems& items);

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, std::initializer_list<RecordItem> items);

dstd::Expected<Record, dstd::Error> decode_record(ByteView bytes);

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_RECORD_H
