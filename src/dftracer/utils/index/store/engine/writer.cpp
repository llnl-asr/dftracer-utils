#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/index/store/engine/page_size.h>
#include <dftracer/utils/index/store/engine/writer.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store::engine {
namespace {

using dftracer::utils::ByteView;

constexpr std::size_t SECTION_HEADER_BYTES = 20;

const std::array<std::uint8_t, 8> COUNT_VALUE = {1, 0, 0, 0, 0, 0, 0, 0};

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

/// The bytes one writer may hold in pages when the caller names no quota: the
/// commits of four executor threads, counted in pages of the store's default
/// page size at least, so a page size the caller shrank for a test does not
/// shrink the bound to a few kilobytes.
std::uint64_t default_page_quota(Executor& executor, std::uint32_t page_size) {
    const std::uint64_t threads =
        std::max<std::uint64_t>(executor.get_num_threads(), 1);
    const std::uint64_t unit = std::max<std::uint64_t>(
        page_size, constants::store::STORE_DEFAULT_PAGE_BYTES);
    return std::max<std::uint64_t>(16, 4 * threads) * unit;
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

std::size_t varint_size(std::uint64_t value) {
    std::size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7U;
        ++size;
    }
    return size;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::size_t encoded_entry_size(std::size_t key_length,
                               std::uint64_t value_length) {
    const bool big = value_length > BIG_VALUE_BYTES;
    const std::uint64_t stored = big ? 20U : value_length;
    const std::uint64_t value_code =
        (static_cast<std::uint64_t>(stored) << 1U) | (big ? 1U : 0U);
    return varint_size(key_length) + varint_size(value_code) + key_length +
           static_cast<std::size_t>(stored);
}

bool same_name(const dstd::String& name, dstd::StringView other) {
    return name.size() == other.size() &&
           (other.empty() ||
            std::memcmp(name.data(), other.data(), other.size()) == 0);
}

dstd::Vector<std::uint8_t> copy_bytes(ByteView bytes) {
    dstd::Vector<std::uint8_t> out;
    if (bytes.empty()) return out;
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
    out.insert(out.end(), begin, begin + bytes.size());
    return out;
}

/// The exact length SectionBuilder::seal produces for the entries added so far.
struct SectionSizer {
    std::size_t index_bytes = SECTION_HEADER_BYTES;
    std::size_t body_bytes = 0;
    std::size_t group_bytes = 0;
    bool have_group = false;

    void add(std::size_t key_length, std::size_t entry_bytes) {
        if (have_group && group_bytes + entry_bytes > SUBPAGE_BYTES) {
            group_bytes = 0;
            have_group = false;
        }
        if (!have_group) {
            index_bytes += 8U + varint_size(key_length) + key_length;
            have_group = true;
        }
        group_bytes += entry_bytes;
        body_bytes += entry_bytes;
    }

    std::size_t size() const {
        return align_up(index_bytes + body_bytes, SECTION_ALIGN);
    }
};

std::uint64_t random_bits() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::uint64_t seed = static_cast<std::uint64_t>(now) ^
                         (static_cast<std::uint64_t>(::getpid()) << 32U) ^
                         reinterpret_cast<std::uintptr_t>(&seed) ^
                         (counter.fetch_add(1, std::memory_order_relaxed) *
                          0x9E3779B97F4A7C15ULL);
    std::mt19937_64 rng(seed);
    return rng() ^ (rng() << 32U);
}

std::string path_of(dstd::StringView dir, dstd::StringView name) {
    std::string out(dir.data(), dir.size());
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out.append(name.data(), name.size());
    return out;
}

struct KeyGroup {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t key_length = 0;
    std::uint64_t value_length = 0;
};

/// Frees a batch whose page no write carried, including the one a failed page
/// write left behind, and releases the bytes it charged so no wait for them
/// outlives the failure. A discard means a failed write or a writer going
/// away, so the count latches too.
void discard_batch(void* context) {
    auto* pending = static_cast<PendingBatch*>(context);
    if (pending->pages != nullptr) {
        pending->pages->release(pending->charged_bytes);
        pending->pages->latch();
    }
    delete pending;
}

NamespaceItem namespace_item(const NamespaceSpec& spec, NamespaceId id) {
    NamespaceItem item;
    item.local_id = id.value;
    item.name = spec.name;
    item.format_version = spec.format_version;
    item.fold_kind = static_cast<std::uint8_t>(spec.fold);
    item.codec = 0;
    item.durable = spec.durable ? 1 : 0;
    return item;
}

ScopeItem scope_item(const ScopeKey& key, std::uint32_t id,
                     const dstd::Vector<ScopePath>* signature) {
    ScopeItem item;
    item.local_id = id;
    item.kind = key.kind;
    item.paths.reserve(key.paths.size());
    for (std::size_t i = 0; i < key.paths.size(); ++i) {
        ScopePath path;
        if (signature != nullptr) path = (*signature)[i];
        path.logical_path = key.paths[i];
        item.paths.push_back(std::move(path));
    }
    return item;
}

CopyItem copy_item(std::uint32_t copy_id, std::uint32_t namespace_id,
                   std::uint32_t scope_id, std::uint64_t params_hash,
                   std::uint64_t generation, CopyState state) {
    CopyItem item;
    item.local_id = copy_id;
    item.namespace_id = namespace_id;
    item.scope_id = scope_id;
    item.params_hash = params_hash;
    item.generation = generation;
    item.state = state;
    return item;
}

std::atomic<WriterTestHook::SpaceSealedFn>& space_sealed_hook() {
    static std::atomic<WriterTestHook::SpaceSealedFn> hook{nullptr};
    return hook;
}

}  // namespace

