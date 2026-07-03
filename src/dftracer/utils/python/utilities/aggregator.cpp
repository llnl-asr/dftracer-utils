#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/python/utilities/aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/streaming_iterator.h>
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/partition_router.h>
#include <dftracer/utils/utilities/common/arrow/partition_writer.h>
#include <dftracer/utils/utilities/common/query/query.h>
#endif

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

using dftracer::utils::python::wrap_arrow_result;
using dftracer::utils::python::wrap_arrow_table;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using dftracer::utils::python::ArrowStreamingIteratorObject;
using dftracer::utils::python::ArrowStreamingIteratorType;
using dftracer::utils::python::StreamingState;
using dftracer::utils::utilities::common::arrow::ArrowExportResult;
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
using dftracer::utils::utilities::common::arrow::IpcCompression;
using dftracer::utils::utilities::common::arrow::PartitionWriter;
using dftracer::utils::utilities::common::arrow::PartitionWriteStats;
using dftracer::utils::utilities::common::query::Query;
#endif

DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(Aggregator, AggregatorObject)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
// Parse a view query string into an optional Query
static int parse_view_query(PyObject *query_obj, std::optional<Query> &out) {
    if (!query_obj || query_obj == Py_None) {
        out = std::nullopt;
        return 0;
    }
    const char *query_str = PyUnicode_AsUTF8(query_obj);
    if (!query_str) return -1;
    auto parsed = Query::from_string(query_str);
    if (!parsed) {
        PyErr_Format(PyExc_ValueError, "Invalid query: %s",
                     parsed.error().format().c_str());
        return -1;
    }
    out = std::move(*parsed);
    return 0;
}
#endif

