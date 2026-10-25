#include <dftracer/utils/index/store/engine/snapshot.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store::engine {
namespace {

using dftracer::utils::ByteView;

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

ScopeKey scope_key_of(const ScopeItem& item) {
    ScopeKey key;
    key.kind = item.kind;
    key.paths.reserve(item.paths.size());
    for (const ScopePath& path : item.paths)
        key.paths.push_back(path.logical_path);
    return key;
}

bool holds(const dstd::Vector<std::uint64_t>& values, std::uint64_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool holds(const dstd::Vector<std::uint32_t>& values, std::uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

template <class T>
const T* find_by_id(const std::vector<const T*>& items, std::uint32_t id) {
    for (const T* item : items) {
        if (item->local_id == id) return item;
    }
    return nullptr;
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

struct LocalSection {
    std::uint32_t copy_id = 0;
    std::uint64_t sequence = 0;
    std::uint64_t page_offset = 0;
    std::uint32_t offset_in_page = 0;
    SectionView view;
};

struct CopySeed {
    dstd::String name;
    std::uint32_t format_version = 0;
    ScopeKey scope;
    std::size_t file_index = 0;
    std::uint64_t generation = 0;
    std::uint64_t params_hash = 0;
    FoldKind fold = FoldKind::None;
    CopyState state = CopyState::Ready;
    bool bad = false;
    dstd::Vector<detail::SectionRef> sections;
};

struct DropSeed {
    dstd::String name;
    std::uint32_t format_version = 0;
    ScopeKey scope;
    std::uint64_t generation = 0;
    dstd::Vector<std::uint8_t> begin;
    dstd::Vector<std::uint8_t> end;
};

int compare_key(const dstd::String& name_a, std::uint32_t version_a,
                const ScopeKey& scope_a, const dstd::String& name_b,
                std::uint32_t version_b, const ScopeKey& scope_b) {
    const int by_name = name_a.compare(name_b);
    if (by_name != 0) return by_name < 0 ? -1 : 1;
    if (version_a != version_b) return version_a < version_b ? -1 : 1;
    return compare_scopes(scope_a, scope_b);
}

int compare_seed_key(const CopySeed& a, const DropSeed& b) {
    return compare_key(a.name, a.format_version, a.scope, b.name,
                       b.format_version, b.scope);
}

bool same_key(const CopySeed& seed, const dstd::String& name,
              std::uint32_t version, const ScopeKey& scope) {
    return compare_key(seed.name, seed.format_version, seed.scope, name,
                       version, scope) == 0;
}

bool same_key(const DropSeed& seed, const dstd::String& name,
              std::uint32_t version, const ScopeKey& scope) {
    return compare_key(seed.name, seed.format_version, seed.scope, name,
                       version, scope) == 0;
}

/// Reads every page, opens every section and groups the copies of a scope by
/// name, format version and scope key.
void build_tables(dstd::Vector<detail::FileData>& files,
                  dstd::Vector<detail::ScopeGroup>& groups,
                  dstd::Vector<detail::NamespaceGroup>& namespaces) {
    dstd::Vector<CopySeed> copy_seeds;
    dstd::Vector<DropSeed> drop_seeds;

    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
        detail::FileData& file = files[file_index];
        const FileIndex& index = file.reader.index();

        std::vector<const NamespaceItem*> namespace_items;
        namespace_items.reserve(index.namespaces.size());
        for (const NamespaceItem& item : index.namespaces) {
            namespace_items.push_back(&item);
        }
        std::vector<const ScopeItem*> scope_items;
        scope_items.reserve(index.scopes.size());
        for (const ScopeItem& item : index.scopes) scope_items.push_back(&item);

        dstd::Vector<std::uint64_t> bad_sequences;
        dstd::Vector<LocalSection> sections;
        for (std::size_t i = 0; i < index.page_refs.size(); ++i) {
            const IndexedPageRef& page = index.page_refs[i];
            detail::LoadedPage& loaded = file.pages[i];
            if (loaded.bad) {
                bad_sequences.push_back(page.sequence);
                continue;
            }
            const ByteView page_bytes = bytes_of(loaded.bytes);
            const std::size_t before = sections.size();
            bool page_ok = true;
            for (const PageSection& page_section : page.ref.sections) {
                if (page_section.offset_in_page > page_bytes.size() ||
                    page_section.length >
                        page_bytes.size() - page_section.offset_in_page) {
                    page_ok = false;
                    break;
                }
                auto view = SectionView::open(page_bytes.subspan(
                    page_section.offset_in_page, page_section.length));
                if (!view) {
                    page_ok = false;
                    break;
                }
                sections.push_back(
                    {page_section.copy_id, page.sequence, page.ref.offset,
                     page_section.offset_in_page, std::move(*view)});
            }
            if (!page_ok) {
                sections.erase(
                    sections.begin() + static_cast<std::ptrdiff_t>(before),
                    sections.end());
                bad_sequences.push_back(page.sequence);
            }
        }

        dstd::Vector<std::uint32_t> bad_copies;
        for (const IndexedPageRef& page : index.page_refs) {
            if (!holds(bad_sequences, page.sequence)) continue;
            for (const PageSection& page_section : page.ref.sections) {
                bad_copies.push_back(page_section.copy_id);
            }
        }

        std::sort(sections.begin(), sections.end(),
                  [](const LocalSection& a, const LocalSection& b) {
                      if (a.copy_id != b.copy_id) return a.copy_id < b.copy_id;
                      if (a.sequence != b.sequence) {
                          return a.sequence < b.sequence;
                      }
                      if (a.page_offset != b.page_offset) {
                          return a.page_offset < b.page_offset;
                      }
                      return a.offset_in_page < b.offset_in_page;
                  });

        for (const CopyItem& copy : index.copies) {
            const NamespaceItem* namespace_item =
                find_by_id(namespace_items, copy.namespace_id);
            const ScopeItem* scope_item =
                find_by_id(scope_items, copy.scope_id);
            if (namespace_item == nullptr || scope_item == nullptr) continue;

            CopySeed seed;
            seed.name = namespace_item->name;
            seed.format_version = namespace_item->format_version;
            seed.fold = static_cast<FoldKind>(namespace_item->fold_kind);
            seed.scope = scope_key_of(*scope_item);
            seed.file_index = file_index;
            seed.generation = copy.generation;
            seed.params_hash = copy.params_hash;
            seed.state = copy.state;
            seed.bad = holds(bad_copies, copy.local_id);

            const auto begin = std::lower_bound(
                sections.begin(), sections.end(), copy.local_id,
                [](const LocalSection& section, std::uint32_t id) {
                    return section.copy_id < id;
                });
            for (auto at = begin; at != sections.end(); ++at) {
                if (at->copy_id != copy.local_id) break;
                seed.sections.push_back({std::move(at->view), at->sequence,
                                         at->page_offset, at->offset_in_page});
            }
            copy_seeds.push_back(std::move(seed));
        }

        for (const DropItem& drop : index.drops) {
            const NamespaceItem* namespace_item =
                find_by_id(namespace_items, drop.namespace_id);
            const ScopeItem* scope_item =
                find_by_id(scope_items, drop.scope_id);
            if (namespace_item == nullptr || scope_item == nullptr) continue;

            DropSeed seed;
            seed.name = namespace_item->name;
            seed.format_version = namespace_item->format_version;
            seed.scope = scope_key_of(*scope_item);
            seed.generation = drop.generation;
            seed.begin = drop.begin_key;
            seed.end = drop.end_key;
            drop_seeds.push_back(std::move(seed));
        }
    }

    std::sort(copy_seeds.begin(), copy_seeds.end(),
              [](const CopySeed& a, const CopySeed& b) {
                  return compare_key(a.name, a.format_version, a.scope, b.name,
                                     b.format_version, b.scope) < 0;
              });
    std::sort(drop_seeds.begin(), drop_seeds.end(),
              [](const DropSeed& a, const DropSeed& b) {
                  return compare_key(a.name, a.format_version, a.scope, b.name,
                                     b.format_version, b.scope) < 0;
              });

    groups.clear();
    std::size_t copy_at = 0;
    std::size_t drop_at = 0;
    while (copy_at < copy_seeds.size() || drop_at < drop_seeds.size()) {
        const bool take_copy =
            drop_at >= drop_seeds.size() ||
            (copy_at < copy_seeds.size() &&
             compare_seed_key(copy_seeds[copy_at], drop_seeds[drop_at]) <= 0);
        detail::ScopeGroup group;
        if (take_copy) {
            group.namespace_name = copy_seeds[copy_at].name;
            group.format_version = copy_seeds[copy_at].format_version;
            group.scope = copy_seeds[copy_at].scope;
        } else {
            group.namespace_name = drop_seeds[drop_at].name;
            group.format_version = drop_seeds[drop_at].format_version;
            group.scope = drop_seeds[drop_at].scope;
        }

        dstd::Vector<DropSeed> group_drops;
        while (copy_at < copy_seeds.size() &&
               same_key(copy_seeds[copy_at], group.namespace_name,
                        group.format_version, group.scope)) {
            CopySeed& seed = copy_seeds[copy_at];
            detail::CopyData copy;
            copy.file_index = seed.file_index;
            copy.generation = seed.generation;
            copy.params_hash = seed.params_hash;
            copy.fold = seed.fold;
            copy.state = seed.state;
            copy.bad = seed.bad;
            copy.sections = std::move(seed.sections);
            group.copies.push_back(std::move(copy));
            ++copy_at;
        }
        while (drop_at < drop_seeds.size() &&
               same_key(drop_seeds[drop_at], group.namespace_name,
                        group.format_version, group.scope)) {
            group_drops.push_back(std::move(drop_seeds[drop_at]));
            ++drop_at;
        }

        std::sort(
            group.copies.begin(), group.copies.end(),
            [&files](const detail::CopyData& a, const detail::CopyData& b) {
                if (a.generation != b.generation) {
                    return a.generation < b.generation;
                }
                const detail::FileData& file_a = files[a.file_index];
                const detail::FileData& file_b = files[b.file_index];
                if (file_a.writer_random != file_b.writer_random) {
                    return file_a.writer_random < file_b.writer_random;
                }
                return file_a.name.compare(file_b.name) < 0;
            });

        std::uint64_t highest = 0;
        for (const detail::CopyData& copy : group.copies) {
            highest = std::max(highest, copy.generation);
        }
        for (const DropSeed& drop : group_drops) {
            highest = std::max(highest, drop.generation);
        }
        group.highest_generation = highest;

        if (group.copies.empty()) {
            group.dropped = true;
        } else {
            group.current = group.copies.size() - 1U;
            const detail::CopyData& current = group.copies[group.current];
            for (const DropSeed& drop : group_drops) {
                if (drop.generation <= current.generation) continue;
                if (drop.begin.empty() && drop.end.empty()) {
                    group.dropped = true;
                } else {
                    group.hidden.push_back(
                        {std::move(drop.begin), std::move(drop.end)});
                }
            }
        }

        groups.push_back(std::move(group));
    }

    namespaces.clear();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const detail::ScopeGroup& group = groups[i];
        if (namespaces.empty() ||
            namespaces.back().name != group.namespace_name ||
            namespaces.back().format_version != group.format_version) {
            detail::NamespaceGroup entry;
            entry.name = group.namespace_name;
            entry.format_version = group.format_version;
            namespaces.push_back(std::move(entry));
        }
        namespaces.back().scopes.push_back(i);
    }
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

    for (const std::string& name : names) {
        const std::string path = path_string(dir, name);
        auto reader = DataFileReader::open(path);
        if (!reader) {
            skipped_.push_back({dstring(name), reader.error()});
            continue;
        }
        const std::uint64_t writer_random = reader->index().writer_random;
        detail::FileData file{
            dstring(name), writer_random, std::move(*reader), {}};
        // A page that does not verify is kept and marks its commit bad; a read
        // error makes the whole file unusable, so it is skipped like a file
        // that fails to open.
        dstd::Optional<dstd::Error> read_failure;
        for (const IndexedPageRef& page : file.reader.index().page_refs) {
            const PageRef ref{page.ref.offset, page.ref.length, page.ref.crc};
            auto bytes = file.reader.read_page(ref);
            detail::LoadedPage loaded;
            if (!bytes) {
                if (!bytes.error().is(dstd::Condition::Parse)) {
                    read_failure = bytes.error();
                    break;
                }
                loaded.bad = true;
            } else {
                loaded.bytes = std::move(*bytes);
            }
            file.pages.push_back(std::move(loaded));
        }
        if (read_failure) {
            skipped_.push_back({dstring(name), *read_failure});
            continue;
        }
        files_.push_back(std::move(file));
    }

    build_tables(files_, groups_, namespaces_);
    return {};
}

dstd::Vector<ScopeGeneration> Snapshot::generations() const {
    dstd::Vector<ScopeGeneration> out;
    out.reserve(groups_.size());
    for (const detail::ScopeGroup& group : groups_) {
        out.push_back({group.namespace_name, group.format_version, group.scope,
                       group.highest_generation});
    }
    return out;
}

const detail::NamespaceGroup* Snapshot::find_namespace(
    const NamespaceSpec& spec) const {
    const auto at = std::lower_bound(
        namespaces_.begin(), namespaces_.end(), spec,
        [](const detail::NamespaceGroup& entry, const NamespaceSpec& wanted) {
            const int cmp = entry.name.compare(wanted.name);
            if (cmp != 0) return cmp < 0;
            return entry.format_version < wanted.format_version;
        });
    if (at == namespaces_.end() || at->name != spec.name ||
        at->format_version != spec.format_version) {
        return nullptr;
    }
    return &*at;
}

const detail::ScopeGroup* Snapshot::find_group(const NamespaceSpec& spec,
                                               const ScopeKey& scope) const {
    const detail::NamespaceGroup* namespace_group = find_namespace(spec);
    if (namespace_group == nullptr) return nullptr;
    const auto at = std::lower_bound(
        namespace_group->scopes.begin(), namespace_group->scopes.end(), scope,
        [this](std::size_t index, const ScopeKey& wanted) {
            return compare_scopes(groups_[index].scope, wanted) < 0;
        });
    if (at == namespace_group->scopes.end() ||
        compare_scopes(groups_[*at].scope, scope) != 0) {
        return nullptr;
    }
    return &groups_[*at];
}

ScopeState Snapshot::state(const NamespaceSpec& spec,
                           const ScopeKey& scope) const {
    const detail::ScopeGroup* group = find_group(spec, scope);
    if (group == nullptr || group->dropped || group->copies.empty()) {
        return ScopeState::Absent;
    }
    const detail::CopyData& copy = group->copies[group->current];
    if (copy.bad) return ScopeState::Bad;
    if (copy.state == CopyState::Failed) return ScopeState::Failed;
    return ScopeState::Ready;
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
    const detail::ScopeGroup* group = find_group(spec, scope);
    if (group == nullptr || group->dropped || group->copies.empty()) {
        return dstd::Optional<Value>{};
    }
    const detail::CopyData& copy = group->copies[group->current];
    if (copy.bad || copy.state != CopyState::Ready) {
        return dstd::Optional<Value>{};
    }
    if (key_hidden(key, group->hidden)) return dstd::Optional<Value>{};

    dstd::Vector<detail::ValueSource> sources;
    for (const detail::SectionRef& section : copy.sections) {
        const auto entry = section.view.find(key);
        if (entry) sources.push_back({copy.file_index, *entry});
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
        for (const std::size_t arm_index : at_key_) {
            const ScopeCursor& cursor = arms_[arm_index].cursor;
            const detail::CopyData& copy = *cursor.copy();
            for (const std::size_t section_index : cursor.matching()) {
                const auto entry =
                    copy.sections[section_index].view.find(cursor.key());
                if (entry) sources_.push_back({copy.file_index, *entry});
            }
        }
    } else {
        const ScopeCursor& cursor = arms_[arm_].cursor;
        const detail::CopyData& copy = *cursor.copy();
        const auto& matching = cursor.matching();
        if (!matching.empty()) {
            const auto entry =
                copy.sections[matching.back()].view.find(cursor.key());
            if (entry) sources_.push_back({copy.file_index, *entry});
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
    } else {
        for (const std::size_t arm_index : at_key_) {
            arms_[arm_index].cursor.next();
        }
    }
    settle();
}

void ScanCursor::settle() {
    at_key_.clear();
    if (fold_ == FoldKind::None) {
        while (arm_ < arms_.size() && arms_[arm_].cursor.at_end()) ++arm_;
        if (arm_ >= arms_.size()) {
            at_end_ = true;
            key_ = {};
            return;
        }
        key_ = arms_[arm_].cursor.key();
        at_end_ = false;
        return;
    }

    bool found = false;
    ByteView best;
    for (const Arm& arm : arms_) {
        if (arm.cursor.at_end()) continue;
        const ByteView candidate = arm.cursor.key();
        if (!found || compare_bytes(candidate, best) < 0) {
            best = candidate;
            found = true;
        }
    }
    if (!found) {
        at_end_ = true;
        key_ = {};
        return;
    }
    for (std::size_t i = 0; i < arms_.size(); ++i) {
        if (arms_[i].cursor.at_end()) continue;
        if (compare_bytes(arms_[i].cursor.key(), best) == 0) {
            at_key_.push_back(i);
        }
    }
    key_ = best;
    at_end_ = false;
}

void ScopeCursor::open(const detail::CopyData& copy,
                       const dstd::Vector<detail::HiddenRange>& hidden,
                       ByteView begin, ByteView end) {
    copy_ = &copy;
    hidden_ = &hidden;
    end_ = end;
    cursors_.clear();
    cursors_.reserve(copy.sections.size());
    for (const detail::SectionRef& section : copy.sections) {
        cursors_.push_back(section.view.lower_bound(begin));
    }
    settle();
}

void ScopeCursor::advance_matching() {
    for (const std::size_t index : matching_) cursors_[index].next();
}

void ScopeCursor::settle() {
    while (true) {
        matching_.clear();
        bool found = false;
        ByteView best;
        for (const SectionView::Cursor& cursor : cursors_) {
            if (cursor.at_end()) continue;
            const ByteView candidate = cursor.entry().key();
            if (!found || compare_bytes(candidate, best) < 0) {
                best = candidate;
                found = true;
            }
        }
        if (!found) {
            at_end_ = true;
            key_ = {};
            return;
        }
        if (!end_.empty() && compare_bytes(best, end_) >= 0) {
            at_end_ = true;
            key_ = {};
            return;
        }
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            if (cursors_[i].at_end()) continue;
            if (compare_bytes(cursors_[i].entry().key(), best) == 0) {
                matching_.push_back(i);
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
    if (!end.empty()) {
        const auto* start = reinterpret_cast<const std::uint8_t*>(end.data());
        cursor.end_storage_.insert(cursor.end_storage_.end(), start,
                                   start + end.size());
    }
    const ByteView bound = bytes_of(cursor.end_storage_);

    FoldKind fold = FoldKind::None;
    for (const ScopeKey& scope : scopes) {
        const detail::ScopeGroup* group = find_group(spec, scope);
        if (group == nullptr || group->dropped || group->copies.empty()) {
            continue;
        }
        const detail::CopyData& copy = group->copies[group->current];
        if (copy.bad || copy.state != CopyState::Ready) continue;
        if (fold == FoldKind::None) fold = copy.fold;
        ScanCursor::Arm arm;
        arm.scope = scope;
        arm.cursor.open(copy, group->hidden, begin, bound);
        if (arm.cursor.at_end()) continue;
        cursor.arms_.push_back(std::move(arm));
    }
    cursor.fold_ = fold;
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
    const detail::NamespaceGroup* namespace_group = find_namespace(spec);
    if (namespace_group != nullptr) {
        scopes.reserve(namespace_group->scopes.size());
        for (const std::size_t index : namespace_group->scopes) {
            scopes.push_back(groups_[index].scope);
        }
    }
    return scan(spec, scopes, begin, end);
}

}  // namespace dftracer::utils::index::store::engine
