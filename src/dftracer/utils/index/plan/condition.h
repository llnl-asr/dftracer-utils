#ifndef DFTRACER_UTILS_INDEX_PLAN_CONDITION_H
#define DFTRACER_UTILS_INDEX_PLAN_CONDITION_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/index/store/file.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::index::plan {

using ChunkSet = std::set<std::uint64_t>;

/// `in` and `not in` lists longer than this prune nothing: probing each value
/// costs more than the scan it could save.
inline constexpr std::size_t SEMI_JOIN_CAP = 4096;

/// Whether `node` is an `in` or `not in` list longer than SEMI_JOIN_CAP.
bool too_wide(const duql::QueryNode& node);

/// One source of pruning evidence for one file and one query. Every answer is
/// sound: a chunk or file is ruled out only when this source proves that no
/// event in it can match, and missing data means "no evidence", never a skip.
class Condition {
   public:
    virtual ~Condition() = default;
    /// The extension whose data this source reads.
    virtual index::store::IndexExtension extension() const = 0;
    /// The extension's name; a plugin extension's registered name.
    virtual std::string name() const {
        return std::string(index::store::extension_name(extension()));
    }
    /// False only when no event of the file can match `root`.
    virtual bool file_may_match(const duql::QueryNode&) { return true; }
    /// The chunks of `candidates` in which an event may match the query leaf
    /// `leaf` (a result may also name chunks outside `candidates`, which the
    /// evaluator intersects away); nullopt when this source has no evidence
    /// for the leaf.
    virtual std::optional<ChunkSet> may_match(const duql::QueryNode& leaf,
                                              const ChunkSet& candidates) = 0;
    /// True when every answer of may_match is exactly the matching chunks,
    /// so no other source can rule out more and the evaluator stops there.
    virtual bool exact() const { return false; }
    /// The chunks in which every event matches `leaf`; nullopt without
    /// evidence.
    virtual std::optional<ChunkSet> all_match(const duql::QueryNode&) {
        return std::nullopt;
    }
};

using Conditions = std::vector<std::unique_ptr<Condition>>;

struct FileIndexData;

/// The Conditions over one file's loaded index data, cheapest and most
/// selective first: file bloom, name postings, dimension statistics
/// (dictionaries and min/max), the ts histogram and the chunk blooms.
Conditions make_query_conditions(FileIndexData& data);

/// The Condition over metadata records (dft.metadata): a chunk may hold a
/// record matching a leaf only when one of its records has the leaf's field
/// and, for a leaf on `name`, one of its record names satisfies it. `metadata`
/// is borrowed and must outlive the Conditions.
Conditions make_metadata_conditions(
    const ankerl::unordered_dense::map<std::uint64_t,
                                       index::store::ChunkMetadata>& metadata);

/// False only when some Condition proves no event of the file matches.
bool file_may_match(const duql::QueryNode& root, const Conditions& conditions);

/// The chunks of `universe` that may hold an event matching `root`: a leaf
/// keeps the chunks every Condition allows, AND intersects, OR unions, NOT p
/// keeps the chunks where not every event is proved to match p, and a leaf no
/// Condition knows keeps everything.
ChunkSet evaluate(const duql::QueryNode& root, const Conditions& conditions,
                  const ChunkSet& universe);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_CONDITION_H
