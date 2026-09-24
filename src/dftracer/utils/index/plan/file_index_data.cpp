#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/plan/file_index_data.h>

namespace dftracer::utils::index::plan {

namespace kinds = index::extensions::kinds;

const StringViewMap<std::uint64_t>* GranuleCounts::get() const {
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
    const bool ok = db && fid >= 0 && db->extension_current(fid, ext);
    current_.emplace(key, ok);
    return ok;
}

const FileIndexData::Zones& FileIndexData::zones(const std::string& path) {
    auto it = zones_.find(path);
    if (it != zones_.end()) return it->second;
    auto& out = zones_[path];
    if (current(IndexExtension::ZONEMAP))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::ZONEMAP, path))
            if (auto zone = kinds::decode_zone(bytes))
                out.emplace(granule, std::move(*zone));
    return out;
}

const FileIndexData::Counts& FileIndexData::counts(const std::string& path) {
    auto it = counts_.find(path);
    if (it != counts_.end()) return it->second;
    auto& out = counts_[path];
    if (current(IndexExtension::COUNTS))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::COUNTS, path)) {
            auto counts = kinds::decode_counts(bytes);
            if (!counts || !counts->compressed) continue;
            auto& g = out[granule];
            g.observed = counts->observed;
            g.compressed = std::move(*counts->compressed);
        }
    return out;
}

const FileIndexData::Blooms& FileIndexData::blooms(const std::string& path) {
    auto it = blooms_.find(path);
    if (it != blooms_.end()) return it->second;
    auto& out = blooms_[path];
    if (current(IndexExtension::BLOOM))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::BLOOM, path))
            if (auto bloom = kinds::decode_bloom(bytes))
                out.emplace(granule, std::move(*bloom));
    return out;
}

const index::extensions::ScalableBloomFilter* FileIndexData::file_bloom(
    const std::string& path) {
    auto it = file_blooms_.find(path);
    if (it == file_blooms_.end()) {
        std::optional<index::extensions::ScalableBloomFilter> bloom;
        if (current(IndexExtension::BLOOM))
            if (auto bytes =
                    db->path_file_value(fid, IndexExtension::BLOOM, path))
                bloom = kinds::decode_bloom(*bytes);
        it = file_blooms_.emplace(path, std::move(bloom)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const std::set<std::uint64_t>& FileIndexData::posting_chunks(
    const std::string& path, const std::string& val) {
    auto key = path + '\0' + val;
    auto it = posting_chunks_.find(key);
    if (it != posting_chunks_.end()) return it->second;
    auto& out = posting_chunks_[key];
    if (current(IndexExtension::POSTINGS))
        for (auto g : db->posting_granules(fid, path, kinds::value_hash(val)))
            out.insert(g);
    return out;
}

std::optional<bool> FileIndexData::posting_contains(const std::string& path,
                                                    const std::string& val) {
    if (!current(IndexExtension::POSTINGS)) return std::nullopt;
    auto has = has_postings_.find(path);
    if (has == has_postings_.end())
        has = has_postings_.emplace(path, db->has_postings(fid, path)).first;
    if (!has->second) return std::nullopt;
    auto key = path + '\0' + val;
    auto it = posting_membership_.find(key);
    if (it != posting_membership_.end()) return it->second;
    const bool present = db->has_posting(fid, path, kinds::value_hash(val));
    posting_membership_.emplace(key, present);
    return present;
}

}  // namespace dftracer::utils::index::plan
