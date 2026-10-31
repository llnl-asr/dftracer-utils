#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/store/extent_size.h>
#include <dftracer/utils/store/writer.h>
#include <unistd.h>

#include <algorithm>
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

namespace dftracer::utils::store::engine {
namespace {

using dftracer::utils::dstd::ByteView;

constexpr std::size_t SEGMENT_HEADER_BYTES = 20;

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

/// The bytes one writer may hold in extents when the caller names no quota: the
/// commits of four executor threads, counted in extents of the default size at
/// least, so a shrunk test extent size does not shrink the bound.
std::uint64_t default_extent_quota(Executor& executor,
                                   std::uint32_t extent_size) {
    const std::uint64_t threads =
        std::max<std::uint64_t>(executor.get_num_threads(), 1);
    const std::uint64_t unit = std::max<std::uint64_t>(
        extent_size, constants::store::STORE_DEFAULT_EXTENT_BYTES);
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

std::size_t encoded_entry_size(std::size_t key_length,
                               std::uint64_t value_length) {
    const bool big = value_length > BIG_VALUE_BYTES;
    const std::uint64_t stored = big ? 20U : value_length;
    const std::uint64_t value_code =
        (static_cast<std::uint64_t>(stored) << 1U) | (big ? 1U : 0U);
    return dstd::varint_size(key_length) + dstd::varint_size(value_code) +
           key_length + static_cast<std::size_t>(stored);
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

struct SegmentSizer {
    std::size_t index_bytes = SEGMENT_HEADER_BYTES;
    std::size_t body_bytes = 0;
    std::size_t group_bytes = 0;
    bool have_group = false;

    void add(std::size_t key_length, std::size_t entry_bytes) {
        if (have_group && group_bytes + entry_bytes > GRANULE_BYTES) {
            group_bytes = 0;
            have_group = false;
        }
        if (!have_group) {
            index_bytes += 8U + dstd::varint_size(key_length) + key_length;
            have_group = true;
        }
        group_bytes += entry_bytes;
        body_bytes += entry_bytes;
    }

    std::size_t size() const {
        return dstd::bits::align_up(index_bytes + body_bytes, SEGMENT_ALIGN);
    }
};

std::uint64_t random_bits() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = steady_now_ns();
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

/// Frees a batch whose extent no write carried and releases the bytes it
/// charged, so no wait outlives the failure. A discard means a failed write or
/// a writer going away, so the count latches too.
void discard_batch(void* context) {
    auto* pending = static_cast<PendingBatch*>(context);
    if (pending->extents != nullptr) {
        pending->extents->release(pending->charged_bytes);
        pending->extents->latch();
    }
    delete pending;
}

NamespaceItem namespace_item(const NamespaceSpec& spec, NamespaceId id) {
    NamespaceItem item;
    item.local_id = id.value;
    item.name = spec.name;
    item.format_version = spec.format_version;
    item.fold_kind = static_cast<std::uint8_t>(spec.fold);
    item.codec = static_cast<std::uint8_t>(spec.codec);
    item.durable = spec.durable ? 1 : 0;
    return item;
}

ScopeItem scope_item(const ScopeKey& key, std::uint32_t id) {
    ScopeItem item;
    item.local_id = id;
    item.kind = key.kind;
    item.paths.reserve(key.paths.size());
    for (const dstd::String& path : key.paths) item.paths.push_back(path);
    return item;
}

CopyItem copy_item(std::uint32_t copy_id, std::uint32_t namespace_id,
                   std::uint32_t scope_id, std::uint64_t params_hash,
                   std::uint64_t generation, CopyState state,
                   const dstd::Vector<SourceStat>& sources) {
    CopyItem item;
    item.local_id = copy_id;
    item.namespace_id = namespace_id;
    item.scope_id = scope_id;
    item.params_hash = params_hash;
    item.generation = generation;
    item.state = state;
    item.sources = sources;
    return item;
}

bool same_sources(const dstd::Vector<SourceStat>& a,
                  const dstd::Vector<SourceStat>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].mtime != b[i].mtime || a[i].size != b[i].size ||
            a[i].hash != b[i].hash) {
            return false;
        }
    }
    return true;
}

std::atomic<WriterTestHook::SpaceSealedFn>& space_sealed_hook() {
    static std::atomic<WriterTestHook::SpaceSealedFn> hook{nullptr};
    return hook;
}

}  // namespace

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

    std::uint32_t extent_size = 0;
    if (options.extent_size) {
        auto chosen = choose_extent_size(0, options.extent_size);
        if (!chosen) return dstd::Unexpected<dstd::Error>(chosen.error());
        extent_size = *chosen;
    } else {
        auto chosen = extent_size_for(dir);
        if (!chosen) return dstd::Unexpected<dstd::Error>(chosen.error());
        extent_size = *chosen;
    }

    std::uint64_t extent_quota = 0;
    if (options.extent_quota_bytes) {
        extent_quota = *options.extent_quota_bytes;
        if (extent_quota == 0) {
            return dstd::Unexpected<dstd::Error>(dstd::Error(
                dstd::Condition::InvalidArgument, "extent quota is zero"));
        }
    } else {
        extent_quota = default_extent_quota(executor, extent_size);
    }

    // Charged to the current pool, so a caller that builds a writer inside its
    // own pool bounds the extent memory with that pool's limit.
    auto extent_pool = memory::current_pool().child(
        "index store writer", memory::PoolKind::Scratch, memory::Limits{});
    if (!extent_pool) {
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::Internal,
                        "no pool for the writer's extent bytes"));
    }

    auto snapshot = Snapshot::open(dir);
    if (!snapshot) return dstd::Unexpected<dstd::Error>(snapshot.error());

    auto listed = snapshot->generations();
    if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
    dstd::Vector<ScopeGeneration> generations = std::move(*listed);
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
        DataFileWriter::create(path, extent_size, options.host, options.label);
    if (!file) return dstd::Unexpected<dstd::Error>(file.error());

    std::unique_ptr<Writer> writer(
        new Writer(std::move(name), std::move(path), extent_size,
                   std::move(generations), std::move(*file),
                   std::move(extent_pool), extent_quota, executor, options));
    writer->committer_.start();
    return writer;
}

