// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/c_api.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/record_schema.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace dftracer::utils;

using dftracer::utils::dstd::Condition;
using dftracer::utils::trace::index::IndexedFile;
using dftracer::utils::trace::index::Indexer;
using dftracer::utils::trace::index::IndexerOptions;
using dftracer::utils::trace::index::IndexStatus;

struct dftu_indexer : dftracer::utils::dstd::HeapObject {
    Indexer indexer;
};

struct dftu_indexer_file_list : dftracer::utils::dstd::HeapObject {
    dstd::Vector<IndexedFile> files;
};

namespace {

dftu_status invalid_argument(dftu_error* err, const char* message) {
    return common::fill_error(err,
                              dstd::Error(Condition::InvalidArgument, message));
}

dftu_indexer_report to_c(const IndexStatus& s) {
    return dftu_indexer_report{s.total,
                               s.ready.size(),
                               s.needs_work.size(),
                               s.indexed,
                               s.aggregation_needs_rebuild ? 1 : 0,
                               s.truncated.size()};
}

char* copy_c_string(dstd::StringView s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) dstd::fatal("out of memory copying a result string");
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

template <class Fn>
dftu_status string_call(dftu_indexer* ix, char** out, dftu_error* err, Fn fn) {
    if (!out) return invalid_argument(err, "out is NULL");
    *out = nullptr;
    if (!ix) return invalid_argument(err, "indexer is NULL");
    const dstd::Result<dstd::String> s = fn(ix->indexer);
    if (!s) return common::fill_error(err, s.error());
    *out = copy_c_string(*s);
    return DFTU_OK;
}

template <class Fn>
dftu_status status_call(dftu_indexer* ix, dftu_indexer_report* out,
                        dftu_error* err, Fn fn) {
    if (!out) return invalid_argument(err, "out is NULL");
    if (!ix) return invalid_argument(err, "indexer is NULL");
    const dstd::Result<IndexStatus> status = fn(ix->indexer);
    if (!status) return common::fill_error(err, status.error());
    *out = to_c(*status);
    return DFTU_OK;
}

template <class Fn>
dftu_status schema_call(char** out, dftu_error* err, Fn fn) {
    if (!out) return invalid_argument(err, "out is NULL");
    *out = nullptr;
    const dstd::Result<dstd::String> text = fn();
    if (!text) return common::fill_error(err, text.error());
    *out = copy_c_string(*text);
    return DFTU_OK;
}

dftu_status null_argument(char** out, dftu_error* err, const char* what) {
    if (out) *out = nullptr;
    return invalid_argument(err, what);
}

}  // namespace

