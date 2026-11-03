// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H
#define DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H

#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/file.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::index::plan {

using index::store::IndexExtension;

/// One granule's value counts; the map is decompressed on first use.
struct GranuleCounts {
    std::uint64_t observed = 0;
    dstd::Vector<std::uint8_t> compressed;
    mutable dstd::Optional<dstd::StringViewMap<std::uint64_t>> values;

    const dstd::StringViewMap<std::uint64_t>* get() const;
};

/// Whether a dftracer path is a field every record carries at its top level
/// (pid, tid, ts, dur, ph, id, or below one), which the catalog leaves out.
bool unrecorded(dstd::StringView path);

/// One file's index data for pruning, shared by the Conditions built for one
/// query. Each kind's data for a path is read the first time a leaf needs it,
/// and only when that kind is current for the file.
struct FileIndexData {
    /// Borrowed; both outlive the FileIndexData.
    const index::Snapshot* snapshot = nullptr;
    const index::FileRef* file = nullptr;
    /// The file was indexed by path, so no field is a dftracer fixed field.
    bool by_path = false;
    std::uint64_t total_chunks = 0;
    dstd::Set<std::uint64_t> all_chunks;
    /// Lines per chunk from the member table: a chunk whose lines are not all
    /// observed records holds records the kinds never saw.
    ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> chunk_lines;

    using Zones = ankerl::unordered_dense::map<std::uint64_t,
                                               index::extensions::kinds::Zone>;
    using Counts = ankerl::unordered_dense::map<std::uint64_t, GranuleCounts>;
    using Blooms =
        ankerl::unordered_dense::map<std::uint64_t,
                                     index::extensions::ScalableBloomFilter>;

    /// Whether `path` is a dftracer fixed dimension, built from the record's
    /// top-level field.
    bool fixed_field(dstd::StringView path) const;
    /// The name the evidence answering a query on `path` is stored under, or
    /// nullopt when no evidence answers it: `args.<k>` reads the args key
    /// `k` unless `k` is a fixed dimension, and a bare dftracer path that a
    /// record may hold at its top level, where the evaluator looks first
    /// and the args evidence does not, has none. The accessors below take
    /// query paths and map them through this.
    const dstd::Optional<dstd::String>& evidence_name(const dstd::String& path);

    const Zones& zones(const dstd::String& path);
    const Counts& counts(const dstd::String& path);
    const Blooms& blooms(const dstd::String& path);
    using Catalog =
        dstd::Vector<dstd::Pair<dstd::String, index::store::PathStat>>;
    /// The file's path catalog; empty when it is not current.
    const Catalog& catalog();

    /// The file-level bloom of `path`; nullptr when there is none.
    const index::extensions::ScalableBloomFilter* file_bloom(
        const dstd::String& path);

    /// Chunks holding `val` of `path` per its postings; empty when the
    /// postings name none.
    const dstd::Set<std::uint64_t>& posting_chunks(const dstd::String& path,
                                                   const dstd::String& val);
    /// Whether the file holds `val` of `path` per its postings; nullopt when
    /// the file has no postings for the path.
    dstd::Optional<bool> posting_contains(const dstd::String& path,
                                          const dstd::String& val);

   private:
    bool current(IndexExtension ext);

    ankerl::unordered_dense::map<std::uint16_t, bool> current_;
    dstd::Optional<Catalog> catalog_;
    dstd::StringViewMap<dstd::Optional<dstd::String>> evidence_names_;
    dstd::StringViewMap<Zones> zones_;
    dstd::StringViewMap<Counts> counts_;
    dstd::StringViewMap<Blooms> blooms_;
    dstd::StringViewMap<dstd::Optional<index::extensions::ScalableBloomFilter>>
        file_blooms_;
    dstd::StringViewMap<dstd::Set<std::uint64_t>> posting_chunks_;
    dstd::StringViewMap<bool> posting_membership_;
    dstd::StringViewMap<bool> has_postings_;
};

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_FILE_INDEX_DATA_H
