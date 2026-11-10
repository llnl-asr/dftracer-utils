// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/hash_map.h>
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

dstd::Result<bool> indexed_by_path(const Snapshot& snapshot,
                                   const FileRef& file) {
    const auto id = index::schema(snapshot, file);
    if (!id) return id.error();
    if (!*id) return false;
    const auto schema = find_schema(**id);
    if (!schema) return schema.error();
    return *schema && (*schema)->decoder == Decoder::Path;
}

// Chunk i is gzip member i; every member is a candidate until a Condition
// rules it out, whether or not the index holds data for it.
dstd::Result<void> load_chunks(FileIndexData& ctx) {
    const auto loaded = index::spans(*ctx.snapshot, *ctx.file);
    if (!loaded) return loaded.error();
    const auto& spans = *loaded;
    ctx.total_chunks = spans.size();
    for (std::uint64_t i = 0; i < spans.size(); ++i) {
        ctx.all_chunks.insert(ctx.all_chunks.end(), i);
        if (spans[i].first_line_num > 0 &&
            spans[i].last_line_num >= spans[i].first_line_num)
            ctx.chunk_lines[i] =
                spans[i].last_line_num - spans[i].first_line_num + 1;
    }
    return {};
}

// Array positions an any() leaf is expanded over, per file; wider arrays keep
// the leaf as is, which prunes nothing, to bound the work per chunk.
constexpr std::size_t MAX_ANY_POSITIONS = 256;

bool is_position(dstd::StringView digits) {
    return !digits.empty() &&
           std::all_of(digits.begin(), digits.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

using Positions = dstd::HashMap<dstd::String, dstd::Vector<dstd::String>>;

// For each any() path, the catalog paths `<path>.<k>` in position order
// (`args.<path>.<k>` for a dftracer arg written without the prefix).
dstd::Result<Positions> any_positions(const Snapshot& snapshot,
                                      const FileRef& file, const Query& query) {
    Positions out;
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>> found;
    const auto listed = index::catalog(snapshot, file);
    if (!listed) return listed.error();
    for (const dstd::String& path : query.any_paths()) {
        found.clear();
        for (const dstd::String& base : {path, "args." + path}) {
            for (const auto& [p, stat] : *listed) {
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
dstd::Result<dstd::Optional<Query>> expand_any(const Snapshot& snapshot,
                                               const FileRef& file,
                                               const Query& query) {
    if (query.any_paths().empty()) return dstd::Optional<Query>();
    const auto positions = any_positions(snapshot, file, query);
    if (!positions) return positions.error();
    duql::detail::QueryNodePtr root =
        expand(duql::detail::QueryAccess::root(query), *positions);
    return dstd::Optional<Query>(
        duql::detail::QueryAccess::from_node(std::move(root)));
}

dstd::Result<ChunkPrunerOutput> prune_file_chunks(const Snapshot& snapshot,
                                                  const dstd::String& file_path,
                                                  const Query& query_in) {
    ChunkPrunerOutput out;
    const auto file = snapshot.file(get_logical_path(file_path));
    out.success = true;
    out.file_may_match = true;
    if (!file) return out;

    const auto expanded = expand_any(snapshot, *file, query_in);
    if (!expanded) return expanded.error();
    const Query& query = *expanded ? **expanded : query_in;

    FileIndexData ctx;
    ctx.snapshot = &snapshot;
    ctx.file = &*file;
    const auto by_path = indexed_by_path(snapshot, *file);
    if (!by_path.has_value()) return by_path.error();
    ctx.by_path = *by_path;
    auto conds = make_query_conditions(ctx);
    if (!file_may_match(duql::detail::QueryAccess::root(query), conds)) {
        if (ctx.error) return std::move(*ctx.error);
        out.file_may_match = false;
        return out;
    }

    if (auto loaded = load_chunks(ctx); !loaded) return loaded.error();
    out.total_checkpoints = ctx.total_chunks;
    if (ctx.all_chunks.empty()) {
        if (ctx.error) return std::move(*ctx.error);
        return out;
    }

    auto candidates =
        evaluate(duql::detail::QueryAccess::root(query), conds, ctx.all_chunks);
    if (ctx.error) return std::move(*ctx.error);
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

dstd::Result<ChunkExplanation> explain_file_chunks(
    const Snapshot& snapshot, const dstd::String& file_path,
    const Query& query_in) {
    ChunkExplanation out;
    const auto file = snapshot.file(get_logical_path(file_path));
    if (!file) return out;
    out.indexed = true;
    const auto expanded = expand_any(snapshot, *file, query_in);
    if (!expanded) return expanded.error();
    const Query& query = *expanded ? **expanded : query_in;

    FileIndexData ctx;
    ctx.snapshot = &snapshot;
    ctx.file = &*file;
    const auto by_path = indexed_by_path(snapshot, *file);
    if (!by_path.has_value()) return by_path.error();
    ctx.by_path = *by_path;
    auto conds = make_query_conditions(ctx);
    if (auto loaded = load_chunks(ctx); !loaded) return loaded.error();
    out.total_chunks = ctx.total_chunks;

    out.file_may_match =
        file_may_match(duql::detail::QueryAccess::root(query), conds);
    if (out.file_may_match) {
        auto read = evaluate(duql::detail::QueryAccess::root(query), conds,
                             ctx.all_chunks);
        out.read.assign(read.begin(), read.end());
        out.file_may_match = ctx.all_chunks.empty() || !out.read.empty();
    }
    if (ctx.error) return std::move(*ctx.error);

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

coro::CoroTask<dstd::Result<ChunkPrunerOutput>> ChunkPruner::operator()(
    const ChunkPrunerInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("prune chunks");
    dstd::Optional<Snapshot> owned;
    const Snapshot* snapshot = input.snapshot;
    if (!snapshot) {
        auto opened = open_snapshot(input.index_path);
        if (!opened) co_return opened.error();
        owned.emplace(std::move(*opened));
        snapshot = &*owned;
    }
    co_return prune_file_chunks(*snapshot, input.file_path, input.query);
}

dstd::Result<ChunkPrunerBatchOutput> ChunkPruner::process_batch(
    const ChunkPrunerBatchInput& input) {
    ChunkPrunerBatchOutput batch_out;
    dstd::Optional<Snapshot> owned;
    const Snapshot* snapshot = input.snapshot;
    auto failed = [](const dstd::Error& e) {
        return common::make_error<"ChunkPruner: batch failed: {}">(
            common::ErrorCode::Indexer, e.message());
    };
    if (!snapshot) {
        auto opened = open_snapshot(input.index_path);
        if (!opened) return failed(opened.error());
        owned.emplace(std::move(*opened));
        snapshot = &*owned;
    }
    batch_out.outputs.reserve(input.items.size());
    for (const auto& item : input.items) {
        auto pruned = prune_file_chunks(*snapshot, item.file_path, item.query);
        if (!pruned) return failed(pruned.error());
        batch_out.outputs.push_back(std::move(*pruned));
    }
    return batch_out;
}

}  // namespace dftracer::utils::index::plan
