// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <cstdio>
#include <utility>

namespace dftracer::utils::index {

struct Indexer::Impl {
    dstd::Vector<dstd::String> paths;
    /// The scan from open_scanned(), used by the first call that needs sizes
    /// and mtimes. Empty for open(), whose calls stat the files.
    mutable std::mutex opened_mutex;
    mutable dstd::Vector<runtime::fs::FoundFile> opened;
    IndexerOptions options;
    store::ExtensionMask extensions;

    Runtime& runtime() const {
        return options.runtime ? *options.runtime : default_runtime();
    }

    build::ChunkIndexerConfig bloom_config() const {
        const BloomOptions bloom = options.bloom.value_or(BloomOptions{});
        build::ChunkIndexerConfig c;
        for (const auto& field : bloom.fields)
            c.extra_dimensions.push_back(build::extra_dimension_name(field));
        c.false_positive_rate = bloom.false_positive_rate;
        c.path_budget = bloom.path_budget;
        c.stats_share = bloom.stats_share;
        c.auto_max_distinct = bloom.auto_max_distinct;
        c.expected_entries_per_chunk = bloom.expected_entries_per_chunk;
        c.extensions = extensions;
        return c;
    }

    // The traces grouped by index root, in open order within a root.
    dstd::HashMap<dstd::String, dstd::Vector<const dstd::String*>> by_index()
        const {
        dstd::HashMap<dstd::String, dstd::Vector<const dstd::String*>> out;
        for (const auto& p : paths)
            out[trace::internal::determine_index_path(p, options.index_dir)]
                .push_back(&p);
        return out;
    }

