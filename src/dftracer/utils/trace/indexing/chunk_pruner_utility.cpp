#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/query/ast.h>
#include <dftracer/utils/trace/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/trace/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/indexing/queries/queries.h>
#include <dftracer/utils/trace/indexing/scalable_bloom_filter.h>
#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace dftracer::utils::trace::indexing {

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
namespace query_ns = query;

namespace {

struct ChunkMeta {
    std::unordered_map<std::string, ChunkDimensionStatsResult> dim_stats;
};

const std::unordered_set<std::string> HASH_DIMENSIONS = {"hhash", "fhash",
                                                         "shash"};

bool looks_like_hash(const std::string& value) {
    if (value.size() < 16) return false;
    for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::optional<IndexDatabase::HashType> dim_to_hash_type(
    const std::string& dim) {
    if (dim == "fhash") return IndexDatabase::HashType::FILE;
    if (dim == "hhash") return IndexDatabase::HashType::HOST;
    if (dim == "shash") return IndexDatabase::HashType::STRING;
    return std::nullopt;
}

struct PrunerContext {
    int file_info_id;
    std::uint64_t total_chunks;
    std::set<std::uint64_t> all_chunks;
    std::unordered_map<std::uint64_t, ChunkMeta> chunks;
    std::unordered_map<std::string,
                       std::unordered_map<std::uint64_t, ScalableBloomFilter>>
        bloom_filters;
    std::unordered_map<std::uint64_t,
                       utilities::common::statistics::TimestampHistogram>
        ts_histograms;

    // Hash resolution: human-readable value -> hash strings
    std::unordered_map<std::string, std::vector<std::string>> hash_cache;
    std::unordered_map<std::string, std::set<std::uint64_t>> name_postings;
    std::unordered_map<std::string, bool> name_file_membership;
    const IndexDatabase* db = nullptr;
    int fid = -1;

    BloomFilterCache* cache;
    std::string index_path;

    /// File-level blooms for the dimensions this query touches, the first
    /// pruning tier: a definite miss here skips the file without loading a
    /// single chunk record.
    std::unordered_map<std::string, ScalableBloomFilter> file_blooms;

    /// Resolves a hash-dimension value to its hash(es); returns empty for
    /// non-hash dimensions or a value that already looks like a hash.
    const std::vector<std::string>& resolve_hashes(const std::string& dim,
                                                   const std::string& val) {
        static const std::vector<std::string> empty;
        if (!HASH_DIMENSIONS.count(dim) || looks_like_hash(val)) return empty;

        auto key = dim + ":" + val;
        auto it = hash_cache.find(key);
        if (it != hash_cache.end()) return it->second;

        if (db) {
            auto hash_type = dim_to_hash_type(dim);
            if (hash_type) {
                auto hash = db->resolve_name_to_hash(*hash_type, val);
                auto& cached = hash_cache[key];
                if (hash) {
                    cached.push_back(std::move(*hash));
                }
                return cached;
            }
        }
        return empty;
    }

    const std::set<std::uint64_t>& resolve_name_chunks(const std::string& val) {
        static const std::set<std::uint64_t> empty;
        auto it = name_postings.find(val);
        if (it != name_postings.end()) return it->second;
        if (!db || fid < 0) return empty;

        auto chunk_ids = db->query_name_chunk_postings(val, fid);
        auto& cached = name_postings[val];
        cached.insert(chunk_ids.begin(), chunk_ids.end());
        return cached;
    }

    std::optional<bool> file_contains_name(const std::string& val) {
        auto it = name_file_membership.find(val);
        if (it != name_file_membership.end()) return it->second;
        if (!db || fid < 0) return std::nullopt;

        auto name_id = db->query_name_id(val);
        if (!name_id.has_value()) {
            return std::nullopt;
        }

        auto file_ids = db->query_name_file_postings(val);
        const bool present =
            std::find(file_ids.begin(), file_ids.end(), fid) != file_ids.end();
        name_file_membership[val] = present;
        return present;
    }
};

std::string literal_to_string(const query_ns::LiteralNode& lit) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, uint64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
                return std::to_string(v);
            else
                return {};
        },
        lit.value);
}

