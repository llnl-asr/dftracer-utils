// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/plan/file_index_data.h>
#include <dftracer/utils/trace/index/tables.h>

namespace dftracer::utils::trace::index::plan {

bool unrecorded(dstd::StringView path) {
    const dstd::StringView head = path.substr(0, path.find('.'));
    return head == "pid" || head == "tid" || head == "ts" || head == "dur" ||
           head == "ph" || head == "id";
}

const dstd::StringViewMap<std::uint64_t>* GranuleCounts::get() const {
    if (!values && !compressed.empty())
        values = trace::index::extensions::ChunkDimensionStats::
            decompress_value_counts(compressed.data(), compressed.size());
    return values ? &*values : nullptr;
}

bool FileIndexData::current(IndexExtension ext) {
    const auto key = static_cast<std::uint16_t>(ext);
    auto it = current_.find(key);
    if (it != current_.end()) return it->second;
    const bool ok = snapshot && file &&
                    trace::index::extension_current(*snapshot, *file, ext);
    current_.emplace(key, ok);
    return ok;
}

const FileIndexData::Catalog& FileIndexData::catalog() {
    if (!catalog_) {
        catalog_.emplace();
        if (current(IndexExtension::Catalog)) {
            auto listed = trace::index::catalog(*snapshot, *file);
            if (listed)
                *catalog_ = std::move(*listed);
            else
                fail(std::move(listed).error());
        }
    }
    return *catalog_;
}

bool FileIndexData::fixed_field(dstd::StringView path) const {
    if (by_path) return false;
    return path == "name" || path == "cat" || path == "pid" || path == "tid" ||
           path == "pid_tid" || path == "hhash" || path == "fhash" ||
           path == "shash" || path == "ts" || path == "dur" || path == "te";
}

const dstd::Optional<dstd::String>& FileIndexData::evidence_name(
    dstd::StringView path) {
    auto it = evidence_names_.find(path);
    if (it != evidence_names_.end()) return it->second;
    dstd::Optional<dstd::String> name;
    if (by_path || fixed_field(path)) {
        name = path;
    } else if (path.starts_with("args.")) {
        if (!fixed_field(dstd::StringView(path).substr(5)))
            name = path.substr(5);
    } else if (!unrecorded(path) && !catalog().empty()) {
        name = path;
        for (const auto& [p, stat] : catalog())
            if (dstd::StringView(p) == path ||
                (p.size() > path.size() && p.starts_with(path) &&
                 p[path.size()] == '.')) {
                name.reset();
                break;
            }
    }
    return evidence_names_.emplace(path, std::move(name)).first->second;
}

bool FileIndexData::may_hold_strings(dstd::StringView path) {
    const auto& name = evidence_name(path);
    if (!name) return true;
    if (fixed_field(*name))
        return *name != "pid" && *name != "tid" && *name != "ts" &&
               *name != "dur" && *name != "te";
    const dstd::String leaf = by_path ? *name : "args." + *name;
    for (const auto& [p, stat] : catalog()) {
        if (dstd::StringView(p) != leaf) continue;
        constexpr auto STRING_BIT =
            1U << static_cast<unsigned>(trace::index::store::PathType::String);
        return (stat.seen & STRING_BIT) != 0 ||
               stat.type == trace::index::store::PathType::String ||
               stat.type == trace::index::store::PathType::Mixed;
    }
    return true;
}

const FileIndexData::Zones& FileIndexData::zones(dstd::StringView path) {
    auto it = zones_.find(path);
    if (it != zones_.end()) return it->second;
    auto& out = zones_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Zonemap)) {
        auto listed = trace::index::granules(*snapshot, *file,
                                             IndexExtension::Zonemap, *name);
        if (!listed) {
            fail(std::move(listed).error());
            return out;
        }
        for (auto& [granule, bytes] : *listed)
            if (auto zone = trace::index::extensions::kinds::decode_zone(bytes))
                out.emplace(granule, std::move(*zone));
    }
    return out;
}