Writer::Writer(dstd::String name, dstd::String path, std::uint32_t extent_size,
               dstd::Vector<ScopeGeneration> generations, DataFileWriter file,
               memory::PoolHandle extent_pool, std::uint64_t extent_quota,
               Executor& executor, const WriterOptions& options)
    : file_(std::move(file)),
      committer_(file_, executor, options.commit_bytes,
                 options.commit_interval),
      extent_pool_(std::move(extent_pool)),
      extent_bytes_(extent_quota, *extent_pool_.get()),
      extents_(file_, executor.io_backend(), executor, extent_size,
               &Writer::wrote_extent, this, &discard_batch,
               &Writer::extents_idle),
      name_(std::move(name)),
      path_(std::move(path)),
      extent_size_(extent_size),
      generations_(std::move(generations)) {}

Writer::~Writer() {
    if (!closed_.load(std::memory_order_acquire)) {
        extents_.seal_all();
        extents_.wait_idle();
    }
}

DataFileWriter& WriterTestHook::file(Writer& writer) { return writer.file_; }

void WriterTestHook::set_space_sealed(SpaceSealedFn fn) {
    space_sealed_hook().store(fn, std::memory_order_release);
}

WriterExtentsCounters WriterTestHook::extents(const Writer& writer) {
    return writer.extent_bytes_.counters();
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
        entry->params_hash = 0;
        entry->sources.clear();
        entry->announced = false;
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
                    const dstd::Vector<SourceStat>* sources) {
    Batch batch;
    batch.writer_ = this;
    batch.namespace_id_ = namespace_id.value;
    batch.scope_ = scope;
    batch.params_hash_ = params_hash;
    if (sources != nullptr) {
        batch.sources_ = *sources;
        if (batch.sources_.size() != scope.paths.size()) {
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

Batch Writer::building_batch(NamespaceId namespace_id, const ScopeKey& scope) {
    Batch batch = this->batch(namespace_id, scope, 0, nullptr);
    batch.building_ = true;
    return batch;
}

dstd::Expected<void, dstd::Error> Writer::apply(
    dstd::Span<const CopyChange> changes) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    for (const CopyChange& change : changes) {
        if (change.scope == nullptr ||
            (change.kind == ChangeKind::Ready && change.sources != nullptr &&
             change.sources->size() != change.scope->paths.size())) {
            return invalid_error();
        }
    }
    dstd::Vector<QueuedChange> queued;
    queued.reserve(changes.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        dstd::Vector<NamespaceEntry*> namespaces;
        dstd::Vector<ScopeEntry*> scopes;
        dstd::Vector<CopyEntry*> copies;
        namespaces.reserve(changes.size());
        scopes.reserve(changes.size());
        copies.reserve(changes.size());
        dstd::HashSet<std::uint64_t> seen;
        for (const CopyChange& change : changes) {
            NamespaceEntry* namespace_entry =
                find_namespace(change.namespace_id.value);
            if (namespace_entry == nullptr) return invalid_error();
            ScopeEntry* scope_entry = find_scope(*change.scope);
            CopyEntry* copy_entry = nullptr;
            if (change.kind == ChangeKind::Ready) {
                if (scope_entry != nullptr) {
                    copy_entry =
                        find_copy(change.namespace_id.value, scope_entry->id);
                }
                if (copy_entry == nullptr || !copy_entry->building) {
                    return invalid_error();
                }
            } else if (scope_entry == nullptr) {
                scope_entry = add_scope(*change.scope);
            }
            if (!seen.insert(
                         copy_key(change.namespace_id.value, scope_entry->id))
                     .second) {
                return invalid_error();
            }
            namespaces.push_back(namespace_entry);
            scopes.push_back(scope_entry);
            copies.push_back(copy_entry);
        }
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const CopyChange& change = changes[i];
            PendingBatch* batch = nullptr;
            switch (change.kind) {
                case ChangeKind::Ready:
                    batch = publish_locked(*namespaces[i], *scopes[i],
                                           *copies[i], *change.scope, change);
                    break;
                case ChangeKind::Failed:
                    batch = fail_locked(*namespaces[i], *scopes[i],
                                        *change.scope, change.params_hash);
                    break;
                case ChangeKind::Dropped:
                    batch =
                        drop_locked(*namespaces[i], *scopes[i], *change.scope);
                    break;
            }
            queued.push_back(
                QueuedChange{batch, change.kind == ChangeKind::Dropped});
        }
    }
    committer_.on_changes(
        dstd::Span<const QueuedChange>(queued.data(), queued.size()));
    return {};
}

PendingBatch* Writer::publish_locked(const NamespaceEntry& namespace_entry,
                                     const ScopeEntry& scope_entry,
                                     CopyEntry& copy_entry,
                                     const ScopeKey& scope,
                                     const CopyChange& change) {
    copy_entry.building = false;
    copy_entry.params_hash = change.params_hash;
    copy_entry.sources.clear();
    if (change.sources != nullptr) copy_entry.sources = *change.sources;

    auto* batch = new PendingBatch;
    batch->namespace_id = namespace_entry.id.value;
    batch->namespace_item =
        namespace_item(namespace_entry.spec, namespace_entry.id);
    batch->scope_id = scope_entry.id;
    batch->scope_item = scope_item(scope, scope_entry.id);
    batch->copy_item =
        copy_item(copy_entry.copy_id, namespace_entry.id.value, scope_entry.id,
                  copy_entry.params_hash, copy_entry.generation,
                  CopyState::Ready, copy_entry.sources);
    batch->publish = true;
    batch->durable = namespace_entry.spec.durable;
    return batch;
}

PendingBatch* Writer::fail_locked(const NamespaceEntry& namespace_entry,
                                  const ScopeEntry& scope_entry,
                                  const ScopeKey& scope,
                                  std::uint64_t params_hash) {
    CopyEntry* existing = find_copy(namespace_entry.id.value, scope_entry.id);
    const std::uint64_t highest =
        existing != nullptr ? existing->highest_generation
                            : generation_of(namespace_entry.spec, scope);
    const std::uint64_t generation = highest + 1U;
    if (existing == nullptr) {
        existing =
            add_copy(namespace_entry.id, scope_entry.id, generation, false);
    } else {
        existing->live = false;
        existing->building = false;
        existing->generation = generation;
        existing->highest_generation = generation;
    }

    auto* pending = new PendingBatch;
    pending->namespace_id = namespace_entry.id.value;
    pending->namespace_item =
        namespace_item(namespace_entry.spec, namespace_entry.id);
    pending->scope_id = scope_entry.id;
    pending->scope_item = scope_item(scope, scope_entry.id);
    // A failed copy is a new copy id, so it is announced on its own.
    pending->copy_item =
        copy_item(next_copy_id_++, namespace_entry.id.value, scope_entry.id,
                  params_hash, generation, CopyState::Failed, {});
    pending->durable = namespace_entry.spec.durable;
    return pending;
}

PendingBatch* Writer::drop_locked(const NamespaceEntry& namespace_entry,
                                  const ScopeEntry& scope_entry,
                                  const ScopeKey& scope) {
    CopyEntry* existing = find_copy(namespace_entry.id.value, scope_entry.id);
    const std::uint64_t highest =
        existing != nullptr ? existing->highest_generation
                            : generation_of(namespace_entry.spec, scope);
    const std::uint64_t generation = highest + 1U;
    if (existing == nullptr) {
        existing =
            add_copy(namespace_entry.id, scope_entry.id, generation, false);
    } else {
        existing->live = false;
        existing->building = false;
        existing->generation = generation;
        existing->highest_generation = generation;
    }

    auto* pending = new PendingBatch;
    pending->namespace_id = namespace_entry.id.value;
    pending->namespace_item =
        namespace_item(namespace_entry.spec, namespace_entry.id);
    pending->scope_id = scope_entry.id;
    pending->scope_item = scope_item(scope, scope_entry.id);
    // A drop announces no copy: its generation is all the committer needs.
    pending->copy_item.local_id = 0;
    pending->copy_item.generation = generation;
    pending->durable = namespace_entry.spec.durable;
    return pending;
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

void Writer::extents_idle(void* context) {
    static_cast<Writer*>(context)->extent_bytes_.nudge();
}

void Writer::wrote_extent(void* context, WrittenExtent&& extent) {
    auto* writer = static_cast<Writer*>(context);
    std::uint64_t bytes = 0;
    for (const ExtentSegment& segment : extent.segments)
        bytes += segment.length;
    writer->extent_bytes_.release(bytes);
    writer->committer_.on_extent(std::move(extent));
}

dstd::Expected<Writer::SegmentBytes, dstd::Error> Writer::build_segment(
    const Batch& batch) {
    SegmentBytes segment;
    if (batch.entries_.empty()) return segment;

    const auto key_of = [&batch](std::size_t index) {
        return batch.entries_[index].key;
    };
    const auto value_of = [&batch](std::size_t index) {
        return batch.entries_[index].value;
    };

    dstd::Vector<std::size_t> order(batch.entries_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    const auto before = [&key_of](std::size_t a, std::size_t b) {
        const int cmp = compare_bytes(key_of(a), key_of(b));
        if (cmp != 0) return cmp < 0;
        return a < b;
    };
    if (!std::is_sorted(order.begin(), order.end(), before)) {
        std::sort(order.begin(), order.end(), before);
    }

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
        group.key_length = batch.entries_[order[i]].key.size();
        if (batch.fold_ == FoldKind::Concat) {
            std::uint64_t total = 0;
            for (std::size_t at = i; at < end; ++at) {
                total += batch.entries_[order[at]].value.size();
            }
            group.value_length = total;
        } else if (batch.fold_ == FoldKind::None ||
                   batch.fold_ == FoldKind::Last) {
            group.value_length = batch.entries_[order[end - 1U]].value.size();
        } else {
            group.value_length = 8;
        }
        groups.push_back(group);
        i = end;
    }

    SegmentSizer sizer;
    for (const KeyGroup& group : groups) {
        sizer.add(group.key_length,
                  encoded_entry_size(group.key_length, group.value_length));
    }
    if (sizer.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return invalid_error();
    }

    // Charged before the seal places big values and before any id is
    // assigned, so a Busy commit changes no writer state.
    const std::uint64_t charged = static_cast<std::uint64_t>(sizer.size());
    if (extent_bytes_.take(charged) == WriterExtents::Take::Busy) {
        return dstd::Unexpected<dstd::Error>(dstd::Error(WriterErrc::Busy));
    }

    const FoldFn fold = fold_fn(batch.fold_);
    SegmentBuilder& builder = segment.builder;
    for (std::size_t i = 0; i < order.size(); ++i) {
        builder.add_ref(key_of(order[i]), value_of(order[i]));
    }
    BigPlacement placement{this, &segment.big_runs};
    auto sealed = builder.plan(fold, &Writer::place_big, &placement);
    if (auto error = latched()) {
        extent_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(*error);
    }
    if (!sealed) {
        extent_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(sealed.error());
    }
    if (*sealed != charged) {
        extent_bytes_.release(charged);
        return dstd::Unexpected<dstd::Error>(
            dstd::Error(dstd::Condition::Internal,
                        "store segment size differs from its plan"));
    }

    segment.length = *sealed;
    segment.entry_count = builder.entry_count();
    segment.first_key = copy_bytes(builder.first_key());
    segment.last_key = copy_bytes(builder.last_key());
    return segment;
}

dstd::Expected<void, dstd::Error> Writer::prepare(const Batch& batch,
                                                  PendingBatch& pending) {
    NamespaceEntry* namespace_entry = find_namespace(batch.namespace_id_);
    if (namespace_entry == nullptr) return invalid_error();
    ScopeEntry* scope_entry = find_scope(batch.scope_);
    if (scope_entry == nullptr) scope_entry = add_scope(batch.scope_);

    CopyEntry* copy_entry = find_or_add_copy(namespace_entry->spec,
                                             NamespaceId{batch.namespace_id_},
                                             batch.scope_, scope_entry->id);
    if (batch.building_) {
        if (!copy_entry->building) {
            // The copy readers see stays theirs until this one is published.
            if (copy_entry->announced) {
                const std::uint64_t generation =
                    copy_entry->highest_generation + 1U;
                copy_entry->copy_id = next_copy_id_++;
                copy_entry->generation = generation;
                copy_entry->highest_generation = generation;
            }
            copy_entry->params_hash = 0;
            copy_entry->sources.clear();
            copy_entry->announced = true;
            copy_entry->building = true;
        }
    } else if (copy_entry->building) {
        return invalid_error();
    } else if (copy_entry->announced) {
        if (copy_entry->params_hash != batch.params_hash_ ||
            !same_sources(copy_entry->sources, batch.sources_)) {
            return invalid_error();
        }
    } else {
        copy_entry->params_hash = batch.params_hash_;
        copy_entry->sources = batch.sources_;
        copy_entry->announced = true;
    }

    pending.namespace_id = namespace_entry->id.value;
    pending.namespace_item =
        namespace_item(namespace_entry->spec, namespace_entry->id);
    pending.scope_id = scope_entry->id;
    pending.scope_item = scope_item(batch.scope_, scope_entry->id);
    pending.copy_item = copy_item(
        copy_entry->copy_id, namespace_entry->id.value, scope_entry->id,
        copy_entry->params_hash, copy_entry->generation,
        copy_entry->building ? CopyState::Building : CopyState::Ready,
        copy_entry->sources);
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

    auto segment = build_segment(batch);
    if (!segment) {
        if (segment.error().is_code(WriterErrc::Busy)) return busy_error();
        return dstd::Unexpected<dstd::Error>(segment.error());
    }
    const std::uint64_t segment_bytes =
        static_cast<std::uint64_t>(segment->length);
    const bool charged = segment_bytes != 0;

    auto* pending = new PendingBatch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            if (charged) extent_bytes_.release(segment_bytes);
            delete pending;
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        auto prepared = prepare(batch, *pending);
        if (!prepared) {
            if (charged) extent_bytes_.release(segment_bytes);
            delete pending;
            return prepared;
        }
    }

    if (!charged) {
        delete pending;
        return {};
    }

    pending->extents = &extent_bytes_;
    pending->charged_bytes = segment_bytes;
    pending->big_runs = std::move(segment->big_runs);

    ExtentSegment info;
    info.copy_id = pending->copy_item.local_id;
    info.entry_count = segment->entry_count;
    info.ordinal = next_ordinal_.fetch_add(1, std::memory_order_relaxed);
    info.first_key = std::move(segment->first_key);
    info.last_key = std::move(segment->last_key);

    auto appended = extents_.append_with(
        pending->namespace_id, segment->length, std::move(info), pending,
        static_cast<PageCodec>(pending->namespace_item.codec),
        [](void* builder, std::uint8_t* out) {
            static_cast<const SegmentBuilder*>(builder)->write_to(out);
        },
        &segment->builder);
    if (!appended) {
        extent_bytes_.release(segment_bytes);
        // The extent writer reports this error from every later append, so a
        // wait for bytes it will never release has to end here.
        extent_bytes_.latch();
        delete pending;
        return appended;
    }
    // A waiter needs a release. A write in flight brings one. With none in
    // flight, this append's extent must be sealed or the waiter waits forever.
    // Sealing only then keeps extents full under steady back pressure.
    if (extent_bytes_.waiting() && !extents_.writing()) {
        auto sealed = extents_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            extent_bytes_.latch();
        }
    }
    return {};
}