ByteView Batch::view_at(std::size_t offset, std::size_t length) const {
    if (length == 0) return {};
    return {arena_.data() + offset, length};
}

void Batch::record_error(dstd::Condition condition) {
    if (!error_) error_ = dstd::Error(condition);
}

void Batch::add(ByteView key, ByteView value) {
    Entry entry;
    entry.key_offset = arena_.size();
    entry.key_length = key.size();
    if (!key.empty()) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(key.data());
        arena_.insert(arena_.end(), begin, begin + key.size());
    }
    entry.value_offset = arena_.size();
    entry.value_length = value.size();
    if (!value.empty()) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
        arena_.insert(arena_.end(), begin, begin + value.size());
    }
    entries_.push_back(entry);
}

void Batch::put(ByteView key, ByteView value) {
    if (error_) return;
    if (fold_ != FoldKind::None) {
        record_error(dstd::Condition::InvalidArgument);
        return;
    }
    add(key, value);
}

void Batch::fold(ByteView key, ByteView value) {
    if (error_) return;
    if (fold_ == FoldKind::None) {
        record_error(dstd::Condition::InvalidArgument);
        return;
    }
    if (fold_ == FoldKind::Count) {
        add(key, ByteView{COUNT_VALUE.data(), COUNT_VALUE.size()});
        return;
    }
    auto checked = check_fold_value(fold_, value);
    if (!checked) {
        error_ = checked.error();
        return;
    }
    add(key, value);
}

void Writer::latch(const dstd::Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latched_error_) latched_error_ = error;
}

dstd::Optional<dstd::Error> Writer::latched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latched_error_;
}

