#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/comparator.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregators.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <dftracer/utils/utilities/composites/dft/comparator/comparison_utility.h>
#include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft::aggregators;
using namespace dftracer::utils::utilities::composites::dft::comparator;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using dftracer::utils::python::arrow_result_to_table;
using dftracer::utils::utilities::common::arrow::ArrowExportResult;
#endif

static Runtime *get_runtime(ComparatorObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void Comparator_dealloc(ComparatorObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Comparator_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds) {
    ComparatorObject *self = (ComparatorObject *)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int Comparator_init(ComparatorObject *self, PyObject *args,
                           PyObject *kwds) {
    static const char *kwlist[] = {"runtime", NULL};
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", (char **)kwlist,
                                     &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }

    return 0;
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

struct ComparatorArgs {
    std::string baseline;
    std::string variant;
    std::string query;
    std::string group_by;
    std::string format;
    double time_interval_ms = 5000.0;
    double threshold = 0.0;
    std::size_t executor_threads = 0;
    std::string index_dir;
    bool force_rebuild = false;
    std::string config_path;
};

static int parse_comparator_args(PyObject *args, PyObject *kwds,
                                 ComparatorArgs &out) {
    static const char *kwlist[] = {
        "baseline",  "variant",          "query",     "group_by",
        "format",    "time_interval_ms", "threshold", "executor_threads",
        "index_dir", "force_rebuild",    "config",    NULL};

    const char *baseline = NULL;
    const char *variant = NULL;
    const char *query = "";
    const char *group_by = "";
    const char *format = "table";
    double time_interval_ms = 5000.0;
    double threshold = 0.0;
    Py_ssize_t executor_threads = 0;
    const char *index_dir = "";
    int force_rebuild = 0;
    const char *config = "";

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "ss|sssddnsps", (char **)kwlist, &baseline, &variant,
            &query, &group_by, &format, &time_interval_ms, &threshold,
            &executor_threads, &index_dir, &force_rebuild, &config))
        return -1;

    out.baseline = baseline;
    out.variant = variant;
    out.query = query;
    out.group_by = group_by;
    out.format = format;
    out.time_interval_ms = time_interval_ms;
    out.threshold = threshold;
    out.executor_threads = static_cast<std::size_t>(executor_threads);
    out.index_dir = index_dir;
    out.force_rebuild = force_rebuild != 0;
    out.config_path = config;

    return 0;
}

namespace {

void flatten_nodes(const ComparisonNode &node,
                   std::vector<const ComparisonNode *> &out) {
    out.push_back(&node);
    for (const auto &child : node.children) {
        flatten_nodes(child, out);
    }
}

CoroTask<EventAggregatorUtilityOutput> run_aggregation(
    std::vector<std::string> input_files, AggregationConfig agg_config,
    std::optional<common::query::Query> query, std::string index_dir,
    std::size_t checkpoint_size, bool force_rebuild,
    std::size_t executor_threads) {
    constexpr std::size_t CHUNK_SIZE_MB = 4;
    constexpr std::size_t BATCH_SIZE_MB = 4;

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Comparator Aggregation")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);
    Pipeline pipeline(pipeline_config);

    EventAggregatorUtility merger;
    std::atomic<int> global_chunk_idx{0};

    auto streaming_task = make_task(
        [&](CoroScope &ctx) -> CoroTask<void> {
            auto chunk_chan = coro::make_channel<ChunkAggregatorInput>(0);
            auto result_chan = coro::make_channel<ChunkAggregationOutput>(2);

            co_await ctx.scope([&](CoroScope &scope) -> CoroTask<void> {
                for (const auto &file_path : input_files) {
                    auto *global_chunk_idx_ptr = &global_chunk_idx;
                    scope.spawn([file_path, ch = chunk_chan->producer(),
                                 index_dir, checkpoint_size, force_rebuild,
                                 agg_config, query, global_chunk_idx_ptr](
                                    CoroScope & /*fctx*/) mutable
                                    -> CoroTask<void> {
                        [[maybe_unused]] auto producer_guard = ch.guard();

                        std::string index_path =
                            composites::dft::internal::determine_index_path(
                                file_path, index_dir);

                        auto meta_input =
                            composites::dft::MetadataCollectorUtilityInput::
                                from_file(file_path)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(force_rebuild)
                                    .with_index(index_path);
                        auto metadata =
                            co_await composites::dft::MetadataCollectorUtility{}
                                .process(meta_input);

                        if (!metadata.success) {
                            co_return;
                        }

                        FileChunkMapperUtility file_mapper;
                        auto mapper_input =
                            FileChunkMapperInput::from_metadata(metadata)
                                .with_config(agg_config)
                                .with_checkpoint_size(checkpoint_size)
                                .with_target_chunk_size(CHUNK_SIZE_MB)
                                .with_batch_size(BATCH_SIZE_MB * 1024 * 1024);
                        mapper_input.query = query;
                        auto file_chunks =
                            co_await file_mapper.process(mapper_input);

                        int start_idx = global_chunk_idx_ptr->fetch_add(
                            static_cast<int>(file_chunks.size()));
                        for (int i = 0;
                             i < static_cast<int>(file_chunks.size()); ++i) {
                            file_chunks[i].chunk_index = start_idx + i;
                        }

                        for (auto &chunk : file_chunks) {
                            if (!co_await ch.send(std::move(chunk))) {
                                co_return;
                            }
                        }
                        co_return;
                    });
                }

                for (std::size_t w = 0; w < executor_threads; ++w) {
                    (void)w;
                    scope.spawn([chunk_chan, rp = result_chan->producer(),
                                 result_chan](
                                    CoroScope &wctx) mutable -> CoroTask<void> {
                        [[maybe_unused]] auto producer_guard = rp.guard();
                        while (auto input = co_await wctx.receive(chunk_chan)) {
                            ChunkAggregatorUtility agg;
                            auto output = co_await agg.process(*input);
                            if (!co_await result_chan->send(
                                    std::move(output))) {
                                co_return;
                            }
                        }
                        co_return;
                    });
                }

                auto *merger_ptr = &merger;
                scope.spawn([result_chan,
                             merger_ptr](CoroScope &mctx) -> CoroTask<void> {
                    while (auto output = co_await mctx.receive(result_chan)) {
                        merger_ptr->merge_chunk(std::move(*output));
                    }
                    co_return;
                });

                co_return;
            });

            co_return;
        },
        "StreamingAggregate");

    EventAggregatorUtilityOutput result;
    auto post_task = make_task(
        [&](CoroScope & /*ctx*/) -> CoroTask<bool> {
            result = merger.finalize();
            co_return result.success;
        },
        "Finalize");

    post_task->depends_on(streaming_task);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(post_task);
    pipeline.execute();

    co_return result;
}

}  // namespace

