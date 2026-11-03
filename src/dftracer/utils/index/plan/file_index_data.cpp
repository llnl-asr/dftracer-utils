// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/plan/file_index_data.h>
#include <dftracer/utils/index/tables.h>

namespace dftracer::utils::index::plan {

namespace kinds = index::extensions::kinds;

bool unrecorded(dstd::StringView path) {
    const dstd::StringView head = path.substr(0, path.find('.'));
    return head == "pid" || head == "tid" || head == "ts" || head == "dur" ||
           head == "ph" || head == "id";
}

const dstd::StringViewMap<std::uint64_t>* GranuleCounts::get() const {
    if (!values && !compressed.empty())
        values =
            index::extensions::ChunkDimensionStats::decompress_value_counts(
                compressed.data(), compressed.size());
    return values ? &*values : nullptr;
}

bool FileIndexData::current(IndexExtension ext) {
    const auto key = static_cast<std::uint16_t>(ext);
    auto it = current_.find(key);
    if (it != current_.end()) return it->second;
    const bool ok =
        snapshot && file && index::extension_current(*snapshot, *file, ext);
    current_.emplace(key, ok);
    return ok;
}

const FileIndexData::Catalog& FileIndexData::catalog() {
    if (!catalog_) {
        catalog_.emplace();
        if (current(IndexExtension::Catalog))
            *catalog_ = common::unwrap(index::catalog(*snapshot, *file));
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
    const dstd::String& path) {
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

const FileIndexData::Zones& FileIndexData::zones(const dstd::String& path) {
    auto it = zones_.find(path);
    if (it != zones_.end()) return it->second;
    auto& out = zones_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Zonemap))
        for (auto& [granule, bytes] : common::unwrap(index::granules(
                 *snapshot, *file, IndexExtension::Zonemap, *name)))
            if (auto zone = kinds::decode_zone(bytes))
                out.emplace(granule, std::move(*zone));
    return out;
}

const FileIndexData::Counts& FileIndexData::counts(const dstd::String& path) {
    auto it = counts_.find(path);
    if (it != counts_.end()) return it->second;
    auto& out = counts_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Counts))
        for (auto& [granule, bytes] : common::unwrap(index::granules(
                 *snapshot, *file, IndexExtension::Counts, *name))) {
            auto counts = kinds::decode_counts(bytes);
            if (!counts || !counts->compressed) continue;
            auto& g = out[granule];
            g.observed = counts->observed;
            g.compressed = std::move(*counts->compressed);
        }
    return out;
}

const FileIndexData::Blooms& FileIndexData::blooms(const dstd::String& path) {
    auto it = blooms_.find(path);
    if (it != blooms_.end()) return it->second;
    auto& out = blooms_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Bloom))
        for (auto& [granule, bytes] : common::unwrap(index::granules(
                 *snapshot, *file, IndexExtension::Bloom, *name)))
            if (auto bloom = kinds::decode_bloom(bytes))
                out.emplace(granule, std::move(*bloom));
    return out;
}

const index::extensions::ScalableBloomFilter* FileIndexData::file_bloom(
    const dstd::String& path) {
    auto it = file_blooms_.find(path);
    if (it == file_blooms_.end()) {
        dstd::Optional<index::extensions::ScalableBloomFilter> bloom;
        const auto& name = evidence_name(path);
        if (name && current(IndexExtension::Bloom))
            if (auto bytes = common::unwrap(index::path_file(
                    *snapshot, *file, IndexExtension::Bloom, *name)))
                bloom = kinds::decode_bloom(*bytes);
        it = file_blooms_.emplace(path, std::move(bloom)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const dstd::Set<std::uint64_t>& FileIndexData::posting_chunks(
    const dstd::String& path, const dstd::String& val) {
    auto key = path + '\0' + val;
    auto it = posting_chunks_.find(key);
    if (it != posting_chunks_.end()) return it->second;
    auto& out = posting_chunks_[key];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::Postings))
        for (auto g : common::unwrap(index::posting_granules(
                 *snapshot, *file, *name, kinds::value_hash(val))))
            out.insert(g);
    return out;
}

dstd::Optional<bool> FileIndexData::posting_contains(const dstd::String& path,
                                                     const dstd::String& val) {
    const auto& name = evidence_name(path);
    if (!name || !current(IndexExtension::Postings)) return dstd::nullopt;
    auto has = has_postings_.find(path);
    if (has == has_postings_.end())
        has = has_postings_
                  .emplace(path, common::unwrap(index::has_postings(
                                     *snapshot, *file, *name)))
                  .first;
    if (!has->second) return dstd::nullopt;
    auto key = path + '\0' + val;
    auto it = posting_membership_.find(key);
    if (it != posting_membership_.end()) return it->second;
    const bool present = common::unwrap(
        index::has_posting(*snapshot, *file, *name, kinds::value_hash(val)));
    posting_membership_.emplace(key, present);
    return present;
}

}  // namespace dftracer::utils::index::plan