dstd::Expected<std::unique_ptr<Writer>, dstd::Error> Writer::create(
    dstd::StringView dir, Executor& executor, const WriterOptions& options) {
    if (!executor.has_io_backend()) {
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::InvalidArgument,
                        "store writer needs an executor with an I/O backend"));
    }

    std::uint32_t page_size = 0;
    if (options.page_size) {
        auto chosen = choose_page_size(0, options.page_size);
        if (!chosen) return dstd::Unexpected<dstd::Error>(chosen.error());
        page_size = *chosen;
    } else {
        auto chosen = page_size_for(dir);
        if (!chosen) return dstd::Unexpected<dstd::Error>(chosen.error());
        page_size = *chosen;
    }

    std::uint64_t page_quota = 0;
    if (options.page_quota_bytes) {
        page_quota = *options.page_quota_bytes;
        if (page_quota == 0) {
            return dstd::Unexpected<dstd::Error>(dstd::Error(
                dstd::Condition::InvalidArgument, "page quota is zero"));
        }
    } else {
        page_quota = default_page_quota(executor, page_size);
    }

    // The bytes of the pages in flight are charged here, so a caller that
    // builds one writer inside its own pool bounds this writer's page memory
    // with that pool's limit.
    auto page_pool = memory::current_pool().child(
        "index store writer", memory::PoolKind::Scratch, memory::Limits{});
    if (!page_pool) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Internal, "no pool for the writer's page bytes"));
    }

    auto snapshot = Snapshot::open(dir);
    if (!snapshot) return dstd::Unexpected<dstd::Error>(snapshot.error());

    dstd::Vector<ScopeGeneration> generations = snapshot->generations();
    std::sort(generations.begin(), generations.end(),
              [](const ScopeGeneration& a, const ScopeGeneration& b) {
                  const int by_name =
                      a.namespace_name.compare(b.namespace_name);
                  if (by_name != 0) return by_name < 0;
                  if (a.format_version != b.format_version) {
                      return a.format_version < b.format_version;
                  }
                  return compare_scopes(a.scope, b.scope) < 0;
              });

    dstd::String name =
        data_file_name(options.host, static_cast<std::uint32_t>(::getpid()),
                       random_bits(), options.label);
    dstd::String path(path_of(dir, name));
    auto file =
        DataFileWriter::create(path, page_size, options.host, options.label);
    if (!file) return dstd::Unexpected<dstd::Error>(file.error());

    std::unique_ptr<Writer> writer(new Writer(
        std::move(name), std::move(path), page_size, std::move(generations),
        std::move(*file), std::move(page_pool), page_quota, executor, options));
    writer->committer_.start();
    return writer;
}

Writer::Writer(dstd::String name, dstd::String path, std::uint32_t page_size,
               dstd::Vector<ScopeGeneration> generations, DataFileWriter file,
               memory::PoolHandle page_pool, std::uint64_t page_quota,
               Executor& executor, const WriterOptions& options)
    : file_(std::move(file)),
      committer_(file_, executor, options.commit_bytes,
                 options.commit_interval),
      page_pool_(std::move(page_pool)),
      page_bytes_(page_quota, *page_pool_.get()),
      pages_(file_, executor.io_backend(), executor, page_size,
             &Writer::wrote_page, this, &discard_batch, &Writer::pages_idle),
      name_(std::move(name)),
      path_(std::move(path)),
      page_size_(page_size),
      generations_(std::move(generations)) {}

Writer::~Writer() {
    if (!closed_.load(std::memory_order_acquire)) {
        pages_.seal_all();
        pages_.wait_idle();
    }
}

DataFileWriter& WriterTestHook::file(Writer& writer) { return writer.file_; }

void WriterTestHook::set_space_sealed(SpaceSealedFn fn) {
    space_sealed_hook().store(fn, std::memory_order_release);
}

WriterPagesCounters WriterTestHook::pages(const Writer& writer) {
    return writer.page_bytes_.counters();
}

Writer::NamespaceEntry* Writer::find_namespace(std::uint32_t id) {
    for (NamespaceEntry& entry : namespaces_) {
        if (entry.id.value == id) return &entry;
    }
    return nullptr;
}

Writer::NamespaceEntry* Writer::find_namespace(dstd::StringView name) {
    for (NamespaceEntry& entry : namespaces_) {
        if (same_name(entry.spec.name, name)) return &entry;
    }
    return nullptr;
}

Writer::ScopeEntry* Writer::find_scope(const ScopeKey& scope) {
    const auto at = scopes_.find(scope);
    return at == scopes_.end() ? nullptr : at->second.get();
}

Writer::ScopeEntry* Writer::add_scope(const ScopeKey& scope) {
    auto entry = dstd::make_unique<ScopeEntry>();
    entry->id = next_scope_id_++;
    return scopes_.emplace(scope, std::move(entry)).first->second.get();
}

Writer::CopyEntry* Writer::find_copy(std::uint32_t namespace_id,
                                     std::uint32_t scope_id) {
    const auto at = copies_.find(copy_key(namespace_id, scope_id));
    return at == copies_.end() ? nullptr : at->second.get();
}

Writer::CopyEntry* Writer::add_copy(NamespaceId id, std::uint32_t scope_id,
                                    std::uint64_t generation, bool live) {
    auto entry = dstd::make_unique<CopyEntry>();
    entry->copy_id = next_copy_id_++;
    entry->generation = generation;
    entry->highest_generation = generation;
    entry->live = live;
    return copies_.emplace(copy_key(id.value, scope_id), std::move(entry))
        .first->second.get();
}

