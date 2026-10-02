#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/index_write_lock.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <cstdio>
#include <utility>

namespace dftracer::utils::index {

struct Indexer::Impl {
    std::vector<std::string> paths;
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
    ankerl::unordered_dense::map<std::string, std::vector<const std::string*>>
    by_index() const {
        ankerl::unordered_dense::map<std::string,
                                     std::vector<const std::string*>>
            out;
        for (const auto& p : paths)
            out[trace::internal::determine_index_path(p, options.index_dir)]
                .push_back(&p);
        return out;
    }

    build::ResolverInput resolver_input() const {
        build::ResolverInput in;
        in.files = paths;
        in.index_dir = options.index_dir;
        in.require_checkpoints = options.checkpoints;
        in.require_bloom = options.bloom && options.bloom->required;
        in.require_aggregation = options.aggregation.has_value();
        in.checkpoint_size = options.checkpoint_size;
        in.aggregation_config = options.aggregation;
        if (options.bloom) in.bloom_config = bloom_config();
        in.schema = options.schema;
        return in;
    }

    build::ResolveAndBuildInput build_input(bool force) const {
        build::ResolveAndBuildInput in;
        in.files = paths;
        in.index_dir = options.index_dir;
        in.checkpoint_size = options.checkpoint_size;
        in.parallelism = options.parallelism;
        in.force_rebuild = force;
        in.require_checkpoints = options.checkpoints;
        in.require_bloom = options.bloom && options.bloom->required;
        in.build_bloom = options.bloom.has_value();
        if (options.bloom) in.bloom_config = bloom_config();
        in.require_aggregation = options.aggregation.has_value();
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
    ankerl::unordered_dense::set<std::string_view> seen;
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
    throw DFTUtilsException::cat(ErrorCode::IO, "failed to index ", f.file_path,
                                 ": ", f.message);
}

store::IndexExtension tier_extension(const std::string& name) {
    if (extensions::find_plugin_extension(name))
        return store::IndexExtension::PLUGIN;
    auto ext = store::parse_extension(name);
    if (!ext || !(store::PRUNING_EXTENSIONS.has(*ext) ||
                  *ext == store::IndexExtension::STATS ||
                  *ext == store::IndexExtension::METADATA))
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT, "not a tier extension: ", name,
            " (expected zonemap, bloom, counts, postings, dft.stats, "
            "dft.metadata or a registered plugin extension)");
    return *ext;
}

template <class T, class Fn>
T block_on(Runtime& rt, const char* name, Fn fn) {
    T out;
    rt.run_blocking(name, [&](CoroScope& scope) -> coro::CoroTask<void> {
        out = co_await fn(scope);
    });
    return out;
}

}  // namespace

Indexer::Indexer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Indexer Indexer::open(std::vector<std::string> paths, IndexerOptions options) {
    if (paths.empty())
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "Indexer::open: no trace paths given");
    auto impl = std::make_shared<Impl>();
    impl->options = std::move(options);
    for (const auto& name : impl->options.extensions) {
        auto ext = store::parse_extension(name);
        if (!ext || !store::PRUNING_EXTENSIONS.has(*ext))
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "Indexer::open: not a pruning extension: ", name,
                " (expected zonemap, bloom, counts or postings)");
        impl->extensions.add(*ext);
    }
    if (impl->options.bloom) impl->bloom_config().validate();
    for (auto& p : paths) {
        std::error_code ec;
        if (!fs::exists(p, ec))
            throw DFTUtilsException::cat(ErrorCode::NOT_FOUND,
                                         "Indexer::open: no such path: ", p);
        if (!fs::is_directory(p, ec)) {
            impl->paths.push_back(std::move(p));
            continue;
        }
        std::vector<utilities::filesystem::FileEntry> matched;
        impl->runtime().run_blocking(
            "indexer-open", [&](CoroScope& scope) -> coro::CoroTask<void> {
                utilities::filesystem::PatternDirectoryScannerUtility scanner;
                matched = co_await scanner(
                    scope,
                    utilities::filesystem::PatternDirectoryScannerUtilityInput{
                        p, utilities::filesystem::trace_file_patterns(),
                        false});
            });
        for (const auto& e : matched) impl->paths.push_back(e.path.string());
    }
    for (const auto& [index_path, traces] : impl->by_index())
        load_index_schemas(index_path);
    if (!impl->options.schema.empty()) get_schema(impl->options.schema);
    return Indexer(std::move(impl));
}

