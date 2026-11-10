// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/scope_filter.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/varint.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <system_error>
#include <utility>

namespace dftracer::utils::store::engine {
namespace {

using dftracer::utils::Runtime;
using dftracer::utils::dstd::ByteView;
using dftracer::utils::dstd::read_u32_le;
using dftracer::utils::dstd::read_u64_le;

dstd::Unexpected<dstd::Error> io_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Io));
}

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

int compare_bytes(ByteView a, ByteView b) {
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    if (common != 0) {
        const int cmp = std::memcmp(a.data(), b.data(), common);
        if (cmp != 0) return cmp;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

ByteView bytes_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

ByteView bytes_of(const dstd::RawBuffer<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String path_string(dstd::StringView dir, dstd::StringView name) {
    dstd::String out(dir.data(), dir.size());
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out.append(name.data(), name.size());
    return out;
}

bool namespace_is_shared(const detail::FileData& file,
                         const NamespaceSpec& spec) {
    for (const NamespaceItem& item : file.reader.index().namespaces) {
        if (item.name == spec.name &&
            item.format_version == spec.format_version) {
            return item.shared != 0;
        }
    }
    return false;
}

void warn_skipped(dstd::StringView dir, dstd::StringView name,
                  const dstd::Error& error) {
    const dstd::String path = path_string(dir, name);
    const bool unsupported = error.is(dstd::Condition::Unsupported);
    DFTRACER_UTILS_LOG_WARN(
        "store: skipping data file {}: {}", path,
        unsupported ? "unsupported version" : "cannot be read");
}

bool has_data_file_suffix(dstd::StringView name) {
    return name.size() > DATA_FILE_SUFFIX.size() &&
           name.compare(name.size() - DATA_FILE_SUFFIX.size(),
                        DATA_FILE_SUFFIX.size(), DATA_FILE_SUFFIX) == 0;
}

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

/// Set by the fork handler in the child of a fork: the pools of the parent do
/// not exist there, so a fork-join on a runtime this process inherited would
/// wait for workers that are gone.
std::atomic<bool> INHERITED_POOLS{false};

void mark_forked_child() noexcept {
    INHERITED_POOLS.store(true, std::memory_order_relaxed);
}

// Registered at load, before main, so any later fork child is marked.
[[maybe_unused]] const bool FORK_HANDLER_REGISTERED =
    ::pthread_atfork(nullptr, nullptr, mark_forked_child) == 0;

/// The process-wide default runtime, when a fork-join on it can finish here:
/// not on its own worker, not in its parallel regions and not in a fork child.
/// A runtime created here has live workers, so it resets the inherited pools.
Runtime* fork_join_runtime() {
    const dstd::SharedPtr<Runtime> known = peek_default_runtime();
    Runtime& runtime = default_runtime();
    if (known == nullptr) {
        INHERITED_POOLS.store(false, std::memory_order_relaxed);
    }
    if (INHERITED_POOLS.load(std::memory_order_relaxed) ||
        coro::Executor::current() == runtime.executor() ||
        dftracer::utils::detail::in_parallel_region()) {
        return nullptr;
    }
    return &runtime;
}

template <class Body>
dstd::Result<void> for_each_index(Runtime* runtime, std::size_t count,
                                  std::int64_t grain, const Body& body) {
    if (runtime == nullptr) {
        for (std::size_t i = 0; i < count; ++i) body(i);
        return {};
    }
    return runtime->parallel_for(static_cast<std::int64_t>(count), grain,
                                 [&](std::int64_t begin, std::int64_t limit) {
                                     for (std::int64_t i = begin; i < limit;
                                          ++i) {
                                         body(static_cast<std::size_t>(i));
                                     }
                                 });
}

dstd::Unexpected<dstd::Error> bad_extent_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

/// Each part is length-prefixed, so no two keys give the same bytes.
dstd::String group_key(dstd::StringView name, std::uint32_t version,
                       ScopeKind kind, dstd::Span<const dstd::String> paths) {
    dstd::String key;
    const auto add_u32 = [&key](std::uint32_t value) {
        char bytes[4];
        dstd::write_u32_le(reinterpret_cast<std::uint8_t*>(bytes), value);
        key.append(bytes, sizeof(bytes));
    };
    add_u32(static_cast<std::uint32_t>(name.size()));
    key.append(name.data(), name.size());
    add_u32(version);
    add_u32(static_cast<std::uint32_t>(kind));
    add_u32(static_cast<std::uint32_t>(paths.size()));
    for (const dstd::String& path : paths) {
        add_u32(static_cast<std::uint32_t>(path.size()));
        key.append(path.data(), path.size());
    }
    return key;
}

/// The raw bytes [offset, offset + length) of an extent, decoded from `run`
/// (the extent's stored bytes from `run_begin` on) into `owned`. A Parse error
/// when a page lies outside `run` or does not decode.
dstd::Expected<ByteView, dstd::Error> decode_covering(
    const ExtentPages& pages, std::uint32_t offset, std::uint32_t length,
    ByteView run, std::uint64_t run_begin, dstd::Vector<std::uint8_t>& owned,
    const PageDict* dict) {
    if (offset > pages.raw_size() || length > pages.raw_size() - offset) {
        return bad_extent_error();
    }
    const auto covered = pages.covering(offset, length);
    if (covered.first >= covered.second) return bad_extent_error();
    const PageEntry& first = pages.entry(covered.first);
    const PageEntry& last = pages.entry(covered.second - 1);
    const std::uint32_t base = first.raw_offset;
    owned.resize(static_cast<std::size_t>(last.raw_offset) + last.raw_size -
                 base);
    for (std::size_t page = covered.first; page < covered.second; ++page) {
        const PageEntry& entry = pages.entry(page);
        if (entry.stored_offset < run_begin ||
            entry.stored_offset - run_begin > run.size() ||
            entry.stored_size >
                run.size() - (entry.stored_offset - run_begin)) {
            return bad_extent_error();
        }
        const ByteView stored{run.data() + (entry.stored_offset - run_begin),
                              entry.stored_size};
        auto decoded = ExtentPages::decode_page(
            entry, stored, owned.data() + (entry.raw_offset - base), dict);
        if (!decoded) return bad_extent_error();
    }
    return ByteView{owned.data() + (offset - base), length};
}

/// Reads the page table of the extent at `ref` into `slot`: Good, Bad when the
/// extent lies outside the file or its table does not open, Failed with
/// `slot.error` when a read fails. The read takes the whole extent, up to
/// READ_RUN_BYTES, to warm the operating system cache.
detail::LoadState read_table(const DataFileReader& reader, const ExtentRef& ref,
                             detail::ExtentSlot& slot, BlockReads& reads) {
    const auto state_of = [&](const dstd::Error& error) {
        if (!error.is(dstd::Condition::Io)) return detail::LoadState::Bad;
        slot.error = error;
        return detail::LoadState::Failed;
    };
    dstd::RawBuffer<std::uint8_t> head;
    auto read = reader.read_range(
        ref.offset, std::min<std::size_t>(ref.length, READ_RUN_BYTES), head,
        &reads);
    if (!read) return state_of(read.error());
    const std::size_t table = ExtentPages::table_size(bytes_of(head));
    if (table == 0 || table > ref.length) return detail::LoadState::Bad;
    if (table > head.size()) {
        read = reader.read_range(ref.offset, table, head, &reads);
        if (!read) return state_of(read.error());
    }
    auto pages =
        ExtentPages::open_table(ByteView{head.data(), table}, ref.length);
    if (!pages) return detail::LoadState::Bad;
    slot.ref = ref;
    slot.pages = std::move(*pages);
    return detail::LoadState::Good;
}

void fill_segment_blocks(detail::Commit& commit) {
    struct Listed {
        std::uint32_t copy_id;
        std::uint32_t block;
        const ExtentSegment* segment;
    };
    dstd::Vector<Listed> listed;
    for (std::size_t block = 0; block < commit.blocks.size(); ++block) {
        for (const ExtentSegment& segment :
             commit.blocks[block].listed->segments) {
            listed.push_back(
                {segment.copy_id, static_cast<std::uint32_t>(block), &segment});
        }
    }
    std::sort(
        listed.begin(), listed.end(), [](const Listed& a, const Listed& b) {
            if (a.copy_id != b.copy_id) return a.copy_id < b.copy_id;
            if (a.block != b.block) return a.block < b.block;
            return a.segment->offset_in_extent < b.segment->offset_in_extent;
        });
    std::size_t at = 0;
    while (at < listed.size()) {
        std::size_t end = at + 1;
        while (end < listed.size() &&
               listed[end].copy_id == listed[at].copy_id) {
            ++end;
        }
        auto block = dstd::make_unique<detail::LazyBlock>();
        block->kind = detail::BlockKind::Segments;
        block->state.store(static_cast<std::uint8_t>(detail::LoadState::Good),
                           std::memory_order_relaxed);
        block->owner = &commit;
        block->segments = dstd::Vector<detail::SegmentSlot>(end - at);
        for (std::size_t i = at; i < end; ++i) {
            detail::SegmentSlot& slot = block->segments[i - at];
            slot.sequence = commit.sequence;
            slot.block = listed[i].block;
            slot.offset_in_extent = listed[i].segment->offset_in_extent;
            slot.length = listed[i].segment->length;
            slot.entry_count = listed[i].segment->entry_count;
            slot.ordinal = listed[i].segment->ordinal;
            slot.first_key = bytes_of(listed[i].segment->first_key);
            slot.last_key = bytes_of(listed[i].segment->last_key);
        }
        commit.segment_copies.push_back(listed[at].copy_id);
        commit.segment_blocks.push_back(std::move(block));
        at = end;
    }
}

detail::LazyBlock* segment_block_of(detail::Commit& commit,
                                    std::uint32_t copy_id) {
    const auto at = std::lower_bound(commit.segment_copies.begin(),
                                     commit.segment_copies.end(), copy_id);
    if (at == commit.segment_copies.end() || *at != copy_id) return nullptr;
    return commit
        .segment_blocks[static_cast<std::size_t>(at -
                                                 commit.segment_copies.begin())]
        .get();
}

bool erase_covers(ByteView key, const detail::EraseRange& range) {
    if (!range.begin.empty() && compare_bytes(key, range.begin) < 0) {
        return false;
    }
    if (!range.end.empty() && compare_bytes(key, range.end) >= 0) return false;
    return true;
}

/// The ordinal below which the erases of one copy hide every version of `key`,
/// or zero when none covers it. Ordinals start at one.
std::uint64_t erase_limit(ByteView key,
                          const dstd::Vector<detail::EraseRange>& erases) {
    std::uint64_t limit = 0;
    for (const detail::EraseRange& range : erases) {
        if (!erase_covers(key, range)) continue;
        limit = std::max(limit, range.ordinal);
    }
    return limit;
}

bool key_hidden(ByteView key, const dstd::Vector<detail::HiddenRange>& hidden) {
    for (const detail::HiddenRange& range : hidden) {
        if (!range.begin.empty() &&
            compare_bytes(key, bytes_of(range.begin)) < 0) {
            continue;
        }
        if (!range.end.empty() &&
            compare_bytes(key, bytes_of(range.end)) >= 0) {
            continue;
        }
        return true;
    }
    return false;
}

dstd::Vector<std::uint8_t> prefix_end(ByteView prefix) {
    dstd::Vector<std::uint8_t> out;
    if (prefix.empty()) return out;
    out.resize(prefix.size());
    std::memcpy(out.data(), prefix.data(), prefix.size());
    for (std::size_t i = out.size(); i > 0; --i) {
        if (out[i - 1U] != 0xFFU) {
            ++out[i - 1U];
            out.resize(i);
            return out;
        }
    }
    out.clear();
    return out;
}

/// The fewest data files an open spreads over the default runtime.
constexpr std::size_t PARALLEL_OPEN_FILES = 8;

constexpr std::int64_t COMMIT_GRAIN = 4;

/// Builds the blocks, extents and segment blocks of one commit from its extent
/// refs, deduplicated by offset and in offset order, so the extents the commit
/// holds read together. A record may list the same extent ref again, and one
/// ref belongs to one commit: the first listing of an offset is the one kept.
void fill_commit(detail::Commit& commit,
                 dstd::Vector<const ExtentRefItem*> refs) {
    dstd::stable_sort(refs.begin(), refs.end(),
                      [](const ExtentRefItem* a, const ExtentRefItem* b) {
                          return a->offset < b->offset;
                      });
    dstd::Vector<detail::CommitExtent> blocks;
    for (const ExtentRefItem* ref : refs) {
        if (blocks.empty() || blocks.back().ref.offset != ref->offset) {
            blocks.push_back(
                {ExtentRef{ref->offset, ref->length, ref->crc}, ref});
        }
    }
    commit.extent_slots = dstd::Vector<detail::ExtentSlot>(blocks.size());
    commit.blocks = std::move(blocks);
    fill_segment_blocks(commit);
}

dstd::String verified_key(const detail::FileData& file,
                          std::uint64_t sequence) {
    return dstd::fmt<"{}:{}:{}">(file.name, file.writer_random, sequence);
}

void build_commits(detail::FileData& file, std::size_t file_index) {
    const FileIndex& index = file.reader.index();
    // A closed file holds no commit object: every commit of it is synced, and
    // its directory names the segments of each copy.
    if (index.tail.has_value()) return;
    const dstd::Vector<IndexedExtentRef>& extent_refs = index.extent_refs;
    dstd::Vector<std::size_t> order(extent_refs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    dstd::stable_sort(order.begin(), order.end(),
                      [&extent_refs](std::size_t a, std::size_t b) {
                          return extent_refs[a].sequence <
                                 extent_refs[b].sequence;
                      });
    for (std::size_t at = 0; at < order.size();) {
        const std::uint64_t sequence = extent_refs[order[at]].sequence;
        auto commit = dstd::make_unique<detail::Commit>();
        commit->sequence = sequence;
        commit->file_index = file_index;
        dstd::Vector<const ExtentRefItem*> refs;
        bool synced = true;
        while (at < order.size() &&
               extent_refs[order[at]].sequence == sequence) {
            const ExtentRefItem& ref = extent_refs[order[at]].ref;
            if (ref.synced == 0) synced = false;
            refs.push_back(&ref);
            ++at;
        }
        // A clean close syncs every extent before its footer.
        commit->synced = index.closed || synced;
        fill_commit(*commit, std::move(refs));
        commit->index_state.store(
            static_cast<std::uint8_t>(detail::LoadState::Good),
            std::memory_order_relaxed);
        file.commits.push_back(std::move(commit));
    }
    for (const CommitRefItem& summary : index.commit_refs) {
        auto commit = dstd::make_unique<detail::Commit>();
        commit->sequence = summary.sequence;
        commit->file_index = file_index;
        commit->synced = index.closed || summary.synced != 0;
        commit->summary = &summary;
        file.commits.push_back(std::move(commit));
    }
    std::sort(file.commits.begin(), file.commits.end(),
              [](const dstd::UniquePtr<detail::Commit>& a,
                 const dstd::UniquePtr<detail::Commit>& b) {
                  return a->sequence < b->sequence;
              });
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> chain_directory(
    const detail::FileData& file) {
    const FileIndex& index = file.reader.index();
    dstd::Vector<DirectoryCommit> commits;
    for (const dstd::UniquePtr<detail::Commit>& commit : file.commits) {
        if (commit->summary != nullptr) {
            for (const CommitCopy& range : commit->summary->copies) {
                commits.push_back({commit->sequence, range.copy_id,
                                   bytes_of(range.first_key),
                                   bytes_of(range.last_key), BlockHandle{}});
            }
            continue;
        }
        for (std::size_t at = 0; at < commit->segment_blocks.size(); ++at) {
            const std::uint32_t copy_id = commit->segment_copies[at];
            for (const detail::SegmentSlot& slot :
                 commit->segment_blocks[at]->segments) {
                commits.push_back({commit->sequence, copy_id, slot.first_key,
                                   slot.last_key, BlockHandle{}});
            }
        }
    }
    return build_scope_directory(index.namespaces, index.scopes, index.copies,
                                 index.drops, index.erases, commits);
}

dstd::Expected<void, dstd::Error> build_file_directory(detail::FileData& file) {
    const FileIndex& index = file.reader.index();
    if (!index.tail.has_value()) {
        auto bytes = chain_directory(file);
        if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
        auto leaf = dstd::make_unique<detail::LazyBlock>();
        leaf->kind = detail::BlockKind::Leaf;
        leaf->levels = 1;
        leaf->bytes = std::move(*bytes);
        auto directory = ScopeDirectory::open(bytes_of(leaf->bytes));
        if (!directory) {
            return dstd::Unexpected<dstd::Error>(directory.error());
        }
        auto groups = dstd::make_unique<detail::GroupSlots>();
        groups->assign(directory->size());
        leaf->directory =
            dstd::make_unique<ScopeDirectory>(std::move(*directory));
        leaf->groups = std::move(groups);
        leaf->state.store(static_cast<std::uint8_t>(detail::LoadState::Good),
                          std::memory_order_relaxed);
        file.roots.push_back(std::move(leaf));
        file.directory_levels = 1;
        return {};
    }
    const FileTail& tail = *index.tail;
    file.top_table = tail.top_table;
    file.directory_levels = tail.directory_levels;
    auto table = SlotTable::open(bytes_of(file.top_table));
    if (!table) {
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::Parse));
    }
    file.top = *table;
    for (std::size_t i = 0; i < table->size(); ++i) {
        BlockHandle handle;
        std::size_t at = 0;
        const ByteView payload = table->payload(i);
        if (!read_block_handle(payload, at, handle) || at != payload.size()) {
            return dstd::Unexpected<dstd::Error>(
                dstd::Error(dstd::Condition::Parse));
        }
        auto block = dstd::make_unique<detail::LazyBlock>();
        block->handle = handle;
        block->levels = tail.directory_levels;
        block->kind = tail.directory_levels == 1 ? detail::BlockKind::Leaf
                                                 : detail::BlockKind::Middle;
        file.roots.push_back(std::move(block));
    }
    file.extent_slots.assign(tail.extent_count);
    return {};
}

detail::Commit* commit_of(const detail::FileData& file,
                          std::uint64_t sequence) {
    const auto at = std::lower_bound(
        file.commits.begin(), file.commits.end(), sequence,
        [](const dstd::UniquePtr<detail::Commit>& commit,
           std::uint64_t wanted) { return commit->sequence < wanted; });
    if (at == file.commits.end() || (*at)->sequence != sequence) {
        return nullptr;
    }
    return at->get();
}

int compare_directory_keys(const DirectoryKey& a, const DirectoryKey& b) {
    const int by_name = a.name.compare(b.name);
    if (by_name != 0) return by_name < 0 ? -1 : 1;
    if (a.format_version != b.format_version) {
        return a.format_version < b.format_version ? -1 : 1;
    }
    if (a.kind != b.kind) {
        return static_cast<std::uint8_t>(a.kind) <
                       static_cast<std::uint8_t>(b.kind)
                   ? -1
                   : 1;
    }
    const std::size_t common = std::min(a.paths.size(), b.paths.size());
    for (std::size_t i = 0; i < common; ++i) {
        const int by_path = a.paths[i].compare(b.paths[i]);
        if (by_path != 0) return by_path < 0 ? -1 : 1;
    }
    if (a.paths.size() != b.paths.size()) {
        return a.paths.size() < b.paths.size() ? -1 : 1;
    }
    return 0;
}

ScopeKey scope_of(const DirectoryKey& key) {
    ScopeKey scope;
    scope.kind = key.kind;
    scope.paths.reserve(key.paths.size());
    for (const dstd::StringView path : key.paths) {
        scope.paths.push_back(dstd::String(path.data(), path.size()));
    }
    return scope;
}

}  // namespace

dstd::Expected<Snapshot, dstd::Error> Snapshot::open(
    dstd::StringView dir, dstd::SharedPtr<detail::VerifiedCommits> verified) {
    Snapshot snapshot;
    snapshot.verified_ = std::move(verified);
    auto loaded = snapshot.load(dir);
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    return snapshot;
}

dstd::Expected<void, dstd::Error> Snapshot::load(dstd::StringView dir) {
    std::size_t cache_bytes =
        common::constants::store::STORE_DEFAULT_PAGE_CACHE_BYTES;
    if (common::Env::get<dstd::StringView>(
            common::constants::store::STORE_PAGE_CACHE_BYTES_ENV)
            .has_value()) {
        auto parsed = common::Env::get<std::uint64_t>(
            common::constants::store::STORE_PAGE_CACHE_BYTES_ENV);
        if (!parsed.has_value()) {
            return dstd::Unexpected<dstd::Error>(
                dstd::Error(dstd::Condition::InvalidArgument));
        }
        cache_bytes = static_cast<std::size_t>(*parsed);
    }
    pages_ = dstd::make_unique<PageCache>(cache_bytes);
    groups_ = dstd::make_unique<detail::GroupIndex>();
    const auto start = std::chrono::steady_clock::now();
    dstd::Vector<dstd::String> names;
    const auto dir_handle = dstd::fs::Dir::open(dstd::PathView(dir));
    if (!dir_handle) return io_error();
    auto listing = dir_handle->entries();
    if (!listing) return io_error();
    for (const dstd::fs::DirEntry& entry : *listing) {
        if (has_data_file_suffix(entry.name)) names.emplace_back(entry.name);
    }
    if (listing->error() != 0) return io_error();
    std::sort(names.begin(), names.end());
    // A summary that cannot be read only leaves every file to open as before.
    auto summary = read_summary(dir);
    dstd::HashMap<dstd::String, const SummaryEntry*> summarized;
    if (summary) {
        for (const SummaryEntry& entry : *summary) {
            summarized.emplace(entry.name, &entry);
        }
    }
    const auto listed = std::chrono::steady_clock::now();

    Runtime* const runtime =
        names.size() >= PARALLEL_OPEN_FILES ? fork_join_runtime() : nullptr;

    dstd::Vector<dstd::Optional<detail::FileData>> opened(names.size());
    dstd::Vector<dstd::Optional<dstd::Error>> open_errors(names.size());
    dstd::Vector<std::uint8_t> from_summary(names.size(), 0);
    const auto open_one = [&](std::size_t index) {
        const dstd::String path = path_string(dir, names[index]);
        const auto listed_entry = summarized.find(names[index]);
        if (listed_entry != summarized.end()) {
            auto reader =
                DataFileReader::from_summary(path, *listed_entry->second);
            if (reader) {
                detail::FileData& file =
                    opened[index].emplace(std::move(*reader));
                file.name = names[index];
                file.writer_random = file.reader.index().writer_random;
                from_summary[index] = 1;
                return;
            }
        }
        auto reader = DataFileReader::open(path, FooterItems::Directory);
        if (!reader) {
            // Another process just created it and has not written its header,
            // or crashed before that. Either way it holds no commit.
            const auto size = dstd::fs::file_size(dstd::PathView(path));
            if (!size || *size >= FILE_HEADER_BYTES) {
                open_errors[index] = reader.error();
            }
            return;
        }
        detail::FileData& file = opened[index].emplace(std::move(*reader));
        file.name = names[index];
        file.writer_random = file.reader.index().writer_random;
    };
    if (auto ran = for_each_index(runtime, names.size(), 1, open_one); !ran) {
        return dstd::Unexpected<dstd::Error>(ran.error());
    }
    const auto open_done = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < names.size(); ++index) {
        if (open_errors[index]) {
            warn_skipped(dir, names[index], *open_errors[index]);
            skipped_.push_back({names[index], *open_errors[index]});
            continue;
        }
        if (opened[index]) files_.push_back(std::move(*opened[index]));
    }

    const auto group_commits = [&](std::size_t file_index) {
        build_commits(files_[file_index], file_index);
        if (!verified_) return;
        const std::lock_guard<std::mutex> held(verified_->mutex);
        for (const auto& commit : files_[file_index].commits) {
            if (commit->synced) continue;
            if (verified_->keys.contains(
                    verified_key(files_[file_index], commit->sequence))) {
                commit->synced = true;
            }
        }
    };
    if (auto ran =
            for_each_index(runtime, files_.size(), COMMIT_GRAIN, group_commits);
        !ran) {
        return dstd::Unexpected<dstd::Error>(ran.error());
    }

    dstd::Vector<detail::FileData> kept;
    kept.reserve(files_.size());
    for (detail::FileData& file : files_) {
        auto directory = build_file_directory(file);
        if (!directory) {
            warn_skipped(dir, file.name, directory.error());
            skipped_.push_back({file.name, directory.error()});
            continue;
        }
        kept.push_back(std::move(file));
    }
    files_ = std::move(kept);
    for (detail::FileData& file : files_) {
        const std::uint64_t size = file.reader.file_size();
        file.windows =
            dstd::Vector<std::atomic<std::uint32_t>>(static_cast<std::size_t>(
                (size + READ_RUN_BYTES - 1) / READ_RUN_BYTES));
    }
    for (std::size_t index = 0; index < files_.size(); ++index) {
        for (const dstd::UniquePtr<detail::Commit>& commit :
             files_[index].commits) {
            commit->file_index = index;
        }
    }
    const auto built = std::chrono::steady_clock::now();

    reads_ = dstd::make_unique<detail::ReadCounters>();
    open_stats_ = OpenStats{};
    open_stats_.files = files_.size();
    open_stats_.files_summarized = static_cast<std::size_t>(
        std::count(from_summary.begin(), from_summary.end(), 1));
    for (const detail::FileData& file : files_) {
        open_stats_.commits += file.commits.size();
        open_stats_.extents += file.reader.index().extent_refs.size();
    }
    open_stats_.ms_list =
        std::chrono::duration<double, std::milli>(listed - start).count();
    open_stats_.ms_open_files =
        std::chrono::duration<double, std::milli>(open_done - listed).count();
    open_stats_.ms_tables =
        std::chrono::duration<double, std::milli>(built - open_done).count();
    open_stats_.ms_total = ms_since(start);
    return {};
}

namespace {

struct FileCheckOut {
    dstd::Optional<dstd::Error> failure;
    std::uint64_t commits = 0;
    std::uint64_t blocks = 0;
    std::uint64_t metadata = 0;
    std::uint64_t segments = 0;
    dstd::Vector<BadCommit> bad_commits;
    dstd::Vector<BadSegment> bad_segments;
};

struct BigRun {
    BigRef ref;
    std::uint64_t sequence = 0;
};

struct PendingSegments {
    std::uint64_t sequence = 0;
    std::uint32_t copy_id = 0;
    BlockHandle segments;
};

template <class T, class Offset>
void sort_by_offset(dstd::Vector<T>& items, Offset offset_of) {
    std::sort(items.begin(), items.end(), [&](const T& a, const T& b) {
        if (offset_of(a) != offset_of(b)) return offset_of(a) < offset_of(b);
        return a.sequence < b.sequence;
    });
}

}  // namespace

struct Snapshot::FileCheck {
    const Snapshot& snapshot;
    const detail::FileData& file;
    FileCheckOut& out;
    detail::PageHold walked;

    void fail(const dstd::Error& error) { out.failure = error; }

    void bad_commit(std::uint64_t sequence) {
        out.bad_commits.push_back({file.name, sequence});
    }

    template <class T, class Offset>
    void bad_commits_from(const dstd::Vector<T>& items, std::size_t at,
                          Offset offset_of) {
        const auto offset = offset_of(items[at]);
        for (; at < items.size() && offset_of(items[at]) == offset; ++at) {
            bad_commit(items[at].sequence);
        }
    }

    // Verifies one block. False when the read itself failed; `good` is false
    // when its bytes do not match.
    bool verify(const ExtentRef& ref, bool& good) {
        BlockReads reads;
        auto verified = file.reader.verify_block(ref, &reads);
        snapshot.reads_->add(0, 1, 0, reads);
        good = static_cast<bool>(verified);
        if (!verified && !verified.error().is(dstd::Condition::Parse)) {
            fail(verified.error());
            return false;
        }
        return true;
    }

    static dstd::Error block_error(const detail::LoadStatus& status) {
        return status.state == detail::LoadState::Failed
                   ? status.error
                   : dstd::Error(dstd::Condition::Parse);
    }

    bool walkable(const detail::ExtentSlot& extent,
                  const detail::SegmentSlot& slot, std::uint64_t sequence,
                  dstd::Vector<BigRun>* bigs) {
        auto bytes = snapshot.read_raw(file, extent, slot.offset_in_extent,
                                       slot.length, walked);
        if (!bytes) return false;
        auto view = SegmentView::open(*bytes);
        if (!view) return false;
        if (bigs != nullptr) {
            for (SegmentView::Cursor at = view->lower_bound(ByteView{});
                 !at.at_end(); at.next()) {
                if (at.entry().is_big()) {
                    bigs->push_back({at.entry().big_ref(), sequence});
                }
            }
        }
        return true;
    }

    // A file without a tail: its commits are checked one by one.
    void check_commits() {
        for (const dstd::UniquePtr<detail::Commit>& owned : file.commits) {
            detail::Commit& commit = *owned;
            ++out.commits;
            const detail::LoadStatus status = snapshot.load_index(commit);
            if (status.state != detail::LoadState::Good) {
                if (status.state == detail::LoadState::Failed) {
                    fail(status.error);
                    return;
                }
                bad_commit(commit.sequence);
                continue;
            }
            out.blocks += commit.blocks.size();
            if (!commit.synced) {
                const detail::LoadStatus checked =
                    snapshot.verify_commit(commit);
                if (checked.state == detail::LoadState::Failed) {
                    fail(checked.error);
                    return;
                }
                if (checked.state != detail::LoadState::Good) {
                    bad_commit(commit.sequence);
                    continue;
                }
            } else if (!check_synced_blocks(commit)) {
                return;
            }
            check_commit_segments(commit);
        }
    }

    bool check_synced_blocks(detail::Commit& commit) {
        for (std::size_t block = 0; block < commit.blocks.size(); ++block) {
            const detail::CommitExtent& held = commit.blocks[block];
            bool good = false;
            if (!verify(held.ref, good)) return false;
            if (!good) {
                bad_commit(commit.sequence);
                continue;
            }
            if (held.listed == nullptr || held.listed->segments.empty()) {
                continue;
            }
            const detail::LoadStatus extent =
                snapshot.load_extent(commit, block);
            if (extent.state == detail::LoadState::Failed) {
                fail(extent.error);
                return false;
            }
            if (extent.state != detail::LoadState::Good) {
                bad_commit(commit.sequence);
            }
        }
        return true;
    }

    void check_commit_segments(detail::Commit& commit) {
        for (const dstd::UniquePtr<detail::LazyBlock>& block :
             commit.segment_blocks) {
            for (const detail::SegmentSlot& slot : block->segments) {
                // An extent that does not verify is reported as its
                // commit, since its bytes are not there to walk.
                if (commit.synced &&
                    snapshot.load_extent(commit, slot.block).state !=
                        detail::LoadState::Good) {
                    continue;
                }
                ++out.segments;
                const detail::ExtentSlot& extent =
                    commit.extent_slots[slot.block];
                if (walkable(extent, slot, commit.sequence, nullptr)) {
                    continue;
                }
                out.bad_segments.push_back(
                    {file.name, commit.blocks[slot.block].ref.offset,
                     slot.offset_in_extent});
            }
        }
    }

    // Returns false when a block of the tree does not load.
    bool walk_tree(const detail::LazyBlock& node, std::uint32_t level,
                   dstd::Vector<PendingSegments>& pending) {
        detail::LazyBlock& block = const_cast<detail::LazyBlock&>(node);
        ++out.metadata;
        const detail::LoadStatus status = snapshot.load_block(file, block);
        if (status.state != detail::LoadState::Good) {
            fail(block_error(status));
            return false;
        }
        if (level > 1) {
            for (const dstd::UniquePtr<detail::LazyBlock>& child :
                 node.children) {
                if (!walk_tree(*child, level - 1, pending)) return false;
            }
            return true;
        }
        for (std::size_t i = 0; i < node.directory->size(); ++i) {
            auto entry = node.directory->entry(i);
            if (!entry) {
                fail(dstd::Error(dstd::Condition::Parse));
                return false;
            }
            for (const DirectoryCopy& copy : entry->copies) {
                for (const DirectoryCommit& range : copy.commits) {
                    pending.push_back(
                        {range.sequence, copy.local_id, range.segments});
                }
            }
        }
        return true;
    }

    void count_commits(const dstd::Vector<PendingSegments>& pending) {
        dstd::Vector<std::uint64_t> sequences;
        sequences.reserve(pending.size());
        for (const PendingSegments& entry : pending) {
            sequences.push_back(entry.sequence);
        }
        std::sort(sequences.begin(), sequences.end());
        sequences.erase(std::unique(sequences.begin(), sequences.end()),
                        sequences.end());
        out.commits = sequences.size();
    }

    // Per extent: 0 not checked yet, 1 good, 2 bad. False on a failed read.
    bool classify_extent(const detail::SegmentSlot& slot,
                         dstd::Vector<std::uint8_t>& seen,
                         std::uint8_t& known_out) {
        std::uint8_t& known = seen[slot.extent_index];
        if (known == 0) {
            ++out.blocks;
            bool good = false;
            if (!verify(slot.extent, good)) return false;
            known = good ? 1 : 2;
            if (known == 1) {
                const detail::LoadStatus extent =
                    snapshot.load_extent(file, slot.extent_index, slot.extent);
                if (extent.state == detail::LoadState::Failed) {
                    fail(extent.error);
                    return false;
                }
                if (extent.state != detail::LoadState::Good) known = 2;
            }
        }
        known_out = known;
        return true;
    }

    bool check_segment_block(const detail::LazyBlock& block,
                             dstd::Vector<std::uint8_t>& seen,
                             dstd::Vector<BigRun>& bigs) {
        for (const detail::SegmentSlot& slot : block.segments) {
            if (slot.extent_index >= seen.size()) {
                bad_commit(slot.sequence);
                continue;
            }
            std::uint8_t known = 0;
            if (!classify_extent(slot, seen, known)) return false;
            if (known != 1) {
                bad_commit(slot.sequence);
                continue;
            }
            ++out.segments;
            if (walkable(file.extent_slots.slot(slot.extent_index), slot,
                         slot.sequence, &bigs)) {
                continue;
            }
            out.bad_segments.push_back(
                {file.name, slot.extent.offset, slot.offset_in_extent});
        }
        return true;
    }

    bool check_pending(dstd::Vector<PendingSegments>& pending,
                       dstd::Vector<BigRun>& bigs) {
        const auto offset_of = [](const PendingSegments& entry) {
            return entry.segments.offset;
        };
        // Per extent: 0 not checked yet, 1 good, 2 bad.
        dstd::Vector<std::uint8_t> seen(file.extent_slots.size(), 0);
        sort_by_offset(pending, offset_of);
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].segments.empty()) continue;
            if (i > 0 && offset_of(pending[i - 1U]) == offset_of(pending[i])) {
                continue;
            }
            detail::LazyBlock block;
            block.kind = detail::BlockKind::Segments;
            block.handle = pending[i].segments;
            ++out.metadata;
            const detail::LoadStatus status = snapshot.load_block(file, block);
            if (status.state != detail::LoadState::Good) {
                if (status.state == detail::LoadState::Failed) {
                    fail(status.error);
                    return false;
                }
                bad_commits_from(pending, i, offset_of);
                continue;
            }
            if (!check_segment_block(block, seen, bigs)) return false;
        }
        return true;
    }

    // Each run is read once. No directory block names them.
    bool check_bigs(dstd::Vector<BigRun>& bigs) {
        const auto offset_of = [](const BigRun& run) { return run.ref.offset; };
        sort_by_offset(bigs, offset_of);
        for (std::size_t i = 0; i < bigs.size(); ++i) {
            if (i > 0 && offset_of(bigs[i - 1U]) == offset_of(bigs[i])) {
                continue;
            }
            ++out.blocks;
            const BigRef& big = bigs[i].ref;
            bool good = big.length != 0 &&
                        big.length <= std::numeric_limits<std::uint32_t>::max();
            if (good &&
                !verify(
                    ExtentRef{big.offset,
                              static_cast<std::uint32_t>(big.length), big.crc},
                    good)) {
                return false;
            }
            if (!good) bad_commits_from(bigs, i, offset_of);
        }
        return true;
    }

    void run() {
        if (!file.reader.index().tail.has_value()) {
            check_commits();
            return;
        }
        dstd::Vector<PendingSegments> pending;
        for (const dstd::UniquePtr<detail::LazyBlock>& root : file.roots) {
            if (!walk_tree(*root, file.directory_levels, pending)) return;
        }
        count_commits(pending);
        dstd::Vector<BigRun> bigs;
        if (!check_pending(pending, bigs)) return;
        check_bigs(bigs);
    }
};

