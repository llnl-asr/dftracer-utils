#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/abi.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::Condition;
using dftracer::utils::DFTUtilsException;
using dftracer::utils::index::IndexedFile;
using dftracer::utils::index::Indexer;
using dftracer::utils::index::IndexerOptions;
using dftracer::utils::index::IndexStatus;

struct dftu_indexer {
    Indexer indexer;
    std::string error;
};

struct dftu_indexer_file_list {
    std::vector<IndexedFile> files;
};

namespace {

thread_local std::string handle_free_error;
thread_local std::string schema_error;

dftu_error fill_error(std::string& storage, Condition condition,
                      std::uint64_t domain, std::int32_t code,
                      std::string message) {
    storage = std::move(message);
    return dftu_error{domain, code, static_cast<std::int32_t>(condition),
                      storage.c_str()};
}

dftu_error current_error(std::string& storage) {
    try {
        throw;
    } catch (const DFTUtilsException& e) {
        return fill_error(storage, e.condition(), e.domain(), e.code_int(),
                          e.what());
    } catch (const std::exception& e) {
        return fill_error(
            storage, Condition::Internal, dftracer::utils::CORE_DOMAIN.id,
            static_cast<std::int32_t>(Condition::Internal), e.what());
    } catch (...) {
        return fill_error(
            storage, Condition::Internal, dftracer::utils::CORE_DOMAIN.id,
            static_cast<std::int32_t>(Condition::Internal), "unknown error");
    }
}

dftu_error invalid(std::string& storage, const char* message) {
    return fill_error(
        storage, Condition::InvalidArgument, dftracer::utils::CORE_DOMAIN.id,
        static_cast<std::int32_t>(Condition::InvalidArgument), message);
}

dftu_indexer_report to_c(const IndexStatus& s) {
    return dftu_indexer_report{s.total,
                               s.ready.size(),
                               s.needs_work.size(),
                               s.indexed,
                               s.aggregation_needs_rebuild ? 1 : 0,
                               s.truncated.size()};
}

template <class Fn>
dftu_indexer_string_result string_call(dftu_indexer* ix, Fn fn) {
    dftu_indexer_string_result r{};
    if (!ix) {
        r.u.err = invalid(handle_free_error, "indexer is NULL");
        return r;
    }
    try {
        const std::string s = fn(ix->indexer);
        char* out = static_cast<char*>(std::malloc(s.size() + 1));
        if (!out) throw std::bad_alloc();
        std::memcpy(out, s.c_str(), s.size() + 1);
        r.u.value = out;
        r.ok = 1;
    } catch (...) {
        r.u.err = current_error(ix->error);
    }
    return r;
}

template <class Fn>
dftu_indexer_status_result status_call(dftu_indexer* ix, Fn fn) {
    dftu_indexer_status_result r{};
    if (!ix) {
        r.u.err = invalid(handle_free_error, "indexer is NULL");
        return r;
    }
    try {
        r.u.value = to_c(fn(ix->indexer));
        r.ok = 1;
    } catch (...) {
        r.u.err = current_error(ix->error);
    }
    return r;
}

// A schema call: a string result or the error in schema_error.
template <class Fn>
dftu_indexer_string_result schema_call(Fn fn) {
    dftu_indexer_string_result r{};
    try {
        const std::string s = fn();
        char* out = static_cast<char*>(std::malloc(s.size() + 1));
        if (!out) throw std::bad_alloc();
        std::memcpy(out, s.c_str(), s.size() + 1);
        r.u.value = out;
        r.ok = 1;
    } catch (...) {
        r.u.err = current_error(schema_error);
    }
    return r;
}

dftu_indexer_string_result null_argument(const char* what) {
    dftu_indexer_string_result r{};
    r.u.err = invalid(schema_error, what);
    return r;
}

}  // namespace