    dstd::Vector<runtime::fs::FoundFile> take_entries() const {
        {
            std::lock_guard<std::mutex> lock(opened_mutex);
            if (!opened.empty()) return std::move(opened);
        }
        dstd::Vector<runtime::fs::FoundFile> out;
        out.reserve(paths.size());
        for (const auto& p : paths) {
            runtime::fs::FoundFile entry;
            entry.path = dstd::Path(p);
            common::InputFileStat stat;
            if (common::stat_input(entry.path, stat) && stat.is_regular_file) {
                entry.size = stat.size;
                entry.mtime_ns = static_cast<std::uint64_t>(stat.mtime_ns);
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    build::ResolverInput resolver_input(
        dstd::Vector<runtime::fs::FoundFile> entries) const {
        build::ResolverInput in;
        in.files = paths;
        in.entries = std::move(entries);
        in.index_dir = options.index_dir;
        in.require_checkpoints = options.checkpoints;
        in.require_bloom = options.bloom && options.bloom->required;
        in.checkpoint_size = options.checkpoint_size;
        in.aggregation_config = options.aggregation;
        if (options.bloom) in.bloom_config = bloom_config();
        in.schema = options.schema;
        return in;
    }

    build::ResolveAndBuildInput build_input(
        bool force, dstd::Vector<runtime::fs::FoundFile> entries) const {
        build::ResolveAndBuildInput in;
        in.files = paths;
        in.entries = std::move(entries);
        in.index_dir = options.index_dir;
        in.checkpoint_size = options.checkpoint_size;
        in.parallelism = options.parallelism;
        in.force_rebuild = force;
        in.require_checkpoints = options.checkpoints;
        in.require_bloom = options.bloom && options.bloom->required;
        in.build_bloom = options.bloom.has_value();
        if (options.bloom) in.bloom_config = bloom_config();
        in.aggregation_config = options.aggregation;
        in.memory_budget = options.memory_budget;
        in.schema = options.schema;
        return in;
    }
};

namespace {

IndexStatus to_status(const build::ResolverResult& r) {
    IndexStatus s;
    s.total = r.all_files.size();
    s.index_path = r.index_path;
    s.aggregation_interval_us = r.stored_time_interval_us;
    s.aggregation_needs_rebuild = !r.stale_aggregation_roots.empty();
    s.ready.reserve(r.cached.size());
    for (const auto& f : r.cached) s.ready.push_back(f.file_path);
    dstd::HashSet<dstd::StringView> seen;
    for (const auto* list :
         {&r.needs_checkpoint, &r.needs_bloom, &r.needs_aggregation}) {
        for (const auto& item : *list) {
            if (seen.insert(item.file_path).second)
                s.needs_work.push_back(item.file_path);
        }
    }
    return s;
}

void throw_on_failures(const build::ResolverResult& r) {
    if (r.failures.empty()) return;
    const auto& f = r.failures.front();
    throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                         "failed to index ", f.file_path, ": ",
                                         f.message);
}

void throw_on_error(const dstd::Error& error, dstd::StringView what) {
    throw common::DFTUtilsException::cat(common::ErrorCode::Io, what, ": ",
                                         error.message());
}

/// Clears plugin extension `name` of every file in `traces`.
coro::CoroTask<void> drop_plugin_extension(
    const dstd::String& index_path, const dstd::String& name,
    const dstd::Vector<const dstd::String*>& traces) {
    auto opened = Index::open(normalize_index_root(index_path));
    if (!opened) throw_on_error(opened.error(), "drop extension " + name);
    dstd::Vector<dstd::String> logical;
    logical.reserve(traces.size());
    for (const auto* p : traces)
        logical.push_back(store::internal::get_logical_path(*p));
    dstd::Vector<dstd::StringView> views;
    views.reserve(logical.size());
    for (const auto& path : logical) views.emplace_back(path);
    auto dropped = co_await remove_plugin(
        *opened, dstd::StringView(name),
        dstd::Span<const dstd::StringView>(views.data(), views.size()));
    if (!dropped) throw_on_error(dropped.error(), "drop extension " + name);
}

store::IndexExtension tier_extension(const dstd::String& name) {
    if (extensions::find_plugin_extension(name))
        return store::IndexExtension::Plugin;
    auto ext = store::parse_extension(name);
    if (!ext || !(store::PRUNING_EXTENSIONS.has(*ext) ||
                  *ext == store::IndexExtension::Stats ||
                  *ext == store::IndexExtension::Metadata))
        throw common::DFTUtilsException::cat(
            common::ErrorCode::InvalidArgument, "not a tier extension: ", name,
            " (expected zonemap, bloom, counts, postings, dft.stats, "
            "dft.metadata or a registered plugin extension)");
    return *ext;
}

template <class T, class Fn>
T block_on(Runtime& rt, const char* name, Fn fn) {
    T out;
    rt.block_on(coro::named(
        name, coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            out = co_await fn(scope);
        })));
    return out;
}

coro::CoroTask<bool> has_store(dstd::String index_path) {
    const dstd::String store_path = store_of(index_path);
    auto checking = runtime::fs::exists(dstd::PathView(store_path));
    co_return common::unwrap(co_await std::move(checking));
}

}  // namespace

Indexer::Indexer(dstd::SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

void Indexer::validate_options(Impl& impl) {
    for (const auto& name : impl.options.extensions) {
        auto ext = store::parse_extension(name);
        if (!ext || !store::PRUNING_EXTENSIONS.has(*ext))
            throw common::DFTUtilsException::cat(
                common::ErrorCode::InvalidArgument,
                "Indexer::open: not a pruning extension: ", name,
                " (expected zonemap, bloom, counts or postings)");
        impl.extensions.add(*ext);
    }
    if (impl.options.bloom) impl.bloom_config().validate();
}

Indexer Indexer::finish_open(dstd::SharedPtr<Impl> impl) {
    for (const auto& [index_path, traces] : impl->by_index())
        load_index_schemas(index_path);
    if (!impl->options.schema.empty()) get_schema(impl->options.schema);
    return Indexer(std::move(impl));
}

Indexer Indexer::open(dstd::Vector<dstd::String> paths,
                      IndexerOptions options) {
    if (paths.empty())
        throw common::DFTUtilsException(common::ErrorCode::InvalidArgument,
                                        "Indexer::open: no trace paths given");
    auto impl = dstd::make_shared<Impl>();
    impl->options = std::move(options);
    validate_options(*impl);
    for (auto& p : paths) {
        common::InputFileStat self;
        const bool known = common::stat_input(p, self);
        const bool is_directory = known && self.is_directory;
        const bool is_regular_file = known && self.is_regular_file;
        const auto present = dstd::fs::exists(dstd::PathView(p));
        if (!is_directory && !is_regular_file && !(present && *present))
            throw common::DFTUtilsException::cat(
                common::ErrorCode::NotFound,
                "Indexer::open: no such path: ", p);
        if (!is_directory) {
            impl->paths.push_back(std::move(p));
            continue;
        }
        runtime::fs::FindOptions find_options;
        find_options.patterns = runtime::fs::names_of(
            common::constants::indexer::INPUT_FILE_PATTERNS);
        find_options.recursive = false;
        find_options.with_stat = false;
        auto found = impl->runtime().block_on(coro::named(
            "indexer-open", coro::with_scope([&](coro::Scope& scope) {
                return runtime::fs::find_all(scope, dstd::PathView(p),
                                             find_options);
            })));
        if (!found) throw common::DFTUtilsException(found.error());
        const auto& matched = *found;
        for (const auto& e : matched)
            impl->paths.push_back(dstd::String(e.path.str()));
    }
    return finish_open(std::move(impl));
}

Indexer detail::open_scanned(dstd::Vector<runtime::fs::FoundFile> files,
                             IndexerOptions options) {
    if (files.empty())
        throw common::DFTUtilsException(common::ErrorCode::InvalidArgument,
                                        "open_scanned: no trace files given");
    auto impl = dstd::make_shared<Indexer::Impl>();
    impl->options = std::move(options);
    Indexer::validate_options(*impl);
    for (auto& e : files) {
        impl->paths.emplace_back(e.path.str());
        impl->opened.push_back(std::move(e));
    }
    return Indexer::finish_open(std::move(impl));
}

coro::CoroTask<IndexStatus> Indexer::status(coro::Scope& scope) const {
    co_return co_await status_of(scope, impl_->take_entries());
}

coro::CoroTask<IndexStatus> Indexer::status_of(
    coro::Scope& scope, dstd::Vector<runtime::fs::FoundFile> entries) const {
    build::Resolver resolver;
    auto result =
        co_await resolver(scope, impl_->resolver_input(std::move(entries)));
    auto status = to_status(result);
    dstd::HashMap<dstd::String, dstd::Vector<const build::ResolvedFile*>>
        by_index;
    for (const auto& f : result.cached)
        by_index[trace::internal::determine_index_path(
                     f.file_path, impl_->options.index_dir)]
            .push_back(&f);
    for (const auto& [index_path, ready] : by_index) {
        const auto snapshot = common::unwrap(plan::open_snapshot(index_path));
        for (const auto* f : ready) {
            const auto file =
                snapshot.file(store::internal::get_logical_path(f->file_path));
            if (!file) continue;
            const auto meta = common::unwrap(file_metadata(snapshot, *file));
            if (meta && meta->truncated)
                status.truncated.push_back(f->file_path);
        }
    }
    co_return status;
}

coro::CoroTask<IndexStatus> Indexer::build(coro::Scope& scope) {
    auto entries = impl_->take_entries();
    auto before = co_await status_of(scope, entries);
    if (before.needs_work.empty() && !before.aggregation_needs_rebuild)
        co_return before;
    const std::size_t indexed = before.aggregation_needs_rebuild
                                    ? before.total
                                    : before.needs_work.size();
    throw_on_failures(co_await build::resolve_and_build_index(
        impl_->build_input(false, entries)));
    auto after = co_await status_of(scope, std::move(entries));
    after.indexed = indexed;
    co_return after;
}

coro::CoroTask<IndexStatus> Indexer::rebuild(coro::Scope& scope) {
    auto entries = impl_->take_entries();
    throw_on_failures(co_await build::resolve_and_build_index(
        impl_->build_input(true, entries)));
    auto after = co_await status_of(scope, std::move(entries));
    after.indexed = impl_->paths.size();
    co_return after;
}

coro::CoroTask<dstd::Vector<IndexedFile>> Indexer::files(coro::Scope&) const {
    dstd::Vector<IndexedFile> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        auto checking = has_store(index_path);
        const bool store_present = co_await std::move(checking);
        if (!store_present) continue;
        const auto snapshot = common::unwrap(plan::open_snapshot(index_path));
        for (const auto* p : traces) {
            const auto logical = store::internal::get_logical_path(*p);
            const auto file = snapshot.file(logical);
            if (!file) continue;
            IndexedFile f;
            f.path = *p;
            f.index_path = index_path;
            f.size_bytes = file->stat().size;
            f.mtime = file->stat().mtime;
            const auto meta = common::unwrap(file_metadata(snapshot, *file));
            f.truncated = meta && meta->truncated;
            const auto recorded = common::unwrap(schema(snapshot, *file));
            if (recorded) f.schema.assign(recorded->data(), recorded->size());
            out.push_back(std::move(f));
        }
    }
    co_return out;
}

coro::CoroTask<dstd::Vector<FileManifest>> Indexer::manifest(
    coro::Scope&) const {
    const auto config = impl_->bloom_config();
    dstd::Vector<FileManifest> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        auto checking = has_store(index_path);
        const bool store_present = co_await std::move(checking);
        if (!store_present) continue;
        const auto snapshot = common::unwrap(plan::open_snapshot(index_path));
        const std::uint64_t agg_params =
            impl_->options.aggregation.params_hash();
        for (const auto* p : traces) {
            const auto file =
                snapshot.file(store::internal::get_logical_path(*p));
            if (!file) continue;
            FileManifest m{*p, index_path, {}};
            const auto recorded = common::unwrap(schema(snapshot, *file));
            const dstd::String id =
                recorded ? dstd::String(recorded->data(), recorded->size())
                         : dstd::String{};
            const RecordSchema* rec_schema =
                recorded ? &get_schema(id) : nullptr;
            const auto file_config =
                rec_schema ? build::for_schema(config, *rec_schema) : config;
            for (auto ext : store::ALL_EXTENSIONS) {
                const auto state = extension_state(snapshot, *file, ext);
                if (!state) continue;
                std::uint64_t want = file_config.params_hash(ext);
                if (ext == store::IndexExtension::Profile)
                    want = rec_schema ? rec_schema->params_hash() : 0;
                else if (ext == store::IndexExtension::Agg)
                    want = agg_params;
                m.extensions.push_back(
                    {dstd::String(store::extension_name(ext)), state->version,
                     state->params_hash, state->ready,
                     extension_current(snapshot, *file, ext) &&
                         state->params_hash == want});
            }
            for (const auto& copy : common::unwrap(plugins(snapshot, *file))) {
                const dstd::String name(copy.name.data(), copy.name.size());
                const auto ext = extensions::find_plugin_extension(name);
                const bool current =
                    ext && ext->current(store::ExtensionState{
                               copy.version, copy.params_hash, copy.ready});
                m.extensions.push_back({name, copy.version, copy.params_hash,
                                        copy.ready, current});
            }
            out.push_back(std::move(m));
        }
    }
    co_return out;
}

