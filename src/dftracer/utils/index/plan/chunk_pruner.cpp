// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/index/plan/file_index_data.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>

namespace dftracer::utils::index::plan {

using index::store::internal::get_logical_path;

namespace {

bool indexed_by_path(const Snapshot& snapshot, const FileRef& file) {
    const auto id = common::unwrap(index::schema(snapshot, file));
    const RecordSchema* schema = id ? find_schema(*id) : nullptr;
    return schema && schema->decoder == Decoder::Path;
}

// Chunk i is gzip member i; every member is a candidate until a Condition
// rules it out, whether or not the index holds data for it.
void load_chunks(FileIndexData& ctx) {
    const auto spans = common::unwrap(index::spans(*ctx.snapshot, *ctx.file));
    ctx.total_chunks = spans.size();
    for (std::uint64_t i = 0; i < spans.size(); ++i) {
        ctx.all_chunks.insert(ctx.all_chunks.end(), i);
        if (spans[i].first_line_num > 0 &&
            spans[i].last_line_num >= spans[i].first_line_num)
            ctx.chunk_lines[i] =
                spans[i].last_line_num - spans[i].first_line_num + 1;
    }
}

// Array positions an any() leaf is expanded over, per file; wider arrays keep
// the leaf as is, which prunes nothing, to bound the work per chunk.
constexpr std::size_t MAX_ANY_POSITIONS = 256;

bool is_position(dstd::StringView digits) {
    return !digits.empty() &&
           std::all_of(digits.begin(), digits.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

using Positions = dstd::Map<dstd::String, dstd::Vector<dstd::String>>;

// For each any() path, the catalog paths `<path>.<k>` in position order
// (`args.<path>.<k>` for a dftracer arg written without the prefix).
Positions any_positions(const Snapshot& snapshot, const FileRef& file,
                        const Query& query) {
    Positions out;
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>> found;
    const auto catalog = common::unwrap(index::catalog(snapshot, file));
    for (const dstd::String& path : query.any_paths()) {
        found.clear();
        for (const dstd::String& base : {path, "args." + path}) {
            for (const auto& [p, stat] : catalog) {
                if (p.size() <= base.size() + 1 || !p.starts_with(base) ||
                    p[base.size()] != '.')
                    continue;
                const dstd::StringView k =
                    dstd::StringView(p).substr(base.size() + 1);
                if (!is_position(k)) continue;
                if (const auto position = dstd::from_chars<std::uint64_t>(k))
                    found.emplace_back(*position,
                                       dstd::String(p.data(), p.size()));
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
duql::detail::QueryNodePtr expand_field_leaf(const Leaf& leaf,
                                             const Positions& positions) {
    if (!leaf.field.any) return duql::detail::make_node(Leaf(leaf));
    const auto& paths = positions.at(leaf.field.path);
    if (paths.size() > MAX_ANY_POSITIONS)
        return duql::detail::make_node(Leaf(leaf));
    auto at = [&](const dstd::String& path) {
        Leaf copy(leaf);
        copy.field = duql::detail::FieldNode{path};
        return duql::detail::make_node(std::move(copy));
    };
    // No position: the plain path, which the catalog check rules out.
    if (paths.empty()) return at(leaf.field.path);
    duql::detail::QueryNodePtr out = at(paths.front());
    for (std::size_t i = 1; i < paths.size(); ++i)
        out = duql::detail::make_node(
            duql::detail::OrNode{std::move(out), at(paths[i])});
    return out;
}

template <class Leaf>
duql::detail::QueryNodePtr expand_leaf(const Leaf& leaf,
                                       const Positions& positions) {
    if constexpr (std::is_same_v<Leaf, duql::detail::ExprLeaf>) {
        return duql::detail::make_node(Leaf(leaf));
    } else {
        return expand_field_leaf(leaf, positions);
    }
}

duql::detail::QueryNodePtr expand(const duql::detail::QueryNode& node,
                                  const Positions& positions) {
    return std::visit(
        [&](auto&& n) -> duql::detail::QueryNodePtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::detail::AndNode>) {
                return duql::detail::make_node(duql::detail::AndNode{
                    expand(*n.left, positions), expand(*n.right, positions)});
            } else if constexpr (std::is_same_v<T, duql::detail::OrNode>) {
                return duql::detail::make_node(duql::detail::OrNode{
                    expand(*n.left, positions), expand(*n.right, positions)});
            } else if constexpr (std::is_same_v<T, duql::detail::NotNode>) {
                return duql::detail::make_node(
                    duql::detail::NotNode{expand(*n.operand, positions)});
            } else {
                return expand_leaf(n, positions);
            }
        },
        node.data);
}

// The query with every any() leaf replaced by the OR of the leaf on each
// array position the file's catalog holds, which matches exactly the same
// records; nullopt when the query has no any() leaf.
dstd::Optional<Query> expand_any(const Snapshot& snapshot, const FileRef& file,
                                 const Query& query) {
    if (query.any_paths().empty()) return dstd::nullopt;
    duql::detail::QueryNodePtr root =
        expand(duql::detail::QueryAccess::root(query),
               any_positions(snapshot, file, query));
    return duql::detail::QueryAccess::from_node(std::move(root));
}

ChunkPrunerOutput prune_file_chunks(const Snapshot& snapshot,
                                    const dstd::String& file_path,
                                    const Query& query_in) {
    ChunkPrunerOutput out;
    const auto file = snapshot.file(get_logical_path(file_path));
    out.success = true;
    out.file_may_match = true;
    if (!file) return out;

    const dstd::Optional<Query> expanded =
        expand_any(snapshot, *file, query_in);
    const Query& query = expanded ? *expanded : query_in;

    FileIndexData ctx;
    ctx.snapshot = &snapshot;
    ctx.file = &*file;
    ctx.by_path = indexed_by_path(snapshot, *file);
    auto conds = make_query_conditions(ctx);
    if (!file_may_match(duql::detail::QueryAccess::root(query), conds)) {
        out.file_may_match = false;
        return out;
    }

    load_chunks(ctx);
    out.total_checkpoints = ctx.total_chunks;
    if (ctx.all_chunks.empty()) return out;

    auto candidates =
        evaluate(duql::detail::QueryAccess::root(query), conds, ctx.all_chunks);
    out.candidate_checkpoints.assign(candidates.begin(), candidates.end());
    out.file_may_match = !out.candidate_checkpoints.empty();
    return out;
}

}  // namespace

namespace {

// What `own`, one extension's Conditions, rules out on its own.
ChunkExplanation::ByExtension explain_alone(const Query& query,
                                            dstd::String name, Conditions own,
                                            const FileIndexData& ctx) {
    ChunkExplanation::ByExtension by{std::move(name), false, {}};
    by.file_ruled_out =
        !file_may_match(duql::detail::QueryAccess::root(query), own);
    if (!by.file_ruled_out) {
        auto kept = evaluate(duql::detail::QueryAccess::root(query), own,
                             ctx.all_chunks);
        for (auto c : ctx.all_chunks)
            if (!kept.contains(c)) by.removed.push_back(c);
    }
    return by;
}

}  // namespace

ChunkExplanation explain_file_chunks(const Snapshot& snapshot,
                                     const dstd::String& file_path,
                                     const Query& query_in) {
    ChunkExplanation out;
    const auto file = snapshot.file(get_logical_path(file_path));
    if (!file) return out;
    out.indexed = true;
    const dstd::Optional<Query> expanded =
        expand_any(snapshot, *file, query_in);
    const Query& query = expanded ? *expanded : query_in;

    FileIndexData ctx;
    ctx.snapshot = &snapshot;
    ctx.file = &*file;
    ctx.by_path = indexed_by_path(snapshot, *file);
    auto conds = make_query_conditions(ctx);
    load_chunks(ctx);
    out.total_chunks = ctx.total_chunks;

    out.file_may_match =
        file_may_match(duql::detail::QueryAccess::root(query), conds);
    if (out.file_may_match) {
        auto read = evaluate(duql::detail::QueryAccess::root(query), conds,
                             ctx.all_chunks);
        out.read.assign(read.begin(), read.end());
        out.file_may_match = ctx.all_chunks.empty() || !out.read.empty();
    }

    for (auto ext : index::store::ALL_EXTENSIONS) {
        if (!index::store::PRUNING_EXTENSIONS.has(ext) ||
            !index::extension_current(snapshot, *file, ext))
            continue;
        Conditions own;
        for (auto& c : conds)
            if (c && c->extension() == ext) own.push_back(std::move(c));
        out.by_extension.push_back(explain_alone(
            query, dstd::String(index::store::extension_name(ext)),
            std::move(own), ctx));
    }
    for (auto& c : conds) {
        if (!c || c->extension() != index::store::IndexExtension::Plugin)
            continue;
        dstd::String name = c->name();
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
    dstd::Optional<Snapshot> owned;
    const Snapshot* snapshot = input.snapshot;
    if (!snapshot) {
        owned.emplace(common::unwrap(open_snapshot(input.index_path)));
        snapshot = &*owned;
    }
    co_return prune_file_chunks(*snapshot, input.file_path, input.query);
}

dstd::Result<ChunkPrunerBatchOutput> ChunkPruner::process_batch(
    const ChunkPrunerBatchInput& input) {
    ChunkPrunerBatchOutput batch_out;
    try {
        dstd::Optional<Snapshot> owned;
        const Snapshot* snapshot = input.snapshot;
        if (!snapshot) {
            owned.emplace(common::unwrap(open_snapshot(input.index_path)));
            snapshot = &*owned;
        }
        batch_out.outputs.reserve(input.items.size());
        for (const auto& item : input.items)
            batch_out.outputs.push_back(
                prune_file_chunks(*snapshot, item.file_path, item.query));
    } catch (const std::exception& e) {
        return common::make_error(
            common::ErrorCode::Indexer,
            dstd::String("ChunkPruner: batch failed: ") + e.what());
    }
    return batch_out;
}

}  // namespace dftracer::utils::index::plan