dstd::Expected<CheckReport, dstd::Error> Snapshot::check() const {
    CheckReport report;
    report.files = files_.size();
    const std::size_t file_count = files_.size();
    dstd::Vector<FileCheckOut> outs(file_count);

    const auto check_file = [&](std::size_t index) {
        FileCheck{*this, files_[index], outs[index], {}}.run();
    };
    if (auto ran =
            for_each_index(fork_join_runtime(), file_count, 1, check_file);
        !ran) {
        return dstd::Unexpected<dstd::Error>(ran.error());
    }

    for (std::size_t index = 0; index < file_count; ++index) {
        FileCheckOut& out = outs[index];
        if (out.failure) {
            return dstd::Unexpected<dstd::Error>(*out.failure);
        }
        report.commits += out.commits;
        report.blocks += out.blocks;
        report.metadata_blocks += out.metadata;
        report.segments += out.segments;
        // A commit is reported once, however many of its blocks fail.
        std::sort(out.bad_commits.begin(), out.bad_commits.end(),
                  [](const BadCommit& a, const BadCommit& b) {
                      return a.sequence < b.sequence;
                  });
        for (std::size_t at = 0; at < out.bad_commits.size(); ++at) {
            const BadCommit& bad = out.bad_commits[at];
            if (at > 0 && out.bad_commits[at - 1U].sequence == bad.sequence) {
                continue;
            }
            report.bad_commits.push_back(bad);
        }
        for (const BadSegment& bad : out.bad_segments) {
            report.bad_segments.push_back(bad);
        }
    }
    const ReadStats reads = read_stats();
    report.bytes_read = reads.bytes_read;
    return report;
}