coro::CoroTask<dstd::Vector<FileExplain>> Indexer::explain(
    coro::Scope&, dstd::String query) const {
    auto parsed = duql::Query::from_string(query);
    if (!parsed)
        throw common::DFTUtilsException::cat(
            common::ErrorCode::InvalidArgument,
            "Indexer::explain: ", parsed.error().format());
    dstd::Vector<FileExplain> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        auto checking = has_store(index_path);
        const bool store_present = co_await std::move(checking);
        if (!store_present) {
            for (const auto* p : traces) {
                FileExplain f;
                f.path = *p;
                out.push_back(std::move(f));
            }
            continue;
        }
        const auto snapshot = common::unwrap(plan::open_snapshot(index_path));
        const duql::Query& q = *parsed;
        for (const auto* p : traces) {
            auto e = plan::explain_file_chunks(snapshot, *p, q);
            FileExplain f{*p,
                          e.indexed,
                          e.file_may_match,
                          e.total_chunks,
                          std::move(e.read),
                          {}};
            for (auto& by : e.by_extension)
                f.extensions.push_back({std::move(by.name), by.file_ruled_out,
                                        std::move(by.removed)});
            out.push_back(std::move(f));
        }
    }
    co_return out;
}

coro::CoroTask<IndexStatus> Indexer::rebuild_extension(coro::Scope& scope,
                                                       dstd::String extension) {
    const auto ext = tier_extension(extension);
    auto entries = impl_->take_entries();
    auto in = impl_->build_input(false, entries);
    in.build_bloom = true;
    in.require_bloom = true;
    in.bloom_config = impl_->bloom_config();
    in.bloom_config.extensions.add(ext);
    in.rebuild_extensions.add(ext);
    throw_on_failures(co_await build::resolve_and_build_index(std::move(in)));
    auto after = co_await status_of(scope, std::move(entries));
    after.indexed = impl_->paths.size();
    co_return after;
}