extern "C" {

void dftu_indexer_options_init(dftu_indexer_options* out) {
    if (!out) return;
    const IndexerOptions d;
    *out = dftu_indexer_options{nullptr,
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
                                nullptr};
}

dftu_indexer_open_result dftu_indexer_open(
    const char* const* paths, uint64_t n, const dftu_indexer_options* options) {
    dftu_indexer_open_result r{};
    if (!paths || n == 0) {
        r.u.err = invalid(handle_free_error, "paths is NULL or empty");
        return r;
    }
    try {
        std::vector<std::string> list;
        list.reserve(n);
        for (uint64_t i = 0; i < n; ++i) {
            if (!paths[i]) {
                r.u.err = invalid(handle_free_error, "a path is NULL");
                return r;
            }
            list.emplace_back(paths[i]);
        }
        IndexerOptions opts;
        if (options) {
            if (options->index_dir) opts.index_dir = options->index_dir;
            if (options->checkpoint_size)
                opts.checkpoint_size = options->checkpoint_size;
            opts.parallelism = options->parallelism;
            opts.memory_budget = options->memory_budget;
            if (options->schema) opts.schema = options->schema;
            opts.checkpoints = options->checkpoints != 0;
            if (options->bloom) {
                opts.bloom->required = options->bloom_required != 0;
                if (options->bloom_field_count && !options->bloom_fields) {
                    r.u.err = invalid(handle_free_error,
                                      "bloom_fields is NULL with a count");
                    return r;
                }
                for (uint32_t i = 0; i < options->bloom_field_count; ++i) {
                    if (!options->bloom_fields[i]) {
                        r.u.err =
                            invalid(handle_free_error, "a bloom field is NULL");
                        return r;
                    }
                    opts.bloom->fields.emplace_back(options->bloom_fields[i]);
                }
            } else {
                opts.bloom.reset();
            }
            if (options->extension_count && !options->extensions) {
                r.u.err = invalid(handle_free_error,
                                  "extensions is NULL with a count");
                return r;
            }
            if (options->extension_count) opts.extensions.clear();
            for (uint32_t i = 0; i < options->extension_count; ++i) {
                if (!options->extensions[i]) {
                    r.u.err =
                        invalid(handle_free_error, "an extension is NULL");
                    return r;
                }
                opts.extensions.emplace_back(options->extensions[i]);
            }
        }
        r.u.value = new dftu_indexer{
            Indexer::open(std::move(list), std::move(opts)), {}};
        r.ok = 1;
    } catch (...) {
        r.u.err = current_error(handle_free_error);
    }
    return r;
}

dftu_indexer_status_result dftu_indexer_status(dftu_indexer* indexer) {
    return status_call(indexer, [](Indexer& ix) { return ix.status(); });
}

dftu_indexer_status_result dftu_indexer_build(dftu_indexer* indexer) {
    return status_call(indexer, [](Indexer& ix) { return ix.build(); });
}

dftu_indexer_status_result dftu_indexer_rebuild(dftu_indexer* indexer) {
    return status_call(indexer, [](Indexer& ix) { return ix.rebuild(); });
}

dftu_indexer_string_result dftu_indexer_manifest(dftu_indexer* indexer) {
    return string_call(indexer, [](Indexer& ix) {
        return dftracer::utils::index::to_json(ix.manifest());
    });
}

dftu_indexer_string_result dftu_indexer_explain(dftu_indexer* indexer,
                                                const char* query) {
    if (indexer && !query) {
        dftu_indexer_string_result r{};
        r.u.err = invalid(indexer->error, "query is NULL");
        return r;
    }
    return string_call(indexer, [query](Indexer& ix) {
        return dftracer::utils::index::to_json(ix.explain(query));
    });
}

dftu_indexer_status_result dftu_indexer_rebuild_extension(
    dftu_indexer* indexer, const char* extension) {
    if (indexer && !extension) {
        dftu_indexer_status_result r{};
        r.u.err = invalid(indexer->error, "extension is NULL");
        return r;
    }
    return status_call(indexer, [extension](Indexer& ix) {
        return ix.rebuild_extension(extension);
    });
}

dftu_indexer_status_result dftu_indexer_drop_extension(dftu_indexer* indexer,
                                                       const char* extension) {
    if (indexer && !extension) {
        dftu_indexer_status_result r{};
        r.u.err = invalid(indexer->error, "extension is NULL");
        return r;
    }
    return status_call(indexer, [extension](Indexer& ix) {
        return ix.drop_extension(extension);
    });
}

void dftu_indexer_string_free(char* s) { std::free(s); }

dftu_indexer_files_result dftu_indexer_files(dftu_indexer* indexer) {
    dftu_indexer_files_result r{};
    if (!indexer) {
        r.u.err = invalid(handle_free_error, "indexer is NULL");
        return r;
    }
    try {
        r.u.value = new dftu_indexer_file_list{indexer->indexer.files()};
        r.ok = 1;
    } catch (...) {
        r.u.err = current_error(indexer->error);
    }
    return r;
}

void dftu_indexer_free(dftu_indexer* indexer) { delete indexer; }

uint64_t dftu_indexer_file_list_count(const dftu_indexer_file_list* list) {
    return list ? list->files.size() : 0;
}

dftu_indexer_file_result dftu_indexer_file_list_get(
    const dftu_indexer_file_list* list, uint64_t i) {
    dftu_indexer_file_result r{};
    if (!list || i >= list->files.size()) {
        r.u.err = invalid(handle_free_error, "file list index out of range");
        return r;
    }
    const auto& f = list->files[i];
    r.u.value = dftu_indexer_file{
        f.path.c_str(), f.index_path.c_str(), f.file_id,       f.size_bytes,
        f.mtime,        f.truncated ? 1 : 0,  f.schema.c_str()};
    r.ok = 1;
    return r;
}

void dftu_indexer_file_list_free(dftu_indexer_file_list* list) { delete list; }

dftu_indexer_string_result dftu_schema_register(const char* text,
                                                const char* source) {
    if (!text) return null_argument("text is NULL");
    return schema_call([&] {
        return dftracer::utils::index::register_schema(
                   text, source ? source : "<text>")
            .id;
    });
}

dftu_indexer_string_result dftu_schema_load(const char* path) {
    if (!path) return null_argument("path is NULL");
    return schema_call([&] {
        dftracer::utils::index::load_schemas(path);
        return dftracer::utils::index::schemas_json();
    });
}

dftu_indexer_string_result dftu_schema_list(void) {
    return schema_call([] { return dftracer::utils::index::schemas_json(); });
}

dftu_indexer_string_result dftu_schema_detect(const char* path) {
    if (!path) return null_argument("path is NULL");
    return schema_call(
        [&] { return dftracer::utils::index::detect_file_schema(path).id; });
}

dftu_indexer_string_result dftu_schema_explain(const char* path) {
    if (!path) return null_argument("path is NULL");
    return schema_call([&] {
        return dftracer::utils::index::to_json(
            dftracer::utils::index::explain_file_schema(path));
    });
}

}  // extern "C"
