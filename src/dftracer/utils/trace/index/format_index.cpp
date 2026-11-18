// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/fileio/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/trace/index/extensions/column_feeder.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>
#include <dftracer/utils/trace/index/format_index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>

namespace dftracer::utils::trace::index {

namespace {

using extensions::BloomParams;
using extensions::ExtensionLeaf;
using extensions::ExtensionOp;
using extensions::ExtensionValue;
using store::IndexExtension;

constexpr store::FeederKind FEEDER = store::FeederKind::Column;

dstd::Vector<dstd::SharedPtr<const extensions::Summary>> make_summaries(
    const FormatIndexSettings& settings) {
    const build::ChunkIndexerConfig& c = settings.config;
    dstd::Vector<dstd::SharedPtr<const extensions::Summary>> out;
    if (c.extensions.has(IndexExtension::Zonemap))
        out.push_back(dstd::make_shared<extensions::ZonemapSummary>(
            c.params_hash(IndexExtension::Zonemap)));
    if (c.extensions.has(IndexExtension::Bloom))
        out.push_back(dstd::make_shared<extensions::BloomSummary>(
            BloomParams{c.expected_entries_per_chunk, c.false_positive_rate},
            c.params_hash(IndexExtension::Bloom)));
    if (c.extensions.has(IndexExtension::Counts))
        out.push_back(dstd::make_shared<extensions::CountsSummary>(
            c.value_counts_cap, c.params_hash(IndexExtension::Counts)));
    if (c.extensions.has(IndexExtension::Postings))
        out.push_back(dstd::make_shared<extensions::PostingsSummary>(
            c.params_hash(IndexExtension::Postings)));
    return out;
}

dstd::Result<bool> plugin_extensions_current(const Snapshot& snapshot,
                                             const FileRef& file) {
    const auto registered = extensions::registered_extensions();
    if (registered.empty()) return true;
    const auto listed = plugins(snapshot, file);
    if (!listed) return listed.error();
    for (const auto& ext : registered) {
        const PluginCopy* copy = nullptr;
        for (const auto& candidate : *listed)
            if (dstd::StringView(candidate.name.data(),
                                 candidate.name.size()) == ext->name())
                copy = &candidate;
        if (copy == nullptr ||
            !ext->current(store::ExtensionState{
                copy->version, copy->params_hash, copy->ready}))
            return false;
    }
    return true;
}

}  // namespace

std::uint64_t format_params_hash(dstd::StringView format,
                                 store::FeederKind feeder) {
    fileio::hash::Fnv1aHashBuilder hasher;
    hasher.update(format);
    hasher.update_value(static_cast<std::uint8_t>(feeder));
    return hasher.finish();
}

dstd::Result<bool> format_file_current(const Snapshot& snapshot,
                                       dstd::StringView path,
                                       const SourceStat& now,
                                       dstd::StringView format,
                                       dstd::StringView plugin_version,
                                       const FormatIndexSettings& settings) {
    const auto file = snapshot.file(path);
    if (!file) return false;
    if (snapshot.freshness(path, now) != Freshness::Current) return false;

    const auto record = format_record(snapshot, *file);
    if (!record) return record.error();
    if (!*record || (*record)->feeder != FEEDER ||
        (*record)->format != format ||
        (*record)->plugin_version != plugin_version)
        return false;
    const std::uint64_t want = format_params_hash(format, FEEDER);
    const auto state = extension_state(snapshot, *file, IndexExtension::Format);
    if (!state || !state->ready || state->params_hash != want ||
        !extension_current(snapshot, *file, IndexExtension::Format))
        return false;

    for (const auto& summary : make_summaries(settings)) {
        const auto built =
            extension_state(snapshot, *file, summary->extension());
        if (!built || !built->ready ||
            built->params_hash != summary->params_hash() ||
            !extension_current(snapshot, *file, summary->extension()))
            return false;
    }
    return plugin_extensions_current(snapshot, *file);
}

coro::CoroTask<dstd::Result<void>> build_format_index(
    const Index& index, dstd::String path, SourceStat stat,
    const dataframe::FormatFileSource& source,
    const FormatIndexSettings& settings) {
    const auto tokens = source.chunk_tokens();
    const std::uint64_t total = tokens.empty() ? 1 : tokens.size();

    extensions::ColumnFeeder feeder(make_summaries(settings));
    if (!settings.config.path_summaries.empty())
        feeder.set_path_filter(
            [&config = settings.config](dstd::StringView path,
                                        store::IndexExtension ext) {
                return config.builds(path, ext);
            });
    dstd::UniquePtr<extensions::ExtensionBuilders> plugin_builders;
    auto registered = extensions::registered_extensions();
    if (!registered.empty()) {
        plugin_builders = dstd::make_unique<extensions::ExtensionBuilders>(
            std::move(registered));
        feeder.set_plugin_builders(plugin_builders.get());
    }

    std::uint64_t rows = 0;
    for (std::uint64_t granule = 0; granule < total; ++granule) {
        dstd::Vector<std::uint64_t> chunks;
        if (tokens.empty()) {
            for (std::uint64_t i = 0; i < source.chunk_count(); ++i)
                chunks.push_back(i);
        } else {
            chunks.push_back(granule);
        }
        auto lazy = dataframe::LazyFrame::scan(source.only_chunks(chunks));
        auto collecting = lazy.collect();
        auto collected = co_await std::move(collecting);
        if (!collected)
            co_return common::make_error<
                "index {}: reading chunk {} failed: {}">(
                common::ErrorCode::Io, path, granule,
                collected.error().message());
        dataframe::DataFrame frame = std::move(*collected);
        for (auto& column : frame.columns)
            if (column.valid() && !column.is_flat())
                column = column.materialize();
        rows += static_cast<std::uint64_t>(frame.num_rows());
        feeder.feed(granule, frame);
    }
    const auto payloads = feeder.finish();
    if (plugin_builders) plugin_builders->finish();

    auto started = index.build(path, stat);
    if (!started) co_return started.error();
    FileBuild build = std::move(*started);
    feeder.write(build, payloads);
    if (plugin_builders) plugin_builders->write(build);

    store::FormatRecord record;
    record.feeder = FEEDER;
    record.format = dstd::String(source.format_name());
    record.plugin_version = dstd::String(source.plugin_version());
    record.params_hash = format_params_hash(source.format_name(), FEEDER);
    record.tokens.assign(tokens.begin(), tokens.end());
    record.chunk_count = source.chunk_count();
    put_format_record(build, record);
    build.finish(IndexExtension::Format, record.params_hash);
    // A file is in the index when its members copy is, and that copy carries
    // the stat the file is judged stale by. A format file has no gzip member,
    // so the copy holds only the row count.
    put_file_metadata(build, 0, rows, 0, false);
    build.finish(IndexExtension::Members, 0);
    clear_unwritten(build);
    auto committing = build.commit();
    auto committed = co_await std::move(committing);
    if (!committed) co_return committed.error();
    co_return dstd::Result<void>{};
}

namespace {

// A filter leaf the summaries can judge, with the text its value borrows.
struct Leaf {
    dstd::String path;
    ExtensionOp op = ExtensionOp::Eq;
    dataframe::TypeId type = dataframe::TypeId::Unknown;
    ExtensionValue value;
    dstd::String text;
};

bool value_of(const dataframe::Scalar& rhs, Leaf& leaf) {
    switch (rhs.tag()) {
        case DFTU_SCALAR_TAG_I64:
            leaf.value.kind = ExtensionValue::Kind::Int64;
            leaf.value.i = rhs.i64();
            return true;
        case DFTU_SCALAR_TAG_U64:
            leaf.value.kind = ExtensionValue::Kind::UInt64;
            leaf.value.u = rhs.u64();
            return true;
        case DFTU_SCALAR_TAG_F64:
            leaf.value.kind = ExtensionValue::Kind::Double;
            leaf.value.f = rhs.f64();
            return true;
        case DFTU_SCALAR_TAG_STR:
            leaf.text.assign(rhs.str().data(), rhs.str().size());
            leaf.value.kind = ExtensionValue::Kind::String;
            return true;
        default:
            return false;
    }
}

bool op_of(dataframe::CmpOp cmp, ExtensionOp& op) {
    switch (cmp) {
        case dataframe::CmpOp::Gt:
            op = ExtensionOp::Gt;
            return true;
        case dataframe::CmpOp::Ge:
            op = ExtensionOp::Ge;
            return true;
        case dataframe::CmpOp::Lt:
            op = ExtensionOp::Lt;
            return true;
        case dataframe::CmpOp::Le:
            op = ExtensionOp::Le;
            return true;
        case dataframe::CmpOp::Eq:
            op = ExtensionOp::Eq;
            return true;
        case dataframe::CmpOp::Ne:
            return false;
    }
    return false;
}

// The `column <cmp> literal` terms of an AND of filters. An OR, a NOT and any
// other shape give nothing, so they judge no chunk.
void collect_leaves(const dataframe::Expr& e, const FormatScan& scan,
                    dstd::Vector<Leaf>& out) {
    dataframe::LogicalOp logical;
    dataframe::Expr a;
    dataframe::Expr b;
    if (dataframe::expr_as_logical(e, &logical, &a, &b)) {
        if (logical == dataframe::LogicalOp::And) {
            collect_leaves(a, scan, out);
            collect_leaves(b, scan, out);
        }
        return;
    }
    std::int32_t col = -1;
    dataframe::CmpOp cmp;
    dataframe::Scalar rhs;
    if (!dataframe::expr_as_col_cmp(e, &col, &cmp, &rhs)) return;
    Leaf leaf;
    if (!op_of(cmp, leaf.op) || !value_of(rhs, leaf) || col < 0) return;
    const auto at = static_cast<std::size_t>(col);
    if (!scan.projection.empty()) {
        if (at >= scan.projection.size()) return;
        leaf.path = scan.projection[at];
    } else {
        if (!scan.schema || at >= scan.schema->fields.size()) return;
        leaf.path = scan.schema->fields[at].name;
    }
    if (scan.schema)
        for (const auto& field : scan.schema->fields)
            if (field.name == leaf.path) leaf.type = field.type.id;
    out.push_back(std::move(leaf));
}

dstd::Optional<dstd::Vector<std::uint64_t>> candidates_in(
    const Snapshot& snapshot, const FormatScan& scan) {
    const dstd::String logical = fileio::get_logical_path(scan.path);
    const auto file = snapshot.file(logical);
    if (!file) return dstd::nullopt;
    common::InputFileStat stat;
    if (!common::stat_input(dstd::String(scan.path), stat))
        return dstd::nullopt;
    const SourceStat now{static_cast<std::uint64_t>(stat.mtime), stat.size, 0};
    if (snapshot.freshness(logical, now) != Freshness::Current)
        return dstd::nullopt;

    const auto record = format_record(snapshot, *file);
    if (!record) {
        DFTRACER_UTILS_LOG_WARN(
            "index of {} unreadable, reading every chunk: {}", scan.path,
            record.error().message());
        return dstd::nullopt;
    }
    if (!*record || (*record)->feeder != FEEDER ||
        (*record)->format != scan.format ||
        (*record)->plugin_version != scan.plugin_version ||
        (*record)->tokens.size() != scan.tokens.size() ||
        !std::equal((*record)->tokens.begin(), (*record)->tokens.end(),
                    scan.tokens.begin()))
        return dstd::nullopt;

    dstd::Vector<Leaf> leaves;
    for (const dataframe::Expr& filter : scan.filters)
        collect_leaves(filter, scan, leaves);
    if (leaves.empty()) return dstd::nullopt;

    const std::uint64_t total = scan.tokens.empty() ? 1 : scan.tokens.size();
    dstd::Vector<bool> ruled_out(total, false);
    bool any = false;

    const extensions::ZonemapSummary zonemap(0);
    const extensions::BloomSummary bloom({}, 0);
    const extensions::CountsSummary counts(4096, 0);
    const dstd::Pair<IndexExtension, const extensions::Summary*> sources[] = {
        {IndexExtension::Zonemap, &zonemap},
        {IndexExtension::Bloom, &bloom},
        {IndexExtension::Counts, &counts}};

    for (const Leaf& leaf : leaves) {
        ExtensionLeaf probe;
        probe.path = leaf.path;
        probe.op = leaf.op;
        probe.type = leaf.type;
        probe.value = leaf.value;
        if (leaf.value.kind == ExtensionValue::Kind::String)
            probe.value.str = &leaf.text;
        for (const auto& [ext, summary] : sources) {
            if (!extension_current(snapshot, *file, ext)) continue;
            auto compiled = summary->compile(probe, leaf.type);
            if (!compiled || !*compiled) continue;
            const extensions::ExtensionQuery& query = **compiled;
            auto stored = index::granules(snapshot, *file, ext, leaf.path);
            if (!stored) {
                DFTRACER_UTILS_LOG_WARN(
                    "index of {} unreadable, reading every chunk: {}",
                    scan.path, stored.error().message());
                return dstd::nullopt;
            }
            for (const auto& [granule, payload] : *stored) {
                if (granule >= total || ruled_out[granule]) continue;
                if (!query.may_match(payload)) {
                    ruled_out[granule] = true;
                    any = true;
                }
            }
            if (ext != IndexExtension::Bloom) continue;
            auto whole = path_file(snapshot, *file, ext, leaf.path);
            if (!whole) {
                DFTRACER_UTILS_LOG_WARN(
                    "index of {} unreadable, reading every chunk: {}",
                    scan.path, whole.error().message());
                return dstd::nullopt;
            }
            if (*whole && !query.file_may_match(**whole)) {
                for (std::uint64_t g = 0; g < total; ++g) ruled_out[g] = true;
                any = true;
            }
        }
    }
    if (!any) return dstd::nullopt;
    dstd::Vector<std::uint64_t> kept;
    for (std::uint64_t g = 0; g < total; ++g)
        if (!ruled_out[g]) kept.push_back(g);
    if (scan.tokens.empty() && !kept.empty()) return dstd::nullopt;
    return kept;
}

}  // namespace

dstd::Optional<dstd::Vector<std::uint64_t>> format_candidate_chunks(
    const FormatScan& scan) {
    if (scan.filters.empty()) return dstd::nullopt;
    const dstd::String root = trace::internal::determine_index_path(
        dstd::String(scan.path), scan.index_dir);
    const auto present = dstd::fs::exists(dstd::PathView(store_of(root)));
    if (!present.has_value() || !*present) return dstd::nullopt;
    const auto snapshot = plan::open_snapshot(root);
    if (!snapshot) {
        DFTRACER_UTILS_LOG_WARN(
            "index of {} unreadable, reading every chunk: {}", scan.path,
            snapshot.error().message());
        return dstd::nullopt;
    }
    return candidates_in(*snapshot, scan);
}

}  // namespace dftracer::utils::trace::index