// true = definitely present, false = definitely absent, nullopt = no
// dictionary for this dimension/chunk.
std::optional<bool> dict_contains(const ChunkMeta& meta, const std::string& dim,
                                  const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return std::nullopt;
    if (!it->second.has_value_counts_payload()) return std::nullopt;
    it->second.ensure_value_counts_decoded();
    if (!it->second.value_counts) return std::nullopt;
    return it->second.value_counts->count(val) > 0;
}

// True if every value in the chunk's dictionary equals val, so the chunk
// can be skipped for a != query.
std::optional<bool> dict_excludes(const ChunkMeta& meta, const std::string& dim,
                                  const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return std::nullopt;
    if (!it->second.has_value_counts_payload()) return std::nullopt;
    it->second.ensure_value_counts_decoded();
    if (!it->second.value_counts) return std::nullopt;
    auto& vc = *it->second.value_counts;
    // If the only value in the chunk IS val, all events match val, so no
    // event matches != val and the chunk can be skipped.
    if (vc.size() == 1 && vc.count(val) > 0) return true;
    return false;
}

// True if the chunk provably holds no event with `dim` == val, so every event
// in it matches `dim != val`. nullopt when there is no dictionary to prove it.
std::optional<bool> dict_lacks(const ChunkMeta& meta, const std::string& dim,
                               const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return std::nullopt;
    if (!it->second.has_value_counts_payload()) return std::nullopt;
    it->second.ensure_value_counts_decoded();
    if (!it->second.value_counts) return std::nullopt;
    return it->second.value_counts->count(val) == 0;
}

// The chunk's dictionary for `dim`, or nullptr when it has none.
const dftracer::utils::StringViewMap<std::uint64_t>* dict_of(
    const ChunkMeta& meta, const std::string& dim) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return nullptr;
    if (!it->second.has_value_counts_payload()) return nullptr;
    it->second.ensure_value_counts_decoded();
    if (!it->second.value_counts) return nullptr;
    return &*it->second.value_counts;
}

bool is_numeric_type(const std::string& vtype) {
    return vtype == "uint" || vtype == "int" || vtype == "double";
}

