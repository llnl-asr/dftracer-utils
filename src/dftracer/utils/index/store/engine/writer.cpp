#include <dftracer/utils/index/store/engine/writer.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    if (!latched_error_) latched_error_ = error;
}

dstd::Expected<Writer, dstd::Error> Writer::create(
    dstd::StringView dir, const WriterOptions& options) {
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
    auto file = DataFileWriter::create(path, options.page_size, options.host,
                                       options.label);
    if (!file) return dstd::Unexpected<dstd::Error>(file.error());
    return Writer(std::move(name), std::move(path), options.page_size,
                  std::move(generations), std::move(*file));
}

Writer::Writer(dstd::String name, dstd::String path, std::uint32_t page_size,
               dstd::Vector<ScopeGeneration> generations, DataFileWriter file)
    : file_(std::move(file)),
      name_(std::move(name)),
      path_(std::move(path)),
      page_size_(page_size),
      generations_(std::move(generations)) {}

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
    const auto at =
        std::lower_bound(scopes_.begin(), scopes_.end(), scope,
                         [](const ScopeEntry& entry, const ScopeKey& wanted) {
                             return compare_scopes(entry.key, wanted) < 0;
                         });
    if (at == scopes_.end() || compare_scopes(at->key, scope) != 0) {
        return nullptr;
    }
    return &*at;
}

Writer::ScopeEntry* Writer::add_scope(const ScopeKey& scope) {
    ScopeEntry entry;
    entry.id = next_scope_id_++;
    entry.key = scope;
    const auto at = std::lower_bound(
        scopes_.begin(), scopes_.end(), scope,
        [](const ScopeEntry& existing, const ScopeKey& wanted) {
            return compare_scopes(existing.key, wanted) < 0;
        });
    return &*scopes_.insert(at, std::move(entry));
}

Writer::CopyEntry* Writer::find_copy(std::uint32_t namespace_id,
                                     const ScopeKey& scope) {
    const auto at = std::lower_bound(
        copies_.begin(), copies_.end(), scope,
        [namespace_id](const CopyEntry& entry, const ScopeKey& wanted) {
            if (entry.namespace_id != namespace_id) {
                return entry.namespace_id < namespace_id;
            }
            return compare_scopes(entry.scope, wanted) < 0;
        });
    if (at == copies_.end() || at->namespace_id != namespace_id ||
        compare_scopes(at->scope, scope) != 0) {
        return nullptr;
    }
    return &*at;
}

Writer::CopyEntry* Writer::add_copy(NamespaceId id, const ScopeKey& scope,
                                    std::uint32_t scope_id,
                                    std::uint64_t generation, bool live) {
    CopyEntry entry;
    entry.namespace_id = id.value;
    entry.scope = scope;
    entry.scope_id = scope_id;
    entry.copy_id = next_copy_id_++;
    entry.generation = generation;
    entry.highest_generation = generation;
    entry.live = live;
    const auto at = std::lower_bound(
        copies_.begin(), copies_.end(), scope,
        [id](const CopyEntry& existing, const ScopeKey& wanted) {
            if (existing.namespace_id != id.value) {
                return existing.namespace_id < id.value;
            }
            return compare_scopes(existing.scope, wanted) < 0;
        });
    return &*copies_.insert(at, std::move(entry));
}

Writer::CopyEntry* Writer::find_or_add_copy(const NamespaceSpec& spec,
                                            NamespaceId id,
                                            const ScopeKey& scope,
                                            std::uint32_t scope_id) {
    CopyEntry* entry = find_copy(id.value, scope);
    if (entry != nullptr && entry->live) return entry;
    if (entry != nullptr) {
        const std::uint64_t generation = entry->highest_generation + 1U;
        entry->scope_id = scope_id;
        entry->copy_id = next_copy_id_++;
        entry->generation = generation;
        entry->highest_generation = generation;
        entry->live = true;
        entry->announced = false;
        return entry;
    }
    return add_copy(id, scope, scope_id, generation_of(spec, scope) + 1U, true);
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
    if (closed_) return invalid_error();
    if (latched_error_) return dstd::Unexpected<dstd::Error>(*latched_error_);
    if (spec.name.empty()) return invalid_error();

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

    const NamespaceEntry* entry = find_namespace(namespace_id.value);
    if (entry == nullptr) {
        batch.record_error(dstd::Condition::InvalidArgument);
        return batch;
    }
    batch.fold_ = entry->spec.fold;
    if (scope_paths != nullptr) {
        batch.signature_ = *scope_paths;
        if (batch.signature_.size() != scope.paths.size()) {
            batch.record_error(dstd::Condition::InvalidArgument);
        }
    }
    return batch;
}

