#ifndef DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H
#define DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/store/index_database.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::index::plan {

using index::store::IndexDatabase;
using index::store::IndexExtension;

/// One granule's value counts; the map is decompressed on first use.
struct GranuleCounts {
    std::uint64_t observed = 0;
    std::vector<std::uint8_t> compressed;
    mutable std::optional<StringViewMap<std::uint64_t>> values;

    const StringViewMap<std::uint64_t>* get() const;
};

/// One file's index data for pruning, shared by the Conditions built for one
/// query. Each kind's data for a path is read the first time a leaf needs it,
/// and only when that kind is current for the file.
struct FileIndexData {
    const IndexDatabase* db = nullptr;
    int fid = -1;
    /// The file was indexed by path, so no field is a dftracer fixed field.
    bool by_path = false;
    std::uint64_t total_chunks = 0;
    std::set<std::uint64_t> all_chunks;
    /// Lines per chunk from the member table: a chunk whose lines are not all
    /// observed records holds records the kinds never saw.
    ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> chunk_lines;

    using Zones = ankerl::unordered_dense::map<std::uint64_t,
                                               index::extensions::kinds::Zone>;
    using Counts = ankerl::unordered_dense::map<std::uint64_t, GranuleCounts>;
    using Blooms =
        ankerl::unordered_dense::map<std::uint64_t,
                                     index::extensions::ScalableBloomFilter>;

    const Zones& zones(const std::string& path);
    const Counts& counts(const std::string& path);
    const Blooms& blooms(const std::string& path);
    /// The file-level bloom of `path`; nullptr when there is none.
    const index::extensions::ScalableBloomFilter* file_bloom(
        const std::string& path);

    /// Chunks holding `val` of `path` per its postings; empty when the
    /// postings name none.
    const std::set<std::uint64_t>& posting_chunks(const std::string& path,
                                                  const std::string& val);
    /// Whether the file holds `val` of `path` per its postings; nullopt when
    /// the file has no postings for the path.
    std::optional<bool> posting_contains(const std::string& path,
                                         const std::string& val);

   private:
    bool current(IndexExtension ext);

    ankerl::unordered_dense::map<std::uint16_t, bool> current_;
    StringViewMap<Zones> zones_;
    StringViewMap<Counts> counts_;
    StringViewMap<Blooms> blooms_;
    StringViewMap<std::optional<index::extensions::ScalableBloomFilter>>
        file_blooms_;
    StringViewMap<std::set<std::uint64_t>> posting_chunks_;
    StringViewMap<bool> posting_membership_;
    StringViewMap<bool> has_postings_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H