coro::CoroTask<IndexStatus> Indexer::drop_extension(coro::Scope& scope,
                                                    dstd::String extension) {
    const auto ext = tier_extension(extension);
    for (const auto& [index_path, traces] : impl_->by_index()) {
        auto checking = has_store(index_path);
        const bool store_present = co_await std::move(checking);
        if (!store_present) continue;
        if (ext == store::IndexExtension::Plugin) {
            co_await drop_plugin_extension(index_path, extension, traces);
            continue;
        }
        auto opened = Index::open(index_path);
        if (!opened)
            throw_on_error(opened.error(), "drop extension " + extension);
        dstd::Vector<dstd::String> logical;
        logical.reserve(traces.size());
        for (const auto* p : traces)
            logical.push_back(store::internal::get_logical_path(*p));
        dstd::Vector<dstd::StringView> views;
        views.reserve(logical.size());
        for (const auto& path : logical) views.emplace_back(path);
        auto dropped = co_await remove_extension(
            *opened, ext,
            dstd::Span<const dstd::StringView>(views.data(), views.size()));
        if (!dropped)
            throw_on_error(dropped.error(), "drop extension " + extension);
    }
    co_return co_await status(scope);
}

IndexStatus Indexer::status() const {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-status",
                                 [this](coro::Scope& s) { return status(s); });
}