Writer::CopyEntry* Writer::find_or_add_copy(const NamespaceSpec& spec,
                                            NamespaceId id,
                                            const ScopeKey& scope,
                                            std::uint32_t scope_id) {
    CopyEntry* entry = find_copy(id.value, scope_id);
    if (entry != nullptr && entry->live) return entry;
    if (entry != nullptr) {
        const std::uint64_t generation = entry->highest_generation + 1U;
        entry->copy_id = next_copy_id_++;
        entry->generation = generation;
        entry->highest_generation = generation;
        entry->live = true;
        return entry;
    }
    return add_copy(id, scope_id, generation_of(spec, scope) + 1U, true);
}

std::uint64_t Writer::generation_of(const NamespaceSpec& spec,
                                    const ScopeKey& scope) const {
    const auto at = std::lower_bound(
        generations_.begin(), generations_.end(), spec,
        [&scope](const ScopeGeneration& entry, const NamespaceSpec& wanted) {
            const int by_name = entry.namespace_name.compare(wanted.name);
            if (by_name != 0) return by_name < 0;
            if (entry.format_version != wanted.format_version) {
                return entry.format_version < wanted.format_version;
            }
            return compare_scopes(entry.scope, scope) < 0;
        });
    if (at == generations_.end() || at->namespace_name != spec.name ||
        at->format_version != spec.format_version ||
        compare_scopes(at->scope, scope) != 0) {
        return 0;
    }
    return at->generation;
}

dstd::Expected<NamespaceId, dstd::Error> Writer::add_namespace(
    const NamespaceSpec& spec) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    if (spec.name.empty()) return invalid_error();

    std::lock_guard<std::mutex> lock(mutex_);
    if (latched_error_) return dstd::Unexpected<dstd::Error>(*latched_error_);
    const dstd::StringView name(spec.name.data(), spec.name.size());
    NamespaceEntry* existing = find_namespace(name);
    if (existing != nullptr) {
        if (existing->spec == spec) return existing->id;
        return invalid_error();
    }
    NamespaceEntry entry;
    entry.id.value = next_namespace_id_++;
    entry.spec = spec;
    namespaces_.push_back(std::move(entry));
    return namespaces_.back().id;
}

