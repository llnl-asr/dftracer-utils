#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/file_index_data.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::plan {

using index::store::IndexDatabase;
using index::store::internal::get_logical_path;

namespace {

// Chunk i is gzip member i; every member is a candidate until a Condition
// rules it out, whether or not the index holds data for it.
void load_chunks(FileIndexData& ctx) {
    const auto spans = ctx.db->query_chunk_spans(ctx.fid);
    ctx.total_chunks = spans.size();
    for (std::uint64_t i = 0; i < spans.size(); ++i) {
        ctx.all_chunks.insert(ctx.all_chunks.end(), i);
        if (spans[i].first_line_num > 0 &&
            spans[i].last_line_num >= spans[i].first_line_num)
            ctx.chunk_lines[i] =
                spans[i].last_line_num - spans[i].first_line_num + 1;
    }
}

namespace q = dftracer::utils::query;

// Array positions an any() leaf is expanded over, per file; wider arrays keep
// the leaf as is, which prunes nothing, to bound the work per chunk.
constexpr std::size_t MAX_ANY_POSITIONS = 256;

bool is_position(std::string_view digits) {
    return !digits.empty() &&
           std::all_of(digits.begin(), digits.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

using Positions = std::map<std::string, std::vector<std::string>>;

// For each any() path, the catalog paths `<path>.<k>` in position order
// (`args.<path>.<k>` for a dftracer arg written without the prefix).
Positions any_positions(const IndexDatabase& db, int fid, const Query& query) {
    Positions out;
    std::vector<std::pair<std::uint64_t, std::string>> found;
    const auto catalog = db.catalog(fid);
    for (const std::string& path : query.any_paths()) {
        found.clear();
        for (const std::string& base : {path, "args." + path}) {
            for (const auto& [p, stat] : catalog) {
                if (p.size() <= base.size() + 1 || !p.starts_with(base) ||
                    p[base.size()] != '.')
                    continue;
                const std::string_view k =
                    std::string_view(p).substr(base.size() + 1);
                if (is_position(k))
                    found.emplace_back(std::stoull(std::string(k)), p);
            }
            if (!found.empty() || path.starts_with("args.")) break;
        }
        std::sort(found.begin(), found.end());
        auto& paths = out[path];
        for (auto& [k, p] : found) paths.push_back(std::move(p));
    }
    return out;
}

template <class Leaf>
q::QueryNodePtr expand_leaf(const Leaf& leaf, const Positions& positions) {
    if (!leaf.field.any) return q::make_node(Leaf(leaf));
    const auto& paths = positions.at(leaf.field.path);
    if (paths.size() > MAX_ANY_POSITIONS) return q::make_node(Leaf(leaf));
    auto at = [&](const std::string& path) {
        Leaf copy(leaf);
        copy.field = q::FieldNode{path};
        return q::make_node(std::move(copy));
    };
    // No position: the plain path, which the catalog check rules out.
    if (paths.empty()) return at(leaf.field.path);
    q::QueryNodePtr out = at(paths.front());
    for (std::size_t i = 1; i < paths.size(); ++i)
        out = q::make_node(q::OrNode{std::move(out), at(paths[i])});
    return out;
}

q::QueryNodePtr expand(const q::QueryNode& node, const Positions& positions) {
    return std::visit(
        [&](auto&& n) -> q::QueryNodePtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q::AndNode>) {
                return q::make_node(q::AndNode{expand(*n.left, positions),
                                               expand(*n.right, positions)});
            } else if constexpr (std::is_same_v<T, q::OrNode>) {
                return q::make_node(q::OrNode{expand(*n.left, positions),
                                              expand(*n.right, positions)});
            } else if constexpr (std::is_same_v<T, q::NotNode>) {
                return q::make_node(q::NotNode{expand(*n.operand, positions)});
            } else {
                return expand_leaf(n, positions);
            }
        },
        node.data);
}

// The query with every any() leaf replaced by the OR of the leaf on each
// array position the file's catalog holds, which matches exactly the same
// records; nullopt when the query has no any() leaf.
std::optional<Query> expand_any(const IndexDatabase& db, int fid,
                                const Query& query) {
    if (query.any_paths().empty()) return std::nullopt;
    const q::QueryNodePtr root =
        expand(query.root(), any_positions(db, fid, query));
    return q::parse_or_throw(q::to_string(*root));
}

ChunkPrunerOutput prune_file_chunks(const IndexDatabase& db,
                                    const std::string& file_path,
                                    const Query& query_in) {
    ChunkPrunerOutput out;
    try {
        const int fid = db.get_file_info_id(get_logical_path(file_path));
        out.success = true;
        out.file_may_match = true;
        if (fid < 0) return out;

        const std::optional<Query> expanded = expand_any(db, fid, query_in);
        const Query& query = expanded ? *expanded : query_in;

        FileIndexData ctx;
        ctx.db = &db;
        ctx.fid = fid;
        auto conds = make_query_conditions(ctx);
        if (!file_may_match(query.root(), conds)) {
            out.file_may_match = false;
            return out;
        }

        load_chunks(ctx);
        out.total_checkpoints = ctx.total_chunks;
        if (ctx.all_chunks.empty()) return out;

        auto candidates = evaluate(query.root(), conds, ctx.all_chunks);
        out.candidate_checkpoints.assign(candidates.begin(), candidates.end());
        out.file_may_match = !out.candidate_checkpoints.empty();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("ChunkPruner: error for %s: %s, assuming match",
                                file_path.c_str(), e.what());
        out = ChunkPrunerOutput{};
        out.success = true;
        out.file_may_match = true;
    }
    return out;
}

}  // namespace