template <typename Visit>
dstd::Expected<void, dstd::Error> Snapshot::walk_directory(
    const detail::FileData& file, Visit&& visit) const {
    dstd::Optional<dstd::Error> failure;
    const auto walk = [&](const detail::LazyBlock& node, std::uint32_t level,
                          auto&& self) -> void {
        if (failure) return;
        detail::LazyBlock& block = const_cast<detail::LazyBlock&>(node);
        const detail::LoadStatus status = load_block(file, block);
        if (status.state != detail::LoadState::Good) {
            failure = status.state == detail::LoadState::Failed
                          ? status.error
                          : dstd::Error(dstd::Condition::Parse);
            return;
        }
        if (level > 1) {
            for (const dstd::UniquePtr<detail::LazyBlock>& child :
                 node.children) {
                self(*child, level - 1, self);
            }
            return;
        }
        for (std::size_t index = 0; index < node.directory->size(); ++index) {
            auto visited = visit(*node.directory, index);
            if (!visited) {
                failure = visited.error();
                return;
            }
        }
    };
    if (file.directory_levels != 0) {
        for (const dstd::UniquePtr<detail::LazyBlock>& root : file.roots) {
            walk(*root, file.directory_levels, walk);
        }
    }
    if (failure) return dstd::Unexpected<dstd::Error>(*failure);
    return {};
}

dstd::Expected<dstd::Vector<ScopeGeneration>, dstd::Error>
Snapshot::generations() const {
    struct Item {
        DirectoryKey key;
        std::uint64_t generation = 0;
    };
    dstd::Vector<Item> items;
    for (const detail::FileData& file : files_) {
        // A block that does not load fails the whole call: a writer takes its
        // generations from here, and a scope left out would start again low.
        auto walked = walk_directory(
            file,
            [&items](const ScopeDirectory& directory,
                     std::size_t index) -> dstd::Expected<void, dstd::Error> {
                auto key = directory.key(index);
                auto highest = directory.highest_generation(index);
                if (!key || !highest) {
                    return dstd::Unexpected<dstd::Error>(
                        dstd::Error(dstd::Condition::Parse));
                }
                items.push_back({std::move(*key), *highest});
                return {};
            });
        if (!walked) return dstd::Unexpected<dstd::Error>(walked.error());
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return compare_directory_keys(a.key, b.key) < 0;
    });
    dstd::Vector<ScopeGeneration> out;
    for (std::size_t at = 0; at < items.size();) {
        std::uint64_t highest = items[at].generation;
        std::size_t end = at + 1;
        while (end < items.size() &&
               compare_directory_keys(items[end].key, items[at].key) == 0) {
            highest = std::max(highest, items[end].generation);
            ++end;
        }
        const DirectoryKey& key = items[at].key;
        out.push_back({dstd::String(key.name.data(), key.name.size()),
                       key.format_version, scope_of(key), highest});
        at = end;
    }
    return out;
}