static int parse_aggregator_args(PyObject *args, PyObject *kwds,
                                 AggregatorInput &input,
                                 std::size_t *buffer_size_out = nullptr,
                                 std::optional<Query> *query_out = nullptr) {
    static const char *kwlist[] = {"directory",
                                   "time_interval_ms",
                                   "group_keys",
                                   "categories",
                                   "names",
                                   "index_dir",
                                   "checkpoint_size",
                                   "force_rebuild",
                                   "parallelism",
                                   "event_batch_size",
                                   "custom_metric_fields",
                                   "compute_percentiles",
                                   "buffer_size",
                                   "query",
                                   NULL};

    const char *directory = NULL;
    double time_interval_ms = 5000.0;
    PyObject *group_keys_obj = Py_None;
    PyObject *categories_obj = Py_None;
    PyObject *names_obj = Py_None;
    const char *index_dir = "";
    Py_ssize_t checkpoint_size = static_cast<Py_ssize_t>(
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    int force_rebuild = 0;
    Py_ssize_t parallelism = 0;
    Py_ssize_t event_batch_size = 10000;
    PyObject *custom_metrics_obj = Py_None;
    int compute_percentiles = 0;
    Py_ssize_t buffer_size = 8;
    PyObject *query_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|dOOOsnpnnOpnO", (char **)kwlist, &directory,
            &time_interval_ms, &group_keys_obj, &categories_obj, &names_obj,
            &index_dir, &checkpoint_size, &force_rebuild, &parallelism,
            &event_batch_size, &custom_metrics_obj, &compute_percentiles,
            &buffer_size, &query_obj))
        return -1;

    if (buffer_size_out) {
        *buffer_size_out = static_cast<std::size_t>(buffer_size);
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (query_out) {
        if (parse_view_query(query_obj, *query_out) < 0) return -1;
    }
#else
    (void)query_obj;
#endif

    input.directory = directory;
    input.config.time_interval_us =
        static_cast<std::uint64_t>(time_interval_ms * 1000.0);
    input.index_dir = index_dir;
    input.checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    input.force_rebuild = force_rebuild != 0;
    input.parallelism = static_cast<std::size_t>(parallelism);
    input.event_batch_size = static_cast<std::size_t>(event_batch_size);
    input.config.compute_percentiles = compute_percentiles != 0;

    if (!parse_str_list(group_keys_obj, "group_keys",
                        input.config.extra_group_keys))
        return -1;
    if (!parse_str_list(custom_metrics_obj, "custom_metric_fields",
                        input.config.custom_metric_fields))
        return -1;

    return 0;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
static bool run_aggregator_pipeline(
    AggregatorObject *self, const AggregatorInput &input,
    std::vector<ArrowExportResult> &results,
    const std::optional<Query> *query = nullptr) {
    auto *rp = &results;
    AggregatorInput input_copy = input;
    std::optional<Query> query_copy;
    if (query) query_copy = *query;

    return run_blocking([&] {
        Runtime *rt = resolve_runtime(self);
        rt->submit(run_coro_scope(
                       rt->executor(),
                       [](CoroScope &scope, std::vector<ArrowExportResult> *out,
                          AggregatorInput input,
                          std::optional<Query> query) -> CoroTask<void> {
                           AggregatorUtility util;
                           util.bind_context(scope);
                           try {
                               auto gen = util.process(input);
                               while (auto batch = co_await gen.next()) {
                                   if (batch->entries.empty()) continue;
                                   AggregationBatch filtered;
                                   if (query) {
                                       filtered = batch->filter(*query);
                                       if (filtered.entries.empty()) continue;
                                   } else {
                                       filtered = std::move(*batch);
                                   }
                                   auto arrow_result = filtered.to_arrow();
                                   if (!arrow_result.valid()) continue;
                                   out->push_back(std::move(arrow_result));
                               }
                               util.unbind_context();
                           } catch (...) {
                               util.unbind_context();
                               throw;
                           }
                       },
                       rp, std::move(input_copy), std::move(query_copy)),
                   "aggregator")
            .get();
    });
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

static CoroTask<void> run_aggregator_stream(
    CoroScope &scope, std::shared_ptr<StreamingState<ArrowExportResult>> state,
    AggregatorInput input, std::optional<Query> query) {
    if (state->cancelled()) {
        state->complete();
        co_return;
    }

    try {
        AggregatorUtility util;
        util.bind_context(scope);
        auto gen = util.process(input);

        while (auto batch = co_await gen.next()) {
            if (state->cancelled()) break;
            if (batch->entries.empty()) continue;

            AggregationBatch filtered;
            if (query) {
                filtered = batch->filter(*query);
                if (filtered.entries.empty()) continue;
            } else {
                filtered = std::move(*batch);
            }

            auto arrow_result = filtered.to_arrow();
            if (!arrow_result.valid()) continue;

            auto result_bytes =
                dftracer::utils::python::byte_size(arrow_result);
            if (!state->push(std::move(arrow_result), result_bytes)) {
                break;
            }
        }

        util.unbind_context();
        state->complete();
    } catch (const std::exception &e) {
        state->fail(std::current_exception());
    } catch (...) {
        state->fail(std::current_exception());
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

// ---------------------------------------------------------------------------
// process() - returns ArrowTable (materialized)
// ---------------------------------------------------------------------------

static PyObject *Aggregator_process(AggregatorObject *self, PyObject *args,
                                    PyObject *kwds) {
    AggregatorInput input;
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    std::optional<Query> query;
    if (parse_aggregator_args(args, kwds, input, nullptr, &query) < 0)
        return NULL;
#else
    if (parse_aggregator_args(args, kwds, input) < 0) return NULL;
#endif

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    std::vector<ArrowExportResult> results;
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (!run_aggregator_pipeline(self, input, results, &query)) {
#else
    if (!run_aggregator_pipeline(self, input, results)) {
#endif
        return NULL;
    }

    PyObject *batch_list = PyList_New(0);
    if (!batch_list) return NULL;

    for (auto &result : results) {
        PyObject *cap = wrap_arrow_result(std::move(result));
        if (!cap) {
            Py_DECREF(batch_list);
            return NULL;
        }
        int rc = PyList_Append(batch_list, cap);
        Py_DECREF(cap);
        if (rc < 0) {
            Py_DECREF(batch_list);
            return NULL;
        }
    }

    return wrap_arrow_table(batch_list);
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// iter_arrow() - returns true streaming iterator
// ---------------------------------------------------------------------------

static PyObject *Aggregator_iter_arrow(AggregatorObject *self, PyObject *args,
                                       PyObject *kwds) {
    AggregatorInput input;
    std::size_t buffer_size = 8;
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    std::optional<Query> query;
    if (parse_aggregator_args(args, kwds, input, &buffer_size, &query) < 0)
        return NULL;
#else
    if (parse_aggregator_args(args, kwds, input, &buffer_size) < 0) return NULL;
#endif

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    auto state = std::make_shared<StreamingState<ArrowExportResult>>(
        dftracer::utils::compute_memory_budget(0));

    ArrowStreamingIteratorObject *iter_obj =
        (ArrowStreamingIteratorObject *)ArrowStreamingIteratorType.tp_new(
            &ArrowStreamingIteratorType, NULL, NULL);
    if (!iter_obj) {
        return NULL;
    }

    iter_obj->cpp_state->state = state;
    iter_obj->cpp_state->pull_next =
        [state]() -> std::optional<ArrowExportResult> { return state->pull(); };
    iter_obj->cpp_state->get_error = [state]() -> std::exception_ptr {
        return state->error();
    };
    iter_obj->cpp_state->cancel = [state]() { state->cancel(); };

    Runtime *rt = resolve_runtime(self);
    AggregatorInput input_copy = input;
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    std::optional<Query> query_copy = std::move(query);
    Py_BEGIN_ALLOW_THREADS rt->submit(
        run_coro_scope(rt->executor(), run_aggregator_stream, state,
                       std::move(input_copy), std::move(query_copy)),
        "aggregator_stream");
#else
    Py_BEGIN_ALLOW_THREADS rt->submit(
        run_coro_scope(rt->executor(), run_aggregator_stream, state,
                       std::move(input_copy), std::nullopt),
        "aggregator_stream");
#endif
    Py_END_ALLOW_THREADS

        return (PyObject *)iter_obj;
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

struct AggregatorViewDef {
    std::string name;
    std::optional<Query> query;
};

struct AggregatorWriteArrowResult {
    std::unordered_map<std::string, PartitionWriteStats> view_stats;
    int64_t total_rows = 0;
    int64_t total_bytes = 0;
    std::string error;
};

static CoroTask<void> run_aggregator_write_arrow(
    CoroScope &scope, AggregatorWriteArrowResult *out, AggregatorInput input,
    std::string output_path, std::vector<AggregatorViewDef> views,
    int64_t chunk_size_bytes, IpcCompression compression) {
    try {
        // If no views specified, create a default "all" view
        if (views.empty()) {
            views.push_back({"all", std::nullopt});
        }

        // Open a writer for each view
        std::vector<PartitionWriter> writers(views.size());
        for (std::size_t i = 0; i < views.size(); ++i) {
            std::string view_path = output_path;
            if (views.size() > 1 || views[i].name != "all") {
                view_path = output_path + "/" + views[i].name;
            }
            int rc = co_await writers[i].open(view_path, chunk_size_bytes,
                                              compression);
            if (rc != 0) {
                out->error = "Failed to open writer for view: " + views[i].name;
                co_return;
            }
        }

        AggregatorUtility util;
        util.bind_context(scope);
        auto gen = util.process(input);

        while (auto batch = co_await gen.next()) {
            if (batch->entries.empty()) continue;

            // Write to each view (with optional filtering)
            for (std::size_t i = 0; i < views.size(); ++i) {
                AggregationBatch filtered_batch;
                if (views[i].query) {
                    filtered_batch = batch->filter(*views[i].query);
                    if (filtered_batch.entries.empty()) continue;
                } else {
                    filtered_batch = *batch;
                }

                auto arrow_result = filtered_batch.to_arrow();
                if (!arrow_result.valid()) continue;

                int rc = co_await writers[i].write_batch(arrow_result);
                if (rc != 0) {
                    util.unbind_context();
                    out->error =
                        "Failed to write batch for view: " + views[i].name;
                    co_return;
                }
            }
        }

        util.unbind_context();

        // Close writers and collect stats
        for (std::size_t i = 0; i < views.size(); ++i) {
            auto stats = co_await writers[i].close();
            out->view_stats[views[i].name] = std::move(stats);
            out->total_rows += out->view_stats[views[i].name].total_rows;
            out->total_bytes +=
                out->view_stats[views[i].name].total_uncompressed_bytes;
        }
    } catch (const std::exception &e) {
        out->error = e.what();
    }
}

static PyObject *Aggregator_write_arrow(AggregatorObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"directory",
                                   "path",
                                   "time_interval_ms",
                                   "group_keys",
                                   "categories",
                                   "names",
                                   "index_dir",
                                   "checkpoint_size",
                                   "force_rebuild",
                                   "parallelism",
                                   "event_batch_size",
                                   "custom_metric_fields",
                                   "compute_percentiles",
                                   "views",
                                   "chunk_size_mb",
                                   "compression",
                                   NULL};

    const char *directory = NULL;
    const char *output_path = NULL;
    double time_interval_ms = 5000.0;
    PyObject *group_keys_obj = Py_None;
    PyObject *categories_obj = Py_None;
    PyObject *names_obj = Py_None;
    const char *index_dir = "";
    Py_ssize_t checkpoint_size = static_cast<Py_ssize_t>(
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    int force_rebuild = 0;
    Py_ssize_t parallelism = 0;
    Py_ssize_t event_batch_size = 10000;
    PyObject *custom_metrics_obj = Py_None;
    int compute_percentiles = 0;
    PyObject *views_obj = Py_None;
    int chunk_size_mb = 32;
    const char *compression_str = "zstd";

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "ss|dOOOsnpnnOpOis", (char **)kwlist, &directory,
            &output_path, &time_interval_ms, &group_keys_obj, &categories_obj,
            &names_obj, &index_dir, &checkpoint_size, &force_rebuild,
            &parallelism, &event_batch_size, &custom_metrics_obj,
            &compute_percentiles, &views_obj, &chunk_size_mb, &compression_str))
        return NULL;

    // Parse views
    std::vector<AggregatorViewDef> views;
    if (views_obj && views_obj != Py_None) {
        if (!PyList_Check(views_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "views must be a list of dicts with 'name' and "
                            "optional 'query' keys");
            return NULL;
        }
        Py_ssize_t n = PyList_Size(views_obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyList_GetItem(views_obj, i);
            if (!PyDict_Check(item)) {
                PyErr_SetString(PyExc_TypeError,
                                "each view must be a dict with 'name' key");
                return NULL;
            }
            AggregatorViewDef view;
            PyObject *name_obj = PyDict_GetItemString(item, "name");
            if (!name_obj) {
                PyErr_SetString(PyExc_ValueError,
                                "each view must have a 'name' key");
                return NULL;
            }
            const char *name_str = PyUnicode_AsUTF8(name_obj);
            if (!name_str) return NULL;
            view.name = name_str;

            PyObject *query_obj = PyDict_GetItemString(item, "query");
            if (query_obj && query_obj != Py_None) {
                const char *query_str = PyUnicode_AsUTF8(query_obj);
                if (!query_str) return NULL;
                auto parsed = Query::from_string(query_str);
                if (!parsed) {
                    PyErr_Format(PyExc_ValueError,
                                 "Invalid query for view '%s': %s", name_str,
                                 parsed.error().format().c_str());
                    return NULL;
                }
                view.query = std::move(*parsed);
            }
            views.push_back(std::move(view));
        }
    }

    // Parse compression
    IpcCompression compression = IpcCompression::ZSTD;
    if (compression_str) {
        std::string comp_lower(compression_str);
        for (auto &c : comp_lower) c = std::tolower(c);
        if (comp_lower == "none") {
            compression = IpcCompression::NONE;
        } else if (comp_lower == "zstd") {
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
            compression = IpcCompression::ZSTD;
#else
            PyErr_SetString(PyExc_ValueError, "ZSTD compression not available");
            return NULL;
#endif
        } else {
            PyErr_Format(PyExc_ValueError,
                         "Unknown compression: %s (use 'none' or 'zstd')",
                         compression_str);
            return NULL;
        }
    }

    int64_t chunk_size_bytes =
        static_cast<int64_t>(chunk_size_mb) * 1024 * 1024;

    // Parse group_keys
    std::vector<std::string> group_keys;
    if (group_keys_obj && group_keys_obj != Py_None) {
        if (!PyList_Check(group_keys_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "group_keys must be a list of str");
            return NULL;
        }
        Py_ssize_t n = PyList_Size(group_keys_obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GetItem(group_keys_obj, i));
            if (!s) return NULL;
            group_keys.emplace_back(s);
        }
    }

    // Parse custom_metric_fields
    std::vector<std::string> custom_metrics;
    if (custom_metrics_obj && custom_metrics_obj != Py_None) {
        if (!PyList_Check(custom_metrics_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "custom_metric_fields must be a list of str");
            return NULL;
        }
        Py_ssize_t n = PyList_Size(custom_metrics_obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char *s =
                PyUnicode_AsUTF8(PyList_GetItem(custom_metrics_obj, i));
            if (!s) return NULL;
            custom_metrics.emplace_back(s);
        }
    }

    AggregatorInput input;
    input.directory = directory;
    input.config.time_interval_us =
        static_cast<std::uint64_t>(time_interval_ms * 1000.0);
    input.config.extra_group_keys = std::move(group_keys);
    input.config.custom_metric_fields = std::move(custom_metrics);
    input.config.compute_percentiles = compute_percentiles != 0;
    input.index_dir = index_dir;
    input.checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    input.force_rebuild = force_rebuild != 0;
    input.parallelism = static_cast<std::size_t>(parallelism);
    input.event_batch_size = static_cast<std::size_t>(event_batch_size);

    std::string output_path_str(output_path);
    AggregatorWriteArrowResult result;
    auto *rp = &result;

    if (!run_blocking([&] {
            Runtime *rt = resolve_runtime(self);
            rt->submit(run_coro_scope(
                           rt->executor(), run_aggregator_write_arrow, rp,
                           std::move(input), output_path_str, std::move(views),
                           chunk_size_bytes, compression),
                       "aggregator_write_arrow")
                .get();
        })) {
        return NULL;
    }

    if (!result.error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, result.error.c_str());
        return NULL;
    }

    // Build result dict
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    PyObject *views_dict = PyDict_New();
    if (!views_dict) {
        Py_DECREF(dict);
        return NULL;
    }

    for (const auto &[view_name, view_stats] : result.view_stats) {
        PyObject *view_dict = PyDict_New();
        if (!view_dict) {
            Py_DECREF(views_dict);
            Py_DECREF(dict);
            return NULL;
        }

        PyObject *files_list = PyList_New(0);
        if (!files_list) {
            Py_DECREF(view_dict);
            Py_DECREF(views_dict);
            Py_DECREF(dict);
            return NULL;
        }

        for (const auto &f : view_stats.files) {
            PyObject *file_str = PyUnicode_FromString(f.c_str());
            if (!file_str || PyList_Append(files_list, file_str) < 0) {
                Py_XDECREF(file_str);
                Py_DECREF(files_list);
                Py_DECREF(view_dict);
                Py_DECREF(views_dict);
                Py_DECREF(dict);
                return NULL;
            }
            Py_DECREF(file_str);
        }

        PyDict_SetItemString(view_dict, "files", files_list);
        dict_set_steal(view_dict, "rows",
                       PyLong_FromLongLong(view_stats.total_rows));
        dict_set_steal(
            view_dict, "bytes",
            PyLong_FromLongLong(view_stats.total_uncompressed_bytes));
        Py_DECREF(files_list);

        PyObject *key = PyUnicode_FromString(view_name.c_str());
        PyDict_SetItem(views_dict, key, view_dict);
        Py_DECREF(key);
        Py_DECREF(view_dict);
    }

    PyDict_SetItemString(dict, "views", views_dict);
    dict_set_steal(dict, "total_rows", PyLong_FromLongLong(result.total_rows));
    dict_set_steal(dict, "total_bytes",
                   PyLong_FromLongLong(result.total_bytes));
    Py_DECREF(views_dict);

    return dict;
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC

static PyObject *Aggregator_call(PyObject *self, PyObject *args,
                                 PyObject *kwds) {
    return Aggregator_process((AggregatorObject *)self, args, kwds);
}

static PyMethodDef Aggregator_methods[] = {
    {"process", (PyCFunction)Aggregator_process, METH_VARARGS | METH_KEYWORDS,
     "process(directory, time_interval_ms=5000.0, group_keys=None,\n"
     "        categories=None, names=None, index_dir='',\n"
     "        checkpoint_size=33554432, force_rebuild=False,\n"
     "        parallelism=0, event_batch_size=10000,\n"
     "        custom_metric_fields=None, compute_percentiles=False)\n"
     "--\n"
     "\n"
     "Run aggregation pipeline, return materialized ArrowTable.\n"
     "\n"
     "Uses parallel, RocksDB-backed, fused indexing and aggregation.\n"
     "\n"
     "Args:\n"
     "    directory (str): Directory containing .pfw/.pfw.gz files.\n"
     "    time_interval_ms (float): Time bucket in milliseconds (default "
     "5000).\n"
     "    group_keys (list[str] or None): Extra grouping dims (default None).\n"
     "    categories (list[str] or None): Category filter (default None).\n"
     "    names (list[str] or None): Name filter (default None).\n"
     "    index_dir (str): Directory for .dftindex stores (default '').\n"
     "    checkpoint_size (int): Checkpoint size (default 33554432).\n"
     "    force_rebuild (bool): Force index rebuild (default False).\n"
     "    parallelism (int): Number of parallel workers (0 = all cores).\n"
     "    event_batch_size (int): Entries per batch (default 10000).\n"
     "    custom_metric_fields (list[str] or None): Extra numeric args\n"
     "        fields to aggregate into ``*_total``/``*_min``/``*_max``/\n"
     "        ``*_mean``/``*_std`` columns (default None).\n"
     "    compute_percentiles (bool): Enable percentile sketch collection\n"
     "        during aggregation (default False).\n"
     "\n"
     "Returns:\n"
     "    ArrowTable: Aggregated results.\n"},
    {"iter_arrow", (PyCFunction)Aggregator_iter_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "iter_arrow(directory, time_interval_ms=5000.0, group_keys=None,\n"
     "           categories=None, names=None, index_dir='',\n"
     "           checkpoint_size=33554432, force_rebuild=False,\n"
     "           parallelism=0, event_batch_size=10000,\n"
     "           custom_metric_fields=None, compute_percentiles=False,\n"
     "           buffer_size=8)\n"
     "--\n"
     "\n"
     "Run aggregation pipeline, stream Arrow batches.\n"
     "\n"
     "Returns immediately with a streaming iterator. Batches are produced\n"
     "in the background with a bounded buffer. GIL is released while waiting\n"
     "for the next batch, allowing other Python threads to run.\n"
     "\n"
     "Uses parallel, RocksDB-backed, fused indexing and aggregation.\n"
     "\n"
     "Args:\n"
     "    directory (str): Directory containing .pfw/.pfw.gz files.\n"
     "    time_interval_ms (float): Time bucket in milliseconds (default "
     "5000).\n"
     "    group_keys (list[str] or None): Extra grouping dims (default None).\n"
     "    categories (list[str] or None): Category filter (default None).\n"
     "    names (list[str] or None): Name filter (default None).\n"
     "    index_dir (str): Directory for .dftindex stores (default '').\n"
     "    checkpoint_size (int): Checkpoint size (default 33554432).\n"
     "    force_rebuild (bool): Force index rebuild (default False).\n"
     "    parallelism (int): Number of parallel workers (0 = all cores).\n"
     "    event_batch_size (int): Entries per batch (default 10000).\n"
     "    custom_metric_fields (list[str] or None): Extra numeric args\n"
     "        fields to aggregate into ``*_total``/``*_min``/``*_max``/\n"
     "        ``*_mean``/``*_std`` columns (default None).\n"
     "    compute_percentiles (bool): Enable percentile sketch collection\n"
     "        during aggregation (default False).\n"
     "    buffer_size (int): Max batches to buffer (default 8).\n"
     "\n"
     "Returns:\n"
     "    _ArrowStreamingIterator: Streaming iterator yielding Arrow record\n"
     "        batches. Supports cancel() to stop early.\n"},
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    {"write_arrow", (PyCFunction)Aggregator_write_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "write_arrow(directory, path, time_interval_ms=5000.0, ..., views=None)\n"
     "--\n"
     "\n"
     "Run aggregation and write results to Arrow IPC files with optional "
     "views.\n"
     "\n"
     "Views allow filtering aggregated entries before writing. Each view\n"
     "writes to a separate subdirectory. Query syntax supports: cat, name,\n"
     "pid, tid, hhash, fhash, time_bucket, extra group keys, and aggregation\n"
     "metrics (count, dur_total, dur_min, dur_max, size_total, etc.).\n"
     "\n"
     "Args:\n"
     "    directory (str): Directory containing .pfw/.pfw.gz files.\n"
     "    path (str): Output directory for Arrow files.\n"
     "    time_interval_ms (float): Time bucket in milliseconds.\n"
     "    group_keys (list[str] or None): Extra grouping dims.\n"
     "    categories (list[str] or None): Category filter.\n"
     "    names (list[str] or None): Name filter.\n"
     "    index_dir (str): Directory for .dftindex stores.\n"
     "    checkpoint_size (int): Checkpoint size.\n"
     "    force_rebuild (bool): Force index rebuild.\n"
     "    parallelism (int): Number of parallel workers.\n"
     "    event_batch_size (int): Entries per batch.\n"
     "    custom_metric_fields (list[str] or None): Extra numeric fields.\n"
     "    compute_percentiles (bool): Enable percentile collection.\n"
     "    views (list[dict] or None): View definitions, each with 'name' and\n"
     "        optional 'query' keys. If None, writes all entries to path.\n"
     "        Example: [{'name': 'io', 'query': 'cat == \"POSIX\"'}]\n"
     "    chunk_size_mb (int): Max uncompressed MB per file (default 32).\n"
     "    compression (str): 'zstd' or 'none' (default 'zstd').\n"
     "\n"
     "Returns:\n"
     "    dict: Statistics with 'views' (per-view stats), 'total_rows',\n"
     "        'total_bytes'. Each view has 'files', 'rows', 'bytes'.\n"},
#endif
    {NULL}};

PyTypeObject AggregatorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.AggregatorUtility", /* tp_name */
    sizeof(AggregatorObject),                            /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)Aggregator_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    Aggregator_call,                          /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "AggregatorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "High-level aggregation pipeline for DFTracer trace files.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n\n"
    "process(directory, time_interval_ms=5000.0, ...) -> ArrowTable\n"
    "    Run aggregation and return a materialized Arrow table.\n\n"
    "iter_arrow(directory, time_interval_ms=5000.0, ...) -> "
    "Iterator[ArrowBatch]\n"
    "    Run aggregation and stream Arrow batches.\n", /* tp_doc */
    0,                                                 /* tp_traverse */
    0,                                                 /* tp_clear */
    0,                                                 /* tp_richcompare */
    0,                                                 /* tp_weaklistoffset */
    0,                                                 /* tp_iter */
    0,                                                 /* tp_iternext */
    Aggregator_methods,                                /* tp_methods */
    0,                                                 /* tp_members */
    0,                                                 /* tp_getset */
    0,                                                 /* tp_base */
    0,                                                 /* tp_dict */
    0,                                                 /* tp_descr_get */
    0,                                                 /* tp_descr_set */
    0,                                                 /* tp_dictoffset */
    (initproc)Aggregator_init,                         /* tp_init */
    0,                                                 /* tp_alloc */
    Aggregator_new,                                    /* tp_new */
};

int init_aggregator(PyObject *m) {
    if (register_type(m, &AggregatorType, "AggregatorUtility") < 0) return -1;

    return 0;
}