dstd::Unexpected<dstd::Error> Writer::busy_error() {
    // With no write in flight, the held bytes sit in open extents nobody
    // writes: seal them, so threads that all hold a partial batch cannot
    // deadlock. With a write in flight its completion frees bytes, and a seal
    // would only write a partial extent.
    if (!extents_.writing()) {
        auto sealed = extents_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            extent_bytes_.latch();
            return dstd::Unexpected<dstd::Error>(sealed.error());
        }
    }
    return dstd::Unexpected<dstd::Error>(dstd::Error(WriterErrc::Busy));
}

coro::CoroTask<void> Writer::space() {
    dstd::Optional<dstd::Error> latched_error = latched();
    if (latched_error) {
        extent_bytes_.latch();
        co_return;
    }
    if (closed_.load(std::memory_order_acquire) || committer_.failed() ||
        extent_bytes_.latched()) {
        co_return;
    }
    // Announced before the check, so the release this waits for comes either
    // from a write in flight or from the seal here.
    WriterExtents::WaitAnnounce announce(extent_bytes_);
    if (!extents_.writing()) {
        auto sealed = extents_.seal_all();
        if (!sealed) {
            committer_.latch(sealed.error());
            extent_bytes_.latch();
            co_return;
        }
    }
    if (auto* hook = space_sealed_hook().load(std::memory_order_acquire)) {
        hook(*this);
    }
    auto wait = extent_bytes_.wait_release(announce.epoch());
    co_await wait;
    co_return;
}