static int run_comparison_pipeline(ComparatorObject *self,
                                   const ComparatorArgs &cargs,
                                   ComparisonOutput &output,
                                   std::string &error_msg) {
    ComparatorArgs args_copy = cargs;
    auto *output_ptr = &output;

    Py_BEGIN_ALLOW_THREADS try {
        ComparisonConfig config;
        if (!args_copy.config_path.empty()) {
            std::string parse_error;
            auto parsed = ComparisonConfig::from_json_file(
                args_copy.config_path, parse_error);
            if (!parsed) {
                error_msg = "Config error: " + parse_error;
                goto done;
            }
            config = std::move(*parsed);
        } else {
            config = ComparisonConfig::from_cli(
                args_copy.baseline, args_copy.variant, args_copy.query,
                args_copy.group_by);
        }

        config.format = args_copy.format;
        config.no_color = true;
        if (args_copy.executor_threads > 0)
            config.executor_threads = args_copy.executor_threads;
        if (!args_copy.index_dir.empty())
            config.index_dir = args_copy.index_dir;
        if (args_copy.force_rebuild)
            config.force_rebuild = args_copy.force_rebuild;
        if (args_copy.threshold > 0.0)
            config.defaults.threshold_pct = args_copy.threshold;
        if (args_copy.time_interval_ms > 0.0)
            config.defaults.time_interval_ms = args_copy.time_interval_ms;

        config.resolve();

        if (config.executor_threads == 0) {
            config.executor_threads = dftracer_utils_hardware_concurrency();
        }
        if (config.checkpoint_size == 0) {
            config.checkpoint_size =
                indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
        }

        // Create temp index dir if needed
        std::string temp_index_dir;
        if (config.index_dir.empty()) {
            auto temp_path = fs::temp_directory_path();
            temp_path /=
                "dftracer_cmp_py_" + std::to_string(std::time(nullptr)) + "_" +
                std::to_string(static_cast<int>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id())));
            temp_index_dir = temp_path.string();
            fs::create_directories(temp_index_dir);
            config.index_dir = temp_index_dir;
        }

        // Enumerate files
        auto enumerate_files =
            [](std::string path) -> CoroTask<std::vector<std::string>> {
            std::vector<std::string> files;
            if (fs::is_regular_file(path)) {
                files.push_back(path);
                co_return files;
            }
            filesystem::PatternDirectoryScannerUtility scanner;
            filesystem::PatternDirectoryScannerUtilityInput scan_input{
                path, {".pfw", ".pfw.gz"}, false};
            auto entries = co_await scanner.process(scan_input);
            files.reserve(entries.size());
            for (const auto &e : entries) {
                files.push_back(e.path.string());
            }
            co_return files;
        };

        Runtime *rt = get_runtime(self);

        auto *error_msg_ptr = &error_msg;
        auto task = [config, output_ptr, enumerate_files,
                     error_msg_ptr]() -> CoroTask<void> {
            auto baseline_files = co_await enumerate_files(config.baseline);
            auto variant_files = co_await enumerate_files(config.variant);

            if (baseline_files.empty()) {
                *error_msg_ptr =
                    "No trace files found in baseline: " + config.baseline;
                co_return;
            }
            if (variant_files.empty()) {
                *error_msg_ptr =
                    "No trace files found in variant: " + config.variant;
                co_return;
            }

            // Build indexes upfront
            {
                if (config.force_rebuild && !baseline_files.empty()) {
                    const std::string shared_index_path =
                        composites::dft::internal::determine_index_path(
                            baseline_files.front(), config.index_dir);
                    if (fs::exists(shared_index_path)) {
                        fs::remove_all(shared_index_path);
                    }
                }
                std::unordered_set<std::string> seen;
                std::vector<std::string> all_files;
                for (const auto &f : baseline_files) {
                    if (seen.insert(f).second) all_files.push_back(f);
                }
                for (const auto &f : variant_files) {
                    if (seen.insert(f).second) all_files.push_back(f);
                }
                std::vector<indexer::IndexBuildConfig> idx_configs;
                idx_configs.reserve(all_files.size());
                for (const auto &file_path : all_files) {
                    idx_configs.push_back(
                        indexer::IndexBuildConfig::for_file(file_path)
                            .with_checkpoint_size(config.checkpoint_size)
                            .with_force_rebuild(false)
                            .with_index_dir(config.index_dir));
                }
                std::vector<CoroTask<indexer::IndexBuildResult>> idx_tasks;
                idx_tasks.reserve(idx_configs.size());
                for (const auto &cfg : idx_configs) {
                    idx_tasks.push_back(
                        indexer::IndexBuilderUtility{}.process(cfg));
                }
                co_await coro::when_all(std::move(idx_tasks));
            }

            output_ptr->baseline_path = config.baseline;
            output_ptr->variant_path = config.variant;
            output_ptr->baseline_file_count = baseline_files.size();
            output_ptr->variant_file_count = variant_files.size();

            auto start_time = std::chrono::high_resolution_clock::now();

            for (auto &node : config.nodes) {
                std::vector<const ComparisonNode *> visitors;
                flatten_nodes(node, visitors);

                std::vector<ComparisonVisitorPair> pairs;
                pairs.reserve(visitors.size());

                for (const auto *visitor : visitors) {
                    std::optional<common::query::Query> query;
                    if (!visitor->composed_query.empty()) {
                        auto result = common::query::Query::from_string(
                            visitor->composed_query);
                        if (!result) {
                            *error_msg_ptr = "Invalid query for node '" +
                                             visitor->name +
                                             "': " + result.error().format();
                            co_return;
                        }
                        query = std::move(*result);
                    }

                    AggregationConfig agg_cfg;
                    agg_cfg.time_interval_us = static_cast<std::uint64_t>(
                        config.defaults.time_interval_ms * 1000.0);
                    agg_cfg.extra_group_keys = {};
                    agg_cfg.compute_statistics = true;
                    agg_cfg.compute_percentiles = true;
                    agg_cfg.percentiles = visitor->resolved_percentiles;
                    agg_cfg.sketch_accuracy = 0.01;
                    agg_cfg.track_process_parents = false;

                    auto [base_result, var_result] = co_await coro::when_all(
                        run_aggregation(
                            baseline_files, agg_cfg, query, config.index_dir,
                            config.checkpoint_size, config.force_rebuild,
                            config.executor_threads),
                        run_aggregation(
                            variant_files, agg_cfg, query, config.index_dir,
                            config.checkpoint_size, config.force_rebuild,
                            config.executor_threads));

                    if (pairs.empty()) {
                        output_ptr->baseline_meta = extract_metadata(
                            base_result.aggregations, baseline_files.size());
                        output_ptr->variant_meta = extract_metadata(
                            var_result.aggregations, variant_files.size());
                    }

                    ComparisonVisitorPair pair;
                    pair.baseline = std::move(base_result);
                    pair.variant = std::move(var_result);
                    pair.node = *visitor;
                    pairs.push_back(std::move(pair));
                }

                ComparisonUtilityInput cmp_input;
                cmp_input.visitors = std::move(pairs);
                cmp_input.root_node = node;
                cmp_input.baseline_file_count = baseline_files.size();
                cmp_input.variant_file_count = variant_files.size();

                ComparisonUtility cmp;
                auto cmp_output = co_await cmp.process(cmp_input);
                output_ptr->nodes.push_back(std::move(cmp_output.result));
            }

            // Inject metadata rows into root SUMMARY.
            auto meta_rows = build_metadata_metrics(output_ptr->baseline_meta,
                                                    output_ptr->variant_meta);
            for (auto &node : output_ptr->nodes) {
                node.summary.metrics.insert(node.summary.metrics.begin(),
                                            meta_rows.begin(), meta_rows.end());
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> duration =
                end_time - start_time;
            output_ptr->execution_time_ms = duration.count();
        };

        rt->submit(task(), "comparator").get();

        // Clean up temp index dir
        if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
            fs::remove_all(temp_index_dir);
        }
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
done:
    Py_END_ALLOW_THREADS

        return error_msg.empty()
        ? 0
        : -1;
}