void Writer::add_namespace_item(const NamespaceSpec& spec, NamespaceId id,
                                dstd::Vector<RecordItem>& items) {
    NamespaceItem item;
    item.local_id = id.value;
    item.name = spec.name;
    item.format_version = spec.format_version;
    item.fold_kind = static_cast<std::uint8_t>(spec.fold);
    item.codec = 0;
    item.durable = spec.durable ? 1 : 0;
    items.push_back(std::move(item));
}

void Writer::add_scope_item(const ScopeEntry& entry,
                            const dstd::Vector<ScopePath>* signature,
                            dstd::Vector<RecordItem>& items) {
    ScopeItem item;
    item.local_id = entry.id;
    item.kind = entry.key.kind;
    item.paths.reserve(entry.key.paths.size());
    for (std::size_t i = 0; i < entry.key.paths.size(); ++i) {
        ScopePath path;
        if (signature != nullptr) path = (*signature)[i];
        path.logical_path = entry.key.paths[i];
        item.paths.push_back(std::move(path));
    }
    items.push_back(std::move(item));
}

BigRef Writer::place_big(void* context, ByteView value) {
    auto* writer = static_cast<Writer*>(context);
    auto placed = writer->file_.append_big(value);
    if (!placed) {
        writer->latch(placed.error());
        return BigRef{};
    }
    return BigRef{placed->offset, placed->length, placed->crc};
}

dstd::Expected<dstd::Vector<Writer::SectionBytes>, dstd::Error>
Writer::build_sections(const Batch& batch) {
    dstd::Vector<SectionBytes> out;
    if (batch.entries_.empty()) return out;

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

    dstd::Vector<std::pair<std::size_t, std::size_t>> runs;
    std::size_t run_begin = 0;
    SectionSizer sizer;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::size_t entry_bytes =
            encoded_entry_size(groups[i].key_length, groups[i].value_length);
        SectionSizer candidate = sizer;
        candidate.add(groups[i].key_length, entry_bytes);
        if (candidate.size() > page_size_) {
            if (i == run_begin) return invalid_error();
            runs.push_back({run_begin, i});
            run_begin = i;
            candidate = SectionSizer{};
            candidate.add(groups[i].key_length, entry_bytes);
            if (candidate.size() > page_size_) return invalid_error();
        }
        sizer = candidate;
    }
    runs.push_back({run_begin, groups.size()});

    const FoldFn fold = fold_fn(batch.fold_);
    for (const auto& run : runs) {
        SectionBuilder builder;
        const std::size_t begin = groups[run.first].begin;
        const std::size_t end = groups[run.second - 1U].end;
        for (std::size_t i = begin; i < end; ++i) {
            builder.add(key_of(order[i]), value_of(order[i]));
        }
        auto sealed = builder.seal(fold, &Writer::place_big, this);
        if (latched_error_)
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        if (!sealed) {
            return dstd::Unexpected<dstd::Error>(sealed.error());
        }
        if (sealed->size() > page_size_) return invalid_error();
        SectionBytes section;
        section.bytes = std::move(*sealed);
        section.entry_count = builder.entry_count();
        section.first_key = copy_bytes(builder.first_key());
        section.last_key = copy_bytes(builder.last_key());
        out.push_back(std::move(section));
    }
    return out;
}