dstd::Expected<void, dstd::Error> Writer::fail(NamespaceId namespace_id,
                                               const ScopeKey& scope,
                                               std::uint64_t params_hash) {
    const CopyChange change{namespace_id, &scope, params_hash, nullptr,
                            ChangeKind::Failed};
    return apply(dstd::Span<const CopyChange>(&change, 1));
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
    PendingBatch* pending = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latched_error_) {
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        NamespaceEntry* namespace_entry = find_namespace(namespace_id.value);
        if (namespace_entry == nullptr) return invalid_error();
        ScopeEntry* scope_entry = find_scope(scope);
        if (scope_entry == nullptr) scope_entry = add_scope(scope);
        pending = drop_locked(*namespace_entry, *scope_entry, scope);
    }
    committer_.on_drop(pending, copy_bytes(begin), copy_bytes(end));
    return {};
}

dstd::Expected<void, dstd::Error> Writer::erase(NamespaceId namespace_id,
                                                const ScopeKey& scope,
                                                ByteView begin, ByteView end) {
    if (closed_.load(std::memory_order_acquire)) return invalid_error();
    if (committer_.failed()) {
        return dstd::Unexpected<dstd::Error>(committer_.error());
    }
    auto* pending = new PendingBatch;
    EraseItem item;
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
        CopyEntry* copy_entry =
            scope_entry == nullptr
                ? nullptr
                : find_copy(namespace_id.value, scope_entry->id);
        if (copy_entry == nullptr || !copy_entry->live) {
            delete pending;
            return {};
        }
        item.copy_id = copy_entry->copy_id;
        item.ordinal = next_ordinal_.fetch_add(1, std::memory_order_relaxed);
        item.begin_key = copy_bytes(begin);
        item.end_key = copy_bytes(end);

        pending->namespace_id = namespace_entry->id.value;
        pending->namespace_item =
            namespace_item(namespace_entry->spec, namespace_entry->id);
        pending->scope_id = scope_entry->id;
        pending->scope_item = scope_item(scope, scope_entry->id);
        pending->copy_item = copy_item(
            copy_entry->copy_id, namespace_entry->id.value, scope_entry->id,
            copy_entry->params_hash, copy_entry->generation,
            copy_entry->building ? CopyState::Building : CopyState::Ready,
            copy_entry->sources);
        pending->durable = namespace_entry->spec.durable;
    }
    committer_.on_erase(pending, std::move(item));
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
    auto sealed = extents_.seal_all();
    if (!sealed) {
        committer_.latch(sealed.error());
        extent_bytes_.latch();
        co_return sealed;
    }
    auto idle = co_await extents_.wait_idle_async();
    if (!idle) {
        committer_.latch(idle.error());
        extent_bytes_.latch();
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
    auto sealed = extents_.seal_all();
    if (!sealed) {
        committer_.latch(sealed.error());
        extent_bytes_.latch();
        first = sealed.error();
    }
    auto idle = co_await extents_.wait_idle_async();
    if (!idle) {
        committer_.latch(idle.error());
        extent_bytes_.latch();
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

}  // namespace dftracer::utils::store::engine
