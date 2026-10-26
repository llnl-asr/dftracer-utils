#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/varint.h>
#include <dftracer/utils/runtime/runtime.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store::engine {
namespace {

using dftracer::utils::ByteView;
using dftracer::utils::Executor;
using dftracer::utils::read_u32_le;
using dftracer::utils::read_u64_le;
using dftracer::utils::Runtime;

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

dstd::String dstring(std::string_view value) {
    return dstd::String(value.data(), value.size());
}

std::string path_string(dstd::StringView dir, std::string_view name) {
    std::string out(dir.data(), dir.size());
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out.append(name.data(), name.size());
    return out;
}

bool has_data_file_suffix(std::string_view name) {
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".dfs") == 0;
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

/// Registered when this code loads, before main, so the child of any later fork
/// knows that the pools it inherits are empty.
[[maybe_unused]] const bool FORK_HANDLER_REGISTERED =
    ::pthread_atfork(nullptr, nullptr, mark_forked_child) == 0;

/// The process-wide default runtime, when a fork-join on it can finish here:
/// not on a worker of the runtime itself, not inside one of its parallel
/// regions and not in the child of a fork, whose copy of an inherited runtime
/// has no workers. A runtime created here has live workers, so it makes the
/// pools this process's own again.
Runtime* fork_join_runtime() {
    const std::shared_ptr<Runtime> known = peek_default_runtime();
    Runtime& runtime = default_runtime();
    if (known == nullptr) {
        INHERITED_POOLS.store(false, std::memory_order_relaxed);
    }
    if (INHERITED_POOLS.load(std::memory_order_relaxed) ||
        Executor::current() == runtime.executor() ||
        dftracer::utils::detail::in_parallel_region()) {
        return nullptr;
    }
    return &runtime;
}

/// Runs `body(i)` for every i below `count`, in parallel on `runtime` when it
/// is set and serially on this thread when it is not.
template <class Body>
void for_each_index(Runtime* runtime, std::size_t count, std::int64_t grain,
                    const Body& body) {
    if (runtime == nullptr) {
        for (std::size_t i = 0; i < count; ++i) body(i);
        return;
    }
    runtime->parallel_for(static_cast<std::int64_t>(count), grain,
                          [&](std::int64_t begin, std::int64_t limit) {
                              for (std::int64_t i = begin; i < limit; ++i) {
                                  body(static_cast<std::size_t>(i));
                              }
                          });
}

/// The error of a page of a synced commit whose crc or sections fail.
dstd::Unexpected<dstd::Error> bad_page_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

/// Opens the view of one section over the bytes of its page and publishes it
/// in the slot, unless another thread did first.
dstd::Expected<const SectionView*, dstd::Error> publish_view(
    detail::SectionSlot& slot, const dstd::Vector<std::uint8_t>& bytes) {
    if (SectionView* known = slot.view.load(std::memory_order_acquire)) {
        return known;
    }
    if (slot.offset_in_page > bytes.size() ||
        slot.length > bytes.size() - slot.offset_in_page) {
        return bad_page_error();
    }
    auto opened = SectionView::open_index(
        ByteView{bytes.data() + slot.offset_in_page, slot.length});
    if (!opened) return bad_page_error();
    SectionView* fresh = new SectionView(std::move(*opened));
    SectionView* expected = nullptr;
    if (slot.view.compare_exchange_strong(expected, fresh,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return fresh;
    }
    delete fresh;
    return expected;
}

/// One in-memory section block per copy of a commit the chain lists: the
/// sections of that copy, with the block of the page that holds each. Good
/// from the start, since the record is in memory.
void fill_section_blocks(detail::Commit& commit) {
    struct Listed {
        std::uint32_t copy_id;
        std::uint32_t block;
        const PageSection* section;
    };
    dstd::Vector<Listed> listed;
    for (std::size_t block = 0; block < commit.blocks.size(); ++block) {
        for (const PageSection& section :
             commit.blocks[block].listed->sections) {
            listed.push_back(
                {section.copy_id, static_cast<std::uint32_t>(block), &section});
        }
    }
    std::sort(listed.begin(), listed.end(),
              [](const Listed& a, const Listed& b) {
                  if (a.copy_id != b.copy_id) return a.copy_id < b.copy_id;
                  if (a.block != b.block) return a.block < b.block;
                  return a.section->offset_in_page < b.section->offset_in_page;
              });
    std::size_t at = 0;
    while (at < listed.size()) {
        std::size_t end = at + 1;
        while (end < listed.size() &&
               listed[end].copy_id == listed[at].copy_id) {
            ++end;
        }
        auto block = dstd::make_unique<detail::LazyBlock>();
        block->kind = detail::BlockKind::Sections;
        block->state.store(static_cast<std::uint8_t>(detail::LoadState::Good),
                           std::memory_order_relaxed);
        block->owner = &commit;
        block->sections = dstd::Vector<detail::SectionSlot>(end - at);
        for (std::size_t i = at; i < end; ++i) {
            detail::SectionSlot& slot = block->sections[i - at];
            slot.block = listed[i].block;
            slot.offset_in_page = listed[i].section->offset_in_page;
            slot.length = listed[i].section->length;
            slot.entry_count = listed[i].section->entry_count;
            slot.first_key = bytes_of(listed[i].section->first_key);
            slot.last_key = bytes_of(listed[i].section->last_key);
        }
        commit.section_copies.push_back(listed[at].copy_id);
        commit.section_blocks.push_back(std::move(block));
        at = end;
    }
}

/// The section block of one copy of one commit, or null when the commit holds
/// no section of it.
detail::LazyBlock* section_block_of(detail::Commit& commit,
                                    std::uint32_t copy_id) {
    const auto at = std::lower_bound(commit.section_copies.begin(),
                                     commit.section_copies.end(), copy_id);
    if (at == commit.section_copies.end() || *at != copy_id) return nullptr;
    return commit
        .section_blocks[static_cast<std::size_t>(at -
                                                 commit.section_copies.begin())]
        .get();
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

/// How many files one parallel item of the commit phase covers: grouping the
/// refs of a file is cheap, but a root of many small files still spreads.
constexpr std::int64_t COMMIT_GRAIN = 4;

/// Builds the blocks, pages and section blocks of one commit from its page
/// refs, deduplicated by offset and in offset order, so the pages the commit
/// holds read together. A record may list the same page ref again, and one ref
/// belongs to one commit: the first listing of an offset is the one kept.
void fill_commit(detail::Commit& commit, std::vector<const PageRefItem*> refs) {
    std::stable_sort(refs.begin(), refs.end(),
                     [](const PageRefItem* a, const PageRefItem* b) {
                         return a->offset < b->offset;
                     });
    dstd::Vector<detail::CommitBlock> blocks;
    for (const PageRefItem* ref : refs) {
        if (blocks.empty() || blocks.back().ref.offset != ref->offset) {
            blocks.push_back(
                {PageRef{ref->offset, ref->length, ref->crc}, ref});
        }
    }
    commit.pages = dstd::Vector<detail::PageSlot>(blocks.size());
    commit.blocks = std::move(blocks);
    fill_section_blocks(commit);
}

/// One commit per sequence of a file: those the chain lists with their page
/// refs, built now, and those a checkpoint or the footer lists by summary,
/// whose page refs are read with their first query.
void build_commits(detail::FileData& file, std::size_t file_index) {
    const FileIndex& index = file.reader.index();
    // A closed file holds no commit object: every commit of it is synced, and
    // its directory names the sections of each copy.
    if (index.tail.has_value()) return;
    const dstd::Vector<IndexedPageRef>& page_refs = index.page_refs;
    std::vector<std::size_t> order(page_refs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&page_refs](std::size_t a, std::size_t b) {
                         return page_refs[a].sequence < page_refs[b].sequence;
                     });
    for (std::size_t at = 0; at < order.size();) {
        const std::uint64_t sequence = page_refs[order[at]].sequence;
        auto commit = dstd::make_unique<detail::Commit>();
        commit->sequence = sequence;
        commit->file_index = file_index;
        std::vector<const PageRefItem*> refs;
        bool synced = true;
        while (at < order.size() && page_refs[order[at]].sequence == sequence) {
            const PageRefItem& ref = page_refs[order[at]].ref;
            if (ref.synced == 0) synced = false;
            refs.push_back(&ref);
            ++at;
        }
        // A clean close syncs every page before its footer.
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

/// The scope directory of a file without a footer, built from the items and
/// commits its chain gave: each commit's copies with their key ranges, from
/// the summary of one a checkpoint lists and from the sections of one the
/// chain lists itself.
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
        for (std::size_t at = 0; at < commit->section_blocks.size(); ++at) {
            const std::uint32_t copy_id = commit->section_copies[at];
            for (const detail::SectionSlot& slot :
                 commit->section_blocks[at]->sections) {
                commits.push_back({commit->sequence, copy_id, slot.first_key,
                                   slot.last_key, BlockHandle{}});
            }
        }
    }
    return build_scope_directory(index.namespaces, index.scopes, index.copies,
                                 index.drops, commits);
}

/// The directory blocks of one file: the top table of a closed file and the
/// blocks it names, while its entries stay in its footer; or one in-memory
/// leaf over the chain of a file without a footer, Good at once.
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
    file.pages.assign(tail.page_count);
    return {};
}

/// The commit of a file with this sequence, or null.
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

bool operator==(const NamespaceSpec& a, const NamespaceSpec& b) {
    return a.name == b.name && a.format_version == b.format_version &&
           a.fold == b.fold && a.durable == b.durable;
}

bool operator!=(const NamespaceSpec& a, const NamespaceSpec& b) {
    return !(a == b);
}

ScopeKey ScopeKey::root() { return ScopeKey{}; }

ScopeKey ScopeKey::file(dstd::StringView path) {
    ScopeKey key;
    key.kind = ScopeKind::File;
    key.paths.push_back(dstring(path));
    return key;
}

ScopeKey ScopeKey::files(dstd::Span<const dstd::StringView> paths) {
    ScopeKey key;
    key.kind = ScopeKind::Files;
    key.paths.reserve(paths.size());
    for (const dstd::StringView path : paths)
        key.paths.push_back(dstring(path));
    std::sort(key.paths.begin(), key.paths.end());
    key.paths.erase(std::unique(key.paths.begin(), key.paths.end()),
                    key.paths.end());
    return key;
}

int compare_scopes(const ScopeKey& a, const ScopeKey& b) {
    if (a.kind != b.kind) {
        return static_cast<std::uint8_t>(a.kind) <
                       static_cast<std::uint8_t>(b.kind)
                   ? -1
                   : 1;
    }
    const std::size_t common =
        a.paths.size() < b.paths.size() ? a.paths.size() : b.paths.size();
    for (std::size_t i = 0; i < common; ++i) {
        const int cmp = a.paths[i].compare(b.paths[i]);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
    }
    if (a.paths.size() < b.paths.size()) return -1;
    if (a.paths.size() > b.paths.size()) return 1;
    return 0;
}

bool operator==(const ScopeKey& a, const ScopeKey& b) {
    return compare_scopes(a, b) == 0;
}

bool operator!=(const ScopeKey& a, const ScopeKey& b) { return !(a == b); }

bool operator<(const ScopeKey& a, const ScopeKey& b) {
    return compare_scopes(a, b) < 0;
}

dstd::Expected<Snapshot, dstd::Error> Snapshot::open(dstd::StringView dir) {
    Snapshot snapshot;
    auto loaded = snapshot.load(dir);
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    return snapshot;
}

dstd::Expected<void, dstd::Error> Snapshot::load(dstd::StringView dir) {
    const auto start = std::chrono::steady_clock::now();
    const std::string dir_string(dir.data(), dir.size());
    std::vector<std::string> names;
    std::error_code error;
    std::filesystem::directory_iterator iterator(dir_string, error);
    if (error) return io_error();
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::string name = iterator->path().filename().string();
        if (has_data_file_suffix(name)) names.push_back(name);
        iterator.increment(error);
        if (error) return io_error();
    }
    std::sort(names.begin(), names.end());
    const auto listed = std::chrono::steady_clock::now();