dstd::Expected<PackPlan, dstd::Error> Snapshot::pack_plan(
    std::uint64_t small_file_bytes, double dead_ratio) const {
    struct Row {
        std::size_t file = 0;
        DirectoryKey key;
        /// The newest copy this file has that a reader can see.
        std::uint64_t copy_generation = 0;
        CopyState copy_state = CopyState::Ready;
        std::uint64_t params_hash = 0;
        dstd::Vector<SourceStat> sources;
        std::uint64_t drop_generation = 0;
        std::uint64_t range_drop_generation = 0;
    };
    dstd::Vector<Row> rows;
    for (std::size_t file = 0; file < files_.size(); ++file) {
        auto walked = walk_directory(
            files_[file],
            [&rows, file](const ScopeDirectory& directory, std::size_t index)
                -> dstd::Expected<void, dstd::Error> {
                auto entry = directory.entry(index);
                if (!entry) {
                    return dstd::Unexpected<dstd::Error>(entry.error());
                }
                Row row;
                row.file = file;
                row.key = entry->key;
                for (const DirectoryCopy& copy : entry->copies) {
                    if (copy.state == CopyState::Building ||
                        copy.generation < row.copy_generation) {
                        continue;
                    }
                    row.copy_generation = copy.generation;
                    row.copy_state = copy.state;
                    row.params_hash = copy.params_hash;
                    row.sources = copy.sources;
                }
                for (const DirectoryDrop& drop : entry->drops) {
                    std::uint64_t& newest =
                        drop.begin.empty() && drop.end.empty()
                            ? row.drop_generation
                            : row.range_drop_generation;
                    newest = std::max(newest, drop.generation);
                }
                rows.push_back(std::move(row));
                return {};
            });
        if (!walked) return dstd::Unexpected<dstd::Error>(walked.error());
    }
    dstd::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return compare_directory_keys(a.key, b.key) < 0;
    });

    const auto spec_of = [this](const DirectoryKey& key) {
        NamespaceSpec spec;
        spec.name = dstd::String(key.name.data(), key.name.size());
        spec.format_version = key.format_version;
        for (const detail::FileData& file : files_) {
            for (const NamespaceItem& item : file.reader.index().namespaces) {
                if (item.name != spec.name ||
                    item.format_version != key.format_version) {
                    continue;
                }
                spec.fold = static_cast<FoldKind>(item.fold_kind);
                spec.codec = static_cast<PageCodec>(item.codec);
                spec.durable = item.durable != 0;
                spec.shared = item.shared != 0;
                return spec;
            }
        }
        return spec;
    };

    struct Group {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t newest_row = 0;
        bool dropped = false;
        dstd::Vector<std::size_t> holders;
    };
    const auto reads_after = [this](std::size_t a, std::size_t b) {
        const detail::FileData& file_a = files_[a];
        const detail::FileData& file_b = files_[b];
        if (file_a.writer_random != file_b.writer_random) {
            return file_a.writer_random > file_b.writer_random;
        }
        return file_a.name.compare(file_b.name) > 0;
    };
    dstd::Vector<Group> groups;
    dstd::Vector<std::uint8_t> file_live(files_.size(), 0);
    dstd::Vector<dstd::Set<std::size_t>> file_replaced_by(files_.size());
    for (std::size_t at = 0; at < rows.size();) {
        Group group;
        group.begin = at;
        std::uint64_t current = 0;
        std::uint64_t whole_drop = 0;
        for (group.end = at;
             group.end < rows.size() &&
             compare_directory_keys(rows[group.end].key, rows[at].key) == 0;
             ++group.end) {
            const Row& row = rows[group.end];
            // The tie-break of find_group, so the state and sources pack keeps
            // belong to the copy whose rows it scans.
            const bool newer =
                row.copy_generation > current ||
                (row.copy_generation != 0 && row.copy_generation == current &&
                 reads_after(row.file, rows[group.newest_row].file));
            if (newer) {
                current = row.copy_generation;
                group.newest_row = group.end;
            }
            whole_drop = std::max(whole_drop, row.drop_generation);
        }
        group.dropped = whole_drop > current;
        const bool shared = spec_of(rows[at].key).shared;
        for (std::size_t i = group.begin; i < group.end; ++i) {
            const Row& row = rows[i];
            const bool holds =
                group.dropped
                    ? row.drop_generation == whole_drop
                    : (shared && row.copy_generation != 0 &&
                       row.copy_generation >= whole_drop) ||
                          (current != 0 && row.copy_generation == current) ||
                          row.range_drop_generation > current;
            if (holds) {
                group.holders.push_back(row.file);
                file_live[row.file] = 1;
            }
        }
        for (std::size_t i = group.begin; i < group.end; ++i) {
            for (const std::size_t holder : group.holders) {
                if (holder != rows[i].file) {
                    file_replaced_by[rows[i].file].insert(holder);
                }
            }
        }
        at = group.end;
        if (!group.holders.empty()) groups.push_back(std::move(group));
    }

    const auto closed = [this](std::size_t file) {
        return files_[file].reader.index().tail.has_value();
    };
    const auto is_small = [&](std::size_t file) {
        return files_[file].reader.file_size() < small_file_bytes;
    };
    dstd::Vector<std::uint64_t> live_bytes(files_.size(), 0);
    dstd::Vector<std::uint64_t> total_bytes(files_.size(), 0);
    bool any_large = false;
    for (std::size_t file = 0; file < files_.size(); ++file) {
        any_large = any_large ||
                    (closed(file) && file_live[file] != 0 && !is_small(file));
    }
    if (any_large) {
        for (const Group& group : groups) {
            const DirectoryKey& key = rows[group.begin].key;
            auto found = find_group(spec_of(key), scope_of(key));
            if (!found) return dstd::Unexpected<dstd::Error>(found.error());
            if (*found == nullptr) continue;
            const detail::ScopeGroup& held = **found;
            for (std::size_t c = 0; c < held.copies.size(); ++c) {
                const detail::CopyData& copy = held.copies[c];
                if (is_small(copy.file_index)) continue;
                std::uint64_t bytes = 0;
                for (const detail::CopyCommit& commit : copy.commits) {
                    auto run = copy_segments(copy, commit);
                    if (!run) return dstd::Unexpected<dstd::Error>(run.error());
                    if (run->block == nullptr) continue;
                    for (std::size_t i = run->begin; i < run->end; ++i) {
                        bytes += run->block->segments[i].length;
                    }
                }
                total_bytes[copy.file_index] += bytes;
                if (!held.dropped &&
                    (c == held.current ||
                     (held.shared && c >= held.first_visible))) {
                    live_bytes[copy.file_index] += bytes;
                }
            }
        }
    }

    dstd::Vector<std::uint8_t> packed(files_.size(), 0);
    std::size_t small_live = 0;
    for (std::size_t file = 0; file < files_.size(); ++file) {
        if (closed(file) && file_live[file] != 0 && is_small(file)) {
            ++small_live;
        }
    }
    PackPlan plan;
    for (std::size_t file = 0; file < files_.size(); ++file) {
        if (!closed(file) || file_live[file] == 0) continue;
        const bool wanted =
            is_small(file)
                ? small_live >= 2
                : total_bytes[file] != 0 &&
                      static_cast<double>(total_bytes[file] -
                                          live_bytes[file]) >=
                          dead_ratio * static_cast<double>(total_bytes[file]);
        if (!wanted) continue;
        packed[file] = 1;
        ++plan.files_packed;
    }
    for (const Group& group : groups) {
        bool chosen = false;
        bool held_open = false;
        for (const std::size_t holder : group.holders) {
            chosen = chosen || packed[holder] != 0;
            held_open = held_open || !closed(holder);
        }
        if (!chosen || held_open) continue;
        const Row& newest = rows[group.newest_row];
        PackTarget target;
        target.spec = spec_of(rows[group.begin].key);
        target.scope = scope_of(rows[group.begin].key);
        if (group.dropped) {
            target.state = ScopeState::Absent;
        } else {
            target.state = newest.copy_state == CopyState::Failed
                               ? ScopeState::Failed
                               : ScopeState::Ready;
            target.params_hash = newest.params_hash;
            target.sources = newest.sources;
        }
        plan.targets.push_back(std::move(target));
    }
    for (std::size_t file = 0; file < files_.size(); ++file) {
        if (!closed(file) || file_live[file] != 0) continue;
        ReplacedFile replaced;
        replaced.name = files_[file].name;
        replaced.size = files_[file].reader.file_size();
        // A file without a footer may have lost commits to a crash, so it
        // cannot stand in for the last complete copy.
        bool complete = true;
        for (const std::size_t holder : file_replaced_by[file]) {
            complete = complete && closed(holder);
            replaced.replaced_by.push_back(files_[holder].name);
        }
        if (complete) plan.replaced.push_back(std::move(replaced));
    }
    return plan;
}

detail::GroupSlots::~GroupSlots() {
    for (std::atomic<ScopeGroup*>& slot : slots_) {
        // Each group came from dstd::make_unique, so its own deleter frees it.
        dstd::UniquePtr<ScopeGroup> owned(slot.load(std::memory_order_relaxed));
    }
}