IndexStatus Indexer::build() {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-build",
                                 [this](coro::Scope& s) { return build(s); });
}

IndexStatus Indexer::rebuild() {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-rebuild",
                                 [this](coro::Scope& s) { return rebuild(s); });
}

dstd::Vector<IndexedFile> Indexer::files() const {
    return block_on<dstd::Vector<IndexedFile>>(
        impl_->runtime(), "indexer-files",
        [this](coro::Scope& s) { return files(s); });
}

dstd::Vector<FileManifest> Indexer::manifest() const {
    return block_on<dstd::Vector<FileManifest>>(
        impl_->runtime(), "indexer-manifest",
        [this](coro::Scope& s) { return manifest(s); });
}

dstd::Vector<FileExplain> Indexer::explain(dstd::StringView query) const {
    const dstd::String text(query);
    return block_on<dstd::Vector<FileExplain>>(
        impl_->runtime(), "indexer-explain",
        [this, &text](coro::Scope& s) { return explain(s, text); });
}

IndexStatus Indexer::rebuild_extension(dstd::StringView extension) {
    const dstd::String name(extension);
    return block_on<IndexStatus>(
        impl_->runtime(), "indexer-rebuild-extension",
        [this, &name](coro::Scope& s) { return rebuild_extension(s, name); });
}