    // The phases run on the process-wide default runtime, one item per file and
    // then one per file again for its commits, or serially on this thread for
    // a few files, where starting the pool costs more than it saves, and when
    // a fork-join there could not finish.
    Runtime* const runtime =
        names.size() >= PARALLEL_OPEN_FILES ? fork_join_runtime() : nullptr;

    std::vector<dstd::Optional<detail::FileData>> opened(names.size());
    std::vector<dstd::Optional<dstd::Error>> open_errors(names.size());
    const auto open_one = [&](std::size_t index) {
        auto reader = DataFileReader::open(path_string(dir, names[index]),
                                           FooterItems::Directory);
        if (!reader) {
            open_errors[index] = reader.error();
            return;
        }
        detail::FileData& file = opened[index].emplace(std::move(*reader));
        file.name = dstring(names[index]);
        file.writer_random = file.reader.index().writer_random;
    };
    for_each_index(runtime, names.size(), 1, open_one);
    const auto open_done = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < names.size(); ++index) {
        if (open_errors[index]) {
            skipped_.push_back({dstring(names[index]), *open_errors[index]});
            continue;
        }
        files_.push_back(std::move(*opened[index]));
    }

    // One commit per sequence of every file that is not closed, each behind a
    // pointer its sections and copies keep. No page and no big value is read
    // here: the blocks of a commit are read the first time a query needs one.
    const auto group_commits = [&](std::size_t file_index) {
        build_commits(files_[file_index], file_index);
    };
    for_each_index(runtime, files_.size(), COMMIT_GRAIN, group_commits);

    // The scope directory of every file: the blocks a closed file's footer
    // names, read on demand, or one leaf built in memory over the chain of a
    // file without a footer. A directory that does not open skips its file.
    dstd::Vector<detail::FileData> kept;
    kept.reserve(files_.size());
    for (detail::FileData& file : files_) {
        auto directory = build_file_directory(file);
        if (!directory) {
            skipped_.push_back({file.name, directory.error()});
            continue;
        }
        kept.push_back(std::move(file));
    }
    files_ = std::move(kept);
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
    for (const detail::FileData& file : files_) {
        open_stats_.commits += file.commits.size();
        open_stats_.pages += file.reader.index().page_refs.size();
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

dstd::Expected<CheckReport, dstd::Error> Snapshot::check() const {
    CheckReport report;
    report.files = files_.size();
    const std::size_t file_count = files_.size();
    std::vector<dstd::Optional<dstd::Error>> failures(file_count);
    std::vector<std::uint64_t> commits(file_count, 0);
    std::vector<std::uint64_t> blocks(file_count, 0);
    std::vector<std::uint64_t> metadata(file_count, 0);
    std::vector<std::uint64_t> sections(file_count, 0);
    std::vector<dstd::Vector<BadCommit>> bad_commits(file_count);
    std::vector<dstd::Vector<BadSection>> bad_sections(file_count);

    const auto check_file = [&](std::size_t index) {
        const detail::FileData& file = files_[index];
        const auto fail = [&](const dstd::Error& error) {
            failures[index] = error;
        };
        const auto bad_commit = [&](std::uint64_t sequence) {
            bad_commits[index].push_back({file.name, sequence});
        };
        const auto block_error = [](const detail::LoadStatus& status) {
            return status.state == detail::LoadState::Failed
                       ? status.error
                       : dstd::Error(dstd::Condition::Parse);
        };
        // A big value run a closed file holds, with a commit whose section
        // names it.
        struct BigRun {
            BigRef ref;
            std::uint64_t sequence = 0;
        };
        // Every entry of the section decodes, not only its index. The big
        // value runs it names are added to `bigs` when given.
        const auto walkable = [](const dstd::Vector<std::uint8_t>& bytes,
                                 const detail::SectionSlot& slot,
                                 std::uint64_t sequence,
                                 dstd::Vector<BigRun>* bigs) {
            if (slot.offset_in_page > bytes.size() ||
                slot.length > bytes.size() - slot.offset_in_page) {
                return false;
            }
            auto view = SectionView::open(
                ByteView{bytes.data() + slot.offset_in_page, slot.length});
            if (!view) return false;
            if (bigs != nullptr) {
                for (SectionView::Cursor at = view->lower_bound(ByteView{});
                     !at.at_end(); at.next()) {
                    if (at.entry().is_big()) {
                        bigs->push_back({at.entry().big_ref(), sequence});
                    }
                }
            }
            return true;
        };

        if (!file.reader.index().tail.has_value()) {
            for (const dstd::UniquePtr<detail::Commit>& owned : file.commits) {
                detail::Commit& commit = *owned;
                ++commits[index];
                const detail::LoadStatus status = load_index(commit);
                if (status.state != detail::LoadState::Good) {
                    if (status.state == detail::LoadState::Failed) {
                        fail(status.error);
                        return;
                    }
                    bad_commit(commit.sequence);
                    continue;
                }
                blocks[index] += commit.blocks.size();
                if (!commit.synced) {
                    const detail::LoadStatus checked = verify_commit(commit);
                    if (checked.state == detail::LoadState::Failed) {
                        fail(checked.error);
                        return;
                    }
                    if (checked.state != detail::LoadState::Good) {
                        // Its bytes are not there to walk.
                        bad_commit(commit.sequence);
                        continue;
                    }
                } else {
                    for (std::size_t block = 0; block < commit.blocks.size();
                         ++block) {
                        const detail::CommitBlock& held = commit.blocks[block];
                        if (held.listed != nullptr &&
                            !held.listed->sections.empty()) {
                            const detail::LoadStatus page =
                                load_page(commit, block);
                            if (page.state == detail::LoadState::Failed) {
                                fail(page.error);
                                return;
                            }
                            if (page.state != detail::LoadState::Good) {
                                bad_commit(commit.sequence);
                            }
                            continue;
                        }
                        BlockReads reads;
                        auto verified =
                            file.reader.verify_block(held.ref, &reads);
                        reads_->add(0, 1, reads);
                        if (verified) continue;
                        if (verified.error().is(dstd::Condition::Parse)) {
                            bad_commit(commit.sequence);
                        } else {
                            fail(verified.error());
                            return;
                        }
                    }
                }
                for (const dstd::UniquePtr<detail::LazyBlock>& block :
                     commit.section_blocks) {
                    for (const detail::SectionSlot& slot : block->sections) {
                        // A page that does not verify is reported as its
                        // commit, since its bytes are not there to walk.
                        if (commit.synced &&
                            load_page(commit, slot.block).state !=
                                detail::LoadState::Good) {
                            continue;
                        }
                        ++sections[index];
                        const dstd::Vector<std::uint8_t>& bytes =
                            commit.pages[slot.block].bytes;
                        if (walkable(bytes, slot, commit.sequence, nullptr)) {
                            continue;
                        }
                        bad_sections[index].push_back(
                            {file.name, commit.blocks[slot.block].ref.offset,
                             slot.offset_in_page});
                    }
                }
            }
            return;
        }

        // A closed file: every directory block, every section block and every
        // page, each page counted once.
        struct Pending {
            std::uint64_t sequence = 0;
            std::uint32_t copy_id = 0;
            BlockHandle sections;
        };
        dstd::Vector<Pending> pending;
        bool stopped = false;
        const auto walk = [&](const detail::LazyBlock& node,
                              std::uint32_t level, auto&& self) -> void {
            if (stopped) return;
            detail::LazyBlock& block = const_cast<detail::LazyBlock&>(node);
            ++metadata[index];
            const detail::LoadStatus status = load_block(file, block);
            if (status.state != detail::LoadState::Good) {
                fail(block_error(status));
                stopped = true;
                return;
            }
            if (level > 1) {
                for (const dstd::UniquePtr<detail::LazyBlock>& child :
                     node.children) {
                    self(*child, level - 1, self);
                }
                return;
            }
            for (std::size_t i = 0; i < node.directory->size(); ++i) {
                auto entry = node.directory->entry(i);
                if (!entry) {
                    fail(dstd::Error(dstd::Condition::Parse));
                    stopped = true;
                    return;
                }
                for (const DirectoryCopy& copy : entry->copies) {
                    for (const DirectoryCommit& range : copy.commits) {
                        pending.push_back(
                            {range.sequence, copy.local_id, range.sections});
                    }
                }
            }
        };
        for (const dstd::UniquePtr<detail::LazyBlock>& root : file.roots) {
            walk(*root, file.directory_levels, walk);
        }
        if (stopped) return;

        std::vector<std::uint64_t> sequences;
        sequences.reserve(pending.size());
        for (const Pending& entry : pending)
            sequences.push_back(entry.sequence);
        std::sort(sequences.begin(), sequences.end());
        sequences.erase(std::unique(sequences.begin(), sequences.end()),
                        sequences.end());
        commits[index] = sequences.size();
        std::vector<std::uint8_t> seen(file.pages.size(), 0);
        dstd::Vector<BigRun> bigs;

        std::sort(pending.begin(), pending.end(),
                  [](const Pending& a, const Pending& b) {
                      if (a.sections.offset != b.sections.offset) {
                          return a.sections.offset < b.sections.offset;
                      }
                      return a.sequence < b.sequence;
                  });
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].sections.empty()) continue;
            if (i > 0 &&
                pending[i - 1U].sections.offset == pending[i].sections.offset) {
                continue;
            }
            detail::LazyBlock block;
            block.kind = detail::BlockKind::Sections;
            block.handle = pending[i].sections;
            ++metadata[index];
            const detail::LoadStatus status = load_block(file, block);
            if (status.state != detail::LoadState::Good) {
                if (status.state == detail::LoadState::Failed) {
                    fail(status.error);
                    return;
                }
                for (std::size_t at = i; at < pending.size(); ++at) {
                    if (pending[at].sections.offset ==
                        pending[i].sections.offset) {
                        bad_commit(pending[at].sequence);
                    }
                }
                continue;
            }
            for (const detail::SectionSlot& slot : block.sections) {
                if (slot.page_index < seen.size() &&
                    seen[slot.page_index] == 0) {
                    seen[slot.page_index] = 1;
                    ++blocks[index];
                }
                const detail::LoadStatus page =
                    load_page(file, slot.page_index, slot.page);
                if (page.state == detail::LoadState::Failed) {
                    fail(page.error);
                    return;
                }
                if (page.state != detail::LoadState::Good) {
                    // Its bytes are not there to walk.
                    bad_commit(pending[i].sequence);
                    continue;
                }
                ++sections[index];
                if (walkable(file.pages.slot(slot.page_index).bytes, slot,
                             pending[i].sequence, &bigs)) {
                    continue;
                }
                bad_sections[index].push_back(
                    {file.name, slot.page.offset, slot.offset_in_page});
            }
        }

        // The big value runs, each read once: no block of the directory names
        // them, only the entries that hold them.
        std::sort(bigs.begin(), bigs.end(),
                  [](const BigRun& a, const BigRun& b) {
                      if (a.ref.offset != b.ref.offset) {
                          return a.ref.offset < b.ref.offset;
                      }
                      return a.sequence < b.sequence;
                  });
        for (std::size_t i = 0; i < bigs.size(); ++i) {
            if (i > 0 && bigs[i - 1U].ref.offset == bigs[i].ref.offset) {
                continue;
            }
            ++blocks[index];
            const BigRef& big = bigs[i].ref;
            bool bad = big.length == 0 ||
                       big.length > std::numeric_limits<std::uint32_t>::max();
            if (!bad) {
                BlockReads reads;
                auto verified = file.reader.verify_block(
                    PageRef{big.offset, static_cast<std::uint32_t>(big.length),
                            big.crc},
                    &reads);
                reads_->add(0, 1, reads);
                if (!verified) {
                    if (!verified.error().is(dstd::Condition::Parse)) {
                        fail(verified.error());
                        return;
                    }
                    bad = true;
                }
            }
            if (!bad) continue;
            for (std::size_t at = i;
                 at < bigs.size() && bigs[at].ref.offset == big.offset; ++at) {
                bad_commit(bigs[at].sequence);
            }
        }
    };
    for_each_index(fork_join_runtime(), file_count, 1, check_file);

    for (std::size_t index = 0; index < file_count; ++index) {
        if (failures[index]) {
            return dstd::Unexpected<dstd::Error>(*failures[index]);
        }
        report.commits += commits[index];
        report.blocks += blocks[index];
        report.metadata_blocks += metadata[index];
        report.sections += sections[index];
        // A commit is reported once, however many of its blocks fail.
        std::sort(bad_commits[index].begin(), bad_commits[index].end(),
                  [](const BadCommit& a, const BadCommit& b) {
                      return a.sequence < b.sequence;
                  });
        for (std::size_t at = 0; at < bad_commits[index].size(); ++at) {
            const BadCommit& bad = bad_commits[index][at];
            if (at > 0 &&
                bad_commits[index][at - 1U].sequence == bad.sequence) {
                continue;
            }
            report.bad_commits.push_back(bad);
        }
        for (const BadSection& bad : bad_sections[index]) {
            report.bad_sections.push_back(bad);
        }
    }
    const ReadStats reads = read_stats();
    report.bytes_read = reads.bytes_read;
    return report;
}

