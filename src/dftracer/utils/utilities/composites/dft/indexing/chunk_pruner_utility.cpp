#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/sqlite/async.h>
#include <dftracer/utils/utilities/common/query/ast.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace dftracer::utils::utilities::composites::dft::indexing {

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;
namespace query_ns = common::query;

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

struct PrunerContext {
    int file_info_id;
    std::uint64_t total_chunks;
    std::set<std::uint64_t> all_chunks;
    std::unordered_map<std::uint64_t, ChunkMeta> chunks;
    std::unordered_map<std::string,
                       std::unordered_map<std::uint64_t, BloomFilter>>
        bloom_filters;

    // Hash resolution: human-readable value → hash strings
    std::unordered_map<std::string, std::vector<std::string>> hash_cache;
    const sqlite::SqliteDatabase* db = nullptr;
    int fid = -1;

    BloomFilterCache* cache;
    std::string idx_path;

    // Resolve a value for a hash dimension.
    // Returns the hash strings if the dimension is a hash dim and
    // the value doesn't look like a hash already.
    const std::vector<std::string>& resolve_hashes(const std::string& dim,
                                                   const std::string& val) {
        static const std::vector<std::string> empty;
        if (!HASH_DIMENSIONS.count(dim) || looks_like_hash(val)) return empty;

        auto key = dim + ":" + val;
        auto it = hash_cache.find(key);
        if (it != hash_cache.end()) return it->second;

        if (db) {
            auto hashes = queries::query_hash_by_resolved(*db, dim, val);
            auto& cached = hash_cache[key];
            cached = std::move(hashes);
            return cached;
        }
        return empty;
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

// Tier 1: Check if value exists in dictionary (exact)
// Returns: true = definitely present, false = definitely absent
// nullopt = dictionary unavailable
std::optional<bool> dict_contains(const ChunkMeta& meta, const std::string& dim,
                                  const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return std::nullopt;
    if (!it->second.value_counts) return std::nullopt;
    return it->second.value_counts->count(val) > 0;
}

// Tier 1: Check if all values in chunk are NOT equal to val
// Returns true if chunk can be safely skipped for != query
std::optional<bool> dict_excludes(const ChunkMeta& meta, const std::string& dim,
                                  const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return std::nullopt;
    if (!it->second.value_counts) return std::nullopt;
    auto& vc = *it->second.value_counts;
    // If the only value in the chunk IS val, all events match val
    // → no events match != val → skip
    if (vc.size() == 1 && vc.count(val) > 0) return true;
    return false;
}

bool is_numeric_type(const std::string& vtype) {
    return vtype == "uint" || vtype == "int" || vtype == "double";
}

int compare_values(const std::string& a, const std::string& b,
                   const std::string& vtype) {
    if (is_numeric_type(vtype)) {
        try {
            double da = std::stod(a);
            double db = std::stod(b);
            if (da < db) return -1;
            if (da > db) return 1;
            return 0;
        } catch (...) {
            // Fall through to string comparison
        }
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Tier 2: Range check against min/max
bool range_may_match(const ChunkMeta& meta, const std::string& dim,
                     query_ns::CompareOp op, const std::string& val) {
    auto it = meta.dim_stats.find(dim);
    if (it == meta.dim_stats.end()) return true;
    const auto& ds = it->second;
    if (ds.min_value.empty() && ds.max_value.empty()) return true;

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

// Tier 3: Bloom filter probe with hash resolution
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

// Recursive AST evaluation: returns candidate chunk set
std::set<std::uint64_t> evaluate_node(const query_ns::QueryNode& node,
                                      PrunerContext& ctx);

std::set<std::uint64_t> eval_compare(const query_ns::CompareNode& n,
                                     PrunerContext& ctx) {
    std::set<std::uint64_t> result;
    auto val_str = literal_to_string(n.value);

    for (auto ckpt : ctx.all_chunks) {
        auto chunk_it = ctx.chunks.find(ckpt);
        ChunkMeta empty_meta;
        auto& meta =
            (chunk_it != ctx.chunks.end()) ? chunk_it->second : empty_meta;

        if (n.op == query_ns::CompareOp::EQ) {
            // Tier 1: dictionary
            auto dict = dict_contains(meta, n.field.path, val_str);
            if (dict.has_value()) {
                if (*dict) result.insert(ckpt);
                continue;
            }
            // Tier 3: bloom
            if (bloom_may_contain(ctx, n.field.path, ckpt, val_str))
                result.insert(ckpt);
        } else if (n.op == query_ns::CompareOp::NE) {
            // Tier 1: dictionary exclusivity
            auto excl = dict_excludes(meta, n.field.path, val_str);
            if (excl.has_value() && *excl) continue;
            // Cannot safely skip without dictionary
            result.insert(ckpt);
        } else {
            // Range operators: Tier 2
            if (range_may_match(meta, n.field.path, n.op, val_str))
                result.insert(ckpt);
        }
    }
    return result;
}
std::set<std::uint64_t> eval_in(const query_ns::InNode& n, PrunerContext& ctx) {
    std::set<std::uint64_t> result;

    for (auto ckpt : ctx.all_chunks) {
        auto chunk_it = ctx.chunks.find(ckpt);
        ChunkMeta empty_meta;
        auto& meta =
            (chunk_it != ctx.chunks.end()) ? chunk_it->second : empty_meta;

        bool may_match = false;
        for (const auto& elem : n.values.elements) {
            auto val_str = literal_to_string(elem);
            // Tier 1
            auto dict = dict_contains(meta, n.field.path, val_str);
            if (dict.has_value()) {
                if (*dict) {
                    may_match = true;
                    break;
                }
                continue;
            }
            // Tier 3
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
            // No dictionary → cannot safely skip for NOT
            result.insert(ckpt);
            continue;
        }
        auto& meta = chunk_it->second;

        auto dim_it = meta.dim_stats.find(n.field.path);
        if (dim_it == meta.dim_stats.end() || !dim_it->second.value_counts) {
            // No dictionary — cannot safely skip
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
        // If every value in chunk is in the exclude list, skip
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
                auto inner = evaluate_node(*n.operand, ctx);
                std::set<std::uint64_t> complement;
                std::set_difference(
                    ctx.all_chunks.begin(), ctx.all_chunks.end(), inner.begin(),
                    inner.end(), std::inserter(complement, complement.begin()));
                return complement;
            } else {
                return ctx.all_chunks;
            }
        },
        node.data);
}

}  // namespace

coro::CoroTask<ChunkPrunerOutput> ChunkPrunerUtility::process(
    const ChunkPrunerInput& input) {
    auto do_query = [&input]() -> ChunkPrunerOutput {
        ChunkPrunerOutput out;
        out.success = false;
        out.file_may_match = false;

        try {
            IndexDatabase idx_db(input.idx_path);
            int fid =
                idx_db.get_file_info_id(get_logical_path(input.file_path));
            if (fid < 0) {
                out.success = true;
                out.file_may_match = true;
                return out;
            }

            // Load chunk dimension stats
            auto dim_stats_rows =
                queries::query_chunk_dimension_stats(idx_db.sql_db(), fid);

            PrunerContext ctx;
            ctx.file_info_id = fid;
            ctx.cache = input.cache;
            ctx.idx_path = input.idx_path;
            ctx.db = &idx_db.sql_db();
            ctx.fid = fid;

            for (const auto& ds : dim_stats_rows) {
                ctx.all_chunks.insert(ds.checkpoint_idx);
                ctx.chunks[ds.checkpoint_idx].dim_stats[ds.dimension] = ds;
            }

            // Load bloom filters for all dimensions
            auto indexed_dims =
                queries::query_index_dimensions(idx_db.sql_db(), fid);
            auto all_chunk_blooms = queries::query_chunk_bloom_filters_batch(
                idx_db.sql_db(), fid, indexed_dims);

            for (const auto& [dim, chunk_blooms] : all_chunk_blooms) {
                for (const auto& cb : chunk_blooms) {
                    ctx.all_chunks.insert(cb.checkpoint_idx);
                    BloomFilter bf = BloomFilter::from_blob(
                        cb.bloom_data.data(), cb.bloom_data.size());
                    if (input.cache) {
                        input.cache->put(input.idx_path, dim, cb.checkpoint_idx,
                                         bf);
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

    co_return co_await sqlite::run(do_query);
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