extern "C" {

void dftu_indexer_options_init(dftu_indexer_options* out) {
    if (!out) return;
    const IndexerOptions d;
    *out = dftu_indexer_options{static_cast<std::uint32_t>(sizeof(*out)),
                                nullptr,
                                d.checkpoint_size,
                                static_cast<std::uint32_t>(d.parallelism),
                                d.checkpoints ? 1 : 0,
                                d.bloom ? 1 : 0,
                                d.bloom && d.bloom->required ? 1 : 0,
                                nullptr,
                                0,
                                nullptr,
                                0,
                                d.memory_budget,
                                nullptr,
                                d.bloom ? d.bloom->path_budget : 0,
                                d.bloom ? d.bloom->stats_share : 0.05,
                                d.aggregation.time_interval_us,
                                d.aggregation.compute_percentiles ? 1 : 0,
                                d.aggregation.group_by_file ? 1 : 0};
}

dftu_status dftu_indexer_open(const char* const* paths, std::uint64_t n,
                              const dftu_indexer_options* options,
                              dftu_indexer** out, dftu_error* err) {
    if (!out) return invalid_argument(err, "out is NULL");
    *out = nullptr;
    if (!paths || n == 0) {
        return invalid_argument(err, "paths is NULL or empty");
    }
    dstd::Vector<dstd::String> list;
    list.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
        if (!paths[i]) return invalid_argument(err, "a path is NULL");
        list.emplace_back(paths[i]);
    }
    IndexerOptions opts;
    if (options) {
        dftu_indexer_options in;
        dftu_indexer_options_init(&in);
        if (!common::copy_struct(&in, sizeof(in), options, options->struct_size,
                                 offsetof(dftu_indexer_options, index_dir))) {
            return invalid_argument(
                err, "options struct_size does not reach the first field");
        }
        if (in.index_dir) opts.index_dir = in.index_dir;
        if (in.checkpoint_size) opts.checkpoint_size = in.checkpoint_size;
        opts.parallelism = in.parallelism;
        opts.memory_budget = in.memory_budget;
        if (in.schema) opts.schema = in.schema;
        opts.checkpoints = in.checkpoints != 0;
        if (in.bloom) {
            opts.bloom->required = in.bloom_required != 0;
            opts.bloom->path_budget = in.bloom_path_budget;
            opts.bloom->stats_share = in.bloom_stats_share;
            if (in.bloom_field_count && !in.bloom_fields) {
                return invalid_argument(err,
                                        "bloom_fields is NULL with a count");
            }
            for (std::uint32_t i = 0; i < in.bloom_field_count; ++i) {
                if (!in.bloom_fields[i]) {
                    return invalid_argument(err, "a bloom field is NULL");
                }
                opts.bloom->fields.emplace_back(in.bloom_fields[i]);
            }
        } else {
            opts.bloom.reset();
        }
        if (in.aggregation_time_interval_us)
            opts.aggregation.time_interval_us = in.aggregation_time_interval_us;
        if (in.aggregation_compute_percentiles)
            opts.aggregation.compute_percentiles = true;
        if (in.aggregation_group_by_file) opts.aggregation.group_by_file = true;
        if (in.extension_count && !in.extensions) {
            return invalid_argument(err, "extensions is NULL with a count");
        }
        if (in.extension_count) opts.extensions.clear();
        for (std::uint32_t i = 0; i < in.extension_count; ++i) {
            if (!in.extensions[i]) {
                return invalid_argument(err, "an extension is NULL");
            }
            opts.extensions.emplace_back(in.extensions[i]);
        }
    }
    auto opened = Indexer::open(std::move(list), std::move(opts));
    if (!opened) return common::fill_error(err, opened.error());
    *out = new dftu_indexer{{}, std::move(*opened)};
    return DFTU_OK;
}

dftu_status dftu_indexer_status(dftu_indexer* indexer, dftu_indexer_report* out,
                                dftu_error* err) {
    return status_call(indexer, out, err,
                       [](Indexer& ix) { return ix.status(); });
}

dftu_status dftu_indexer_build(dftu_indexer* indexer, dftu_indexer_report* out,
                               dftu_error* err) {
    return status_call(indexer, out, err,
                       [](Indexer& ix) { return ix.build(); });
}

dftu_status dftu_indexer_rebuild(dftu_indexer* indexer,
                                 dftu_indexer_report* out, dftu_error* err) {
    return status_call(indexer, out, err,
                       [](Indexer& ix) { return ix.rebuild(); });
}

dftu_status dftu_indexer_manifest(dftu_indexer* indexer, char** out,
                                  dftu_error* err) {
    return string_call(
        indexer, out, err, [](Indexer& ix) -> dstd::Result<dstd::String> {
            auto manifest = ix.manifest();
            if (!manifest) return manifest.error();
            return dftracer::utils::trace::index::to_json(*manifest);
        });
}

dftu_status dftu_indexer_explain(dftu_indexer* indexer, const char* query,
                                 char** out, dftu_error* err) {
    if (!query) return null_argument(out, err, "query is NULL");
    return string_call(
        indexer, out, err, [query](Indexer& ix) -> dstd::Result<dstd::String> {
            auto explained = ix.explain(query);
            if (!explained) return explained.error();
            return dftracer::utils::trace::index::to_json(*explained);
        });
}