coro::CoroTask<IndexStatus> Indexer::status(CoroScope& scope) const {
    build::Resolver resolver;
    auto result = co_await resolver(scope, impl_->resolver_input());
    auto status = to_status(result);
    ankerl::unordered_dense::map<std::string,
                                 std::vector<const build::ResolvedFile*>>
        by_index;
    for (const auto& f : result.cached)
        if (f.file_id >= 0)
            by_index[trace::internal::determine_index_path(
                         f.file_path, impl_->options.index_dir)]
                .push_back(&f);
    for (const auto& [index_path, ready] : by_index) {
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        std::vector<int> ids;
        ids.reserve(ready.size());
        for (const auto* f : ready) ids.push_back(f->file_id);
        auto meta = db.query_file_metadata_batch(ids);
        for (const auto* f : ready) {
            auto it = meta.find(f->file_id);
            if (it != meta.end() && it->second.truncated)
                status.truncated.push_back(f->file_path);
        }
    }
    co_return status;
}

coro::CoroTask<IndexStatus> Indexer::build(CoroScope& scope) {
    auto before = co_await status(scope);
    if (before.needs_work.empty() && !before.aggregation_needs_rebuild)
        co_return before;
    const std::size_t indexed = before.aggregation_needs_rebuild
                                    ? before.total
                                    : before.needs_work.size();
    throw_on_failures(co_await build::resolve_and_build_index(
        &scope, impl_->build_input(false)));
    auto after = co_await status(scope);
    after.indexed = indexed;
    co_return after;
}

coro::CoroTask<IndexStatus> Indexer::rebuild(CoroScope& scope) {
    throw_on_failures(co_await build::resolve_and_build_index(
        &scope, impl_->build_input(true)));
    auto after = co_await status(scope);
    after.indexed = impl_->paths.size();
    co_return after;
}

coro::CoroTask<std::vector<IndexedFile>> Indexer::files(CoroScope&) const {
    std::vector<IndexedFile> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        if (!fs::exists(index_path)) continue;
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        auto registry = db.query_all_file_registry();
        std::vector<int> ids;
        for (const auto* p : traces) {
            auto it = registry.find(store::internal::get_logical_path(*p));
            if (it != registry.end()) ids.push_back(it->second.file_id);
        }
        auto meta = db.query_file_metadata_batch(ids);
        for (const auto* p : traces) {
            auto logical = store::internal::get_logical_path(*p);
            auto it = registry.find(logical);
            if (it == registry.end()) continue;
            auto stat = db.get_file_stat(logical);
            IndexedFile f;
            f.path = *p;
            f.index_path = index_path;
            f.file_id = it->second.file_id;
            if (stat) {
                f.size_bytes = stat->size;
                f.mtime = stat->mtime;
            }
            auto m = meta.find(it->second.file_id);
            f.truncated = m != meta.end() && m->second.truncated;
            f.schema = db.file_schema(it->second.file_id).value_or("");
            out.push_back(std::move(f));
        }
    }
    co_return out;
}

coro::CoroTask<std::vector<FileManifest>> Indexer::manifest(CoroScope&) const {
    const auto config = impl_->bloom_config();
    std::vector<FileManifest> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        if (!fs::exists(index_path)) continue;
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        // dftracer.agg is current at the requested config, else at the one
        // the tier holds.
        std::optional<std::uint64_t> agg_params;
        if (impl_->options.aggregation)
            agg_params = impl_->options.aggregation->params_hash();
        else if (auto stored = schemas::dft::agg::tier::read_config(*db.db()))
            agg_params = stored->params_hash;
        for (const auto* p : traces) {
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(*p));
            if (fid < 0) continue;
            FileManifest m{*p, index_path, {}};
            const auto id = db.file_schema(fid);
            const RecordSchema* schema = id ? &get_schema(*id) : nullptr;
            const auto file_config =
                schema ? build::for_schema(config, *schema) : config;
            for (auto ext : store::ALL_EXTENSIONS) {
                auto state = db.extension_state(fid, ext);
                if (!state) continue;
                std::uint64_t want = file_config.params_hash(ext);
                if (ext == store::IndexExtension::PROFILE)
                    want = schema ? schema->params_hash() : 0;
                else if (ext == store::IndexExtension::AGG)
                    want = agg_params ? *agg_params : 0;
                m.extensions.push_back({std::string(store::extension_name(ext)),
                                        state->version, state->params_hash,
                                        state->ready,
                                        db.extension_current(fid, ext) &&
                                            state->params_hash == want});
            }
            for (auto& [name, state] : db.plugin_extension_states(fid)) {
                const auto ext = extensions::find_plugin_extension(name);
                const bool current = ext && ext->current(state);
                m.extensions.push_back({std::move(name), state.version,
                                        state.params_hash, state.ready,
                                        current});
            }
            out.push_back(std::move(m));
        }
    }
    co_return out;
}