Batch Writer::batch(NamespaceId namespace_id, const ScopeKey& scope,
                    std::uint64_t params_hash,
                    const dstd::Vector<ScopePath>* scope_paths) {
    Batch batch;
    batch.writer_ = this;
    batch.namespace_id_ = namespace_id;
    batch.scope_ = scope;
    batch.params_hash_ = params_hash;
    if (scope_paths != nullptr) {
        batch.signature_ = *scope_paths;
        if (batch.signature_.size() != scope.paths.size()) {
            batch.record_error(dstd::Condition::InvalidArgument);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const NamespaceEntry* entry = find_namespace(namespace_id.value);
    if (entry == nullptr) {
        batch.record_error(dstd::Condition::InvalidArgument);
        return batch;
    }
    batch.fold_ = entry->spec.fold;
    return batch;
}

BigRef Writer::place_big(void* context, ByteView value) {
    auto* placement = static_cast<BigPlacement*>(context);
    auto placed = placement->writer->file_.append_big(value);
    if (!placed) {
        placement->writer->latch(placed.error());
        return BigRef{};
    }
    placement->runs->push_back(*placed);
    return BigRef{placed->offset, placed->length, placed->crc};
}

void Writer::pages_idle(void* context) {
    static_cast<Writer*>(context)->page_bytes_.nudge();
}

void Writer::wrote_page(void* context, WrittenPage&& page) {
    auto* writer = static_cast<Writer*>(context);
    std::uint64_t bytes = 0;
    for (const PageSection& section : page.sections) bytes += section.length;
    writer->page_bytes_.release(bytes);
    writer->committer_.on_page(std::move(page));
}

dstd::Expected<Writer::SectionBytes, dstd::Error> Writer::build_section(
    const Batch& batch) {
    SectionBytes section;
    if (batch.entries_.empty()) return section;

    const auto key_of = [&batch](std::size_t index) {
        const Batch::Entry& entry = batch.entries_[index];
        return batch.view_at(entry.key_offset, entry.key_length);
    };
    const auto value_of = [&batch](std::size_t index) {
        const Batch::Entry& entry = batch.entries_[index];
        return batch.view_at(entry.value_offset, entry.value_length);
    };

    dstd::Vector<std::size_t> order(batch.entries_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&key_of](std::size_t a, std::size_t b) {
                  const int cmp = compare_bytes(key_of(a), key_of(b));
                  if (cmp != 0) return cmp < 0;
                  return a < b;
              });

    dstd::Vector<KeyGroup> groups;
    for (std::size_t i = 0; i < order.size();) {
        std::size_t end = i + 1U;
        while (end < order.size() &&
               compare_bytes(key_of(order[end]), key_of(order[i])) == 0) {
            ++end;
        }
        KeyGroup group;
        group.begin = i;
        group.end = end;
        group.key_length = batch.entries_[order[i]].key_length;
        if (batch.fold_ == FoldKind::Concat) {
            std::uint64_t total = 0;
            for (std::size_t at = i; at < end; ++at) {
                total += batch.entries_[order[at]].value_length;
            }
            group.value_length = total;
        } else if (batch.fold_ == FoldKind::None ||
                   batch.fold_ == FoldKind::Last) {
            group.value_length = batch.entries_[order[end - 1U]].value_length;
        } else {
            group.value_length = 8;
        }
        groups.push_back(group);
        i = end;
    }

    // One batch is one section: a section of a page or more becomes a run of
    // its own in PageWriter, and a page holds the sections of several batches.
    SectionSizer sizer;
    for (const KeyGroup& group : groups) {
        sizer.add(group.key_length,
                  encoded_entry_size(group.key_length, group.value_length));
    }
    if (sizer.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return invalid_error();
    }

    // Charged before the seal places big values and before any id is
    // assigned, so a Busy commit writes nothing and changes no writer state.
    const std::uint64_t charged = static_cast<std::uint64_t>(sizer.size());
    if (page_bytes_.take(charged) == WriterPages::Take::Busy) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(WriterErrc::Busy));
    }

    const FoldFn fold = fold_fn(batch.fold_);
    SectionBuilder builder;
    for (std::size_t i = 0; i < order.size(); ++i) {
        builder.add(key_of(order[i]), value_of(order[i]));
    }
    BigPlacement placement{this, &section.big_runs};
    auto sealed = builder.seal(fold, &Writer::place_big, &placement);
    if (auto error = latched()) {
        page_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(*error);
    }
    if (!sealed) {
        page_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(sealed.error());
    }
    if (sealed->size() != charged) {
        page_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::Internal,
                        "store section size differs from its plan"));
    }

    section.bytes = std::move(*sealed);
    section.entry_count = builder.entry_count();
    section.first_key = copy_bytes(builder.first_key());
    section.last_key = copy_bytes(builder.last_key());
    return section;
}

dstd::Expected<void, dstd::Error> Writer::prepare(const Batch& batch,
                                                  PendingBatch& pending) {
    NamespaceEntry* namespace_entry = find_namespace(batch.namespace_id_.value);
    if (namespace_entry == nullptr) return invalid_error();
    ScopeEntry* scope_entry = find_scope(batch.scope_);
    if (scope_entry == nullptr) scope_entry = add_scope(batch.scope_);

    CopyEntry* copy_entry =
        find_or_add_copy(namespace_entry->spec, batch.namespace_id_,
                         batch.scope_, scope_entry->id);

    pending.namespace_id = namespace_entry->id.value;
    pending.namespace_item =
        namespace_item(namespace_entry->spec, namespace_entry->id);
    pending.scope_id = scope_entry->id;
    pending.scope_item =
        scope_item(batch.scope_, scope_entry->id,
                   batch.signature_.empty() ? nullptr : &batch.signature_);
    pending.copy_item = copy_item(
        copy_entry->copy_id, namespace_entry->id.value, scope_entry->id,
        batch.params_hash_, copy_entry->generation, CopyState::Ready);
    pending.durable = namespace_entry->spec.durable;
    return {};
}