const FileIndexData::Counts& FileIndexData::counts(dstd::StringView path) {
    auto it = counts_.find(path);
    if (it != counts_.end()) return it->second;
    auto& out = counts_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Counts)) {
        auto listed = trace::index::granules(*snapshot, *file,
                                             IndexExtension::Counts, *name);
        if (!listed) {
            fail(std::move(listed).error());
            return out;
        }
        for (auto& [granule, bytes] : *listed) {
            auto counts = trace::index::extensions::kinds::decode_counts(bytes);
            if (!counts || !counts->compressed) continue;
            auto& g = out[granule];
            g.observed = counts->observed;
            g.compressed = std::move(*counts->compressed);
        }
    }
    return out;
}

const FileIndexData::Blooms& FileIndexData::blooms(dstd::StringView path) {
    auto it = blooms_.find(path);
    if (it != blooms_.end()) return it->second;
    auto& out = blooms_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Bloom)) {
        auto listed = trace::index::granules(*snapshot, *file,
                                             IndexExtension::Bloom, *name);
        if (!listed) {
            fail(std::move(listed).error());
            return out;
        }
        for (auto& [granule, bytes] : *listed)
            if (auto bloom =
                    trace::index::extensions::kinds::decode_bloom(bytes))
                out.emplace(granule, std::move(*bloom));
    }
    return out;
}

const trace::index::extensions::ScalableBloomFilter* FileIndexData::file_bloom(
    dstd::StringView path) {
    auto it = file_blooms_.find(path);
    if (it == file_blooms_.end()) {
        dstd::Optional<trace::index::extensions::ScalableBloomFilter> bloom;
        const auto& name = evidence_name(path);
        if (name && current(IndexExtension::Bloom)) {
            auto bytes = trace::index::path_file(*snapshot, *file,
                                                 IndexExtension::Bloom, *name);
            if (!bytes)
                fail(std::move(bytes).error());
            else if (*bytes)
                bloom = trace::index::extensions::kinds::decode_bloom(**bytes);
        }
        it = file_blooms_.emplace(path, std::move(bloom)).first;
    }
    return it->second ? &*it->second : nullptr;
}

namespace {

dstd::String posting_key(dstd::StringView path, std::uint64_t value_hash) {
    dstd::String key;
    key.reserve(path.size() + 1 + 8);
    key.append(path);
    key.push_back('\0');
    dstd::append_u64_be(key, value_hash);
    return key;
}

}  // namespace

const dstd::Set<std::uint64_t>& FileIndexData::posting_chunks(
    dstd::StringView path, std::uint64_t value_hash) {
    auto key = posting_key(path, value_hash);
    auto it = posting_chunks_.find(key);
    if (it != posting_chunks_.end()) return it->second;
    auto& out = posting_chunks_[key];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Postings)) {
        auto granules =
            trace::index::posting_granules(*snapshot, *file, *name, value_hash);
        if (!granules) {
            fail(std::move(granules).error());
            return out;
        }
        for (auto g : *granules) out.insert(g);
    }
    return out;
}

dstd::Optional<bool> FileIndexData::posting_contains(dstd::StringView path,
                                                     std::uint64_t value_hash) {
    const auto& name = evidence_name(path);
    if (!name || !current(IndexExtension::Postings)) return dstd::nullopt;
    auto has = has_postings_.find(path);
    if (has == has_postings_.end()) {
        const auto found = trace::index::has_postings(*snapshot, *file, *name);
        if (!found.has_value()) {
            fail(found.error());
            return dstd::nullopt;
        }
        has = has_postings_.emplace(path, *found).first;
    }
    if (!has->second) return dstd::nullopt;
    auto key = posting_key(path, value_hash);
    auto it = posting_membership_.find(key);
    if (it != posting_membership_.end()) return it->second;
    const auto present =
        trace::index::has_posting(*snapshot, *file, *name, value_hash);
    if (!present.has_value()) {
        fail(present.error());
        return dstd::nullopt;
    }
    posting_membership_.emplace(key, *present);
    return *present;
}

}  // namespace dftracer::utils::trace::index::plan