const detail::ScopeGroup* detail::GroupSlots::publish(
    std::size_t index, dstd::UniquePtr<ScopeGroup> group) const {
    ScopeGroup* expected = nullptr;
    ScopeGroup* const fresh = group.get();
    if (slots_[index].compare_exchange_strong(expected, fresh,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        (void)group.release();
        return fresh;
    }
    return expected;
}

dstd::Expected<const detail::ScopeGroup*, dstd::Error> Snapshot::find_group(
    const NamespaceSpec& spec, const ScopeKey& scope) const {
    const dstd::Span<const dstd::String> paths(scope.paths.data(),
                                               scope.paths.size());
    const auto block_error = [](const detail::LoadStatus& status) {
        return status.state == detail::LoadState::Failed
                   ? status.error
                   : dstd::Error(dstd::Condition::Parse);
    };
    const dstd::String key =
        group_key(spec.name, spec.format_version, scope.kind, paths);
    if (const detail::ScopeGroup* known = groups_->find(key)) return known;
    const std::uint64_t hash =
        scope_hash(spec.name, spec.format_version, scope.kind, paths);
    const auto find_leaf = [&](const detail::FileData& file, std::size_t& entry)
        -> dstd::Expected<const detail::LazyBlock*, dstd::Error> {
        if (file.directory_levels == 0) {
            return static_cast<const detail::LazyBlock*>(nullptr);
        }
        const dstd::Optional<FileTail>& tail = file.reader.index().tail;
        if (tail.has_value() &&
            !scope_filter_may_hold(bytes_of(tail->scope_filter), hash)) {
            reads_->add_filtered();
            return static_cast<const detail::LazyBlock*>(nullptr);
        }
        const auto floor = [&](const SlotTable& table) {
            std::size_t low = 0;
            std::size_t high = table.size();
            while (low < high) {
                const std::size_t mid = low + (high - low) / 2;
                if (compare_directory_key(table.key(mid), spec.name,
                                          spec.format_version, scope.kind,
                                          paths) <= 0) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }
            return low;
        };
        std::size_t at = 0;
        if (!(file.roots.size() == 1 && file.top_table.empty())) {
            const std::size_t below = floor(file.top);
            if (below == 0) {
                return static_cast<const detail::LazyBlock*>(nullptr);
            }
            at = below - 1;
        }
        detail::LazyBlock* node = file.roots[at].get();
        while (node->levels > 1) {
            const detail::LoadStatus status = load_block(file, *node);
            if (status.state != detail::LoadState::Good) {
                return dstd::Unexpected<dstd::Error>(block_error(status));
            }
            const std::size_t below = floor(node->separators);
            if (below == 0) {
                return static_cast<const detail::LazyBlock*>(nullptr);
            }
            node = node->children[below - 1].get();
        }
        const detail::LoadStatus status = load_block(file, *node);
        if (status.state != detail::LoadState::Good) {
            return dstd::Unexpected<dstd::Error>(block_error(status));
        }
        const std::size_t found = node->directory->find(
            spec.name, spec.format_version, scope.kind, paths);
        if (found == node->directory->size()) {
            return static_cast<const detail::LazyBlock*>(nullptr);
        }
        entry = found;
        return node;
    };

    std::size_t owner = files_.size();
    std::size_t owner_entry = 0;
    const detail::LazyBlock* owner_leaf = nullptr;
    for (std::size_t file = 0; file < files_.size(); ++file) {
        std::size_t entry = 0;
        auto leaf = find_leaf(files_[file], entry);
        if (!leaf) return dstd::Unexpected<dstd::Error>(leaf.error());
        if (*leaf == nullptr) continue;
        owner = file;
        owner_entry = entry;
        owner_leaf = *leaf;
        break;
    }
    if (owner == files_.size()) return nullptr;
    if (const detail::ScopeGroup* known =
            owner_leaf->groups->get(owner_entry)) {
        groups_->add(key, known);
        return known;
    }

    auto group = dstd::make_unique<detail::ScopeGroup>();
    dstd::Vector<DirectoryDrop> drops;
    for (std::size_t file = owner; file < files_.size(); ++file) {
        const detail::FileData& held = files_[file];
        const bool closed_file = held.reader.index().tail.has_value();
        std::size_t index = 0;
        auto leaf = find_leaf(held, index);
        if (!leaf) return dstd::Unexpected<dstd::Error>(leaf.error());
        if (*leaf == nullptr) continue;
        auto entry = (*leaf)->directory->entry(index);
        if (!entry) return dstd::Unexpected<dstd::Error>(entry.error());
        group->highest_generation =
            std::max(group->highest_generation, entry->highest_generation);
        group->shared = group->shared || namespace_is_shared(held, spec);
        for (const DirectoryCopy& listed : entry->copies) {
            // A copy still building is not readable until it is published.
            if (listed.state == CopyState::Building) continue;
            detail::CopyData copy;
            copy.file_index = file;
            copy.generation = listed.generation;
            copy.params_hash = listed.params_hash;
            copy.fold = static_cast<FoldKind>(listed.fold_kind);
            copy.state = listed.state;
            copy.local_id = listed.local_id;
            copy.sources = listed.sources;
            for (const DirectoryErase& erase : listed.erases) {
                copy.erases.push_back({erase.ordinal, erase.begin, erase.end});
            }
            // The commits that name one offset share its segment block.
            dstd::Vector<dstd::Pair<std::uint64_t, detail::LazyBlock*>> blocks;
            const auto shared_block = [group = group.get(),
                                       &blocks](const BlockHandle& handle) {
                const auto at = std::lower_bound(
                    blocks.begin(), blocks.end(), handle.offset,
                    [](const dstd::Pair<std::uint64_t, detail::LazyBlock*>&
                           known,
                       std::uint64_t offset) { return known.first < offset; });
                if (at != blocks.end() && at->first == handle.offset) {
                    return at->second;
                }
                auto block = dstd::make_unique<detail::LazyBlock>();
                block->kind = detail::BlockKind::Segments;
                block->handle = handle;
                detail::LazyBlock* shared = block.get();
                group->segments.push_back(std::move(block));
                blocks.insert(at, {handle.offset, shared});
                return shared;
            };
            for (const DirectoryCommit& range : listed.commits) {
                detail::Commit* commit =
                    closed_file ? nullptr : commit_of(held, range.sequence);
                if (!closed_file && commit == nullptr) continue;
                detail::LazyBlock* segments = nullptr;
                if (closed_file && !range.segments.empty()) {
                    segments = shared_block(range.segments);
                }
                copy.commits.push_back({range.sequence, range.first_key,
                                        range.last_key, segments, commit});
            }
            group->copies.push_back(std::move(copy));
        }
        drops.insert(drops.end(), entry->drops.begin(), entry->drops.end());
    }

    std::sort(group->copies.begin(), group->copies.end(),
              [this](const detail::CopyData& a, const detail::CopyData& b) {
                  if (a.generation != b.generation) {
                      return a.generation < b.generation;
                  }
                  const detail::FileData& file_a = files_[a.file_index];
                  const detail::FileData& file_b = files_[b.file_index];
                  if (file_a.writer_random != file_b.writer_random) {
                      return file_a.writer_random < file_b.writer_random;
                  }
                  return file_a.name.compare(file_b.name) < 0;
              });
    if (group->copies.empty()) {
        group->dropped = true;
    } else {
        group->current = group->copies.size() - 1U;
        const std::uint64_t current = group->copies[group->current].generation;
        if (group->shared) {
            std::uint64_t floor = 0;
            for (const DirectoryDrop& drop : drops) {
                floor = std::max(floor, drop.generation);
            }
            while (group->first_visible < group->copies.size() &&
                   group->copies[group->first_visible].generation < floor) {
                ++group->first_visible;
            }
        }
        for (const DirectoryDrop& drop : drops) {
            if (drop.generation <= current) continue;
            if (drop.begin.empty() && drop.end.empty()) {
                group->dropped = true;
                continue;
            }
            const auto* begin =
                reinterpret_cast<const std::uint8_t*>(drop.begin.data());
            const auto* end =
                reinterpret_cast<const std::uint8_t*>(drop.end.data());
            group->hidden.push_back(
                {dstd::Vector<std::uint8_t>(begin, begin + drop.begin.size()),
                 dstd::Vector<std::uint8_t>(end, end + drop.end.size())});
        }
    }
    const detail::ScopeGroup* published =
        owner_leaf->groups->publish(owner_entry, std::move(group));
    groups_->add(key, published);
    return published;
}

ScopeState Snapshot::state(const NamespaceSpec& spec,
                           const ScopeKey& scope) const {
    return copy_info(spec, scope).state;
}

CopyInfo Snapshot::copy_info(const NamespaceSpec& spec,
                             const ScopeKey& scope) const {
    CopyInfo info;
    auto found = find_group(spec, scope);
    // The state has no channel for the error of a block that could not be
    // read, so it reads Bad. `get` and a cursor report the error itself.
    if (!found) {
        info.state = ScopeState::Bad;
        return info;
    }
    const detail::ScopeGroup* group = *found;
    if (group == nullptr || group->dropped || group->copies.empty()) {
        return info;
    }
    const detail::CopyData& copy = group->copies[group->current];
    info.generation = copy.generation;
    info.params_hash = copy.params_hash;
    info.sources = copy.sources;
    const detail::LoadStatus status = ensure_copy(copy);
    // The state has no channel for the error of a commit whose block could not
    // be read, so it reads Bad. `get` and a cursor report the error itself.
    if (status.state == detail::LoadState::Bad ||
        status.state == detail::LoadState::Failed) {
        info.state = ScopeState::Bad;
        return info;
    }
    info.state = copy.state == CopyState::Failed ? ScopeState::Failed
                                                 : ScopeState::Ready;
    return info;
}

detail::LoadStatus Snapshot::verify_commit(detail::Commit& commit) const {
    const auto state_of = [&](detail::LoadState known) {
        return detail::LoadStatus{known, known == detail::LoadState::Failed
                                             ? commit.error
                                             : dstd::Error{}};
    };
    const auto known = static_cast<detail::LoadState>(
        commit.state.load(std::memory_order_acquire));
    if (known != detail::LoadState::Unknown) return state_of(known);

    std::lock_guard<std::mutex> locked(commit.mutex);
    const auto again = static_cast<detail::LoadState>(
        commit.state.load(std::memory_order_acquire));
    if (again != detail::LoadState::Unknown) return state_of(again);

    const DataFileReader& reader = files_[commit.file_index].reader;
    dstd::Vector<std::size_t> extent_blocks;
    dstd::Vector<std::size_t> big_blocks;
    for (std::size_t block = 0; block < commit.blocks.size(); ++block) {
        const detail::CommitExtent& held = commit.blocks[block];
        if (held.listed != nullptr && !held.listed->segments.empty()) {
            extent_blocks.push_back(block);
        } else {
            big_blocks.push_back(block);
        }
    }

    // Every extent in one read, on this thread: the commit's lock is held, and
    // a fork-join here could wait on a pool worker that waits on this lock. The
    // stored bytes live only while the commit is checked.
    dstd::Vector<ExtentRef> refs;
    dstd::Vector<ExtentTarget> targets;
    dstd::Vector<dstd::Vector<std::uint8_t>> stored(commit.blocks.size());
    dstd::Vector<std::uint8_t> bad(commit.blocks.size(), 0);
    for (const std::size_t block : extent_blocks) {
        refs.push_back(commit.blocks[block].ref);
        targets.push_back({&stored[block], &bad[block]});
    }
    BlockReads reads;
    bool any_bad = false;
    dstd::Optional<dstd::Error> failure;
    if (!refs.empty()) {
        auto read = reader.read_extents(refs, targets);
        if (!read) {
            failure = read.error();
        } else {
            reads = *read;
            for (const std::size_t block : extent_blocks) {
                if (bad[block] != 0) any_bad = true;
            }
        }
    }
    // A segment that cannot be read makes the whole commit Bad.
    dstd::Vector<dstd::Optional<ExtentPages>> whole(commit.blocks.size());
    dstd::Vector<dstd::SharedPtr<const PageDict>> dicts(commit.blocks.size());
    if (!failure && !any_bad) {
        for (const std::size_t block : extent_blocks) {
            const ByteView bytes = bytes_of(stored[block]);
            auto pages = ExtentPages::open(bytes);
            auto table = ExtentPages::open_table(bytes, bytes.size());
            if (!pages || !table) {
                any_bad = true;
                break;
            }
            auto dict = page_dict(files_[commit.file_index], pages->dict());
            if (!dict) {
                if (dict.error().is(dstd::Condition::Io)) {
                    failure = dict.error();
                } else {
                    any_bad = true;
                }
                break;
            }
            dicts[block] = std::move(*dict);
            whole[block] = std::move(*pages);
            detail::ExtentSlot& slot = commit.extent_slots[block];
            slot.ref = commit.blocks[block].ref;
            slot.pages = std::move(*table);
        }
    }
    if (!failure && !any_bad) {
        dstd::Vector<std::uint8_t> owned;
        for (const dstd::UniquePtr<detail::LazyBlock>& block :
             commit.segment_blocks) {
            for (const detail::SegmentSlot& slot : block->segments) {
                if (slot.block >= whole.size() || !whole[slot.block]) {
                    any_bad = true;
                    break;
                }
                auto bytes =
                    decode_covering(*whole[slot.block], slot.offset_in_extent,
                                    slot.length, bytes_of(stored[slot.block]),
                                    0, owned, dicts[slot.block].get());
                if (!bytes || !SegmentView::open_index(*bytes)) {
                    any_bad = true;
                    break;
                }
            }
            if (any_bad) break;
        }
    }
    for (const std::size_t block : big_blocks) {
        if (failure) break;
        // A big value run keeps no bytes here: only its crc matters, and its
        // bytes are read again when a value is.
        BlockReads block_reads;
        auto verified =
            reader.verify_block(commit.blocks[block].ref, &block_reads);
        reads.calls += block_reads.calls;
        reads.bytes += block_reads.bytes;
        if (verified) continue;
        if (verified.error().is(dstd::Condition::Parse)) {
            any_bad = true;
        } else {
            failure = verified.error();
        }
    }

    detail::LoadState state = detail::LoadState::Good;
    if (any_bad) {
        state = detail::LoadState::Bad;
    } else if (failure) {
        state = detail::LoadState::Failed;
        commit.error = *failure;
    }
    reads_->add(state == detail::LoadState::Failed ? 0 : 1,
                commit.blocks.size(), 0, reads);
    if (state == detail::LoadState::Good && verified_) {
        const std::lock_guard<std::mutex> held(verified_->mutex);
        verified_->keys.insert(
            verified_key(files_[commit.file_index], commit.sequence));
    }
    commit.state.store(static_cast<std::uint8_t>(state),
                       std::memory_order_release);
    return state_of(state);
}

detail::LoadStatus Snapshot::ensure_copy(const detail::CopyData& copy) const {
    detail::LoadStatus worst;
    worst.state = detail::LoadState::Good;
    for (const detail::CopyCommit& held : copy.commits) {
        if (held.commit == nullptr || held.commit->synced) continue;
        detail::Commit& commit = *held.commit;
        detail::LoadStatus status = load_index(commit);
        if (status.state == detail::LoadState::Good) {
            status = verify_commit(commit);
        }
        if (status.state == detail::LoadState::Bad) return status;
        if (status.state == detail::LoadState::Failed &&
            worst.state != detail::LoadState::Failed) {
            worst = status;
        }
    }
    return worst;
}

detail::LoadStatus Snapshot::load_index(detail::Commit& commit) const {
    const auto state_of = [&](detail::LoadState known) {
        return detail::LoadStatus{known, known == detail::LoadState::Good
                                             ? dstd::Error{}
                                             : commit.index_error};
    };
    const auto known = static_cast<detail::LoadState>(
        commit.index_state.load(std::memory_order_acquire));
    if (known != detail::LoadState::Unknown) return state_of(known);

    std::lock_guard<std::mutex> locked(commit.index_mutex);
    const auto again = static_cast<detail::LoadState>(
        commit.index_state.load(std::memory_order_acquire));
    if (again != detail::LoadState::Unknown) return state_of(again);

    BlockReads reads;
    auto refs =
        files_[commit.file_index].reader.read_commit(*commit.summary, &reads);
    reads_->add_index(reads);
    detail::LoadState state = detail::LoadState::Good;
    if (!refs) {
        commit.index_error = refs.error();
        state = refs.error().is(dstd::Condition::Io) ? detail::LoadState::Failed
                                                     : detail::LoadState::Bad;
    } else {
        commit.listed_refs = std::move(*refs);
        dstd::Vector<const ExtentRefItem*> listed;
        listed.reserve(commit.listed_refs.size());
        for (const ExtentRefItem& ref : commit.listed_refs)
            listed.push_back(&ref);
        fill_commit(commit, std::move(listed));
    }
    commit.index_state.store(static_cast<std::uint8_t>(state),
                             std::memory_order_release);
    return state_of(state);
}

namespace {

dstd::Expected<void, dstd::Error> index_result(
    const detail::LoadStatus& status) {
    if (status.state == detail::LoadState::Good) return {};
    if (status.state == detail::LoadState::Failed) {
        return dstd::Unexpected<dstd::Error>(status.error);
    }
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

}  // namespace

detail::LoadStatus Snapshot::load_block(const detail::FileData& file,
                                        detail::LazyBlock& block) const {
    const auto state_of = [&](detail::LoadState known) {
        return detail::LoadStatus{known, known == detail::LoadState::Failed
                                             ? block.error
                                             : dstd::Error{}};
    };
    const auto known = static_cast<detail::LoadState>(
        block.state.load(std::memory_order_acquire));
    if (known != detail::LoadState::Unknown) return state_of(known);

    std::lock_guard<std::mutex> locked(block.mutex);
    const auto again = static_cast<detail::LoadState>(
        block.state.load(std::memory_order_acquire));
    if (again != detail::LoadState::Unknown) return state_of(again);

    BlockReads reads;
    auto bytes = file.reader.read_block(block.handle, &reads);
    reads_->add_metadata(reads);
    detail::LoadState state = detail::LoadState::Good;
    if (!bytes) {
        block.error = bytes.error();
        state = bytes.error().is(dstd::Condition::Io)
                    ? detail::LoadState::Failed
                    : detail::LoadState::Bad;
    } else {
        block.bytes = std::move(*bytes);
        const ByteView body = bytes_of(block.bytes);
        switch (block.kind) {
            case detail::BlockKind::Leaf: {
                auto directory = ScopeDirectory::open(body);
                if (!directory) {
                    state = detail::LoadState::Bad;
                    break;
                }
                auto groups = dstd::make_unique<detail::GroupSlots>();
                groups->assign(directory->size());
                block.directory =
                    dstd::make_unique<ScopeDirectory>(std::move(*directory));
                block.groups = std::move(groups);
                break;
            }
            case detail::BlockKind::Middle: {
                auto table = SlotTable::open(body);
                if (!table) {
                    state = detail::LoadState::Bad;
                    break;
                }
                const std::uint32_t below = block.levels - 1;
                for (std::size_t i = 0; i < table->size(); ++i) {
                    BlockHandle handle;
                    std::size_t at = 0;
                    const ByteView payload = table->payload(i);
                    if (!read_block_handle(payload, at, handle) ||
                        at != payload.size()) {
                        state = detail::LoadState::Bad;
                        break;
                    }
                    auto child = dstd::make_unique<detail::LazyBlock>();
                    child->handle = handle;
                    child->levels = below;
                    child->kind = below == 1 ? detail::BlockKind::Leaf
                                             : detail::BlockKind::Middle;
                    block.children.push_back(std::move(child));
                }
                if (state != detail::LoadState::Good) {
                    block.children.clear();
                    break;
                }
                block.separators = *table;
                break;
            }
            case detail::BlockKind::Segments: {
                struct Raw {
                    ExtentRef extent;
                    std::uint64_t sequence = 0;
                    std::uint32_t extent_index = 0;
                    std::uint32_t offset_in_extent = 0;
                    std::uint32_t length = 0;
                    std::uint64_t entry_count = 0;
                    std::uint64_t ordinal = 0;
                    std::size_t first_at = 0;
                    std::size_t first_size = 0;
                    std::size_t last_at = 0;
                    std::size_t last_size = 0;
                };
                if (body.size() < 4) {
                    state = detail::LoadState::Bad;
                    break;
                }
                const std::uint64_t count = read_u32_le(
                    reinterpret_cast<const std::uint8_t*>(body.data()));
                std::size_t at = 4;
                const std::uint8_t* data =
                    reinterpret_cast<const std::uint8_t*>(body.data());
                if (count > (body.size() - 4) / 52) {
                    state = detail::LoadState::Bad;
                    break;
                }
                dstd::Vector<Raw> raws;
                raws.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t i = 0; i < count; ++i) {
                    if (at + 52 > body.size()) {
                        state = detail::LoadState::Bad;
                        break;
                    }
                    Raw raw;
                    raw.extent_index = read_u32_le(data + at);
                    raw.sequence = read_u64_le(data + at + 4);
                    raw.extent.offset = read_u64_le(data + at + 12);
                    raw.extent.length = read_u32_le(data + at + 20);
                    raw.extent.crc = read_u32_le(data + at + 24);
                    raw.offset_in_extent = read_u32_le(data + at + 28);
                    raw.length = read_u32_le(data + at + 32);
                    raw.entry_count = read_u64_le(data + at + 36);
                    raw.ordinal = read_u64_le(data + at + 44);
                    at += 52;
                    for (std::size_t k = 0; k < 2; ++k) {
                        auto length = decode_varint(body, at);
                        if (!length || *length > body.size() - at) {
                            state = detail::LoadState::Bad;
                            break;
                        }
                        if (k == 0) {
                            raw.first_at = at;
                            raw.first_size = static_cast<std::size_t>(*length);
                        } else {
                            raw.last_at = at;
                            raw.last_size = static_cast<std::size_t>(*length);
                        }
                        at += static_cast<std::size_t>(*length);
                    }
                    if (state != detail::LoadState::Good) break;
                    raws.push_back(raw);
                }
                if (state != detail::LoadState::Good || at != body.size()) {
                    state = detail::LoadState::Bad;
                    break;
                }
                for (std::size_t i = 1; i < raws.size(); ++i) {
                    if (raws[i].sequence < raws[i - 1U].sequence) {
                        state = detail::LoadState::Bad;
                        break;
                    }
                }
                if (state != detail::LoadState::Good) break;
                block.segments = dstd::Vector<detail::SegmentSlot>(raws.size());
                for (std::size_t i = 0; i < raws.size(); ++i) {
                    detail::SegmentSlot& slot = block.segments[i];
                    slot.extent = raws[i].extent;
                    slot.sequence = raws[i].sequence;
                    slot.extent_index = raws[i].extent_index;
                    slot.offset_in_extent = raws[i].offset_in_extent;
                    slot.length = raws[i].length;
                    slot.entry_count = raws[i].entry_count;
                    slot.ordinal = raws[i].ordinal;
                    slot.first_key =
                        body.subspan(raws[i].first_at, raws[i].first_size);
                    slot.last_key =
                        body.subspan(raws[i].last_at, raws[i].last_size);
                }
                break;
            }
        }
    }
    block.state.store(static_cast<std::uint8_t>(state),
                      std::memory_order_release);
    return state_of(state);
}

detail::LoadStatus Snapshot::load_extent(detail::Commit& commit,
                                         std::size_t block) const {
    detail::ExtentSlot& slot = commit.extent_slots[block];
    const auto state_of = [&](detail::LoadState known) {
        return detail::LoadStatus{known, known == detail::LoadState::Failed
                                             ? slot.error
                                             : dstd::Error{}};
    };
    const auto known = static_cast<detail::LoadState>(
        slot.state.load(std::memory_order_acquire));
    if (known != detail::LoadState::Unknown) return state_of(known);

    std::lock_guard<std::mutex> locked(slot.mutex);
    const auto again = static_cast<detail::LoadState>(
        slot.state.load(std::memory_order_acquire));
    if (again != detail::LoadState::Unknown) return state_of(again);

    BlockReads reads;
    const detail::FileData& file = files_[commit.file_index];
    const detail::LoadState state =
        read_table(file.reader, commit.blocks[block].ref, slot, reads);
    reads_->add(0, 1, 0, reads);
    if (state == detail::LoadState::Good) {
        warm_window(file, commit.blocks[block].ref.offset);
    }
    slot.state.store(static_cast<std::uint8_t>(state),
                     std::memory_order_release);
    return state_of(state);
}

detail::LoadStatus Snapshot::load_extent(const detail::FileData& file,
                                         std::size_t extent_index,
                                         const ExtentRef& ref) const {
    if (extent_index >= file.extent_slots.size()) {
        return detail::LoadStatus{detail::LoadState::Bad, dstd::Error{}};
    }
    detail::ExtentSlot& slot = file.extent_slots.slot(extent_index);
    const auto state_of = [&](detail::LoadState known) {
        return detail::LoadStatus{known, known == detail::LoadState::Failed
                                             ? slot.error
                                             : dstd::Error{}};
    };
    const auto known = static_cast<detail::LoadState>(
        slot.state.load(std::memory_order_acquire));
    if (known != detail::LoadState::Unknown) return state_of(known);

    std::lock_guard<std::mutex> locked(slot.mutex);
    const auto again = static_cast<detail::LoadState>(
        slot.state.load(std::memory_order_acquire));
    if (again != detail::LoadState::Unknown) return state_of(again);

    BlockReads reads;
    const detail::LoadState state = read_table(file.reader, ref, slot, reads);
    reads_->add(0, 1, 0, reads);
    if (state == detail::LoadState::Good) warm_window(file, ref.offset);
    slot.state.store(static_cast<std::uint8_t>(state),
                     std::memory_order_release);
    return state_of(state);
}

dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error>
Snapshot::page_dict(const detail::FileData& file, const DictRef& ref) const {
    if (ref.empty()) return dstd::SharedPtr<const PageDict>();
    detail::DictCache& cache = *file.dicts;
    std::lock_guard<std::mutex> locked(cache.mutex);
    for (const dstd::SharedPtr<const PageDict>& known : cache.dicts) {
        if (known->ref().offset == ref.offset) {
            if (known->ref().length != ref.length ||
                known->ref().crc != ref.crc) {
                return bad_extent_error();
            }
            return known;
        }
    }
    BlockReads reads;
    dstd::RawBuffer<std::uint8_t> bytes;
    auto read = file.reader.read_range(ref.offset, ref.length, bytes, &reads);
    reads_->add_metadata(reads);
    if (!read) return dstd::Unexpected<dstd::Error>(read.error());
    auto dict = PageDict::for_decode(bytes_of(bytes), ref);
    if (!dict) return bad_extent_error();
    cache.dicts.push_back(*dict);
    return std::move(*dict);
}

void Snapshot::warm_window(const detail::FileData& file,
                           std::uint64_t offset) const {
    const std::uint64_t window = offset / READ_RUN_BYTES;
    if (window >= file.windows.size()) return;
    if (file.windows[window].fetch_add(1, std::memory_order_relaxed) != 1) {
        return;
    }
    // A failed read here costs only the cache: the query reads its pages.
    BlockReads reads;
    file.reader.warm_range(window * READ_RUN_BYTES, READ_RUN_BYTES, &reads)
        .discard();
    // A reader with direct reads warms nothing.
    if (reads.calls == 0) return;
    reads_->add_window(reads);
}

dstd::Expected<ByteView, dstd::Error> Snapshot::read_raw(
    const detail::FileData& file, const detail::ExtentSlot& extent,
    std::uint32_t offset_in_extent, std::uint32_t length,
    detail::PageHold& hold) const {
    hold.page.reset();
    hold.joined.clear();
    if (!extent.pages.has_value()) return bad_extent_error();
    const ExtentPages& pages = *extent.pages;
    if (offset_in_extent > pages.raw_size() ||
        length > pages.raw_size() - offset_in_extent) {
        return bad_extent_error();
    }
    const auto covered = pages.covering(offset_in_extent, length);
    if (covered.first >= covered.second) return bad_extent_error();
    const std::size_t count = covered.second - covered.first;
    const auto file_key = static_cast<std::uint64_t>(&file - files_.data());
    const auto place = [&](std::size_t page) {
        return extent.ref.offset + pages.entry(page).stored_offset;
    };

    dstd::Vector<CachedPage> held(count);
    std::size_t first_missing = covered.second;
    std::size_t last_missing = covered.first;
    for (std::size_t page = covered.first; page < covered.second; ++page) {
        held[page - covered.first] = pages_->find(file_key, place(page));
        if (held[page - covered.first] != nullptr) continue;
        first_missing = std::min(first_missing, page);
        last_missing = page;
    }
    std::uint64_t decoded = 0;
    BlockReads reads;
    if (first_missing < covered.second) {
        auto dict = page_dict(file, pages.dict());
        if (!dict) {
            if (dict.error().is(dstd::Condition::Io)) {
                return dstd::Unexpected<dstd::Error>(dict.error());
            }
            return bad_extent_error();
        }
        const PageEntry& first = pages.entry(first_missing);
        const PageEntry& last = pages.entry(last_missing);
        const std::uint64_t run_begin = first.stored_offset;
        const std::uint64_t run_end =
            static_cast<std::uint64_t>(last.stored_offset) + last.stored_size;
        if (run_end < run_begin) return bad_extent_error();
        dstd::RawBuffer<std::uint8_t> run;
        auto read = file.reader.read_range(
            extent.ref.offset + run_begin,
            static_cast<std::size_t>(run_end - run_begin), run, &reads);
        if (!read) {
            reads_->add_pages(0, 0, reads);
            if (read.error().is(dstd::Condition::Io)) {
                return dstd::Unexpected<dstd::Error>(read.error());
            }
            return bad_extent_error();
        }
        for (std::size_t page = first_missing; page <= last_missing; ++page) {
            CachedPage& slot = held[page - covered.first];
            if (slot != nullptr) continue;
            const PageEntry& entry = pages.entry(page);
            if (entry.stored_offset < run_begin ||
                entry.stored_size > run.size() ||
                entry.stored_offset - run_begin >
                    run.size() - entry.stored_size) {
                reads_->add_pages(decoded, 0, reads);
                return bad_extent_error();
            }
            // Uninitialized: the decode writes every byte.
            auto fresh = dstd::make_shared<dstd::RawBuffer<std::uint8_t>>(
                entry.raw_size);
            auto ok = ExtentPages::decode_page(
                entry,
                ByteView{run.data() + (entry.stored_offset - run_begin),
                         entry.stored_size},
                fresh->data(), dict->get());
            if (!ok) {
                reads_->add_pages(decoded, 0, reads);
                return bad_extent_error();
            }
            ++decoded;
            slot = pages_->insert(file_key, place(page), std::move(fresh));
        }
    }
    reads_->add_pages(decoded, count - decoded, reads);

    const std::uint32_t base = pages.entry(covered.first).raw_offset;
    if (count == 1) {
        hold.page = std::move(held[0]);
        return ByteView{hold.page->data() + (offset_in_extent - base), length};
    }
    const PageEntry& last = pages.entry(covered.second - 1);
    hold.joined.resize(static_cast<std::size_t>(last.raw_offset) +
                       last.raw_size - base);
    for (std::size_t page = covered.first; page < covered.second; ++page) {
        const dstd::RawBuffer<std::uint8_t>& bytes =
            *held[page - covered.first];
        std::memcpy(hold.joined.data() + (pages.entry(page).raw_offset - base),
                    bytes.data(), bytes.size());
    }
    return ByteView{hold.joined.data() + (offset_in_extent - base), length};
}

dstd::Expected<const detail::ExtentSlot*, dstd::Error> Snapshot::loaded_extent(
    const detail::FileData& file, detail::LazyBlock& block,
    std::size_t index) const {
    if (index >= block.segments.size()) return bad_extent_error();
    const detail::SegmentSlot& slot = block.segments[index];
    if (block.owner == nullptr) {
        const detail::LoadStatus status =
            load_extent(file, slot.extent_index, slot.extent);
        if (status.state == detail::LoadState::Failed) {
            return dstd::Unexpected<dstd::Error>(status.error);
        }
        if (status.state != detail::LoadState::Good) return bad_extent_error();
        return &file.extent_slots.slot(slot.extent_index);
    }
    detail::Commit& commit = *block.owner;
    if (commit.synced) {
        const detail::LoadStatus status = load_extent(commit, slot.block);
        if (status.state == detail::LoadState::Failed) {
            return dstd::Unexpected<dstd::Error>(status.error);
        }
        if (status.state != detail::LoadState::Good) return bad_extent_error();
    } else if (static_cast<detail::LoadState>(commit.state.load(
                   std::memory_order_acquire)) != detail::LoadState::Good) {
        return bad_extent_error();
    }
    return &commit.extent_slots[slot.block];
}

dstd::Expected<ByteView, dstd::Error> Snapshot::read_head(
    const detail::FileData& file, const detail::ExtentSlot& extent,
    const detail::SegmentSlot& slot, std::size_t first_page,
    std::size_t end_page, detail::PageHold& hold) const {
    // The writer gives the header and index pages of their own.
    const ExtentPages& pages = *extent.pages;
    for (std::size_t page = first_page; page < end_page; ++page) {
        const PageEntry& entry = pages.entry(page);
        const std::uint32_t upto = std::min<std::uint32_t>(
            entry.raw_offset + entry.raw_size - slot.offset_in_extent,
            slot.length);
        auto bytes = read_raw(file, extent, slot.offset_in_extent, upto, hold);
        if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
        const std::uint32_t head_size = SegmentView::head_size(*bytes);
        if (head_size != 0) return ByteView{bytes->data(), head_size};
    }
    return ByteView{};
}

dstd::Expected<dstd::UniquePtr<detail::SegmentBody>, dstd::Error>
Snapshot::open_segment(const detail::FileData& file, detail::LazyBlock& block,
                       std::size_t index, ByteView begin, ByteView end) const {
    if (index >= block.segments.size()) return bad_extent_error();
    const detail::SegmentSlot& slot = block.segments[index];
    auto loaded = loaded_extent(file, block, index);
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    const detail::ExtentSlot& extent = **loaded;
    if (!extent.pages.has_value()) return bad_extent_error();
    const ExtentPages& pages = *extent.pages;
    if (slot.offset_in_extent > pages.raw_size() ||
        slot.length > pages.raw_size() - slot.offset_in_extent) {
        return bad_extent_error();
    }
    const auto whole = [&]()
        -> dstd::Expected<dstd::UniquePtr<detail::SegmentBody>, dstd::Error> {
        detail::PageHold hold;
        auto bytes =
            read_raw(file, extent, slot.offset_in_extent, slot.length, hold);
        if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
        auto opened = SegmentView::open_index(*bytes);
        if (!opened) return bad_extent_error();
        return dstd::make_unique<detail::SegmentBody>(std::move(hold),
                                                      std::move(*opened));
    };
    // A segment of many pages is decoded only where the range can lie, since
    // every scope of a file can share it.
    const auto covered = pages.covering(slot.offset_in_extent, slot.length);
    if (covered.first >= covered.second) return bad_extent_error();
    if (covered.second - covered.first <= 2) return whole();

    detail::PageHold head_hold;
    auto head =
        read_head(file, extent, slot, covered.first, covered.second, head_hold);
    if (!head) return dstd::Unexpected<dstd::Error>(head.error());
    if (head->empty()) return whole();
    auto opened = SegmentView::open_head(*head, slot.length);
    if (!opened) return bad_extent_error();
    if (opened->granule_count() == 0) return whole();
    const SegmentView::GranuleSpan span = opened->range_span(begin, end);
    detail::PageHold hold;
    auto window = read_raw(file, extent, slot.offset_in_extent + span.offset,
                           span.length, hold);
    if (!window) return dstd::Unexpected<dstd::Error>(window.error());
    auto view = std::move(*opened).with_window(*window, span.offset);
    if (!view) return bad_extent_error();
    return dstd::make_unique<detail::SegmentBody>(
        std::move(hold), std::move(*view), std::move(head_hold));
}

dstd::Expected<dstd::Optional<SegmentView::Entry>, dstd::Error>
Snapshot::find_in_segment(const detail::FileData& file,
                          detail::LazyBlock& block, std::size_t index,
                          ByteView key, detail::PageHold& holder) const {
    if (index >= block.segments.size()) return bad_extent_error();
    const detail::SegmentSlot& slot = block.segments[index];
    auto loaded = loaded_extent(file, block, index);
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    const detail::ExtentSlot& extent = **loaded;
    if (!extent.pages.has_value()) return bad_extent_error();
    const ExtentPages& pages = *extent.pages;
    if (slot.offset_in_extent > pages.raw_size() ||
        slot.length > pages.raw_size() - slot.offset_in_extent) {
        return bad_extent_error();
    }
    const auto whole = [&]()
        -> dstd::Expected<dstd::Optional<SegmentView::Entry>, dstd::Error> {
        auto bytes =
            read_raw(file, extent, slot.offset_in_extent, slot.length, holder);
        if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
        auto view = SegmentView::open_index(*bytes);
        if (!view) return bad_extent_error();
        return view->find(key);
    };
    const auto covered = pages.covering(slot.offset_in_extent, slot.length);
    if (covered.first >= covered.second) return bad_extent_error();
    if (covered.second - covered.first == 1) return whole();

    detail::PageHold head_hold;
    auto head =
        read_head(file, extent, slot, covered.first, covered.second, head_hold);
    if (!head) return dstd::Unexpected<dstd::Error>(head.error());
    if (head->empty()) return whole();
    auto granule = SegmentView::find_granule(*head, slot.length, key);
    if (!granule) return bad_extent_error();
    if (!granule->has_value()) return dstd::Optional<SegmentView::Entry>{};
    const SegmentView::GranuleSpan span = **granule;
    if (span.offset > slot.length || span.length > slot.length - span.offset) {
        return bad_extent_error();
    }
    auto bytes = read_raw(file, extent, slot.offset_in_extent + span.offset,
                          span.length, holder);
    if (!bytes) return dstd::Unexpected<dstd::Error>(bytes.error());
    auto found = SegmentView::find_in_granule(*bytes, key);
    if (!found) return bad_extent_error();
    return *found;
}

dstd::Expected<detail::SegmentRun, dstd::Error> Snapshot::copy_segments(
    const detail::CopyData& copy, const detail::CopyCommit& held) const {
    const auto block_result = [](const detail::LoadStatus& status)
        -> dstd::Expected<void, dstd::Error> {
        if (status.state == detail::LoadState::Good) return {};
        if (status.state == detail::LoadState::Failed) {
            return dstd::Unexpected<dstd::Error>(status.error);
        }
        return bad_extent_error();
    };
    if (held.commit != nullptr) {
        auto indexed = index_result(load_index(*held.commit));
        if (!indexed) return dstd::Unexpected<dstd::Error>(indexed.error());
        detail::LazyBlock* block =
            segment_block_of(*held.commit, copy.local_id);
        if (block == nullptr) return detail::SegmentRun{};
        return detail::SegmentRun{block, 0, block->segments.size()};
    }
    if (held.segments == nullptr) return detail::SegmentRun{};
    const detail::FileData& file = files_[copy.file_index];
    auto segments = block_result(load_block(file, *held.segments));
    if (!segments) return dstd::Unexpected<dstd::Error>(segments.error());
    const dstd::Vector<detail::SegmentSlot>& slots = held.segments->segments;
    const auto begin = std::lower_bound(
        slots.begin(), slots.end(), held.sequence,
        [](const detail::SegmentSlot& slot, std::uint64_t sequence) {
            return slot.sequence < sequence;
        });
    const auto finish = std::upper_bound(
        begin, slots.end(), held.sequence,
        [](std::uint64_t sequence, const detail::SegmentSlot& slot) {
            return sequence < slot.sequence;
        });
    return detail::SegmentRun{held.segments,
                              static_cast<std::size_t>(begin - slots.begin()),
                              static_cast<std::size_t>(finish - slots.begin())};
}

detail::GroupIndex::Shard& detail::GroupIndex::shard_of(
    const dstd::String& key) const {
    return shards_[dstd::Hash<dstd::String>{}(key) % SHARDS];
}

const detail::ScopeGroup* detail::GroupIndex::find(
    const dstd::String& key) const {
    Shard& shard = shard_of(key);
    std::lock_guard<std::mutex> locked(shard.mutex);
    const auto found = shard.groups.find(key);
    return found == shard.groups.end() ? nullptr : found->second;
}

void detail::GroupIndex::add(const dstd::String& key,
                             const ScopeGroup* group) const {
    Shard& shard = shard_of(key);
    std::lock_guard<std::mutex> locked(shard.mutex);
    shard.groups.emplace(key, group);
}

void detail::ExtentSlots::assign(std::size_t count) {
    count_ = count;
    chunks_ = dstd::Vector<std::atomic<Chunk*>>((count + CHUNK_SLOTS - 1) /
                                                CHUNK_SLOTS);
}

detail::ExtentSlot& detail::ExtentSlots::slot(std::size_t index) const {
    std::atomic<Chunk*>& held = chunks_[index / CHUNK_SLOTS];
    Chunk* chunk = held.load(std::memory_order_acquire);
    if (chunk == nullptr) {
        auto fresh = dstd::make_unique<Chunk>();
        Chunk* expected = nullptr;
        if (held.compare_exchange_strong(expected, fresh.get(),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            chunk = fresh.release();
        } else {
            chunk = expected;
        }
    }
    return chunk->slots[index % CHUNK_SLOTS];
}

detail::ExtentSlots::~ExtentSlots() {
    for (std::atomic<Chunk*>& held : chunks_) {
        // Each chunk came from dstd::make_unique, so its own deleter frees it.
        dstd::UniquePtr<Chunk> owned(held.load(std::memory_order_relaxed));
    }
}

dstd::Expected<void, dstd::Error> Snapshot::read_big(
    std::size_t file_index, BigRef ref, ByteView& view,
    dstd::Vector<std::uint8_t>& owned) const {
    owned.clear();
    view = {};
    if (file_index >= files_.size() || ref.length == 0 ||
        ref.length > std::numeric_limits<std::uint32_t>::max()) {
        return parse_error();
    }
    ExtentRef extent{ref.offset, static_cast<std::uint32_t>(ref.length),
                     ref.crc};
    auto bytes = files_[file_index].reader.read_extent(extent);
    if (!bytes) {
        return dstd::Unexpected<dstd::Error>(bytes.error());
    }
    if (bytes->empty()) return parse_error();
    if (bytes->front() == static_cast<std::uint8_t>(PageCodec::None)) {
        owned = std::move(*bytes);
        view = ByteView{reinterpret_cast<const std::byte*>(owned.data() + 1),
                        owned.size() - 1};
        return {};
    }
    auto raw = decode_meta(bytes_of(*bytes));
    if (!raw) return dstd::Unexpected<dstd::Error>(raw.error());
    owned = std::move(*raw);
    view = bytes_of(owned);
    return {};
}

dstd::Expected<void, dstd::Error> Snapshot::materialize(
    FoldKind fold, dstd::Span<const detail::ValueSource> sources,
    ByteView& view, dstd::Vector<std::uint8_t>& owned) const {
    owned.clear();
    view = {};
    if (sources.empty()) return {};

    if (fold == FoldKind::None || sources.size() == 1) {
        const detail::ValueSource& source = sources.back();
        if (!source.entry.is_big()) {
            view = source.entry.value();
            return {};
        }
        return read_big(source.file_index, source.entry.big_ref(), view, owned);
    }

    const FoldFn fold_value = fold_fn(fold);
    if (fold_value == nullptr) {
        const detail::ValueSource& source = sources.back();
        if (!source.entry.is_big()) {
            view = source.entry.value();
            return {};
        }
        return read_big(source.file_index, source.entry.big_ref(), view, owned);
    }

    const detail::ValueSource& first = sources.front();
    if (first.entry.is_big()) {
        auto read =
            read_big(first.file_index, first.entry.big_ref(), view, owned);
        if (!read) return read;
    } else {
        const ByteView bytes = first.entry.value();
        if (!bytes.empty()) {
            const auto* begin =
                reinterpret_cast<const std::uint8_t*>(bytes.data());
            owned.insert(owned.end(), begin, begin + bytes.size());
        }
    }

    dstd::Vector<std::uint8_t> scratch;
    dstd::Vector<std::uint8_t> step;
    ByteView accumulated = bytes_of(owned);
    for (std::size_t i = 1; i < sources.size(); ++i) {
        ByteView value;
        if (sources[i].entry.is_big()) {
            auto read = read_big(sources[i].file_index,
                                 sources[i].entry.big_ref(), value, scratch);
            if (!read) return read;
        } else {
            value = sources[i].entry.value();
        }
        step.clear();
        fold_value(accumulated, value, step);
        owned.assign(step.begin(), step.end());
        accumulated = bytes_of(owned);
    }
    view = accumulated;
    return {};
}

dstd::Expected<dstd::Optional<Value>, dstd::Error> Snapshot::get(
    const NamespaceSpec& spec, const ScopeKey& scope, ByteView key) const {
    auto found = find_group(spec, scope);
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    const detail::ScopeGroup* group = *found;
    if (group == nullptr || group->dropped || group->copies.empty()) {
        return dstd::Optional<Value>{};
    }
    if (!group->shared) {
        return get_in_copy(group->copies[group->current], *group, key);
    }
    for (std::size_t c = group->copies.size(); c-- > group->first_visible;) {
        auto held = get_in_copy(group->copies[c], *group, key);
        if (!held || held->has_value()) return held;
    }
    return dstd::Optional<Value>{};
}

dstd::Expected<dstd::Optional<Value>, dstd::Error> Snapshot::get_in_copy(
    const detail::CopyData& copy, const detail::ScopeGroup& group,
    ByteView key) const {
    if (copy.state != CopyState::Ready || key_hidden(key, group.hidden)) {
        return dstd::Optional<Value>{};
    }
    const auto holds_key = [key](ByteView first, ByteView last) {
        return compare_bytes(key, first) >= 0 && compare_bytes(key, last) <= 0;
    };
    if (std::none_of(copy.commits.begin(), copy.commits.end(),
                     [&](const detail::CopyCommit& held) {
                         return holds_key(held.first_key, held.last_key);
                     })) {
        return dstd::Optional<Value>{};
    }
    const detail::LoadStatus status = ensure_copy(copy);
    if (status.state == detail::LoadState::Failed) {
        return dstd::Unexpected<dstd::Error>(status.error);
    }
    if (status.state != detail::LoadState::Good) {
        return dstd::Optional<Value>{};
    }

    dstd::Vector<detail::ValueSource> sources;
    // The pages each source points into, one buffer per source and a spare: a
    // deque keeps each buffer in place as it grows.
    dstd::Deque<detail::PageHold> holders(1);
    const std::uint64_t limit = erase_limit(key, copy.erases);
    for (const detail::CopyCommit& held : copy.commits) {
        if (!holds_key(held.first_key, held.last_key)) continue;
        auto held_segments = copy_segments(copy, held);
        if (!held_segments) {
            return dstd::Unexpected<dstd::Error>(held_segments.error());
        }
        if (held_segments->block == nullptr) continue;
        detail::LazyBlock& block = *held_segments->block;
        for (std::size_t i = held_segments->begin; i < held_segments->end;
             ++i) {
            const detail::SegmentSlot& slot = block.segments[i];
            if (!holds_key(slot.first_key, slot.last_key)) continue;
            auto found_entry = find_in_segment(files_[copy.file_index], block,
                                               i, key, holders.back());
            if (!found_entry) {
                return dstd::Unexpected<dstd::Error>(found_entry.error());
            }
            if (found_entry->has_value()) {
                if (slot.ordinal < limit) continue;
                sources.push_back(
                    {copy.file_index, slot.ordinal, **found_entry});
                holders.emplace_back();
            }
        }
    }
    if (sources.empty()) return dstd::Optional<Value>{};
    // The batch ordinal, not the record, tells the versions of a key apart:
    // extent writes complete in any order, so a record mixes batches.
    dstd::stable_sort(
        sources.begin(), sources.end(),
        [](const detail::ValueSource& a, const detail::ValueSource& b) {
            return a.ordinal < b.ordinal;
        });

    Value value;
    ByteView view;
    auto filled = materialize(copy.fold, sources, view, value.owned_);
    if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
    if (!view.empty() && reinterpret_cast<const std::uint8_t*>(view.data()) !=
                             value.owned_.data()) {
        // A view into the pages read for this get, which end with it.
        const auto* begin = reinterpret_cast<const std::uint8_t*>(view.data());
        value.owned_.assign(begin, begin + view.size());
    }
    value.view_ = bytes_of(value.owned_);
    return dstd::Optional<Value>(std::move(value));
}

const ScopeKey& ScanCursor::scope() const {
    static const ScopeKey EMPTY_SCOPE{};
    if (at_end_) return EMPTY_SCOPE;
    if (fold_ != FoldKind::None) {
        if (at_key_.empty()) return EMPTY_SCOPE;
        return arms_[at_key_.front()].scope;
    }
    return arms_[arm_].scope;
}

dstd::Expected<ByteView, dstd::Error> ScanCursor::value() {
    sources_.clear();
    if (at_end_) return ByteView{};
    if (fold_ != FoldKind::None) {
        for (const std::size_t arm_index : at_key_) {
            const ScopeCursor& cursor = arms_[arm_index].cursor;
            const std::size_t file_index = cursor.copy()->file_index;
            for (const std::size_t segment_index : cursor.matching()) {
                sources_.push_back({file_index, cursor.ordinals_[segment_index],
                                    cursor.cursors_[segment_index].entry()});
            }
        }
    } else {
        const ScopeCursor& cursor = arms_[arm_].cursor;
        const auto& matching = cursor.matching();
        if (!matching.empty()) {
            const std::size_t newest = matching.back();
            sources_.push_back({cursor.copy()->file_index,
                                cursor.ordinals_[newest],
                                cursor.cursors_[newest].entry()});
        }
    }
    if (sources_.empty()) return ByteView{};
    ByteView view;
    auto filled = snapshot_->materialize(fold_, sources_, view, buffer_);
    if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
    return view;
}

void ScanCursor::next() {
    if (at_end_) return;
    if (fold_ == FoldKind::None) {
        arms_[arm_].cursor.next();
        if (take_error(arms_[arm_].cursor)) return;
    } else {
        for (const std::size_t arm_index : at_key_) {
            arms_[arm_index].cursor.next();
            if (take_error(arms_[arm_index].cursor)) return;
            push_arm(arm_index);
        }
    }
    settle();
}

bool ScanCursor::take_error(const ScopeCursor& cursor) {
    if (!cursor.error_) return false;
    error_ = cursor.error_;
    at_end_ = true;
    key_ = {};
    at_key_.clear();
    return true;
}

struct ScanCursor::ArmAfter {
    const dstd::Vector<Arm>* arms;
    bool operator()(std::size_t a, std::size_t b) const {
        if ((*arms)[a].merge_rank != (*arms)[b].merge_rank) {
            return (*arms)[a].merge_rank > (*arms)[b].merge_rank;
        }
        const int by_key =
            compare_bytes((*arms)[a].cursor.key(), (*arms)[b].cursor.key());
        if (by_key != 0) return by_key > 0;
        return a > b;
    }
};

void ScanCursor::push_arm(std::size_t index) {
    if (arms_[index].cursor.at_end()) return;
    arm_heap_.push_back(index);
    std::push_heap(arm_heap_.begin(), arm_heap_.end(), ArmAfter{&arms_});
}

bool ScanCursor::open_arm(std::size_t index) {
    Arm& arm = arms_[index];
    arm.opened = true;
    const detail::CopyData& copy = arm.group->copies[arm.copy_index];
    const detail::LoadStatus status = snapshot_->ensure_copy(copy);
    if (status.state == detail::LoadState::Failed) {
        error_ = status.error;
        at_end_ = true;
        key_ = {};
        at_key_.clear();
        return false;
    }
    if (status.state != detail::LoadState::Good) return true;
    arm.cursor.open(*snapshot_, copy, arm.group->hidden,
                    bytes_of(begin_storage_), bytes_of(end_storage_));
    return !take_error(arm.cursor);
}

void ScanCursor::settle() {
    at_key_.clear();
    if (error_) return;
    if (fold_ == FoldKind::None) {
        while (arm_ < arms_.size()) {
            if (!arms_[arm_].opened && !open_arm(arm_)) return;
            if (!arms_[arm_].cursor.at_end()) break;
            ++arm_;
        }
        if (arm_ >= arms_.size()) {
            at_end_ = true;
            key_ = {};
            return;
        }
        key_ = arms_[arm_].cursor.key();
        at_end_ = false;
        return;
    }

    if (arm_heap_.empty()) {
        at_end_ = true;
        key_ = {};
        return;
    }
    const ByteView best = arms_[arm_heap_.front()].cursor.key();
    const std::size_t rank = arms_[arm_heap_.front()].merge_rank;
    while (!arm_heap_.empty() && arms_[arm_heap_.front()].merge_rank == rank &&
           compare_bytes(arms_[arm_heap_.front()].cursor.key(), best) == 0) {
        std::pop_heap(arm_heap_.begin(), arm_heap_.end(), ArmAfter{&arms_});
        at_key_.push_back(arm_heap_.back());
        arm_heap_.pop_back();
    }
    key_ = best;
    at_end_ = false;
}

namespace {

/// The heap order: the smaller key first, on a tie the smaller batch ordinal,
/// and only then the lower segment index.
struct HeapAfter {
    const dstd::Vector<ByteView>* keys;
    const dstd::Vector<std::uint64_t>* ordinals;
    bool operator()(std::size_t a, std::size_t b) const {
        const int by_key = compare_bytes((*keys)[a], (*keys)[b]);
        if (by_key != 0) return by_key > 0;
        if ((*ordinals)[a] != (*ordinals)[b]) {
            return (*ordinals)[a] > (*ordinals)[b];
        }
        return a > b;
    }
};

}  // namespace

void ScopeCursor::open(const Snapshot& snapshot, const detail::CopyData& copy,
                       const dstd::Vector<detail::HiddenRange>& hidden,
                       ByteView begin, ByteView end) {
    snapshot_ = &snapshot;
    copy_ = &copy;
    hidden_ = &hidden;
    begin_ = begin;
    end_ = end;
    error_.reset();
    segments_.clear();
    bodies_.clear();
    ordinals_.clear();
    heap_.clear();
    const auto outside = [&](ByteView first, ByteView last) {
        return compare_bytes(last, begin) < 0 ||
               (!end.empty() && compare_bytes(first, end) >= 0);
    };
    for (const detail::CopyCommit& held : copy.commits) {
        if (outside(held.first_key, held.last_key)) continue;
        auto held_segments = snapshot.copy_segments(copy, held);
        if (!held_segments) {
            error_ = held_segments.error();
            at_end_ = true;
            key_ = {};
            matching_.clear();
            return;
        }
        if (held_segments->block == nullptr) continue;
        const dstd::Vector<detail::SegmentSlot>& slots =
            held_segments->block->segments;
        for (std::size_t i = held_segments->begin; i < held_segments->end;
             ++i) {
            if (outside(slots[i].first_key, slots[i].last_key)) continue;
            segments_.push_back({held_segments->block, i});
            ordinals_.push_back(slots[i].ordinal);
        }
    }
    cursors_.assign(segments_.size(), SegmentView::Cursor{});
    bodies_.resize(segments_.size());
    keys_.assign(segments_.size(), ByteView{});
    loaded_.assign(segments_.size(), 0);
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const detail::SegmentSlot& segment =
            segments_[i].block->segments[segments_[i].index];
        keys_[i] = compare_bytes(segment.first_key, begin) < 0
                       ? begin
                       : segment.first_key;
        heap_.push_back(i);
    }
    std::make_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_, &ordinals_});
    settle();
}