dstd::Expected<void, dstd::Error> Writer::commit(Batch&& batch) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    if (batch.writer_ != this) return invalid_error();
    if (batch.error_) return dstd::Unexpected<dstd::Error>(*batch.error_);

    auto section = build_section(batch);
    if (!section) {
        if (section.error().is_code(WriterErrc::Busy)) return busy_error();
        return dstd::Unexpected<dstd::Error>(section.error());
    }
    const std::uint64_t section_bytes =
        static_cast<std::uint64_t>(section->bytes.size());
    const bool charged = section_bytes != 0;

    auto* pending = new PendingBatch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            if (charged) page_bytes_.release(section_bytes);
            delete pending;
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        auto prepared = prepare(batch, *pending);
        if (!prepared) {
            if (charged) page_bytes_.release(section_bytes);
            delete pending;
            return prepared;
        }
    }

    if (!charged) {
        delete pending;
        return {};
    }

    pending->pages = &page_bytes_;
    pending->charged_bytes = section_bytes;
    pending->big_runs = std::move(section->big_runs);

    PageSection info;
    info.copy_id = pending->copy_item.local_id;
    info.entry_count = section->entry_count;
    info.first_key = std::move(section->first_key);
    info.last_key = std::move(section->last_key);

    auto appended =
        pages_.append(pending->namespace_id,
                      ByteView{section->bytes.data(), section->bytes.size()},
                      std::move(info), pending);
    if (!appended) {
        page_bytes_.release(section_bytes);
        // The page writer reports this error from every later append, so a
        // wait for bytes it will never release has to end here.
        page_bytes_.latch();
        delete pending;
        return appended;
    }
    // A waiter needs a release. A write in flight brings one; with none in
    // flight, this append's page must be sealed or the waiter waits forever.
    // Sealing only then keeps pages full under steady back pressure.
    if (page_bytes_.waiting() && !pages_.writing()) {
        auto sealed = pages_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            page_bytes_.latch();
        }
    }
    return {};
}

dstd::Unexpected<dstd::Error> Writer::busy_error() {
    // With no write in flight, the held bytes sit in open pages nobody writes:
    // seal them, so threads that all hold a partial batch cannot deadlock.
    // With a write in flight its completion frees bytes, and a seal would only
    // write a partial page.
    if (!pages_.writing()) {
        auto sealed = pages_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            page_bytes_.latch();
            return dstd::Unexpected<dstd::Error>(sealed.error());
        }
    }
    return dstd::Unexpected<dstd::Error>(dstd::Error(WriterErrc::Busy));
}

coro::CoroTask<void> Writer::space() {
    dstd::Optional<dstd::Error> latched_error = latched();
    if (latched_error) {
        page_bytes_.latch();
        co_return;
    }
    if (closed_.load(std::memory_order_acquire) || committer_.failed() ||
        page_bytes_.latched()) {
        co_return;
    }
    // Announced before the check: a write in flight at the check releases
    // bytes after the announce, and with none in flight the seal here, or the
    // seal of an append the seal misses, starts one. Either way the release
    // this waits for comes.
    WriterPages::WaitAnnounce announce(page_bytes_);
    if (!pages_.writing()) {
        auto sealed = pages_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            page_bytes_.latch();
            co_return;
        }
    }
    if (auto* hook = space_sealed_hook().load(std::memory_order_acquire)) {
        hook(*this);
    }
    auto wait = page_bytes_.wait_release(announce.epoch());
    co_await wait;
    co_return;
}

dstd::Expected<void, dstd::Error> Writer::fail(NamespaceId namespace_id,
                                               const ScopeKey& scope,
                                               std::uint64_t params_hash) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    auto* pending = new PendingBatch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            delete pending;
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        NamespaceEntry* namespace_entry = find_namespace(namespace_id.value);
        if (namespace_entry == nullptr) {
            delete pending;
            return invalid_error();
        }
        ScopeEntry* scope_entry = find_scope(scope);
        if (scope_entry == nullptr) scope_entry = add_scope(scope);

        CopyEntry* existing = find_copy(namespace_id.value, scope_entry->id);
        const std::uint64_t highest =
            existing != nullptr ? existing->highest_generation
                                : generation_of(namespace_entry->spec, scope);
        const std::uint64_t generation = highest + 1U;
        if (existing == nullptr) {
            existing =
                add_copy(namespace_id, scope_entry->id, generation, false);
        } else {
            existing->live = false;
            existing->generation = generation;
            existing->highest_generation = generation;
        }

        pending->namespace_id = namespace_entry->id.value;
        pending->namespace_item =
            namespace_item(namespace_entry->spec, namespace_entry->id);
        pending->scope_id = scope_entry->id;
        pending->scope_item = scope_item(scope, scope_entry->id, nullptr);
        // A failed copy is a new copy id, so it is announced on its own.
        pending->copy_item = copy_item(
            next_copy_id_++, namespace_entry->id.value, scope_entry->id,
            params_hash, generation, CopyState::Failed);
        pending->durable = namespace_entry->spec.durable;
    }
    committer_.on_fail(pending);
    return {};
}