dstd::Expected<void, dstd::Error> Writer::write_pages(
    std::uint32_t copy_id, const dstd::Vector<SectionBytes>& sections,
    dstd::Vector<RecordItem>& items) {
    dstd::Vector<std::uint8_t> page;
    dstd::Vector<PageSection> page_sections;
    std::size_t used = 0;

    const auto flush = [&]() -> dstd::Expected<void, dstd::Error> {
        if (page.empty()) return {};
        auto reference = file_.append_page(ByteView{page.data(), page.size()});
        if (!reference) {
            latch(reference.error());
            return dstd::Unexpected<dstd::Error>(reference.error());
        }
        PageRefItem item;
        item.offset = reference->offset;
        item.length = reference->length;
        item.crc = reference->crc;
        item.sequence = next_sequence_;
        item.sections = std::move(page_sections);
        items.push_back(std::move(item));
        page.clear();
        page_sections.clear();
        used = 0;
        return {};
    };

    for (const SectionBytes& section : sections) {
        if (used != 0 && used + section.bytes.size() > page_size_) {
            auto flushed = flush();
            if (!flushed) return flushed;
        }
        PageSection page_section;
        page_section.copy_id = copy_id;
        page_section.offset_in_page = static_cast<std::uint32_t>(used);
        page_section.length = static_cast<std::uint32_t>(section.bytes.size());
        page_section.entry_count = section.entry_count;
        page_section.first_key = section.first_key;
        page_section.last_key = section.last_key;
        page.insert(page.end(), section.bytes.begin(), section.bytes.end());
        used += section.bytes.size();
        page_sections.push_back(std::move(page_section));
    }
    return flush();
}

dstd::Expected<void, dstd::Error> Writer::write_commit(
    dstd::Vector<RecordItem>& items) {
    for (RecordItem& item : items) file_.add(std::move(item));
    auto committed = file_.commit();
    if (!committed) {
        latch(committed.error());
        return dstd::Unexpected<dstd::Error>(committed.error());
    }
    return {};
}

dstd::Expected<void, dstd::Error> Writer::commit(Batch&& batch) {
    if (closed_) return invalid_error();
    if (latched_error_) return dstd::Unexpected<dstd::Error>(*latched_error_);
    if (batch.writer_ != this) return invalid_error();
    if (batch.error_) return dstd::Unexpected<dstd::Error>(*batch.error_);

    NamespaceEntry* namespace_entry = find_namespace(batch.namespace_id_.value);
    if (namespace_entry == nullptr) return invalid_error();
    ScopeEntry* scope_entry = find_scope(batch.scope_);
    if (scope_entry == nullptr) scope_entry = add_scope(batch.scope_);

    CopyEntry* copy_entry =
        find_or_add_copy(namespace_entry->spec, batch.namespace_id_,
                         batch.scope_, scope_entry->id);
    const std::uint32_t copy_id = copy_entry->copy_id;
    const std::uint64_t generation = copy_entry->generation;
    const bool new_copy = !copy_entry->announced;

    auto sections = build_sections(batch);
    if (!sections) return dstd::Unexpected<dstd::Error>(sections.error());

    dstd::Vector<RecordItem> items;
    if (!namespace_entry->announced) {
        add_namespace_item(namespace_entry->spec, namespace_entry->id, items);
    }
    if (!scope_entry->announced) {
        add_scope_item(*scope_entry,
                       batch.signature_.empty() ? nullptr : &batch.signature_,
                       items);
    }
    if (new_copy) {
        CopyItem item;
        item.local_id = copy_id;
        item.namespace_id = namespace_entry->id.value;
        item.scope_id = scope_entry->id;
        item.params_hash = batch.params_hash_;
        item.generation = generation;
        item.state = CopyState::Ready;
        items.push_back(std::move(item));
    }

    auto written = write_pages(copy_id, *sections, items);
    if (!written) return written;
    if (items.empty()) return {};

    auto committed = write_commit(items);
    if (!committed) return committed;

    namespace_entry->announced = true;
    scope_entry->announced = true;
    copy_entry->announced = true;
    copy_entry->live = true;
    copy_entry->highest_generation =
        std::max(copy_entry->highest_generation, generation);
    ++next_sequence_;
    return {};
}