bool ScopeCursor::load(std::size_t index) {
    const detail::SegmentLocation& at = segments_[index];
    auto body = snapshot_->open_segment(snapshot_->files_[copy_->file_index],
                                        *at.block, at.index, begin_, end_);
    if (!body) {
        error_ = body.error();
        at_end_ = true;
        key_ = {};
        matching_.clear();
        return false;
    }
    loaded_[index] = 1;
    bodies_[index] = std::move(*body);
    cursors_[index] = bodies_[index]->view.lower_bound(begin_);
    push(index);
    return true;
}

void ScopeCursor::push(std::size_t index) {
    if (cursors_[index].at_end()) {
        bodies_[index].reset();
        return;
    }
    keys_[index] = cursors_[index].entry().key();
    heap_.push_back(index);
    std::push_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_, &ordinals_});
}

void ScopeCursor::advance_matching() {
    for (const std::size_t index : matching_) {
        cursors_[index].next();
        push(index);
    }
}

void ScopeCursor::settle() {
    while (true) {
        matching_.clear();
        if (error_) {
            at_end_ = true;
            key_ = {};
            return;
        }
        if (heap_.empty()) {
            at_end_ = true;
            key_ = {};
            return;
        }
        const std::size_t top = heap_.front();
        if (loaded_[top] == 0) {
            std::pop_heap(heap_.begin(), heap_.end(),
                          HeapAfter{&keys_, &ordinals_});
            heap_.pop_back();
            if (!load(top)) return;
            continue;
        }
        const ByteView best = keys_[top];
        if (!end_.empty() && compare_bytes(best, end_) >= 0) {
            at_end_ = true;
            key_ = {};
            return;
        }
        bool reload = false;
        while (!heap_.empty() &&
               compare_bytes(keys_[heap_.front()], best) == 0) {
            std::pop_heap(heap_.begin(), heap_.end(),
                          HeapAfter{&keys_, &ordinals_});
            const std::size_t index = heap_.back();
            heap_.pop_back();
            if (loaded_[index] == 0) {
                if (!load(index)) return;
                for (const std::size_t taken : matching_) {
                    heap_.push_back(taken);
                    std::push_heap(heap_.begin(), heap_.end(),
                                   HeapAfter{&keys_, &ordinals_});
                }
                reload = true;
                break;
            }
            matching_.push_back(index);
        }
        if (reload) continue;
        const std::uint64_t limit = erase_limit(best, copy_->erases);
        if (limit != 0) {
            std::size_t kept = 0;
            for (const std::size_t index : matching_) {
                if (ordinals_[index] < limit) {
                    cursors_[index].next();
                    push(index);
                    continue;
                }
                matching_[kept] = index;
                ++kept;
            }
            if (kept != matching_.size()) {
                matching_.resize(kept);
                if (matching_.empty()) continue;
            }
        }
        if (hidden_ != nullptr && key_hidden(best, *hidden_)) {
            advance_matching();
            continue;
        }
        key_ = best;
        at_end_ = false;
        return;
    }
}

