#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/scope_directory.h>
#include <dftracer/utils/index/store/engine/varint.h>

#include <algorithm>
#include <limits>

namespace dftracer::utils::index::store::engine {
namespace {

constexpr std::size_t COUNT_BYTES = 4;
constexpr std::size_t SLOT_BYTES = 16;

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

const std::uint8_t* data_of(ByteView bytes) {
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

int compare_views(dstd::StringView a, dstd::StringView b) {
    const int cmp = a.compare(b);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

/// A reader over one key or payload, failing on any byte past its end.
class Cursor {
   public:
    explicit Cursor(ByteView bytes) : bytes_(bytes) {}

    bool varint(std::uint64_t& out) {
        auto value = decode_varint(bytes_, at_);
        if (!value) return false;
        out = *value;
        return true;
    }

    bool u8(std::uint8_t& out) {
        if (bytes_.size() - at_ < 1) return false;
        out = data_of(bytes_)[at_++];
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (bytes_.size() - at_ < 4) return false;
        out = read_u32_le(data_of(bytes_) + at_);
        at_ += 4;
        return true;
    }

    bool u64(std::uint64_t& out) {
        if (bytes_.size() - at_ < 8) return false;
        out = read_u64_le(data_of(bytes_) + at_);
        at_ += 8;
        return true;
    }

    bool bytes(ByteView& out) {
        std::uint64_t length = 0;
        if (!varint(length) || length > bytes_.size() - at_) return false;
        out = bytes_.subspan(at_, static_cast<std::size_t>(length));
        at_ += static_cast<std::size_t>(length);
        return true;
    }

    bool text(dstd::StringView& out) {
        ByteView view;
        if (!bytes(view)) return false;
        out = dstd::StringView(reinterpret_cast<const char*>(view.data()),
                               view.size());
        return true;
    }

    bool done() const { return at_ == bytes_.size(); }

    /// The read position, for the bytes a caller reads in another format.
    std::size_t& position() { return at_; }

   private:
    ByteView bytes_;
    std::size_t at_ = 0;
};

/// Compares a key with (name, version, kind, paths) in directory order,
/// decoding only as far as it must. `prefix_only` stops after the version.
/// A key that does not decode sorts last.
template <class Path>
int compare_key(ByteView key, dstd::StringView name, std::uint32_t version,
                ScopeKind kind, dstd::Span<const Path> paths,
                bool prefix_only) {
    Cursor cursor(key);
    dstd::StringView key_name;
    std::uint32_t key_version = 0;
    if (!cursor.text(key_name) || !cursor.u32(key_version)) return 1;
    const int by_name = compare_views(key_name, name);
    if (by_name != 0) return by_name;
    if (key_version != version) return key_version < version ? -1 : 1;
    if (prefix_only) return 0;
    std::uint8_t key_kind = 0;
    std::uint64_t count = 0;
    if (!cursor.u8(key_kind) || !cursor.varint(count)) return 1;
    if (key_kind != static_cast<std::uint8_t>(kind)) {
        return key_kind < static_cast<std::uint8_t>(kind) ? -1 : 1;
    }
    const std::size_t common =
        std::min<std::size_t>(static_cast<std::size_t>(count), paths.size());
    for (std::size_t i = 0; i < common; ++i) {
        dstd::StringView path;
        if (!cursor.text(path)) return 1;
        const int by_path = compare_views(
            path, dstd::StringView(paths[i].data(), paths[i].size()));
        if (by_path != 0) return by_path;
    }
    if (count != paths.size()) return count < paths.size() ? -1 : 1;
    return 0;
}

/// One entry of a directory being built.
struct Draft {
    const NamespaceItem* namespace_item = nullptr;
    const ScopeItem* scope = nullptr;
    dstd::Vector<const CopyItem*> copies;
    dstd::Vector<const DropItem*> drops;
};

int compare_drafts(const Draft& a, const Draft& b) {
    const int by_name =
        compare_views(a.namespace_item->name, b.namespace_item->name);
    if (by_name != 0) return by_name;
    if (a.namespace_item->format_version != b.namespace_item->format_version) {
        return a.namespace_item->format_version <
                       b.namespace_item->format_version
                   ? -1
                   : 1;
    }
    if (a.scope->kind != b.scope->kind) {
        return static_cast<std::uint8_t>(a.scope->kind) <
                       static_cast<std::uint8_t>(b.scope->kind)
                   ? -1
                   : 1;
    }
    const std::size_t common =
        std::min(a.scope->paths.size(), b.scope->paths.size());
    for (std::size_t i = 0; i < common; ++i) {
        const int by_path = compare_views(a.scope->paths[i].logical_path,
                                          b.scope->paths[i].logical_path);
        if (by_path != 0) return by_path;
    }
    if (a.scope->paths.size() != b.scope->paths.size()) {
        return a.scope->paths.size() < b.scope->paths.size() ? -1 : 1;
    }
    return 0;
}

template <class T>
const T* find_local(const dstd::Vector<const T*>& sorted, std::uint32_t id) {
    const auto at = std::lower_bound(sorted.begin(), sorted.end(), id,
                                     [](const T* item, std::uint32_t wanted) {
                                         return item->local_id < wanted;
                                     });
    return at != sorted.end() && (*at)->local_id == id ? *at : nullptr;
}

void append_text(dstd::Vector<std::uint8_t>& out, dstd::StringView text) {
    encode_varint(out, text.size());
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
    out.insert(out.end(), begin, begin + text.size());
}

void append_view(dstd::Vector<std::uint8_t>& out, ByteView bytes) {
    encode_varint(out, bytes.size());
    out.insert(out.end(), data_of(bytes), data_of(bytes) + bytes.size());
}

void append_vector(dstd::Vector<std::uint8_t>& out,
                   const dstd::Vector<std::uint8_t>& bytes) {
    encode_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> build_scope_directory(
    dstd::Span<const NamespaceItem> namespaces,
    dstd::Span<const ScopeItem> scopes, dstd::Span<const CopyItem> copies,
    dstd::Span<const DropItem> drops,
    dstd::Span<const DirectoryCommit> commits) {
    dstd::Vector<const NamespaceItem*> namespace_ids;
    namespace_ids.reserve(namespaces.size());
    for (const NamespaceItem& item : namespaces) namespace_ids.push_back(&item);
    dstd::Vector<const ScopeItem*> scope_ids;
    scope_ids.reserve(scopes.size());
    for (const ScopeItem& item : scopes) scope_ids.push_back(&item);
    const auto by_id = [](const auto* a, const auto* b) {
        return a->local_id < b->local_id;
    };
    std::stable_sort(namespace_ids.begin(), namespace_ids.end(), by_id);
    std::stable_sort(scope_ids.begin(), scope_ids.end(), by_id);

    // One draft per copy and per drop, merged below by key.
    dstd::Vector<Draft> drafts;
    for (const CopyItem& copy : copies) {
        const NamespaceItem* namespace_item =
            find_local(namespace_ids, copy.namespace_id);
        const ScopeItem* scope = find_local(scope_ids, copy.scope_id);
        if (namespace_item == nullptr || scope == nullptr) continue;
        Draft draft;
        draft.namespace_item = namespace_item;
        draft.scope = scope;
        draft.copies.push_back(&copy);
        drafts.push_back(std::move(draft));
    }
    for (const DropItem& drop : drops) {
        const NamespaceItem* namespace_item =
            find_local(namespace_ids, drop.namespace_id);
        const ScopeItem* scope = find_local(scope_ids, drop.scope_id);
        if (namespace_item == nullptr || scope == nullptr) continue;
        Draft draft;
        draft.namespace_item = namespace_item;
        draft.scope = scope;
        draft.drops.push_back(&drop);
        drafts.push_back(std::move(draft));
    }
    std::stable_sort(drafts.begin(), drafts.end(),
                     [](const Draft& a, const Draft& b) {
                         return compare_drafts(a, b) < 0;
                     });
    dstd::Vector<Draft> entries;
    for (Draft& draft : drafts) {
        if (!entries.empty() && compare_drafts(entries.back(), draft) == 0) {
            Draft& into = entries.back();
            into.copies.insert(into.copies.end(), draft.copies.begin(),
                               draft.copies.end());
            into.drops.insert(into.drops.end(), draft.drops.begin(),
                              draft.drops.end());
            continue;
        }
        entries.push_back(std::move(draft));
    }

    dstd::Vector<DirectoryCommit> sorted_commits(commits.begin(),
                                                 commits.end());
    std::sort(sorted_commits.begin(), sorted_commits.end(),
              [](const DirectoryCommit& a, const DirectoryCommit& b) {
                  if (a.copy_id != b.copy_id) return a.copy_id < b.copy_id;
                  return a.sequence < b.sequence;
              });
    // One range per copy and commit, however many sections gave it.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < sorted_commits.size(); ++i) {
        const DirectoryCommit& next = sorted_commits[i];
        if (kept > 0 && sorted_commits[kept - 1].copy_id == next.copy_id &&
            sorted_commits[kept - 1].sequence == next.sequence) {
            DirectoryCommit& into = sorted_commits[kept - 1];
            if (std::lexicographical_compare(
                    data_of(next.first_key),
                    data_of(next.first_key) + next.first_key.size(),
                    data_of(into.first_key),
                    data_of(into.first_key) + into.first_key.size())) {
                into.first_key = next.first_key;
            }
            if (std::lexicographical_compare(
                    data_of(into.last_key),
                    data_of(into.last_key) + into.last_key.size(),
                    data_of(next.last_key),
                    data_of(next.last_key) + next.last_key.size())) {
                into.last_key = next.last_key;
            }
            continue;
        }
        sorted_commits[kept++] = next;
    }
    sorted_commits.resize(kept);

    dstd::Vector<std::uint8_t> data;
    dstd::Vector<std::uint8_t> slots;
    slots.reserve(entries.size() * SLOT_BYTES);
    const std::size_t data_start = COUNT_BYTES + entries.size() * SLOT_BYTES;
    for (const Draft& entry : entries) {
        const std::size_t key_at = data_start + data.size();
        append_text(data, entry.namespace_item->name);
        append_u32_le(data, entry.namespace_item->format_version);
        data.push_back(static_cast<std::uint8_t>(entry.scope->kind));
        encode_varint(data, entry.scope->paths.size());
        for (const ScopePath& path : entry.scope->paths) {
            append_text(data, path.logical_path);
        }
        const std::size_t payload_at = data_start + data.size();

        std::uint64_t highest = 0;
        for (const CopyItem* copy : entry.copies) {
            highest = std::max(highest, copy->generation);
        }
        for (const DropItem* drop : entry.drops) {
            highest = std::max(highest, drop->generation);
        }
        append_u64_le(data, highest);
        encode_varint(data, entry.copies.size());
        for (const CopyItem* copy : entry.copies) {
            append_u32_le(data, copy->local_id);
            append_u64_le(data, copy->generation);
            append_u64_le(data, copy->params_hash);
            data.push_back(static_cast<std::uint8_t>(copy->state));
            data.push_back(entry.namespace_item->fold_kind);
            const auto first = std::lower_bound(
                sorted_commits.begin(), sorted_commits.end(), copy->local_id,
                [](const DirectoryCommit& commit, std::uint32_t id) {
                    return commit.copy_id < id;
                });
            auto last = first;
            while (last != sorted_commits.end() &&
                   last->copy_id == copy->local_id) {
                ++last;
            }
            encode_varint(data, static_cast<std::uint64_t>(last - first));
            for (auto at = first; at != last; ++at) {
                append_u64_le(data, at->sequence);
                append_view(data, at->first_key);
                append_view(data, at->last_key);
                append_block_handle(data, at->sections);
            }
        }
        encode_varint(data, entry.drops.size());
        for (const DropItem* drop : entry.drops) {
            append_u64_le(data, drop->generation);
            append_vector(data, drop->begin_key);
            append_vector(data, drop->end_key);
        }
        const std::size_t end = data_start + data.size();
        if (end > std::numeric_limits<std::uint32_t>::max()) {
            return invalid_error();
        }
        append_u32_le(slots, static_cast<std::uint32_t>(key_at));
        append_u32_le(slots, static_cast<std::uint32_t>(payload_at - key_at));
        append_u32_le(slots, static_cast<std::uint32_t>(payload_at));
        append_u32_le(slots, static_cast<std::uint32_t>(end - payload_at));
    }

    dstd::Vector<std::uint8_t> out;
    out.reserve(data_start + data.size());
    append_u32_le(out, static_cast<std::uint32_t>(entries.size()));
    out.insert(out.end(), slots.begin(), slots.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

dstd::Expected<ScopeDirectory, dstd::Error> ScopeDirectory::open(
    ByteView bytes) {
    ScopeDirectory directory;
    if (bytes.empty()) return directory;
    if (bytes.size() < COUNT_BYTES) return parse_error();
    const std::uint64_t count = read_u32_le(data_of(bytes));
    if (count > (bytes.size() - COUNT_BYTES) / SLOT_BYTES) return parse_error();
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* slot =
            data_of(bytes) + COUNT_BYTES + i * SLOT_BYTES;
        const std::uint64_t key_at = read_u32_le(slot);
        const std::uint64_t key_length = read_u32_le(slot + 4);
        const std::uint64_t payload_at = read_u32_le(slot + 8);
        const std::uint64_t payload_length = read_u32_le(slot + 12);
        if (key_at > bytes.size() || key_length > bytes.size() - key_at ||
            payload_at > bytes.size() ||
            payload_length > bytes.size() - payload_at) {
            return parse_error();
        }
    }
    directory.bytes_ = bytes;
    directory.count_ = static_cast<std::size_t>(count);
    return directory;
}

ByteView ScopeDirectory::key_bytes(std::size_t index) const {
    const std::uint8_t* slot =
        data_of(bytes_) + COUNT_BYTES + index * SLOT_BYTES;
    return bytes_.subspan(read_u32_le(slot), read_u32_le(slot + 4));
}

ByteView ScopeDirectory::payload_bytes(std::size_t index) const {
    const std::uint8_t* slot =
        data_of(bytes_) + COUNT_BYTES + index * SLOT_BYTES;
    return bytes_.subspan(read_u32_le(slot + 8), read_u32_le(slot + 12));
}

std::size_t ScopeDirectory::find(dstd::StringView name,
                                 std::uint32_t format_version, ScopeKind kind,
                                 dstd::Span<const dstd::String> paths) const {
    std::size_t low = 0;
    std::size_t high = count_;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (compare_key(key_bytes(mid), name, format_version, kind, paths,
                        false) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low < count_ && compare_key(key_bytes(low), name, format_version, kind,
                                    paths, false) == 0) {
        return low;
    }
    return count_;
}

std::pair<std::size_t, std::size_t> ScopeDirectory::namespace_range(
    dstd::StringView name, std::uint32_t format_version) const {
    const dstd::Span<const dstd::String> none;
    const auto bound = [&](bool upper) {
        std::size_t low = 0;
        std::size_t high = count_;
        while (low < high) {
            const std::size_t mid = low + (high - low) / 2;
            const int cmp = compare_key(key_bytes(mid), name, format_version,
                                        ScopeKind::Root, none, true);
            if (upper ? cmp <= 0 : cmp < 0) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    };
    return {bound(false), bound(true)};
}

dstd::Expected<DirectoryKey, dstd::Error> ScopeDirectory::key(
    std::size_t index) const {
    if (index >= count_) return invalid_error();
    Cursor cursor(key_bytes(index));
    DirectoryKey key;
    std::uint8_t kind = 0;
    std::uint64_t count = 0;
    if (!cursor.text(key.name) || !cursor.u32(key.format_version) ||
        !cursor.u8(kind) ||
        kind > static_cast<std::uint8_t>(ScopeKind::Files) ||
        !cursor.varint(count) || count > key_bytes(index).size()) {
        return parse_error();
    }
    key.kind = static_cast<ScopeKind>(kind);
    key.paths.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        dstd::StringView path;
        if (!cursor.text(path)) return parse_error();
        key.paths.push_back(path);
    }
    if (!cursor.done()) return parse_error();
    return key;
}

dstd::Expected<std::uint64_t, dstd::Error> ScopeDirectory::highest_generation(
    std::size_t index) const {
    if (index >= count_) return invalid_error();
    Cursor cursor(payload_bytes(index));
    std::uint64_t highest = 0;
    if (!cursor.u64(highest)) return parse_error();
    return highest;
}

dstd::Expected<DirectoryEntry, dstd::Error> ScopeDirectory::entry(
    std::size_t index) const {
    auto key_result = key(index);
    if (!key_result) return dstd::Unexpected<dstd::Error>(key_result.error());
    DirectoryEntry entry;
    entry.key = std::move(*key_result);

    const ByteView payload = payload_bytes(index);
    Cursor cursor(payload);
    std::uint64_t copy_count = 0;
    if (!cursor.u64(entry.highest_generation) || !cursor.varint(copy_count) ||
        copy_count > payload.size()) {
        return parse_error();
    }
    entry.copies.reserve(static_cast<std::size_t>(copy_count));
    for (std::uint64_t i = 0; i < copy_count; ++i) {
        DirectoryCopy copy;
        std::uint8_t state = 0;
        std::uint64_t commit_count = 0;
        if (!cursor.u32(copy.local_id) || !cursor.u64(copy.generation) ||
            !cursor.u64(copy.params_hash) || !cursor.u8(state) ||
            state > static_cast<std::uint8_t>(CopyState::Failed) ||
            !cursor.u8(copy.fold_kind) || !cursor.varint(commit_count) ||
            commit_count > payload.size()) {
            return parse_error();
        }
        copy.state = static_cast<CopyState>(state);
        copy.commits.reserve(static_cast<std::size_t>(commit_count));
        for (std::uint64_t c = 0; c < commit_count; ++c) {
            DirectoryCommit commit;
            commit.copy_id = copy.local_id;
            if (!cursor.u64(commit.sequence) ||
                !cursor.bytes(commit.first_key) ||
                !cursor.bytes(commit.last_key) ||
                !read_block_handle(payload, cursor.position(),
                                   commit.sections)) {
                return parse_error();
            }
            copy.commits.push_back(commit);
        }
        entry.copies.push_back(std::move(copy));
    }
    std::uint64_t drop_count = 0;
    if (!cursor.varint(drop_count) || drop_count > payload.size()) {
        return parse_error();
    }
    entry.drops.reserve(static_cast<std::size_t>(drop_count));
    for (std::uint64_t i = 0; i < drop_count; ++i) {
        DirectoryDrop drop;
        if (!cursor.u64(drop.generation) || !cursor.bytes(drop.begin) ||
            !cursor.bytes(drop.end)) {
            return parse_error();
        }
        entry.drops.push_back(drop);
    }
    if (!cursor.done()) return parse_error();
    return entry;
}

namespace {

ByteView bytes_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

/// Appends one slot and its key and payload bytes to a table being built.
void append_block(dstd::Vector<std::uint8_t>& out,
                  dstd::Vector<std::uint8_t>& data, ByteView key,
                  ByteView payload, std::size_t data_start) {
    const std::size_t key_at = data_start + data.size();
    data.insert(data.end(), data_of(key), data_of(key) + key.size());
    const std::size_t payload_at = data_start + data.size();
    data.insert(data.end(), data_of(payload),
                data_of(payload) + payload.size());
    append_u32_le(out, static_cast<std::uint32_t>(key_at));
    append_u32_le(out, static_cast<std::uint32_t>(payload_at - key_at));
    append_u32_le(out, static_cast<std::uint32_t>(payload_at));
    append_u32_le(
        out, static_cast<std::uint32_t>(data_start + data.size() - payload_at));
}

}  // namespace

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_slot_table(
    dstd::Span<const TableEntry> entries) {
    dstd::Vector<std::uint8_t> slots;
    dstd::Vector<std::uint8_t> data;
    slots.reserve(entries.size() * SLOT_BYTES);
    const std::size_t data_start = COUNT_BYTES + entries.size() * SLOT_BYTES;
    for (const TableEntry& entry : entries) {
        const ByteView key = entry.key;
        const ByteView payload = entry.payload;
        append_block(slots, data, key, payload, data_start);
    }
    if (data_start + data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    dstd::Vector<std::uint8_t> out;
    out.reserve(data_start + data.size());
    append_u32_le(out, static_cast<std::uint32_t>(entries.size()));
    out.insert(out.end(), slots.begin(), slots.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_separator_table(
    dstd::Span<const TableBlock> children,
    dstd::Span<const BlockHandle> handles) {
    if (children.size() != handles.size()) return invalid_error();
    dstd::Vector<std::uint8_t> slots;
    dstd::Vector<std::uint8_t> data;
    slots.reserve(children.size() * SLOT_BYTES);
    const std::size_t data_start = COUNT_BYTES + children.size() * SLOT_BYTES;
    for (std::size_t i = 0; i < children.size(); ++i) {
        dstd::Vector<std::uint8_t> payload;
        append_block_handle(payload, handles[i]);
        append_block(slots, data, children[i].first_key(), bytes_of(payload),
                     data_start);
    }
    if (data_start + data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }
    dstd::Vector<std::uint8_t> out;
    out.reserve(data_start + data.size());
    append_u32_le(out, static_cast<std::uint32_t>(children.size()));
    out.insert(out.end(), slots.begin(), slots.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

ByteView TableBlock::first_key() const {
    auto table = SlotTable::open(ByteView{
        reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
    if (!table || table->size() == 0) return {};
    return table->key(0);
}

dstd::Expected<dstd::Vector<TableBlock>, dstd::Error> split_slot_table(
    ByteView table, std::size_t per_block) {
    if (per_block == 0) return invalid_error();
    auto opened = SlotTable::open(table);
    if (!opened) return dstd::Unexpected<dstd::Error>(opened.error());
    dstd::Vector<TableBlock> blocks;
    const std::size_t count = opened->size();
    for (std::size_t first = 0; first < count; first += per_block) {
        const std::size_t end = std::min(first + per_block, count);
        dstd::Vector<std::uint8_t> slots;
        dstd::Vector<std::uint8_t> data;
        slots.reserve((end - first) * SLOT_BYTES);
        const std::size_t data_start = COUNT_BYTES + (end - first) * SLOT_BYTES;
        for (std::size_t at = first; at < end; ++at) {
            append_block(slots, data, opened->key(at), opened->payload(at),
                         data_start);
        }
        if (data_start + data.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            return invalid_error();
        }
        TableBlock block;
        block.entries = end - first;
        block.bytes.reserve(data_start + data.size());
        append_u32_le(block.bytes, static_cast<std::uint32_t>(end - first));
        block.bytes.insert(block.bytes.end(), slots.begin(), slots.end());
        block.bytes.insert(block.bytes.end(), data.begin(), data.end());
        blocks.push_back(std::move(block));
    }
    return blocks;
}

dstd::Expected<SlotTable, dstd::Error> SlotTable::open(ByteView bytes) {
    SlotTable table;
    if (bytes.empty()) return table;
    if (bytes.size() < COUNT_BYTES) return parse_error();
    const std::uint64_t count = read_u32_le(data_of(bytes));
    if (count > (bytes.size() - COUNT_BYTES) / SLOT_BYTES) return parse_error();
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* slot =
            data_of(bytes) + COUNT_BYTES + i * SLOT_BYTES;
        const std::uint64_t key_at = read_u32_le(slot);
        const std::uint64_t key_length = read_u32_le(slot + 4);
        const std::uint64_t payload_at = read_u32_le(slot + 8);
        const std::uint64_t payload_length = read_u32_le(slot + 12);
        if (key_at > bytes.size() || key_length > bytes.size() - key_at ||
            payload_at > bytes.size() ||
            payload_length > bytes.size() - payload_at) {
            return parse_error();
        }
    }
    table.bytes_ = bytes;
    table.count_ = static_cast<std::size_t>(count);
    return table;
}

ByteView SlotTable::key(std::size_t index) const {
    const std::uint8_t* slot =
        data_of(bytes_) + COUNT_BYTES + index * SLOT_BYTES;
    return bytes_.subspan(read_u32_le(slot), read_u32_le(slot + 4));
}

ByteView SlotTable::payload(std::size_t index) const {
    const std::uint8_t* slot =
        data_of(bytes_) + COUNT_BYTES + index * SLOT_BYTES;
    return bytes_.subspan(read_u32_le(slot + 8), read_u32_le(slot + 12));
}

int compare_directory_key(ByteView key, dstd::StringView name,
                          std::uint32_t version, ScopeKind kind,
                          dstd::Span<const dstd::String> paths) {
    return compare_key(key, name, version, kind, paths, false);
}

}  // namespace dftracer::utils::index::store::engine