dstd::Expected<void, dstd::Error> Writer::fail(NamespaceId namespace_id,
                                               const ScopeKey& scope,
                                               std::uint64_t params_hash) {
    if (closed_) return invalid_error();
    if (latched_error_) return dstd::Unexpected<dstd::Error>(*latched_error_);

    NamespaceEntry* namespace_entry = find_namespace(namespace_id.value);
    if (namespace_entry == nullptr) return invalid_error();
    ScopeEntry* scope_entry = find_scope(scope);
    if (scope_entry == nullptr) scope_entry = add_scope(scope);

    CopyEntry* existing = find_copy(namespace_id.value, scope);
    const std::uint64_t highest =
        existing != nullptr ? existing->highest_generation
                            : generation_of(namespace_entry->spec, scope);
    const std::uint64_t generation = highest + 1U;
    const std::uint32_t copy_id = next_copy_id_++;

    dstd::Vector<RecordItem> items;
    if (!namespace_entry->announced) {
        add_namespace_item(namespace_entry->spec, namespace_entry->id, items);
    }
    if (!scope_entry->announced) add_scope_item(*scope_entry, nullptr, items);
    CopyItem item;
    item.local_id = copy_id;
    item.namespace_id = namespace_entry->id.value;
    item.scope_id = scope_entry->id;
    item.params_hash = params_hash;
    item.generation = generation;
    item.state = CopyState::Failed;
    items.push_back(std::move(item));

    auto committed = write_commit(items);
    if (!committed) return committed;

    namespace_entry->announced = true;
    scope_entry->announced = true;
    if (existing == nullptr) {
        existing =
            add_copy(namespace_id, scope, scope_entry->id, generation, false);
    }
    existing->live = false;
    existing->generation = generation;
    existing->highest_generation = generation;
    ++next_sequence_;
    return {};
}

dstd::Expected<void, dstd::Error> Writer::drop(NamespaceId namespace_id,
                                               const ScopeKey& scope) {
    return drop(namespace_id, scope, ByteView{}, ByteView{});
}

dstd::Expected<void, dstd::Error> Writer::drop(NamespaceId namespace_id,
                                               const ScopeKey& scope,
                                               ByteView begin, ByteView end) {
    if (closed_) return invalid_error();
    if (latched_error_) return dstd::Unexpected<dstd::Error>(*latched_error_);

    NamespaceEntry* namespace_entry = find_namespace(namespace_id.value);
    if (namespace_entry == nullptr) return invalid_error();
    ScopeEntry* scope_entry = find_scope(scope);
    if (scope_entry == nullptr) scope_entry = add_scope(scope);

    CopyEntry* existing = find_copy(namespace_id.value, scope);
    const std::uint64_t highest =
        existing != nullptr ? existing->highest_generation
                            : generation_of(namespace_entry->spec, scope);
    const std::uint64_t generation = highest + 1U;

    dstd::Vector<RecordItem> items;
    if (!namespace_entry->announced) {
        add_namespace_item(namespace_entry->spec, namespace_entry->id, items);
    }
    if (!scope_entry->announced) add_scope_item(*scope_entry, nullptr, items);
    DropItem item;
    item.namespace_id = namespace_entry->id.value;
    item.scope_id = scope_entry->id;
    item.generation = generation;
    item.begin_key = copy_bytes(begin);
    item.end_key = copy_bytes(end);
    items.push_back(std::move(item));

    auto committed = write_commit(items);
    if (!committed) return committed;

    namespace_entry->announced = true;
    scope_entry->announced = true;
    if (existing == nullptr) {
        existing =
            add_copy(namespace_id, scope, scope_entry->id, generation, false);
    }
    existing->live = false;
    existing->generation = generation;
    existing->highest_generation = generation;
    ++next_sequence_;
    return {};
}

dstd::Expected<void, dstd::Error> Writer::close() {
    if (closed_) {
        if (latched_error_) {
            return dstd::Unexpected<dstd::Error>(*latched_error_);
        }
        return {};
    }
    auto closed = file_.close();
    closed_ = true;
    if (!closed) {
        latch(closed.error());
        return dstd::Unexpected<dstd::Error>(closed.error());
    }
    return {};
}

}  // namespace dftracer::utils::index::store::engine