// -----------------------------------------------------------------------
// compare() -- returns ArrowTable
// -----------------------------------------------------------------------

static PyObject *Comparator_compare(ComparatorObject *self, PyObject *args,
                                    PyObject *kwds) {
    ComparatorArgs cargs;
    if (parse_comparator_args(args, kwds, cargs) < 0) return NULL;

    ComparisonOutput output;
    std::string error_msg;
    if (run_comparison_pipeline(self, cargs, output, error_msg) < 0) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    auto arrow_result = output.to_arrow();
    if (!arrow_result.valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Failed to convert comparison output "
                        "to Arrow");
        return NULL;
    }
    return arrow_result_to_table(std::move(arrow_result));
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

// -----------------------------------------------------------------------
// compare_json() -- returns JSON string
// -----------------------------------------------------------------------

static PyObject *Comparator_compare_json(ComparatorObject *self, PyObject *args,
                                         PyObject *kwds) {
    ComparatorArgs cargs;
    if (parse_comparator_args(args, kwds, cargs) < 0) return NULL;

    ComparisonOutput output;
    std::string error_msg;
    if (run_comparison_pipeline(self, cargs, output, error_msg) < 0) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    TreeTableFormatter formatter;
    std::string json = formatter.render_json(output);
    return PyUnicode_FromStringAndSize(json.data(), (Py_ssize_t)json.size());
}