dftu_status dftu_indexer_rebuild_extension(dftu_indexer* indexer,
                                           const char* extension,
                                           dftu_indexer_report* out,
                                           dftu_error* err) {
    if (!extension) return invalid_argument(err, "extension is NULL");
    return status_call(indexer, out, err, [extension](Indexer& ix) {
        return ix.rebuild_extension(extension);
    });
}

dftu_status dftu_indexer_drop_extension(dftu_indexer* indexer,
                                        const char* extension,
                                        dftu_indexer_report* out,
                                        dftu_error* err) {
    if (!extension) return invalid_argument(err, "extension is NULL");
    return status_call(indexer, out, err, [extension](Indexer& ix) {
        return ix.drop_extension(extension);
    });
}

void dftu_indexer_string_free(char* s) { std::free(s); }

dftu_status dftu_indexer_files(dftu_indexer* indexer,
                               dftu_indexer_file_list** out, dftu_error* err) {
    if (!out) return invalid_argument(err, "out is NULL");
    *out = nullptr;
    if (!indexer) return invalid_argument(err, "indexer is NULL");
    auto listed = indexer->indexer.files();
    if (!listed) return common::fill_error(err, listed.error());
    *out = new dftu_indexer_file_list{{}, std::move(*listed)};
    return DFTU_OK;
}

void dftu_indexer_free(dftu_indexer* indexer) { delete indexer; }

std::uint64_t dftu_indexer_file_list_count(const dftu_indexer_file_list* list) {
    return list ? list->files.size() : 0;
}

dftu_status dftu_indexer_file_list_get(const dftu_indexer_file_list* list,
                                       std::uint64_t i, dftu_indexer_file* out,
                                       dftu_error* err) {
    if (!list || !out || i >= list->files.size()) {
        return invalid_argument(
            err, "file list or out is NULL, or the index is out of range");
    }
    const auto& f = list->files[i];
    *out = dftu_indexer_file{f.path.c_str(),      f.index_path.c_str(),
                             f.size_bytes,        f.mtime,
                             f.truncated ? 1 : 0, f.schema.c_str()};
    return DFTU_OK;
}

void dftu_indexer_file_list_free(dftu_indexer_file_list* list) { delete list; }

dftu_status dftu_schema_register(const char* text, const char* source,
                                 char** out, dftu_error* err) {
    if (!text) return null_argument(out, err, "text is NULL");
    return schema_call(out, err, [&]() -> dstd::Result<dstd::String> {
        const auto registered = dftracer::utils::trace::index::register_schema(
            text, source ? source : "<text>");
        if (!registered) return registered.error();
        return (*registered)->id;
    });
}

dftu_status dftu_schema_load(const char* path, char** out, dftu_error* err) {
    if (!path) return null_argument(out, err, "path is NULL");
    return schema_call(out, err, [&]() -> dstd::Result<dstd::String> {
        const auto loaded = dftracer::utils::trace::index::load_schemas(path);
        if (!loaded) return loaded.error();
        return dftracer::utils::trace::index::schemas_json();
    });
}

dftu_status dftu_schema_list(char** out, dftu_error* err) {
    return schema_call(out, err, []() -> dstd::Result<dstd::String> {
        return dftracer::utils::trace::index::schemas_json();
    });
}

dftu_status dftu_schema_detect(const char* path, char** out, dftu_error* err) {
    if (!path) return null_argument(out, err, "path is NULL");
    return schema_call(out, err, [&]() -> dstd::Result<dstd::String> {
        const auto detected =
            dftracer::utils::trace::index::detect_file_schema(path);
        if (!detected) return detected.error();
        return (*detected)->id;
    });
}

dftu_status dftu_schema_explain(const char* path, char** out, dftu_error* err) {
    if (!path) return null_argument(out, err, "path is NULL");
    return schema_call(out, err, [&]() -> dstd::Result<dstd::String> {
        const auto explained =
            dftracer::utils::trace::index::explain_file_schema(path);
        if (!explained) return explained.error();
        return dftracer::utils::trace::index::to_json(*explained);
    });
}

}  // extern "C"