dataframe::DataFrame Indexer::rowset(dstd::String name) const {
    dstd::Vector<plan::RowSetFile> files;
    for (const auto& f : this->files()) files.push_back({f.path, f.index_path});
    auto frame = plan::stored_rowset(files, name);
    if (!frame)
        throw common::DFTUtilsException::cat(
            common::ErrorCode::InvalidArgument,
            "Indexer::rowset: the index holds no "
            "rows for row set '",
            name, "'; read it with View::duql(\"from ", name, "\")");
    return std::move(*frame);
}

IndexStatus Indexer::drop_extension(dstd::StringView extension) {
    const dstd::String name(extension);
    return block_on<IndexStatus>(
        impl_->runtime(), "indexer-drop-extension",
        [this, &name](coro::Scope& s) { return drop_extension(s, name); });
}

namespace {

void append_string(dstd::String& out, dstd::StringView s) {
    out.push_back('"');
    json::append_json_escaped(out, s);
    out.push_back('"');
}

void append_chunks(dstd::String& out, const dstd::Vector<std::uint64_t>& v) {
    out.push_back('[');
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        out.append(dstd::to_string(v[i]));
    }
    out.push_back(']');
}

const char* json_bool(bool b) { return b ? "true" : "false"; }

}  // namespace

dstd::String to_json(const dstd::Vector<FileManifest>& manifest) {
    dstd::String out = "[";
    for (std::size_t i = 0; i < manifest.size(); ++i) {
        const auto& m = manifest[i];
        if (i) out.push_back(',');
        out.append("{\"path\":");
        append_string(out, m.path);
        out.append(",\"index_path\":");
        append_string(out, m.index_path);
        out.append(",\"extensions\":[");
        for (std::size_t j = 0; j < m.extensions.size(); ++j) {
            const auto& e = m.extensions[j];
            if (j) out.push_back(',');
            out.append("{\"name\":");
            append_string(out, e.name);
            out.append(",\"version\":" + dstd::to_string(e.version));
            char hash[17];
            std::snprintf(hash, sizeof(hash), "%016llx",
                          static_cast<unsigned long long>(e.params_hash));
            out.append(",\"params_hash\":\"").append(hash).push_back('"');
            out.append(",\"ready\":").append(json_bool(e.ready));
            out.append(",\"current\":").append(json_bool(e.current));
            out.push_back('}');
        }
        out.append("]}");
    }
    out.push_back(']');
    return dstd::String(out);
}

dstd::String to_json(const dstd::Vector<FileExplain>& explain) {
    dstd::String out = "[";
    for (std::size_t i = 0; i < explain.size(); ++i) {
        const auto& f = explain[i];
        if (i) out.push_back(',');
        out.append("{\"path\":");
        append_string(out, f.path);
        out.append(",\"indexed\":").append(json_bool(f.indexed));
        out.append(",\"may_match\":").append(json_bool(f.may_match));
        out.append(",\"chunks\":" + dstd::to_string(f.chunks));
        out.append(",\"read\":");
        append_chunks(out, f.read);
        out.append(",\"extensions\":[");
        for (std::size_t j = 0; j < f.extensions.size(); ++j) {
            const auto& e = f.extensions[j];
            if (j) out.push_back(',');
            out.append("{\"name\":");
            append_string(out, e.name);
            out.append(",\"file_ruled_out\":")
                .append(json_bool(e.file_ruled_out));
            out.append(",\"removed\":");
            append_chunks(out, e.removed);
            out.push_back('}');
        }
        out.append("]}");
    }
    out.push_back(']');
    return dstd::String(out);
}

const dstd::Vector<dstd::String>& Indexer::paths() const {
    return impl_->paths;
}

const IndexerOptions& Indexer::options() const { return impl_->options; }

}  // namespace dftracer::utils::index