coro::CoroTask<std::vector<FileExplain>> Indexer::explain(
    CoroScope&, std::string query) const {
    auto parsed = duql::Query::from_string(query);
    if (!parsed)
        throw DFTUtilsException::cat(
            ErrorCode::INVALID_ARGUMENT,
            "Indexer::explain: ", parsed.error().format());
    std::vector<FileExplain> out;
    for (const auto& [index_path, traces] : impl_->by_index()) {
        if (!fs::exists(index_path)) {
            for (const auto* p : traces) {
                FileExplain f;
                f.path = *p;
                out.push_back(std::move(f));
            }
            continue;
        }
        store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
        const duql::Query& q = *parsed;
        for (const auto* p : traces) {
            auto e = plan::explain_file_chunks(db, *p, q);
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

coro::CoroTask<IndexStatus> Indexer::rebuild_extension(CoroScope& scope,
                                                       std::string extension) {
    const auto ext = tier_extension(extension);
    auto in = impl_->build_input(false);
    in.build_bloom = true;
    in.require_bloom = true;
    in.bloom_config = impl_->bloom_config();
    in.bloom_config.extensions.add(ext);
    in.rebuild_extensions.add(ext);
    throw_on_failures(
        co_await build::resolve_and_build_index(&scope, std::move(in)));
    auto after = co_await status(scope);
    after.indexed = impl_->paths.size();
    co_return after;
}

coro::CoroTask<IndexStatus> Indexer::drop_extension(CoroScope& scope,
                                                    std::string extension) {
    const auto ext = tier_extension(extension);
    for (const auto& [index_path, traces] : impl_->by_index()) {
        if (!fs::exists(index_path)) continue;
        std::lock_guard<std::mutex> lock(build::index_write_mutex(index_path));
        store::IndexDatabase db(index_path);
        auto writer = db.begin_write();
        for (const auto* p : traces) {
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(*p));
            if (fid < 0) continue;
            if (ext == store::IndexExtension::PLUGIN)
                store::records::clear_plugin(*writer, fid, extension);
            else
                store::records::clear_file(*writer, ext, fid);
        }
        writer->commit();
    }
    co_return co_await status(scope);
}

IndexStatus Indexer::status() const {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-status",
                                 [this](CoroScope& s) { return status(s); });
}

IndexStatus Indexer::build() {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-build",
                                 [this](CoroScope& s) { return build(s); });
}

IndexStatus Indexer::rebuild() {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-rebuild",
                                 [this](CoroScope& s) { return rebuild(s); });
}

std::vector<IndexedFile> Indexer::files() const {
    return block_on<std::vector<IndexedFile>>(
        impl_->runtime(), "indexer-files",
        [this](CoroScope& s) { return files(s); });
}

std::vector<FileManifest> Indexer::manifest() const {
    return block_on<std::vector<FileManifest>>(
        impl_->runtime(), "indexer-manifest",
        [this](CoroScope& s) { return manifest(s); });
}

std::vector<FileExplain> Indexer::explain(std::string query) const {
    return block_on<std::vector<FileExplain>>(
        impl_->runtime(), "indexer-explain",
        [this, &query](CoroScope& s) { return explain(s, query); });
}

IndexStatus Indexer::rebuild_extension(std::string extension) {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-rebuild-extension",
                                 [this, &extension](CoroScope& s) {
                                     return rebuild_extension(s, extension);
                                 });
}

dataframe::DataFrame Indexer::rowset(std::string name) const {
    std::vector<plan::RowSetFile> files;
    for (const auto& f : this->files()) files.push_back({f.path, f.index_path});
    auto frame = plan::stored_rowset(files, name);
    if (!frame)
        throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                     "Indexer::rowset: the index holds no "
                                     "rows for row set '",
                                     name, "'; read it with View::duql(\"from ",
                                     name, "\")");
    return std::move(*frame);
}

IndexStatus Indexer::drop_extension(std::string extension) {
    return block_on<IndexStatus>(impl_->runtime(), "indexer-drop-extension",
                                 [this, &extension](CoroScope& s) {
                                     return drop_extension(s, extension);
                                 });
}

namespace {

void append_string(std::string& out, std::string_view s) {
    out.push_back('"');
    json::append_json_escaped(out, s);
    out.push_back('"');
}

void append_chunks(std::string& out, const std::vector<std::uint64_t>& v) {
    out.push_back('[');
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        out.append(std::to_string(v[i]));
    }
    out.push_back(']');
}

const char* json_bool(bool b) { return b ? "true" : "false"; }

}  // namespace

std::string to_json(const std::vector<FileManifest>& manifest) {
    std::string out = "[";
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
            out.append(",\"version\":" + std::to_string(e.version));
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
    return out;
}

std::string to_json(const std::vector<FileExplain>& explain) {
    std::string out = "[";
    for (std::size_t i = 0; i < explain.size(); ++i) {
        const auto& f = explain[i];
        if (i) out.push_back(',');
        out.append("{\"path\":");
        append_string(out, f.path);
        out.append(",\"indexed\":").append(json_bool(f.indexed));
        out.append(",\"may_match\":").append(json_bool(f.may_match));
        out.append(",\"chunks\":" + std::to_string(f.chunks));
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
    return out;
}

const std::vector<std::string>& Indexer::paths() const { return impl_->paths; }

const IndexerOptions& Indexer::options() const { return impl_->options; }

}  // namespace dftracer::utils::index