int compare_values(const std::string& a, const std::string& b,
                   const std::string& vtype) {
    if (is_numeric_type(vtype)) {
        try {
            if (vtype == "uint") {
                auto ua = std::stoull(a);
                auto ub = std::stoull(b);
                if (ua < ub) return -1;
                if (ua > ub) return 1;
                return 0;
            }
            if (vtype == "int") {
                auto ia = std::stoll(a);
                auto ib = std::stoll(b);
                if (ia < ib) return -1;
                if (ia > ib) return 1;
                return 0;
            }
            double da = std::stod(a);
            double db = std::stod(b);
            if (da < db) return -1;
            if (da > db) return 1;
            return 0;
        } catch (...) {
        }
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

bool range_may_match(const ChunkMeta& meta, const std::string& dim,
                     query_ns::CompareOp op, const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return true;
    const auto& ds = it->second;
    if (ds.min_value.empty() && ds.max_value.empty()) return true;

    // Some chunk stats can be corrupt (min > max). Never prune on stats we
    // cannot trust; a false negative would silently drop matching events.
    if (is_numeric_type(ds.value_type) && !ds.min_value.empty() &&
        !ds.max_value.empty()) {
        try {
            if (std::stod(ds.min_value) > std::stod(ds.max_value)) return true;
        } catch (...) {
            return true;
        }
    }

    switch (op) {
        case query_ns::CompareOp::GT:
            return compare_values(ds.max_value, val, ds.value_type) > 0;
        case query_ns::CompareOp::GE:
            return compare_values(ds.max_value, val, ds.value_type) >= 0;
        case query_ns::CompareOp::LT:
            return compare_values(ds.min_value, val, ds.value_type) < 0;
        case query_ns::CompareOp::LE:
            return compare_values(ds.min_value, val, ds.value_type) <= 0;
        default:
            return true;
    }
}

bool bloom_probe(PrunerContext& ctx, const std::string& dim, std::uint64_t ckpt,
                 const std::string& val) {
    auto dim_it = ctx.bloom_filters.find(dim);
    if (dim_it == ctx.bloom_filters.end()) return true;
    auto ckpt_it = dim_it->second.find(ckpt);
    if (ckpt_it == dim_it->second.end()) return true;
    return ckpt_it->second.possibly_contains(val);
}

bool bloom_may_contain(PrunerContext& ctx, const std::string& dim,
                       std::uint64_t ckpt, const std::string& val) {
    auto& resolved = ctx.resolve_hashes(dim, val);
    if (!resolved.empty()) {
        for (const auto& hash : resolved) {
            if (bloom_probe(ctx, dim, ckpt, hash)) return true;
        }
        return false;
    }
    return bloom_probe(ctx, dim, ckpt, val);
}

// Zero events in the queried range means the chunk can be skipped.
bool histogram_has_events(PrunerContext& ctx, std::uint64_t ckpt,
                          query_ns::CompareOp op, std::uint64_t ts_val) {
    auto it = ctx.ts_histograms.find(ckpt);
    if (it == ctx.ts_histograms.end()) return true;
    const auto& hist = it->second;
    if (hist.empty()) return true;

    switch (op) {
        case query_ns::CompareOp::GT:
            return hist.count_in_range(ts_val + 1, UINT64_MAX) > 0;
        case query_ns::CompareOp::GE:
            return hist.count_in_range(ts_val, UINT64_MAX) > 0;
        case query_ns::CompareOp::LT:
            return hist.count_in_range(0, ts_val) > 0;
        case query_ns::CompareOp::LE:
            return hist.count_in_range(0, ts_val + 1) > 0;
        default:
            return true;
    }
}

// True when the file may hold `val`, and when the dimension has no file
// bloom at all.
bool file_bloom_may_contain(PrunerContext& ctx, const std::string& dim,
                            const std::string& val) {
    auto it = ctx.file_blooms.find(dim);
    if (it == ctx.file_blooms.end()) return true;

    auto& resolved = ctx.resolve_hashes(dim, val);
    if (!resolved.empty()) {
        for (const auto& hash : resolved) {
            if (it->second.possibly_contains(hash)) return true;
        }
        return false;
    }
    return it->second.possibly_contains(val);
}

// Whether the file can hold any matching event at all. Only equality-shaped
// leaves can answer "definitely not"; everything else stays conservative.
bool file_may_match(const query_ns::QueryNode& node, PrunerContext& ctx) {
    return std::visit(
        [&ctx](auto&& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, query_ns::CompareNode>) {
                if (n.op != query_ns::CompareOp::EQ) return true;
                return file_bloom_may_contain(ctx, n.field.path,
                                              literal_to_string(n.value));
            } else if constexpr (std::is_same_v<T, query_ns::InNode>) {
                for (const auto& elem : n.values.elements) {
                    if (file_bloom_may_contain(ctx, n.field.path,
                                               literal_to_string(elem))) {
                        return true;
                    }
                }
                return false;
            } else if constexpr (std::is_same_v<T, query_ns::AndNode>) {
                return file_may_match(*n.left, ctx) &&
                       file_may_match(*n.right, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::OrNode>) {
                return file_may_match(*n.left, ctx) ||
                       file_may_match(*n.right, ctx);
            } else {
                return true;
            }
        },
        node.data);
}

// Dimensions named by equality-shaped leaves, the only ones tier 0 probes.
void collect_bloom_dimensions(const query_ns::QueryNode& node,
                              std::unordered_set<std::string>& out) {
    std::visit(
        [&out](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, query_ns::CompareNode>) {
                if (n.op == query_ns::CompareOp::EQ) out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, query_ns::InNode>) {
                out.insert(n.field.path);
            } else if constexpr (std::is_same_v<T, query_ns::AndNode> ||
                                 std::is_same_v<T, query_ns::OrNode>) {
                collect_bloom_dimensions(*n.left, out);
                collect_bloom_dimensions(*n.right, out);
            } else if constexpr (std::is_same_v<T, query_ns::NotNode>) {
                collect_bloom_dimensions(*n.operand, out);
            }
        },
        node.data);
}

// Load the file blooms for `query`'s equality dimensions and report whether
// the file survives the file-bloom check.
bool file_survives_file_blooms(PrunerContext& ctx, const IndexDatabase& db,
                               int fid, const query_ns::QueryNode& root) {
    std::unordered_set<std::string> dims;
    collect_bloom_dimensions(root, dims);
    if (dims.empty()) return true;

    std::vector<std::string> dim_list(dims.begin(), dims.end());
    auto blooms = db.query_file_bloom_filters_batch(fid, dim_list);
    for (const auto& [dim, result] : blooms) {
        ctx.file_blooms.emplace(
            dim, ScalableBloomFilter::from_blob(result.bloom_data.data(),
                                                result.bloom_data.size()));
    }
    return file_may_match(root, ctx);
}

// True when the chunk's whole [min, max] range satisfies `op val`, so every
// event in it matches. The dual of range_may_match, which asks whether ANY
// event can.
bool range_all_match(const ChunkMeta& meta, const std::string& dim,
                     query_ns::CompareOp op, const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return false;
    const auto& ds = it->second;
    if (ds.min_value.empty() || ds.max_value.empty()) return false;

    // Corrupt stats (min > max) prove nothing either way.
    if (is_numeric_type(ds.value_type)) {
        try {
            if (std::stod(ds.min_value) > std::stod(ds.max_value)) return false;
        } catch (...) {
            return false;
        }
    }

    switch (op) {
        case query_ns::CompareOp::GT:
            return compare_values(ds.min_value, val, ds.value_type) > 0;
        case query_ns::CompareOp::GE:
            return compare_values(ds.min_value, val, ds.value_type) >= 0;
        case query_ns::CompareOp::LT:
            return compare_values(ds.max_value, val, ds.value_type) < 0;
        case query_ns::CompareOp::LE:
            return compare_values(ds.max_value, val, ds.value_type) <= 0;
        default:
            return false;
    }
}

std::set<std::uint64_t> evaluate_node(const query_ns::QueryNode& node,
                                      PrunerContext& ctx);

// Chunks in which EVERY event matches `node` - the dual of evaluate_node's
// may-match set, and an UNDER-approximation (a chunk left out is not proof of
// a non-match, only of the stats not showing one).
//
// This is what a negation needs. Complementing a may-match set is unsound: a
// chunk holding events on both sides of `dur > 25` is a may-match for the
// operand and still holds matches for its negation, so the difference would
// drop it. Complementing an all-match set is exact, because a chunk is dropped
// only when nothing in it can satisfy the negation.
std::set<std::uint64_t> evaluate_all_match(const query_ns::QueryNode& node,
                                           PrunerContext& ctx);

std::set<std::uint64_t> eval_compare(const query_ns::CompareNode& n,
                                     PrunerContext& ctx) {
    std::set<std::uint64_t> result;
    auto val_str = literal_to_string(n.value);

    if (n.field.path == "name" && n.op == query_ns::CompareOp::EQ) {
        auto contains = ctx.file_contains_name(val_str);
        if (contains.has_value()) {
            if (!*contains) {
                return result;
            }
            auto exact_chunks = ctx.resolve_name_chunks(val_str);
            if (!exact_chunks.empty()) {
                return exact_chunks;
            }
        }
    }

    for (auto ckpt : ctx.all_chunks) {
        auto chunk_it = ctx.chunks.find(ckpt);
        ChunkMeta empty_meta;
        auto& meta =
            (chunk_it != ctx.chunks.end()) ? chunk_it->second : empty_meta;

        if (n.op == query_ns::CompareOp::EQ) {
            auto dict = dict_contains(meta, n.field.path, val_str);
            if (dict.has_value()) {
                if (*dict) result.insert(ckpt);
                continue;
            }
            if (bloom_may_contain(ctx, n.field.path, ckpt, val_str))
                result.insert(ckpt);
        } else if (n.op == query_ns::CompareOp::NE) {
            auto excl = dict_excludes(meta, n.field.path, val_str);
            if (excl.has_value() && *excl) continue;
            result.insert(ckpt);
        } else {
            if (!range_may_match(meta, n.field.path, n.op, val_str)) continue;
            if (n.field.path == "ts") {
                try {
                    auto ts_val = std::stoull(val_str);
                    if (!histogram_has_events(ctx, ckpt, n.op, ts_val))
                        continue;
                } catch (...) {
                }
            }
            result.insert(ckpt);
        }
    }
    return result;
}
std::set<std::uint64_t> eval_in(const query_ns::InNode& n, PrunerContext& ctx) {
    std::set<std::uint64_t> result;

    if (n.field.path == "name") {
        for (const auto& elem : n.values.elements) {
            auto val_str = literal_to_string(elem);
            auto contains = ctx.file_contains_name(val_str);
            if (contains.has_value()) {
                if (!*contains) {
                    continue;
                }
                auto exact_chunks = ctx.resolve_name_chunks(val_str);
                if (!exact_chunks.empty()) {
                    result.insert(exact_chunks.begin(), exact_chunks.end());
                    continue;
                }
            }
        }
        if (!result.empty()) {
            return result;
        }
    }

    for (auto ckpt : ctx.all_chunks) {
        auto chunk_it = ctx.chunks.find(ckpt);
        ChunkMeta empty_meta;
        auto& meta =
            (chunk_it != ctx.chunks.end()) ? chunk_it->second : empty_meta;

        bool may_match = false;
        for (const auto& elem : n.values.elements) {
            auto val_str = literal_to_string(elem);
            auto dict = dict_contains(meta, n.field.path, val_str);
            if (dict.has_value()) {
                if (*dict) {
                    may_match = true;
                    break;
                }
                continue;
            }
            if (bloom_may_contain(ctx, n.field.path, ckpt, val_str)) {
                may_match = true;
                break;
            }
        }
        if (may_match) result.insert(ckpt);
    }
    return result;
}

std::set<std::uint64_t> eval_not_in(const query_ns::NotInNode& n,
                                    PrunerContext& ctx) {
    std::set<std::uint64_t> result;

    for (auto ckpt : ctx.all_chunks) {
        auto chunk_it = ctx.chunks.find(ckpt);
        if (chunk_it == ctx.chunks.end()) {
            // No dictionary, so cannot safely skip for NOT.
            result.insert(ckpt);
            continue;
        }
        auto& meta = chunk_it->second;

        auto dim_it = meta.dim_stats.find(n.field.path);
        if (dim_it == meta.dim_stats.end() ||
            !dim_it->second.has_value_counts_payload()) {
            // No dictionary, so cannot safely skip.
            result.insert(ckpt);
            continue;
        }
        dim_it->second.ensure_value_counts_decoded();
        if (!dim_it->second.value_counts) {
            result.insert(ckpt);
            continue;
        }

        auto& vc = *dim_it->second.value_counts;
        bool all_excluded = true;
        for (const auto& [val, _] : vc) {
            bool in_exclude_list = false;
            for (const auto& elem : n.values.elements) {
                if (literal_to_string(elem) == val) {
                    in_exclude_list = true;
                    break;
                }
            }
            if (!in_exclude_list) {
                all_excluded = false;
                break;
            }
        }
        if (!all_excluded) result.insert(ckpt);
    }
    return result;
}

std::set<std::uint64_t> evaluate_node(const query_ns::QueryNode& node,
                                      PrunerContext& ctx) {
    return std::visit(
        [&ctx](auto&& n) -> std::set<std::uint64_t> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, query_ns::CompareNode>) {
                return eval_compare(n, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::InNode>) {
                return eval_in(n, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::NotInNode>) {
                return eval_not_in(n, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::AndNode>) {
                auto left = evaluate_node(*n.left, ctx);
                auto right = evaluate_node(*n.right, ctx);
                std::set<std::uint64_t> intersection;
                std::set_intersection(
                    left.begin(), left.end(), right.begin(), right.end(),
                    std::inserter(intersection, intersection.begin()));
                return intersection;
            } else if constexpr (std::is_same_v<T, query_ns::OrNode>) {
                auto left = evaluate_node(*n.left, ctx);
                auto right = evaluate_node(*n.right, ctx);
                left.insert(right.begin(), right.end());
                return left;
            } else if constexpr (std::is_same_v<T, query_ns::NotNode>) {
                // A chunk may hold an event matching `not p` unless every
                // event in it matches `p`. Complementing evaluate_node's
                // may-match set instead dropped any chunk holding events on
                // both sides of `p`, which for `not (dur > 25)` was every
                // chunk, so the query returned nothing.
                auto all_match = evaluate_all_match(*n.operand, ctx);
                std::set<std::uint64_t> complement;
                std::set_difference(
                    ctx.all_chunks.begin(), ctx.all_chunks.end(),
                    all_match.begin(), all_match.end(),
                    std::inserter(complement, complement.begin()));
                return complement;
            } else {
                return ctx.all_chunks;
            }
        },
        node.data);
}

std::set<std::uint64_t> all_match_compare(const query_ns::CompareNode& n,
                                          PrunerContext& ctx) {
    std::set<std::uint64_t> result;
    const std::string val = literal_to_string(n.value);
    for (auto ckpt : ctx.all_chunks) {
        auto it = ctx.chunks.find(ckpt);
        if (it == ctx.chunks.end()) continue;  // no stats, nothing provable
        const ChunkMeta& meta = it->second;
        if (n.op == query_ns::CompareOp::EQ) {
            const auto* vc = dict_of(meta, n.field.path);
            if (vc && vc->size() == 1 && vc->count(val) > 0)
                result.insert(ckpt);
        } else if (n.op == query_ns::CompareOp::NE) {
            auto lacks = dict_lacks(meta, n.field.path, val);
            if (lacks.has_value() && *lacks) result.insert(ckpt);
        } else if (range_all_match(meta, n.field.path, n.op, val)) {
            result.insert(ckpt);
        }
    }
    return result;
}

// Every dictionary value is (or is not) in the literal list.
std::set<std::uint64_t> all_match_in(const query_ns::ArrayNode& values,
                                     const std::string& field, bool want_in,
                                     PrunerContext& ctx) {
    std::unordered_set<std::string> wanted;
    for (const auto& elem : values.elements)
        wanted.insert(literal_to_string(elem));

    std::set<std::uint64_t> result;
    for (auto ckpt : ctx.all_chunks) {
        auto it = ctx.chunks.find(ckpt);
        if (it == ctx.chunks.end()) continue;
        const auto* vc = dict_of(it->second, field);
        if (!vc || vc->empty()) continue;
        bool all = true;
        for (const auto& [value, count] : *vc) {
            (void)count;
            if ((wanted.count(std::string(value)) > 0) != want_in) {
                all = false;
                break;
            }
        }
        if (all) result.insert(ckpt);
    }
    return result;
}

std::set<std::uint64_t> evaluate_all_match(const query_ns::QueryNode& node,
                                           PrunerContext& ctx) {
    return std::visit(
        [&ctx](auto&& n) -> std::set<std::uint64_t> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, query_ns::CompareNode>) {
                return all_match_compare(n, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::InNode>) {
                return all_match_in(n.values, n.field.path, true, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::NotInNode>) {
                return all_match_in(n.values, n.field.path, false, ctx);
            } else if constexpr (std::is_same_v<T, query_ns::AndNode>) {
                auto left = evaluate_all_match(*n.left, ctx);
                auto right = evaluate_all_match(*n.right, ctx);
                std::set<std::uint64_t> intersection;
                std::set_intersection(
                    left.begin(), left.end(), right.begin(), right.end(),
                    std::inserter(intersection, intersection.begin()));
                return intersection;
            } else if constexpr (std::is_same_v<T, query_ns::OrNode>) {
                // An under-approximation: a chunk where every event matches
                // either side also has every event matching the disjunction,
                // but so may one where the two cover it between them.
                auto left = evaluate_all_match(*n.left, ctx);
                auto right = evaluate_all_match(*n.right, ctx);
                left.insert(right.begin(), right.end());
                return left;
            } else if constexpr (std::is_same_v<T, query_ns::NotNode>) {
                // Every event matches `not p` exactly when no event matches
                // `p`, which is what evaluate_node's may-match set rules out.
                auto may = evaluate_node(*n.operand, ctx);
                std::set<std::uint64_t> complement;
                std::set_difference(
                    ctx.all_chunks.begin(), ctx.all_chunks.end(), may.begin(),
                    may.end(), std::inserter(complement, complement.begin()));
                return complement;
            } else {
                // A pattern match proves nothing from chunk stats.
                return {};
            }
        },
        node.data);
}

}  // namespace

coro::CoroTask<ChunkPrunerOutput> ChunkPrunerUtility::operator()(
    const ChunkPrunerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("prune chunks");
    auto do_query = [&input]() -> ChunkPrunerOutput {
        ChunkPrunerOutput out;
        out.success = false;
        out.file_may_match = false;

        try {
            std::optional<IndexDatabase> owned_db;
            IndexDatabase* db_ptr = input.external_db;
            if (!db_ptr) {
                owned_db.emplace(input.index_path,
                                 dftracer::utils::utilities::indexer::
                                     IndexOpenMode::ReadOnly);
                db_ptr = &*owned_db;
            }
            IndexDatabase& idx_db = *db_ptr;
            int fid =
                idx_db.get_file_info_id(get_logical_path(input.file_path));
            if (fid < 0) {
                out.success = true;
                out.file_may_match = true;
                return out;
            }

            PrunerContext ctx;
            ctx.file_info_id = fid;
            ctx.cache = input.cache;
            ctx.index_path = input.index_path;
            ctx.db = &idx_db;
            ctx.fid = fid;

            if (!file_survives_file_blooms(ctx, idx_db, fid,
                                           input.query.root())) {
                out.success = true;
                out.file_may_match = false;
                return out;
            }

            auto dim_stats_rows = idx_db.query_chunk_dimension_stats(fid);
            for (const auto& ds : dim_stats_rows) {
                ctx.all_chunks.insert(ds.checkpoint_idx);
                ctx.chunks[ds.checkpoint_idx].dim_stats[ds.dimension] = ds;
            }

            auto chunk_stats = idx_db.query_chunk_statistics(fid);
            for (auto& row : chunk_stats) {
                if (!row.stats.timestamp_histogram.empty()) {
                    ctx.ts_histograms[row.checkpoint_idx] =
                        std::move(row.stats.timestamp_histogram);
                }
            }

            auto indexed_dims = idx_db.query_index_dimensions(fid);
            auto all_chunk_blooms =
                idx_db.query_chunk_bloom_filters_batch(fid, indexed_dims);

            for (const auto& [dim, chunk_blooms] : all_chunk_blooms) {
                for (const auto& cb : chunk_blooms) {
                    ctx.all_chunks.insert(cb.checkpoint_idx);
                    ScalableBloomFilter bf = ScalableBloomFilter::from_blob(
                        cb.bloom_data.data(), cb.bloom_data.size());
                    if (input.cache) {
                        input.cache->put(input.index_path, dim,
                                         cb.checkpoint_idx, bf);
                    }
                    ctx.bloom_filters[dim][cb.checkpoint_idx] = std::move(bf);
                }
            }

            ctx.total_chunks =
                ctx.all_chunks.empty() ? 0 : *ctx.all_chunks.rbegin() + 1;
            out.total_checkpoints = ctx.total_chunks;

            if (ctx.all_chunks.empty()) {
                out.file_may_match = true;
                out.success = true;
                return out;
            }

            auto candidates = evaluate_node(input.query.root(), ctx);

            out.candidate_checkpoints.assign(candidates.begin(),
                                             candidates.end());
            out.file_may_match = !out.candidate_checkpoints.empty();
            out.success = true;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN(
                "ChunkPruner: error for %s: %s, assuming match",
                input.file_path.c_str(), e.what());
            out.file_may_match = true;
            out.success = true;
        }

        return out;
    };

    co_return do_query();
}

Result<ChunkPrunerBatchOutput> ChunkPrunerUtility::process_batch(
    const ChunkPrunerBatchInput& input) {
    ChunkPrunerBatchOutput batch_out;
    batch_out.outputs.resize(input.items.size());

    try {
        std::optional<IndexDatabase> owned_db;
        IndexDatabase* db_ptr = input.external_db;
        if (!db_ptr) {
            owned_db.emplace(
                input.index_path,
                dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
            db_ptr = &*owned_db;
        }
        IndexDatabase& idx_db = *db_ptr;

        std::vector<int> fids;
        fids.reserve(input.items.size());
        std::vector<int> item_to_fid(input.items.size(), -1);
        for (std::size_t i = 0; i < input.items.size(); ++i) {
            int fid = idx_db.get_file_info_id(
                get_logical_path(input.items[i].file_path));
            item_to_fid[i] = fid;
            if (fid >= 0) fids.push_back(fid);
        }

        // One RocksDB column-family scan per field here, instead of one
        // per file, is why fids are batched before the per-file loop.
        auto all_dim_stats = idx_db.query_chunk_dimension_stats_batch(fids);
        auto all_chunk_stats = idx_db.query_chunk_statistics_batch(fids);

        for (std::size_t i = 0; i < input.items.size(); ++i) {
            const auto& item = input.items[i];
            auto& out = batch_out.outputs[i];
            out.success = false;
            out.file_may_match = false;

            int fid = item_to_fid[i];
            if (fid < 0) {
                out.success = true;
                out.file_may_match = true;
                continue;
            }

            try {
                PrunerContext ctx;
                ctx.file_info_id = fid;
                ctx.cache = input.cache;
                ctx.index_path = input.index_path;
                ctx.db = &idx_db;
                ctx.fid = fid;

                if (!file_survives_file_blooms(ctx, idx_db, fid,
                                               item.query.root())) {
                    out.success = true;
                    out.file_may_match = false;
                    continue;
                }

                auto dim_it = all_dim_stats.find(fid);
                if (dim_it != all_dim_stats.end()) {
                    for (const auto& ds : dim_it->second) {
                        ctx.all_chunks.insert(ds.checkpoint_idx);
                        ctx.chunks[ds.checkpoint_idx].dim_stats[ds.dimension] =
                            ds;
                    }
                }

                auto cs_it = all_chunk_stats.find(fid);
                if (cs_it != all_chunk_stats.end()) {
                    for (auto& row : cs_it->second) {
                        if (!row.stats.timestamp_histogram.empty()) {
                            ctx.ts_histograms[row.checkpoint_idx] =
                                std::move(row.stats.timestamp_histogram);
                        }
                    }
                }

                auto indexed_dims = idx_db.query_index_dimensions(fid);
                auto all_chunk_blooms =
                    idx_db.query_chunk_bloom_filters_batch(fid, indexed_dims);
                for (const auto& [dim, chunk_blooms] : all_chunk_blooms) {
                    for (const auto& cb : chunk_blooms) {
                        ctx.all_chunks.insert(cb.checkpoint_idx);
                        ScalableBloomFilter bf = ScalableBloomFilter::from_blob(
                            cb.bloom_data.data(), cb.bloom_data.size());
                        if (input.cache) {
                            input.cache->put(input.index_path, dim,
                                             cb.checkpoint_idx, bf);
                        }
                        ctx.bloom_filters[dim][cb.checkpoint_idx] =
                            std::move(bf);
                    }
                }

                ctx.total_chunks =
                    ctx.all_chunks.empty() ? 0 : *ctx.all_chunks.rbegin() + 1;
                out.total_checkpoints = ctx.total_chunks;

                if (ctx.all_chunks.empty()) {
                    out.file_may_match = true;
                    out.success = true;
                    continue;
                }

                auto candidates = evaluate_node(item.query.root(), ctx);
                out.candidate_checkpoints.assign(candidates.begin(),
                                                 candidates.end());
                out.file_may_match = !out.candidate_checkpoints.empty();
                out.success = true;
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN(
                    "ChunkPruner: error for %s: %s, assuming match",
                    item.file_path.c_str(), e.what());
                out.file_may_match = true;
                out.success = true;
            }
        }

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("ChunkPruner: batch error for index %s: %s",
                                input.index_path.c_str(), e.what());
        return make_error(ErrorCode::INDEXER,
                          "ChunkPruner: batch error for index " +
                              input.index_path + ": " + e.what());
    }

    return batch_out;
}

}  // namespace dftracer::utils::trace::indexing