// -----------------------------------------------------------------------
// compare_table() -- returns formatted table string
// -----------------------------------------------------------------------

static PyObject *Comparator_compare_table(ComparatorObject *self,
                                          PyObject *args, PyObject *kwds) {
    ComparatorArgs cargs;
    if (parse_comparator_args(args, kwds, cargs) < 0) return NULL;

    ComparisonOutput output;
    std::string error_msg;
    if (run_comparison_pipeline(self, cargs, output, error_msg) < 0) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    FormatterOptions fmt_opts;
    fmt_opts.use_color = false;
    fmt_opts.use_unicode = false;
    TreeTableFormatter formatter(fmt_opts);

    // Render to a temporary file and read back as string
    char *buf = NULL;
    std::size_t buf_size = 0;
    FILE *memstream = open_memstream(&buf, &buf_size);
    if (!memstream) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create memory stream");
        return NULL;
    }

    formatter.render(memstream, output);
    fflush(memstream);
    fclose(memstream);

    PyObject *result = PyUnicode_FromStringAndSize(buf, (Py_ssize_t)buf_size);
    free(buf);
    return result;
}

// -----------------------------------------------------------------------
// __call__ delegates to compare()
// -----------------------------------------------------------------------

static PyObject *Comparator_call(PyObject *self, PyObject *args,
                                 PyObject *kwds) {
    return Comparator_compare((ComparatorObject *)self, args, kwds);
}

// -----------------------------------------------------------------------
// Method table
// -----------------------------------------------------------------------

static const char *COMPARE_DOC =
    "compare(baseline, variant, query='', group_by='',\n"
    "        format='table', time_interval_ms=5000.0,\n"
    "        threshold=0.0, executor_threads=0,\n"
    "        index_dir='', force_rebuild=False, config='')\n"
    "--\n"
    "\n"
    "Run comparison pipeline, return materialized ArrowTable.\n"
    "\n"
    "Args:\n"
    "    baseline (str): Baseline trace file or directory.\n"
    "    variant (str): Variant trace file or directory.\n"
    "    query (str): Query filter (default: all events).\n"
    "    group_by (str): Comma-separated group keys.\n"
    "    format (str): Output format (default 'table').\n"
    "    time_interval_ms (float): Time bucket in ms "
    "(default 5000).\n"
    "    threshold (float): Hide changes below this pct.\n"
    "    executor_threads (int): Parallel threads (0=auto).\n"
    "    index_dir (str): Directory for .dftindex stores.\n"
    "    force_rebuild (bool): Force index rebuild.\n"
    "    config (str): JSON config file path.\n"
    "\n"
    "Returns:\n"
    "    ArrowTable: Comparison results.\n";

