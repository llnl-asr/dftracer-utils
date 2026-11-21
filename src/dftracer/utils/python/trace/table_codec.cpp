// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/error_exception.h>
#include <dftracer/utils/python/core/runtime_mixin.h>
#include <dftracer/utils/python/core/seq_helpers.h>
#include <dftracer/utils/python/frame/lazyframe.h>
#include <dftracer/utils/python/trace/table_codec.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/table/table_codec.h>
#include <dftracer/utils/trace/table/table_source.h>
#include <dftracer/utils/trace/writer/output_writer.h>

using dftracer::utils::python::core::guard;
using dftracer::utils::python::core::py_unwrap;
using dftracer::utils::python::core::release_gil;
using namespace dftracer::utils;

namespace {

// Streams the plan into the writer and commits it.
dftracer::utils::coro::CoroTask<
    dftracer::utils::dstd::Result<trace::writer::OutputSummary>>
sink_plan(dataframe::LazyFrame lf, trace::writer::OutputOptions options) {
    auto opening = trace::writer::OutputWriter::open(std::move(options));
    if (!opening) co_return opening.error();
    trace::writer::OutputWriter out = std::move(*opening);
    auto gen = lf.stream(0);
    for (;;) {
        auto settled = co_await gen.settled_next();
        if (settled.stopped) co_await dftracer::utils::coro::stop_here();
        if (settled.error)
            co_return dstd::Unexpected(std::move(*settled.error));
        if (!*settled.value) break;
        auto &next_frame = **settled.value;
        if (!next_frame) co_return next_frame.error();
        auto writing = out.write(*next_frame);
        auto written = co_await std::move(writing);
        if (!written) co_return written.error();
    }
    auto closing = out.close();
    co_return co_await std::move(closing);
}

// _sink_output(plan, path, rows, mode, compress, level, member_size,
//              part_size, build_index, header_args, raw_columns, known_rows,
//              runtime) -> (root, rows, files)
PyObject *sink_output_py(PyObject *, PyObject *args) {
    PyObject *plan_obj = nullptr;
    PyObject *raw_obj = nullptr;
    PyObject *runtime_obj = nullptr;
    const char *path = nullptr;
    const char *rows = nullptr;
    const char *mode = nullptr;
    const char *header = nullptr;
    int compress = 1;
    int level = 6;
    int build_index = 1;
    Py_ssize_t member_size = 0;
    Py_ssize_t part_size = 0;
    long long known_rows = -1;
    if (!PyArg_ParseTuple(args, "OssspinnpzOLO", &plan_obj, &path, &rows, &mode,
                          &compress, &level, &member_size, &part_size,
                          &build_index, &header, &raw_obj, &known_rows,
                          &runtime_obj))
        return nullptr;
    const dataframe::LazyFrame *plan = python::frame::lazyframe_of(plan_obj);
    if (!plan) return nullptr;
    dstd::Vector<dstd::String> raw_names;
    if (!python::core::parse_string_seq(
            raw_obj, "raw_columns must be a sequence of str", raw_names))
        return nullptr;
    auto rt = runtime_from_arg(runtime_obj);
    if (!rt) return nullptr;
    trace::writer::OutputOptions options;
    options.path = dstd::String(path);
    options.rows = dstd::StringView(rows) == "events"
                       ? trace::writer::OutputRows::Events
                       : trace::writer::OutputRows::Records;
    options.mode = dstd::StringView(mode) == "overwrite"
                       ? trace::writer::OutputMode::Overwrite
                       : trace::writer::OutputMode::Error;
    options.compress = compress != 0;
    options.level = level;
    options.member_size = static_cast<std::size_t>(member_size);
    options.part_size = static_cast<std::size_t>(part_size);
    options.build_index = build_index != 0;
    if (header) {
        options.header_args_json = dstd::String(header);
        // A header with no "fields" is completed from the first batch.
        options.fields_from_first_batch =
            options.header_args_json.find("\"fields\"") == dstd::String::npos;
    }
    options.raw_columns = std::move(raw_names);
    options.known_rows = known_rows;
    const dataframe::LazyFrame lf = *plan;
    options.memory_budget = lf.memory_budget();
    return guard([&]() -> PyObject * {
        trace::writer::OutputSummary summary;
        py_unwrap(release_gil([&]() -> dstd::Result<void> {
            auto ran = rt->block_on(
                coro::named("sink_output", sink_plan(lf, options)));
            if (!ran) return ran.error();
            summary = std::move(*ran);
            if (options.build_index)
                return trace::writer::index_output(summary, options);
            return {};
        }));
        PyObject *files =
            PyList_New(static_cast<Py_ssize_t>(summary.files.size()));
        if (!files) return nullptr;
        for (std::size_t i = 0; i < summary.files.size(); ++i) {
            PyObject *name = PyUnicode_FromStringAndSize(
                summary.files[i].data(),
                static_cast<Py_ssize_t>(summary.files[i].size()));
            if (!name) {
                Py_DECREF(files);
                return nullptr;
            }
            PyList_SET_ITEM(files, static_cast<Py_ssize_t>(i), name);
        }
        PyObject *result =
            Py_BuildValue("(s#KN)", summary.root.data(),
                          static_cast<Py_ssize_t>(summary.root.size()),
                          static_cast<unsigned long long>(summary.rows), files);
        return result;
    });
}

// _table_read_counters(reset) -> dict
PyObject *read_counters_py(PyObject *, PyObject *arg) {
    const int reset = PyObject_IsTrue(arg);
    if (reset < 0) return nullptr;
    const auto c = trace::table::read_counters();
    if (reset) trace::table::reset_read_counters();
    return Py_BuildValue(
        "{s:K,s:K,s:K,s:K,s:K,s:K}", "chunks_read",
        static_cast<unsigned long long>(c.chunks_read), "chunks_skipped",
        static_cast<unsigned long long>(c.chunks_skipped), "indexed_files",
        static_cast<unsigned long long>(c.indexed_files), "streamed_files",
        static_cast<unsigned long long>(c.streamed_files), "direct_files",
        static_cast<unsigned long long>(c.direct_files), "bytes_decoded",
        static_cast<unsigned long long>(c.bytes_decoded));
}

PyMethodDef COUNTERS_DEF = {
    "_table_read_counters", read_counters_py, METH_O,
    "_table_read_counters(reset) -> dict\n\nWhat the table reads of this "
    "process did; reset zeroes them after the read."};

PyMethodDef SINK_DEF = {
    "_sink_output", sink_output_py, METH_VARARGS,
    "_sink_output(plan, path, rows, mode, compress, level, member_size, "
    "part_size, build_index, header_args, raw_columns, known_rows, runtime) "
    "-> (root, rows, files)\n\nWrite a plan's rows as events or records."};

}  // namespace

int dftracer::utils::python::trace::init_table_codec(PyObject *m) {
    PyMethodDef *defs[] = {&SINK_DEF, &COUNTERS_DEF};
    for (PyMethodDef *def : defs) {
        PyObject *fn = PyCFunction_NewEx(def, nullptr, nullptr);
        if (!fn) return -1;
        if (PyModule_AddObject(m, def->ml_name, fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }
    return 0;
}
