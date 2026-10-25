#ifndef DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H
#define DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/snapshot.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::store::engine {

struct WriterOptions {
    std::uint32_t page_size = 128 * 1024;
    dstd::String host;
    dstd::String label;
};

/// A namespace inside one writer. Names are unique per writer.
struct NamespaceId {
    std::uint32_t value = 0;

    friend bool operator==(NamespaceId a, NamespaceId b) {
        return a.value == b.value;
    }
};

class Writer;

/// The entries of one commit. One batch is atomic: it becomes visible whole or
/// not at all. Entries are copied into the batch, so the caller's buffers may
/// go away.
class Batch {
   public:
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) noexcept = default;
    Batch& operator=(Batch&&) noexcept = default;
    ~Batch() = default;

    /// InvalidArgument on a fold namespace, reported by commit.
    void put(ByteView key, ByteView value);
    /// InvalidArgument on a non-fold namespace, and when a numeric fold gets a
    /// value that is not 8 bytes. Reported by commit.
    void fold(ByteView key, ByteView value);

   private:
    friend class Writer;

    struct Entry {
        std::size_t key_offset = 0;
        std::size_t key_length = 0;
        std::size_t value_offset = 0;
        std::size_t value_length = 0;
    };

    Batch() = default;

    ByteView view_at(std::size_t offset, std::size_t length) const;
    void add(ByteView key, ByteView value);
    void record_error(dstd::Condition condition);

    const Writer* writer_ = nullptr;
    NamespaceId namespace_id_;
    ScopeKey scope_;
    std::uint64_t params_hash_ = 0;
    dstd::Vector<ScopePath> signature_;
    FoldKind fold_ = FoldKind::None;
    dstd::Optional<dstd::Error> error_;
    dstd::Vector<Entry> entries_;
    dstd::Vector<std::uint8_t> arena_;
};

/// One data file of one process. Entries become visible at commit.
///
/// Not safe from several threads: a writer is driven by one thread at a time.
class Writer {
   public:
    static dstd::Expected<Writer, dstd::Error> create(
        dstd::StringView dir, const WriterOptions& options = WriterOptions{});

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) noexcept = default;
    Writer& operator=(Writer&&) noexcept = default;
    ~Writer() = default;

    dstd::Expected<NamespaceId, dstd::Error> add_namespace(
        const NamespaceSpec& spec);

    /// `scope_paths`, when given, holds one signature per path of `scope`; its
    /// mtime, size and hash are stored with the scope. Without it they are
    /// zero.
    Batch batch(NamespaceId namespace_id, const ScopeKey& scope,
                std::uint64_t params_hash,
                const dstd::Vector<ScopePath>* scope_paths = nullptr);

    dstd::Expected<void, dstd::Error> commit(Batch&& batch);
    /// A copy in state Failed with no entries, under the next generation.
    dstd::Expected<void, dstd::Error> fail(NamespaceId namespace_id,
                                           const ScopeKey& scope,
                                           std::uint64_t params_hash);
    /// Hides every entry of the scope, including later copies with a lower
    /// generation. Ends the writer's current copy of that scope.
    dstd::Expected<void, dstd::Error> drop(NamespaceId namespace_id,
                                           const ScopeKey& scope);
    /// Hides the keys of [begin, end) of the current copy, with an empty
    /// `end` meaning to the last key. Both empty is a whole-scope drop.
    dstd::Expected<void, dstd::Error> drop(NamespaceId namespace_id,
                                           const ScopeKey& scope,
                                           ByteView begin, ByteView end);

    dstd::Expected<void, dstd::Error> close();

    /// The file name and the full path of the data file this writer owns.
    const dstd::String& name() const { return name_; }
    const dstd::String& path() const { return path_; }
    std::uint32_t page_size() const { return page_size_; }

   private:
    struct NamespaceEntry {
        NamespaceId id;
        NamespaceSpec spec;
        bool announced = false;
    };

    struct ScopeEntry {
        std::uint32_t id = 0;
        ScopeKey key;
        bool announced = false;
    };

    struct CopyEntry {
        std::uint32_t namespace_id = 0;
        ScopeKey scope;
        std::uint32_t scope_id = 0;
        std::uint32_t copy_id = 0;
        std::uint64_t generation = 0;
        std::uint64_t highest_generation = 0;
        bool live = false;
        bool announced = false;
    };

    struct SectionBytes {
        dstd::Vector<std::uint8_t> bytes;
        std::uint64_t entry_count = 0;
        dstd::Vector<std::uint8_t> first_key;
        dstd::Vector<std::uint8_t> last_key;
    };

    Writer() = delete;
    Writer(dstd::String name, dstd::String path, std::uint32_t page_size,
           dstd::Vector<ScopeGeneration> generations, DataFileWriter file);

    NamespaceEntry* find_namespace(std::uint32_t id);
    NamespaceEntry* find_namespace(dstd::StringView name);
    ScopeEntry* find_scope(const ScopeKey& scope);
    ScopeEntry* add_scope(const ScopeKey& scope);
    CopyEntry* find_copy(std::uint32_t namespace_id, const ScopeKey& scope);
    CopyEntry* add_copy(NamespaceId id, const ScopeKey& scope,
                        std::uint32_t scope_id, std::uint64_t generation,
                        bool live);
    CopyEntry* find_or_add_copy(const NamespaceSpec& spec, NamespaceId id,
                                const ScopeKey& scope, std::uint32_t scope_id);
    std::uint64_t generation_of(const NamespaceSpec& spec,
                                const ScopeKey& scope) const;

    dstd::Expected<dstd::Vector<SectionBytes>, dstd::Error> build_sections(
        const Batch& batch);
    dstd::Expected<void, dstd::Error> write_pages(
        std::uint32_t copy_id, const dstd::Vector<SectionBytes>& sections,
        dstd::Vector<RecordItem>& items);
    dstd::Expected<void, dstd::Error> write_commit(
        dstd::Vector<RecordItem>& items);
    void add_namespace_item(const NamespaceSpec& spec, NamespaceId id,
                            dstd::Vector<RecordItem>& items);
    void add_scope_item(const ScopeEntry& entry,
                        const dstd::Vector<ScopePath>* signature,
                        dstd::Vector<RecordItem>& items);
    void latch(const dstd::Error& error);

    static BigRef place_big(void* context, ByteView value);

    DataFileWriter file_;
    dstd::String name_;
    dstd::String path_;
    std::uint32_t page_size_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::uint32_t next_namespace_id_ = 1;
    std::uint32_t next_scope_id_ = 1;
    std::uint32_t next_copy_id_ = 1;
    dstd::Vector<NamespaceEntry> namespaces_;
    dstd::Vector<ScopeEntry> scopes_;
    dstd::Vector<CopyEntry> copies_;
    dstd::Vector<ScopeGeneration> generations_;
    dstd::Optional<dstd::Error> latched_error_;
    bool closed_ = false;
};

}  // namespace dftracer::utils::index::store::engine

#endif  // DFTRACER_UTILS_INDEX_STORE_ENGINE_WRITER_H
