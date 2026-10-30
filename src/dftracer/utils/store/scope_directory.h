#ifndef DFTRACER_UTILS_STORE_SCOPE_DIRECTORY_H
#define DFTRACER_UTILS_STORE_SCOPE_DIRECTORY_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/record.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::store {

/// The key range one commit holds for one copy of a file, and the block that
/// holds that copy's segments in that commit. A commit listed by summary
/// without a segment block carries an empty handle.
struct DirectoryCommit {
    std::uint64_t sequence = 0;
    std::uint32_t copy_id = 0;
    ByteView first_key;
    ByteView last_key;
    BlockHandle segments;
};

struct DirectoryErase {
    std::uint64_t ordinal = 0;
    ByteView begin;
    ByteView end;
};

/// The views of a copy point into the directory.
struct DirectoryCopy {
    std::uint32_t local_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    CopyState state = CopyState::Ready;
    std::uint8_t fold_kind = 0;
    /// Empty, or one per path of the entry's scope in its path order.
    dstd::Vector<SourceStat> sources;
    dstd::Vector<DirectoryCommit> commits;
    dstd::Vector<DirectoryErase> erases;
};

struct DirectoryDrop {
    std::uint64_t generation = 0;
    ByteView begin;
    ByteView end;
};

struct DirectoryKey {
    dstd::StringView name;
    std::uint32_t format_version = 0;
    ScopeKind kind = ScopeKind::Root;
    dstd::Vector<dstd::StringView> paths;
};

struct DirectoryEntry {
    DirectoryKey key;
    std::uint64_t highest_generation = 0;
    dstd::Vector<DirectoryCopy> copies;
    dstd::Vector<DirectoryDrop> drops;
};

/// Builds the scope directory of one file: one entry per namespace, format
/// version and scope, sorted by name, version, then scope kind and paths.
/// Copies, erases and drops whose namespace, scope or copy the items do not
/// name are left out, as a reader leaves them out.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> build_scope_directory(
    dstd::Span<const NamespaceItem> namespaces,
    dstd::Span<const ScopeItem> scopes, dstd::Span<const CopyItem> copies,
    dstd::Span<const DropItem> drops, dstd::Span<const EraseItem> erases,
    dstd::Span<const DirectoryCommit> commits);

struct TableEntry {
    ByteView key;
    ByteView payload;
};

/// Writes one slot table over `entries`, in the order given: a count, one
/// fixed slot per entry, then the keys and payloads.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_slot_table(
    dstd::Span<const TableEntry> entries);

struct TableBlock {
    dstd::Vector<std::uint8_t> bytes;
    std::size_t entries = 0;

    ByteView first_key() const;
};

/// Splits one slot table into blocks of at most `per_block` entries. An empty
/// table splits into no block.
dstd::Expected<dstd::Vector<TableBlock>, dstd::Error> split_slot_table(
    ByteView table, std::size_t per_block);

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_separator_table(
    dstd::Span<const TableBlock> children,
    dstd::Span<const BlockHandle> handles);

/// A slot table read in place: a sorted table of fixed slots over the keys and
/// payloads, so a lookup is a binary search and nothing is decoded until an
/// entry is used. The bytes must outlive the view.
class SlotTable {
   public:
    SlotTable() = default;

    /// Checks the slot table and that every slot lies in `bytes`. Empty bytes
    /// open as an empty table.
    static dstd::Expected<SlotTable, dstd::Error> open(ByteView bytes);

    std::size_t size() const { return count_; }

    ByteView key(std::size_t index) const;
    ByteView payload(std::size_t index) const;

   private:
    ByteView bytes_;
    std::size_t count_ = 0;
};

/// Compares a directory key with (name, version, kind, paths) in directory
/// order, decoding only as far as it must. A key that does not decode sorts
/// last. Zero means the same key.
int compare_directory_key(ByteView key, dstd::StringView name,
                          std::uint32_t version, ScopeKind kind,
                          dstd::Span<const dstd::String> paths);

/// A scope directory read in place: a sorted table of fixed slots over the
/// keys and payloads, so a lookup is a binary search and nothing is decoded
/// until an entry is used. The bytes must outlive the view.
class ScopeDirectory {
   public:
    ScopeDirectory() = default;

    /// Checks the slot table and that every slot lies in `bytes`. Empty bytes
    /// open as an empty directory.
    static dstd::Expected<ScopeDirectory, dstd::Error> open(ByteView bytes);

    std::size_t size() const { return count_; }

    std::size_t find(dstd::StringView name, std::uint32_t format_version,
                     ScopeKind kind,
                     dstd::Span<const dstd::String> paths) const;

    std::pair<std::size_t, std::size_t> namespace_range(
        dstd::StringView name, std::uint32_t format_version) const;

    dstd::Expected<DirectoryKey, dstd::Error> key(std::size_t index) const;
    dstd::Expected<std::uint64_t, dstd::Error> highest_generation(
        std::size_t index) const;
    dstd::Expected<DirectoryEntry, dstd::Error> entry(std::size_t index) const;

   private:
    ByteView key_bytes(std::size_t index) const;
    ByteView payload_bytes(std::size_t index) const;

    ByteView bytes_;
    std::size_t count_ = 0;
};

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_SCOPE_DIRECTORY_H