void ScopeCursor::next() {
    if (at_end_) return;
    advance_matching();
    settle();
}

ScanCursor Snapshot::scan(const NamespaceSpec& spec,
                          dstd::Span<const ScopeKey> scopes, ByteView begin,
                          ByteView end) const {
    ScanCursor cursor;
    cursor.snapshot_ = this;
    const auto keep = [](ByteView bytes, dstd::Vector<std::uint8_t>& out) {
        const auto* start = reinterpret_cast<const std::uint8_t*>(bytes.data());
        out.assign(start, start + bytes.size());
    };
    keep(begin, cursor.begin_storage_);
    keep(end, cursor.end_storage_);

    FoldKind fold = FoldKind::None;
    std::size_t shared_rank = 0;
    for (const ScopeKey& scope : scopes) {
        auto found = find_group(spec, scope);
        if (!found) {
            cursor.error_ = found.error();
            cursor.at_end_ = true;
            return cursor;
        }
        const detail::ScopeGroup* group = *found;
        if (group == nullptr || group->dropped || group->copies.empty()) {
            continue;
        }
        if (group->shared) {
            // Arms merge by key, and Last takes the newest copy's value because
            // the arms of a key are visited in order.
            fold = FoldKind::Last;
            ++shared_rank;
            for (std::size_t c = group->first_visible; c <= group->current;
                 ++c) {
                if (group->copies[c].state != CopyState::Ready) continue;
                ScanCursor::Arm arm;
                arm.scope = scope;
                arm.group = group;
                arm.copy_index = c;
                arm.merge_rank = shared_rank;
                cursor.arms_.push_back(std::move(arm));
            }
            continue;
        }
        const detail::CopyData& copy = group->copies[group->current];
        if (copy.state != CopyState::Ready) continue;
        if (fold == FoldKind::None) fold = copy.fold;
        ScanCursor::Arm arm;
        arm.scope = scope;
        arm.group = group;
        arm.copy_index = group->current;
        cursor.arms_.push_back(std::move(arm));
    }
    cursor.fold_ = fold;
    if (fold != FoldKind::None) {
        for (std::size_t i = 0; i < cursor.arms_.size(); ++i) {
            if (!cursor.open_arm(i)) return cursor;
            cursor.push_arm(i);
        }
    }
    cursor.settle();
    return cursor;
}