dstd::Expected<void, dstd::Error> Writer::drop(NamespaceId namespace_id,
                                               const ScopeKey& scope) {
    return drop(namespace_id, scope, ByteView{}, ByteView{});
}

dstd::Expected<void, dstd::Error> Writer::drop(NamespaceId namespace_id,
                                               const ScopeKey& scope,
                                               ByteView begin, ByteView end) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    auto* pending = new PendingBatch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            delete pending;
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        NamespaceEntry* namespace_entry = find_namespace(namespace_id.value);
        if (namespace_entry == nullptr) {
            delete pending;
            return invalid_error();
        }
        ScopeEntry* scope_entry = find_scope(scope);
        if (scope_entry == nullptr) scope_entry = add_scope(scope);

        CopyEntry* existing = find_copy(namespace_id.value, scope_entry->id);
        const std::uint64_t highest =
            existing != nullptr ? existing->highest_generation
                                : generation_of(namespace_entry->spec, scope);
        const std::uint64_t generation = highest + 1U;
        if (existing == nullptr) {
            existing =
                add_copy(namespace_id, scope_entry->id, generation, false);
        } else {
            existing->live = false;
            existing->generation = generation;
            existing->highest_generation = generation;
        }

        pending->namespace_id = namespace_entry->id.value;
        pending->namespace_item =
            namespace_item(namespace_entry->spec, namespace_entry->id);
        pending->scope_id = scope_entry->id;
        pending->scope_item = scope_item(scope, scope_entry->id, nullptr);
        // A drop announces no copy: its generation is all the committer needs.
        pending->copy_item.local_id = 0;
        pending->copy_item.generation = generation;
        pending->durable = namespace_entry->spec.durable;
    }
    committer_.on_drop(pending, copy_bytes(begin), copy_bytes(end));
    return {};
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Writer::flush() {
    if (closed_.load(std::memory_order_acquire)) {
        if (committer_.failed()) {
            co_return dstd::Unexpected<dstd::Error>(committer_.error());
        }
        co_return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::InvalidArgument));
    }
    auto sealed = pages_.seal_all();
    if (!sealed) {
        committer_.latch(sealed.error());
        page_bytes_.latch();
        co_return sealed;
    }
    auto idle = co_await pages_.wait_idle_async();
    if (!idle) {
        committer_.latch(idle.error());
        page_bytes_.latch();
        co_return idle;
    }
    co_return committer_.commit_pending();
}

coro::CoroTask<dstd::Expected<void, dstd::Error>> Writer::close() {
    if (closed_.load(std::memory_order_acquire)) {
        if (committer_.failed()) {
            co_return dstd::Unexpected<dstd::Error>(committer_.error());
        }
        co_return dstd::Expected<void, dstd::Error>{};
    }
    closed_.store(true, std::memory_order_release);

    dstd::Optional<dstd::Error> first;
    auto sealed = pages_.seal_all();
    if (!sealed) {
        committer_.latch(sealed.error());
        page_bytes_.latch();
        first = sealed.error();
    }
    auto idle = co_await pages_.wait_idle_async();
    if (!idle) {
        committer_.latch(idle.error());
        page_bytes_.latch();
        if (!first) first = idle.error();
    }
    auto committed = committer_.commit_pending();
    if (!committed && !first) first = committed.error();

    auto stopped = co_await committer_.stop();
    if (!stopped && !first) first = stopped.error();

    auto file_closed = file_.close();
    if (!file_closed && !first) first = file_closed.error();

    if (first) co_return dstd::Unexpected<dstd::Error>(*first);
    co_return dstd::Expected<void, dstd::Error>{};
}

}  // namespace dftracer::utils::index::store::engine
