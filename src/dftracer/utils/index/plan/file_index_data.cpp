#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/plan/file_index_data.h>

namespace dftracer::utils::index::plan {

namespace kinds = index::extensions::kinds;

bool unrecorded(std::string_view path) {
    const std::string_view head = path.substr(0, path.find('.'));
    return head == "pid" || head == "tid" || head == "ts" || head == "dur" ||
           head == "ph" || head == "id";
}

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

const FileIndexData::Catalog& FileIndexData::catalog() {
    if (!catalog_) {
        catalog_.emplace();
        if (current(IndexExtension::CATALOG)) *catalog_ = db->catalog(fid);
    }
    return *catalog_;
}

bool FileIndexData::fixed_field(std::string_view path) const {
    if (by_path) return false;
    return path == "name" || path == "cat" || path == "pid" || path == "tid" ||
           path == "pid_tid" || path == "hhash" || path == "fhash" ||
           path == "shash" || path == "ts" || path == "dur" || path == "te";
}

const std::optional<std::string>& FileIndexData::evidence_name(
    const std::string& path) {
    auto it = evidence_names_.find(path);
    if (it != evidence_names_.end()) return it->second;
    std::optional<std::string> name;
    if (by_path || fixed_field(path)) {
        name = path;
    } else if (path.starts_with("args.")) {
        if (!fixed_field(std::string_view(path).substr(5)))
            name = path.substr(5);
    } else if (!unrecorded(path) && !catalog().empty()) {
        name = path;
        for (const auto& [p, stat] : catalog())
            if (p == path || (p.size() > path.size() && p.starts_with(path) &&
                              p[path.size()] == '.')) {
                name.reset();
                break;
            }
    }
    return evidence_names_.emplace(path, std::move(name)).first->second;
}

const FileIndexData::Zones& FileIndexData::zones(const std::string& path) {
    auto it = zones_.find(path);
    if (it != zones_.end()) return it->second;
    auto& out = zones_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::ZONEMAP))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::ZONEMAP, *name))
            if (auto zone = kinds::decode_zone(bytes))
                out.emplace(granule, std::move(*zone));
    return out;
}

const FileIndexData::Counts& FileIndexData::counts(const std::string& path) {
    auto it = counts_.find(path);
    if (it != counts_.end()) return it->second;
    auto& out = counts_[path];
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::COUNTS))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::COUNTS, *name)) {
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
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::BLOOM))
        for (auto& [granule, bytes] :
             db->path_granules(fid, IndexExtension::BLOOM, *name))
            if (auto bloom = kinds::decode_bloom(bytes))
                out.emplace(granule, std::move(*bloom));
    return out;
}

const index::extensions::ScalableBloomFilter* FileIndexData::file_bloom(
    const std::string& path) {
    auto it = file_blooms_.find(path);
    if (it == file_blooms_.end()) {
        std::optional<index::extensions::ScalableBloomFilter> bloom;
        const auto& name = evidence_name(path);
        if (name && current(IndexExtension::BLOOM))
            if (auto bytes =
                    db->path_file_value(fid, IndexExtension::BLOOM, *name))
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
    const auto& name = evidence_name(path);
    if (name && current(IndexExtension::POSTINGS))
        for (auto g : db->posting_granules(fid, *name, kinds::value_hash(val)))
            out.insert(g);
    return out;
}

std::optional<bool> FileIndexData::posting_contains(const std::string& path,
                                                    const std::string& val) {
    const auto& name = evidence_name(path);
    if (!name || !current(IndexExtension::POSTINGS)) return std::nullopt;
    auto has = has_postings_.find(path);
    if (has == has_postings_.end())
        has = has_postings_.emplace(path, db->has_postings(fid, *name)).first;
    if (!has->second) return std::nullopt;
    auto key = path + '\0' + val;
    auto it = posting_membership_.find(key);
    if (it != posting_membership_.end()) return it->second;
    const bool present = db->has_posting(fid, *name, kinds::value_hash(val));
    posting_membership_.emplace(key, present);
    return present;
}

}  // namespace dftracer::utils::index::plan