ScanCursor Snapshot::scan(const NamespaceSpec& spec, const ScopeKey& scope,
                          ByteView begin, ByteView end) const {
    const ScopeKey* single = &scope;
    return scan(spec, dstd::Span<const ScopeKey>(single, 1), begin, end);
}

ScanCursor Snapshot::scan(const NamespaceSpec& spec, const ScopeKey& scope,
                          ByteView prefix) const {
    const dstd::Vector<std::uint8_t> limit = prefix_end(prefix);
    return scan(spec, scope, prefix, bytes_of(limit));
}

ScanCursor Snapshot::scan_all(const NamespaceSpec& spec, ByteView begin,
                              ByteView end) const {
    auto listed = namespace_scopes(spec);
    if (!listed) {
        ScanCursor cursor;
        cursor.snapshot_ = this;
        cursor.error_ = listed.error();
        cursor.at_end_ = true;
        return cursor;
    }
    return scan(spec, *listed, begin, end);
}

dstd::Expected<dstd::Vector<ScopeEntry>, dstd::Error> Snapshot::scopes(
    const NamespaceSpec& spec) const {
    auto listed = namespace_scopes(spec);
    if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
    dstd::Vector<ScopeEntry> entries;
    entries.reserve(listed->size());
    for (const ScopeKey& scope : *listed) {
        CopyInfo info = copy_info(spec, scope);
        if (info.state == ScopeState::Absent) continue;
        entries.push_back(ScopeEntry{scope, std::move(info)});
    }
    return entries;
}

dstd::Expected<dstd::Vector<NamespaceDecl>, dstd::Error> Snapshot::namespaces()
    const {
    dstd::Vector<NamespaceDecl> declared;
    for (const detail::FileData& file : files_) {
        for (const NamespaceItem& item : file.reader.index().namespaces) {
            bool known = false;
            for (const NamespaceDecl& entry : declared) {
                if (entry.format_version == item.format_version &&
                    entry.name == item.name) {
                    known = true;
                    break;
                }
            }
            if (known) continue;
            NamespaceDecl entry;
            entry.name = item.name;
            entry.format_version = item.format_version;
            entry.shared = item.shared != 0;
            declared.push_back(std::move(entry));
        }
    }
    std::sort(
        declared.begin(), declared.end(),
        [](const NamespaceDecl& a, const NamespaceDecl& b) {
            const int order =
                dstd::StringView(a.name.data(), a.name.size())
                    .compare(dstd::StringView(b.name.data(), b.name.size()));
            if (order != 0) return order < 0;
            return a.format_version < b.format_version;
        });
    return declared;
}

dstd::Expected<dstd::Vector<ScopeKey>, dstd::Error> Snapshot::namespace_scopes(
    const NamespaceSpec& spec) const {
    dstd::Vector<ScopeKey> scopes;
    dstd::Optional<dstd::Error> failure;
    for (const detail::FileData& file : files_) {
        const auto walk = [&](const detail::LazyBlock& node,
                              std::uint32_t level, auto&& self) -> void {
            if (failure) return;
            detail::LazyBlock& block = const_cast<detail::LazyBlock&>(node);
            const detail::LoadStatus status = load_block(file, block);
            if (status.state != detail::LoadState::Good) {
                failure = status.state == detail::LoadState::Failed
                              ? status.error
                              : dstd::Error(dstd::Condition::Parse);
                return;
            }
            if (level > 1) {
                for (const dstd::UniquePtr<detail::LazyBlock>& child :
                     node.children) {
                    self(*child, level - 1, self);
                }
                return;
            }
            const auto [first, last] =
                node.directory->namespace_range(spec.name, spec.format_version);
            for (std::size_t index = first; index < last; ++index) {
                auto key = node.directory->key(index);
                if (!key) {
                    failure = dstd::Error(dstd::Condition::Parse);
                    return;
                }
                scopes.push_back(scope_of(*key));
            }
        };
        if (file.directory_levels == 0) continue;
        for (const dstd::UniquePtr<detail::LazyBlock>& root : file.roots) {
            walk(*root, file.directory_levels, walk);
        }
    }
    if (failure) return dstd::Unexpected<dstd::Error>(*failure);
    std::sort(scopes.begin(), scopes.end(),
              [](const ScopeKey& a, const ScopeKey& b) {
                  return compare_scopes(a, b) < 0;
              });
    scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
    return scopes;
}

}  // namespace dftracer::utils::store::engine