dstd::Expected<dstd::Vector<ScopeGeneration>, dstd::Error>
Snapshot::generations() const {
    struct Item {
        DirectoryKey key;
        std::uint64_t generation = 0;
    };
    dstd::Vector<Item> items;
    dstd::Optional<dstd::Error> failure;
    for (const detail::FileData& file : files_) {
        // A block that does not load fails the whole call: a writer takes its
        // generations from here, and a scope left out would start again low.
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
            for (std::size_t index = 0; index < node.directory->size();
                 ++index) {
                auto key = node.directory->key(index);
                auto highest = node.directory->highest_generation(index);
                if (!key || !highest) {
                    failure = dstd::Error(dstd::Condition::Parse);
                    return;
                }
                items.push_back({std::move(*key), *highest});
            }
        };
        if (file.directory_levels == 0) continue;
        for (const dstd::UniquePtr<detail::LazyBlock>& root : file.roots) {
            walk(*root, file.directory_levels, walk);
        }
    }
    if (failure) return dstd::Unexpected<dstd::Error>(*failure);
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

detail::GroupSlots::~GroupSlots() {
    // Each group came from dstd::make_unique, so its own deleter frees it.
    for (std::atomic<ScopeGroup*>& slot : slots_) {
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
    // The leaf block and the entry of one scope of one file, reading the
    // blocks on the way there. A null leaf is a scope no entry holds.
    const auto find_leaf = [&](const detail::FileData& file, std::size_t& entry)
        -> dstd::Expected<const detail::LazyBlock*, dstd::Error> {
        if (file.directory_levels == 0) {
            return static_cast<const detail::LazyBlock*>(nullptr);
        }
        // The last separator at or before the scope names the child that
        // can hold it; none means the scope sorts before every entry.
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
        return known;
    }

    // Every file holding the scope adds its copies and drops.
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
        for (const DirectoryCopy& listed : entry->copies) {
            detail::CopyData copy;
            copy.file_index = file;
            copy.generation = listed.generation;
            copy.params_hash = listed.params_hash;
            copy.fold = static_cast<FoldKind>(listed.fold_kind);
            copy.state = listed.state;
            copy.local_id = listed.local_id;
            for (const DirectoryCommit& range : listed.commits) {
                detail::Commit* commit =
                    closed_file ? nullptr : commit_of(held, range.sequence);
                if (!closed_file && commit == nullptr) continue;
                // The section block of a commit of a file that is not closed
                // is found through copy_sections, once its index is loaded.
                detail::LazyBlock* sections = nullptr;
                if (closed_file && !range.sections.empty()) {
                    auto block = dstd::make_unique<detail::LazyBlock>();
                    block->kind = detail::BlockKind::Sections;
                    block->handle = range.sections;
                    sections = block.get();
                    group->sections.push_back(std::move(block));
                }
                copy.commits.push_back({range.sequence, range.first_key,
                                        range.last_key, sections, commit});
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
    return owner_leaf->groups->publish(owner_entry, std::move(group));
}

ScopeState Snapshot::state(const NamespaceSpec& spec,
                           const ScopeKey& scope) const {
    auto found = find_group(spec, scope);
    // The state has no channel for the error of a block that could not be
    // read, so it reads Bad; `get` and a cursor report the error itself.
    if (!found) return ScopeState::Bad;
    const detail::ScopeGroup* group = *found;
    if (group == nullptr || group->dropped || group->copies.empty()) {
        return ScopeState::Absent;
    }
    const detail::CopyData& copy = group->copies[group->current];
    const detail::LoadStatus status = ensure_copy(copy);
    // The state has no channel for the error of a commit whose block could not
    // be read, so it reads Bad; `get` and a cursor report the error itself.
    if (status.state == detail::LoadState::Bad ||
        status.state == detail::LoadState::Failed) {
        return ScopeState::Bad;
    }
    if (copy.state == CopyState::Failed) return ScopeState::Failed;
    return ScopeState::Ready;
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
    std::vector<std::size_t> page_blocks;
    std::vector<std::size_t> big_blocks;
    for (std::size_t block = 0; block < commit.blocks.size(); ++block) {
        const detail::CommitBlock& held = commit.blocks[block];
        if (held.listed != nullptr && !held.listed->sections.empty()) {
            page_blocks.push_back(block);
        } else {
            big_blocks.push_back(block);
        }
    }

    // Every page in one read, on this thread: the commit's lock is held, and a
    // fork-join here could wait on a pool worker that waits on this lock.
    dstd::Vector<PageRef> refs;
    dstd::Vector<PageTarget> targets;
    std::vector<std::uint8_t> bad(commit.blocks.size(), 0);
    for (const std::size_t block : page_blocks) {
        refs.push_back(commit.blocks[block].ref);
        targets.push_back({&commit.pages[block].bytes, &bad[block]});
    }
    BlockReads reads;
    bool any_bad = false;
    dstd::Optional<dstd::Error> failure;
    if (!refs.empty()) {
        auto read = reader.read_pages(refs, targets);
        if (!read) {
            failure = read.error();
        } else {
            reads = *read;
            for (const std::size_t block : page_blocks) {
                if (bad[block] != 0) any_bad = true;
            }
        }
    }
    // Every section of the commit must open, so a section that cannot be read
    // makes the whole commit Bad, as the views of its pages once did.
    if (!failure && !any_bad) {
        for (const dstd::UniquePtr<detail::LazyBlock>& block :
             commit.section_blocks) {
            for (detail::SectionSlot& slot : block->sections) {
                auto view = publish_view(slot, commit.pages[slot.block].bytes);
                if (!view) {
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

    // A bad block decides over a failed read: the commit is torn whatever
    // else could not be read.
    detail::LoadState state = detail::LoadState::Good;
    if (any_bad) {
        state = detail::LoadState::Bad;
    } else if (failure) {
        state = detail::LoadState::Failed;
        commit.error = *failure;
    }
    reads_->add(state == detail::LoadState::Failed ? 0 : 1,
                commit.blocks.size(), reads);
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
        std::vector<const PageRefItem*> listed;
        listed.reserve(commit.listed_refs.size());
        for (const PageRefItem& ref : commit.listed_refs)
            listed.push_back(&ref);
        fill_commit(commit, std::move(listed));
    }
    commit.index_state.store(static_cast<std::uint8_t>(state),
                             std::memory_order_release);
    return state_of(state);
}

namespace {

/// The status of a commit index as a query reports it: the read error of a
/// Failed one, and a Parse error for a record that does not match.
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
            case detail::BlockKind::Sections: {
                struct Raw {
                    PageRef page;
                    std::uint32_t page_index = 0;
                    std::uint32_t offset_in_page = 0;
                    std::uint32_t length = 0;
                    std::uint64_t entry_count = 0;
                    std::size_t first_at = 0;
                    std::size_t first_length = 0;
                    std::size_t last_at = 0;
                    std::size_t last_length = 0;
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
                if (count > (body.size() - 4) / 36) {
                    state = detail::LoadState::Bad;
                    break;
                }
                dstd::Vector<Raw> raws;
                raws.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t i = 0; i < count; ++i) {
                    if (at + 36 > body.size()) {
                        state = detail::LoadState::Bad;
                        break;
                    }
                    Raw raw;
                    raw.page_index = read_u32_le(data + at);
                    raw.page.offset = read_u64_le(data + at + 4);
                    raw.page.length = read_u32_le(data + at + 12);
                    raw.page.crc = read_u32_le(data + at + 16);
                    raw.offset_in_page = read_u32_le(data + at + 20);
                    raw.length = read_u32_le(data + at + 24);
                    raw.entry_count = read_u64_le(data + at + 28);
                    at += 36;
                    for (std::size_t k = 0; k < 2; ++k) {
                        auto length = decode_varint(body, at);
                        if (!length || *length > body.size() - at) {
                            state = detail::LoadState::Bad;
                            break;
                        }
                        if (k == 0) {
                            raw.first_at = at;
                            raw.first_length =
                                static_cast<std::size_t>(*length);
                        } else {
                            raw.last_at = at;
                            raw.last_length = static_cast<std::size_t>(*length);
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
                block.sections = dstd::Vector<detail::SectionSlot>(raws.size());
                for (std::size_t i = 0; i < raws.size(); ++i) {
                    detail::SectionSlot& slot = block.sections[i];
                    slot.page = raws[i].page;
                    slot.page_index = raws[i].page_index;
                    slot.offset_in_page = raws[i].offset_in_page;
                    slot.length = raws[i].length;
                    slot.entry_count = raws[i].entry_count;
                    slot.first_key =
                        body.subspan(raws[i].first_at, raws[i].first_length);
                    slot.last_key =
                        body.subspan(raws[i].last_at, raws[i].last_length);
                }
                break;
            }
        }
    }
    block.state.store(static_cast<std::uint8_t>(state),
                      std::memory_order_release);
    return state_of(state);
}

detail::LoadStatus Snapshot::load_page(detail::Commit& commit,
                                       std::size_t block) const {
    detail::PageSlot& slot = commit.pages[block];
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

    const detail::CommitBlock& held = commit.blocks[block];
    std::uint8_t bad = 0;
    const PageTarget target{&slot.bytes, &bad};
    auto read = files_[commit.file_index].reader.read_pages(
        dstd::Span<const PageRef>(&held.ref, 1),
        dstd::Span<const PageTarget>(&target, 1));
    detail::LoadState state = detail::LoadState::Good;
    if (!read) {
        slot.error = read.error();
        state = detail::LoadState::Failed;
        reads_->add(0, 1, BlockReads{});
    } else {
        reads_->add(0, 1, *read);
        if (bad != 0) state = detail::LoadState::Bad;
    }
    slot.state.store(static_cast<std::uint8_t>(state),
                     std::memory_order_release);
    return state_of(state);
}

detail::LoadStatus Snapshot::load_page(const detail::FileData& file,
                                       std::size_t page_index,
                                       const PageRef& ref) const {
    if (page_index >= file.pages.size()) {
        return detail::LoadStatus{detail::LoadState::Bad, dstd::Error{}};
    }
    detail::PageSlot& slot = file.pages.slot(page_index);
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

    auto bytes = file.reader.read_page(ref);
    detail::LoadState state = detail::LoadState::Good;
    BlockReads reads;
    if (!bytes) {
        slot.error = bytes.error();
        state = bytes.error().is(dstd::Condition::Io)
                    ? detail::LoadState::Failed
                    : detail::LoadState::Bad;
    } else {
        reads.calls = 1;
        reads.bytes = bytes->size();
        slot.bytes = std::move(*bytes);
    }
    reads_->add(0, 1, reads);
    slot.state.store(static_cast<std::uint8_t>(state),
                     std::memory_order_release);
    return state_of(state);
}

dstd::Expected<const SectionView*, dstd::Error> Snapshot::section_view(
    const detail::FileData& file, detail::LazyBlock& block,
    std::size_t index) const {
    if (index >= block.sections.size()) return bad_page_error();
    detail::SectionSlot& slot = block.sections[index];
    if (SectionView* known = slot.view.load(std::memory_order_acquire)) {
        return known;
    }
    const dstd::Vector<std::uint8_t>* bytes = nullptr;
    if (block.owner == nullptr) {
        const detail::LoadStatus status =
            load_page(file, slot.page_index, slot.page);
        if (status.state == detail::LoadState::Failed) {
            return dstd::Unexpected<dstd::Error>(status.error);
        }
        if (status.state != detail::LoadState::Good) return bad_page_error();
        bytes = &file.pages.slot(slot.page_index).bytes;
    } else {
        detail::Commit& commit = *block.owner;
        if (commit.synced) {
            const detail::LoadStatus status = load_page(commit, slot.block);
            if (status.state == detail::LoadState::Failed) {
                return dstd::Unexpected<dstd::Error>(status.error);
            }
            if (status.state != detail::LoadState::Good)
                return bad_page_error();
        } else if (static_cast<detail::LoadState>(commit.state.load(
                       std::memory_order_acquire)) != detail::LoadState::Good) {
            return bad_page_error();
        }
        bytes = &commit.pages[slot.block].bytes;
    }
    return publish_view(slot, *bytes);
}

dstd::Expected<detail::LazyBlock*, dstd::Error> Snapshot::copy_sections(
    const detail::CopyData& copy, const detail::CopyCommit& held) const {
    const auto block_result = [](const detail::LoadStatus& status)
        -> dstd::Expected<void, dstd::Error> {
        if (status.state == detail::LoadState::Good) return {};
        if (status.state == detail::LoadState::Failed) {
            return dstd::Unexpected<dstd::Error>(status.error);
        }
        return bad_page_error();
    };
    if (held.commit != nullptr) {
        auto indexed = index_result(load_index(*held.commit));
        if (!indexed) return dstd::Unexpected<dstd::Error>(indexed.error());
        return section_block_of(*held.commit, copy.local_id);
    }
    if (held.sections == nullptr) {
        return static_cast<detail::LazyBlock*>(nullptr);
    }
    const detail::FileData& file = files_[copy.file_index];
    auto sections = block_result(load_block(file, *held.sections));
    if (!sections) return dstd::Unexpected<dstd::Error>(sections.error());
    return held.sections;
}

void detail::PageSlots::assign(std::size_t count) {
    count_ = count;
    chunks_ = dstd::Vector<std::atomic<Chunk*>>((count + CHUNK_PAGES - 1) /
                                                CHUNK_PAGES);
}

detail::PageSlot& detail::PageSlots::slot(std::size_t index) const {
    std::atomic<Chunk*>& held = chunks_[index / CHUNK_PAGES];
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
    return chunk->slots[index % CHUNK_PAGES];
}

detail::PageSlots::~PageSlots() {
    // Each chunk came from dstd::make_unique, so its own deleter frees it.
    for (std::atomic<Chunk*>& held : chunks_) {
        dstd::UniquePtr<Chunk> owned(held.load(std::memory_order_relaxed));
    }
}

detail::LazyBlock::~LazyBlock() {
    for (SectionSlot& slot : sections) {
        delete slot.view.load(std::memory_order_relaxed);
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
    PageRef page{ref.offset, static_cast<std::uint32_t>(ref.length), ref.crc};
    auto bytes = files_[file_index].reader.read_page(page);
    if (!bytes) {
        return dstd::Unexpected<dstd::Error>(bytes.error());
    }
    owned = std::move(*bytes);
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
    const detail::CopyData& copy = group->copies[group->current];
    if (copy.state != CopyState::Ready || key_hidden(key, group->hidden)) {
        return dstd::Optional<Value>{};
    }
    // A key no range holds is absent, and nothing is read for it.
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
    for (const detail::CopyCommit& held : copy.commits) {
        if (!holds_key(held.first_key, held.last_key)) continue;
        auto held_sections = copy_sections(copy, held);
        if (!held_sections) {
            return dstd::Unexpected<dstd::Error>(held_sections.error());
        }
        if (*held_sections == nullptr) continue;
        detail::LazyBlock& block = **held_sections;
        for (std::size_t i = 0; i < block.sections.size(); ++i) {
            const detail::SectionSlot& slot = block.sections[i];
            if (!holds_key(slot.first_key, slot.last_key)) continue;
            auto view = section_view(files_[copy.file_index], block, i);
            if (!view) return dstd::Unexpected<dstd::Error>(view.error());
            const auto entry = (*view)->find(key);
            if (entry) sources.push_back({copy.file_index, *entry});
        }
    }
    if (sources.empty()) return dstd::Optional<Value>{};

    Value value;
    ByteView view;
    auto filled = materialize(copy.fold, sources, view, value.owned_);
    if (!filled) return dstd::Unexpected<dstd::Error>(filled.error());
    value.view_ = view;
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
        // Each matching section's cursor already stands on the key.
        for (const std::size_t arm_index : at_key_) {
            const ScopeCursor& cursor = arms_[arm_index].cursor;
            const std::size_t file_index = cursor.copy()->file_index;
            for (const std::size_t section_index : cursor.matching()) {
                sources_.push_back(
                    {file_index, cursor.cursors_[section_index].entry()});
            }
        }
    } else {
        const ScopeCursor& cursor = arms_[arm_].cursor;
        const auto& matching = cursor.matching();
        if (!matching.empty()) {
            sources_.push_back({cursor.copy()->file_index,
                                cursor.cursors_[matching.back()].entry()});
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
    // A row after an unreadable page cannot be trusted, so the scan stops.
    error_ = cursor.error_;
    at_end_ = true;
    key_ = {};
    at_key_.clear();
    return true;
}

/// The fold heap order: the smaller key first, and on a tie the earlier arm.
struct ScanCursor::ArmAfter {
    const dstd::Vector<Arm>* arms;
    bool operator()(std::size_t a, std::size_t b) const {
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
    const detail::CopyData& copy = arm.group->copies[arm.group->current];
    const detail::LoadStatus status = snapshot_->ensure_copy(copy);
    if (status.state == detail::LoadState::Failed) {
        // A block of this scope could not be read, so no row after it can be
        // trusted: the cursor stops and reports the error.
        error_ = status.error;
        at_end_ = true;
        key_ = {};
        at_key_.clear();
        return false;
    }
    // A copy with a bad commit is skipped: its cursor stays at its end.
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
    while (!arm_heap_.empty() &&
           compare_bytes(arms_[arm_heap_.front()].cursor.key(), best) == 0) {
        std::pop_heap(arm_heap_.begin(), arm_heap_.end(), ArmAfter{&arms_});
        at_key_.push_back(arm_heap_.back());
        arm_heap_.pop_back();
    }
    key_ = best;
    at_end_ = false;
}

namespace {

/// The heap order: the smaller key first, and on a tie the older section.
struct HeapAfter {
    const dstd::Vector<ByteView>* keys;
    bool operator()(std::size_t a, std::size_t b) const {
        const int by_key = compare_bytes((*keys)[a], (*keys)[b]);
        if (by_key != 0) return by_key > 0;
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
    sections_.clear();
    heap_.clear();
    // A range wholly before begin or at or past end yields nothing.
    const auto outside = [&](ByteView first, ByteView last) {
        return compare_bytes(last, begin) < 0 ||
               (!end.empty() && compare_bytes(first, end) >= 0);
    };
    for (const detail::CopyCommit& held : copy.commits) {
        if (outside(held.first_key, held.last_key)) continue;
        auto held_sections = snapshot.copy_sections(copy, held);
        if (!held_sections) {
            error_ = held_sections.error();
            at_end_ = true;
            key_ = {};
            matching_.clear();
            return;
        }
        if (*held_sections == nullptr) continue;
        const dstd::Vector<detail::SectionSlot>& slots =
            (*held_sections)->sections;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (outside(slots[i].first_key, slots[i].last_key)) continue;
            sections_.push_back({*held_sections, i});
        }
    }
    cursors_.assign(sections_.size(), SectionView::Cursor{});
    keys_.assign(sections_.size(), ByteView{});
    loaded_.assign(sections_.size(), 0);
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        const detail::SectionSlot& section =
            sections_[i].block->sections[sections_[i].index];
        // Its first key at or after begin is at least the larger of the two,
        // so the heap orders it right before its page is read.
        keys_[i] = compare_bytes(section.first_key, begin) < 0
                       ? begin
                       : section.first_key;
        heap_.push_back(i);
    }
    std::make_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_});
    settle();
}

bool ScopeCursor::load(std::size_t index) {
    const detail::SectionLocation& at = sections_[index];
    auto view = snapshot_->section_view(snapshot_->files_[copy_->file_index],
                                        *at.block, at.index);
    if (!view) {
        error_ = view.error();
        at_end_ = true;
        key_ = {};
        matching_.clear();
        return false;
    }
    loaded_[index] = 1;
    cursors_[index] = (*view)->lower_bound(begin_);
    push(index);
    return true;
}

void ScopeCursor::push(std::size_t index) {
    if (cursors_[index].at_end()) return;
    keys_[index] = cursors_[index].entry().key();
    heap_.push_back(index);
    std::push_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_});
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
            // The bound from the record is the smallest key on the heap, so
            // the page of this section is the next one to read.
            std::pop_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_});
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
            std::pop_heap(heap_.begin(), heap_.end(), HeapAfter{&keys_});
            const std::size_t index = heap_.back();
            heap_.pop_back();
            if (loaded_[index] == 0) {
                // A section whose bound ties the key may hold it too: read it,
                // put back what was taken and settle again.
                if (!load(index)) return;
                for (const std::size_t taken : matching_) {
                    heap_.push_back(taken);
                    std::push_heap(heap_.begin(), heap_.end(),
                                   HeapAfter{&keys_});
                }
                reload = true;
                break;
            }
            matching_.push_back(index);
        }
        if (reload) continue;
        // The heap pops a tie oldest first, so matching_ is in commit order.
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
    for (const ScopeKey& scope : scopes) {
        auto found = find_group(spec, scope);
        if (!found) {
            // A block of the directory could not be read, so no scope after
            // it can be trusted: the scan stops and reports the error.
            cursor.error_ = found.error();
            cursor.at_end_ = true;
            return cursor;
        }
        const detail::ScopeGroup* group = *found;
        if (group == nullptr || group->dropped || group->copies.empty()) {
            continue;
        }
        const detail::CopyData& copy = group->copies[group->current];
        if (copy.state != CopyState::Ready) continue;
        if (fold == FoldKind::None) fold = copy.fold;
        ScanCursor::Arm arm;
        arm.scope = scope;
        arm.group = group;
        cursor.arms_.push_back(std::move(arm));
    }
    cursor.fold_ = fold;
    if (fold != FoldKind::None) {
        // A fold merges every scope by key, so every arm opens now.
        for (std::size_t i = 0; i < cursor.arms_.size(); ++i) {
            if (!cursor.open_arm(i)) return cursor;
            cursor.push_arm(i);
        }
    }
    // Scopes one after the other open each arm when the scan reaches it.
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
    if (failure) {
        // The scopes of a block that could not be read are unknown, so the
        // scan holds no row and reports the error.
        ScanCursor cursor;
        cursor.snapshot_ = this;
        cursor.error_ = *failure;
        cursor.at_end_ = true;
        return cursor;
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const ScopeKey& a, const ScopeKey& b) {
                  return compare_scopes(a, b) < 0;
              });
    scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
    return scan(spec, scopes, begin, end);
}

}  // namespace dftracer::utils::index::store::engine