namespace {

// What `own`, one extension's Conditions, rules out on its own.
ChunkExplanation::ByExtension explain_alone(const Query& query,
                                            std::string name, Conditions own,
                                            const FileIndexData& ctx) {
    ChunkExplanation::ByExtension by{std::move(name), false, {}};
    by.file_ruled_out = !file_may_match(query.root(), own);
    if (!by.file_ruled_out) {
        auto kept = evaluate(query.root(), own, ctx.all_chunks);
        for (auto c : ctx.all_chunks)
            if (!kept.contains(c)) by.removed.push_back(c);
    }
    return by;
}

}  // namespace

ChunkExplanation explain_file_chunks(const IndexDatabase& db,
                                     const std::string& file_path,
                                     const Query& query_in) {
    ChunkExplanation out;
    const int fid = db.get_file_info_id(get_logical_path(file_path));
    if (fid < 0) return out;
    out.indexed = true;
    const std::optional<Query> expanded = expand_any(db, fid, query_in);
    const Query& query = expanded ? *expanded : query_in;

    FileIndexData ctx;
    ctx.db = &db;
    ctx.fid = fid;
    auto conds = make_query_conditions(ctx);
    load_chunks(ctx);
    out.total_chunks = ctx.total_chunks;

    out.file_may_match = file_may_match(query.root(), conds);
    if (out.file_may_match) {
        auto read = evaluate(query.root(), conds, ctx.all_chunks);
        out.read.assign(read.begin(), read.end());
        out.file_may_match = ctx.all_chunks.empty() || !out.read.empty();
    }

    for (auto ext : index::store::ALL_EXTENSIONS) {
        if (!index::store::PRUNING_EXTENSIONS.has(ext) ||
            !db.extension_current(fid, ext))
            continue;
        Conditions own;
        for (auto& c : conds)
            if (c && c->extension() == ext) own.push_back(std::move(c));
        out.by_extension.push_back(
            explain_alone(query, std::string(index::store::extension_name(ext)),
                          std::move(own), ctx));
    }
    for (auto& c : conds) {
        if (!c || c->extension() != index::store::IndexExtension::PLUGIN)
            continue;
        std::string name = c->name();
        Conditions own;
        own.push_back(std::move(c));
        out.by_extension.push_back(
            explain_alone(query, std::move(name), std::move(own), ctx));
    }
    return out;
}

coro::CoroTask<ChunkPrunerOutput> ChunkPruner::operator()(
    const ChunkPrunerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("prune chunks");
    std::optional<IndexDatabase> owned_db;
    try {
        if (!input.external_db)
            owned_db.emplace(input.index_path,
                             index::store::IndexOpenMode::ReadOnly);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("ChunkPruner: error for %s: %s, assuming match",
                                input.file_path.c_str(), e.what());
        ChunkPrunerOutput out;
        out.success = true;
        out.file_may_match = true;
        co_return out;
    }
    const IndexDatabase& db =
        input.external_db ? *input.external_db : *owned_db;
    co_return prune_file_chunks(db, input.file_path, input.query);
}

Result<ChunkPrunerBatchOutput> ChunkPruner::process_batch(
    const ChunkPrunerBatchInput& input) {
    ChunkPrunerBatchOutput batch_out;
    try {
        std::optional<IndexDatabase> owned_db;
        if (!input.external_db)
            owned_db.emplace(input.index_path,
                             index::store::IndexOpenMode::ReadOnly);
        const IndexDatabase& db =
            input.external_db ? *input.external_db : *owned_db;
        batch_out.outputs.reserve(input.items.size());
        for (const auto& item : input.items)
            batch_out.outputs.push_back(
                prune_file_chunks(db, item.file_path, item.query));
    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::INDEXER,
            std::string("ChunkPruner: batch failed: ") + e.what());
    }
    return batch_out;
}

}  // namespace dftracer::utils::index::plan