static const char *COMPARE_JSON_DOC =
    "compare_json(baseline, variant, query='', group_by='',\n"
    "             format='table', time_interval_ms=5000.0,\n"
    "             threshold=0.0, executor_threads=0,\n"
    "             index_dir='', force_rebuild=False, "
    "config='')\n"
    "--\n"
    "\n"
    "Run comparison pipeline, return JSON string.\n"
    "\n"
    "Args:\n"
    "    baseline (str): Baseline trace file or directory.\n"
    "    variant (str): Variant trace file or directory.\n"
    "    query (str): Query filter (default: all events).\n"
    "    group_by (str): Comma-separated group keys.\n"
    "    format (str): Output format (default 'table').\n"
    "    time_interval_ms (float): Time bucket in ms "
    "(default 5000).\n"
    "    threshold (float): Hide changes below this pct.\n"
    "    executor_threads (int): Parallel threads (0=auto).\n"
    "    index_dir (str): Directory for .dftindex stores.\n"
    "    force_rebuild (bool): Force index rebuild.\n"
    "    config (str): JSON config file path.\n"
    "\n"
    "Returns:\n"
    "    str: JSON representation of comparison results.\n";

static const char *COMPARE_TABLE_DOC =
    "compare_table(baseline, variant, query='', group_by='',\n"
    "              format='table', time_interval_ms=5000.0,\n"
    "              threshold=0.0, executor_threads=0,\n"
    "              index_dir='', force_rebuild=False, "
    "config='')\n"
    "--\n"
    "\n"
    "Run comparison pipeline, return formatted table string.\n"
    "\n"
    "Args:\n"
    "    baseline (str): Baseline trace file or directory.\n"
    "    variant (str): Variant trace file or directory.\n"
    "    query (str): Query filter (default: all events).\n"
    "    group_by (str): Comma-separated group keys.\n"
    "    format (str): Output format (default 'table').\n"
    "    time_interval_ms (float): Time bucket in ms "
    "(default 5000).\n"
    "    threshold (float): Hide changes below this pct.\n"
    "    executor_threads (int): Parallel threads (0=auto).\n"
    "    index_dir (str): Directory for .dftindex stores.\n"
    "    force_rebuild (bool): Force index rebuild.\n"
    "    config (str): JSON config file path.\n"
    "\n"
    "Returns:\n"
    "    str: Formatted ASCII table of comparison results.\n";

static PyMethodDef Comparator_methods[] = {
    {"compare", (PyCFunction)Comparator_compare, METH_VARARGS | METH_KEYWORDS,
     COMPARE_DOC},
    {"compare_json", (PyCFunction)Comparator_compare_json,
     METH_VARARGS | METH_KEYWORDS, COMPARE_JSON_DOC},
    {"compare_table", (PyCFunction)Comparator_compare_table,
     METH_VARARGS | METH_KEYWORDS, COMPARE_TABLE_DOC},
    {NULL}};

PyTypeObject ComparatorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.ComparatorUtility", /* tp_name */
    sizeof(ComparatorObject),                            /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)Comparator_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    Comparator_call,                          /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "ComparatorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Compare DFTracer trace metrics between baseline and "
    "variant.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool "
    "control.\n"
    "        If None, uses the default global Runtime.\n\n"
    "compare(baseline, variant, ...) -> ArrowTable\n"
    "    Run comparison and return a materialized Arrow "
    "table.\n\n"
    "compare_json(baseline, variant, ...) -> str\n"
    "    Run comparison and return JSON string.\n\n"
    "compare_table(baseline, variant, ...) -> str\n"
    "    Run comparison and return formatted table "
    "string.\n",               /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    Comparator_methods,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Comparator_init, /* tp_init */
    0,                         /* tp_alloc */
    Comparator_new,            /* tp_new */
};

int init_comparator(PyObject *m) {
    if (PyType_Ready(&ComparatorType) < 0) return -1;

    Py_INCREF(&ComparatorType);
    if (PyModule_AddObject(m, "ComparatorUtility",
                           (PyObject *)&ComparatorType) < 0) {
        Py_DECREF(&ComparatorType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
