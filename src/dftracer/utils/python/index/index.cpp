// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/list_helpers.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/runtime_mixin.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/frame/agg_config.h>
#include <dftracer/utils/python/frame/dataframe.h>
#include <dftracer/utils/python/index/index.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>

#include <cstdint>

using dftracer::utils::python::core::guard;
using dftracer::utils::python::core::parse_kw;

using namespace dftracer::utils;

namespace {

struct StaleCheck {
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> changed;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> added;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> removed;
};

// Stat-only: mtime and size are the only signals O(1) to read, so a same-size
// edit with a restored mtime is invisible here.
StaleCheck stale_check(
    const trace::index::Index &index,
    dftracer::utils::dstd::Span<const dftracer::utils::dstd::String> paths) {
    const trace::index::Snapshot snapshot =
        dftracer::utils::python::core::py_unwrap(index.snapshot());

    StaleCheck result;
    dftracer::utils::dstd::StringViewSet listed;
    listed.reserve(paths.size());
    for (const dftracer::utils::dstd::String &path : paths) {
        const dftracer::utils::dstd::String logical =
            fileio::get_logical_path(path);
        listed.insert(dftracer::utils::dstd::String(logical));
        dftracer::utils::common::InputFileStat st;
        if (!dftracer::utils::common::stat_input(path, st)) st = {};
        const trace::index::SourceStat now{static_cast<std::uint64_t>(st.mtime),
                                           st.size};
        switch (snapshot.freshness(logical, now)) {
            case trace::index::Freshness::Current:
                break;
            case trace::index::Freshness::Missing:
                result.added.push_back(path);
                break;
            case trace::index::Freshness::Stale:
            case trace::index::Freshness::Failed:
                result.changed.push_back(path);
                break;
        }
    }

    for (const trace::index::FileRef &file :
         dftracer::utils::python::core::py_unwrap(snapshot.files())) {
        const dftracer::utils::dstd::StringView path = file.path();
        const dftracer::utils::dstd::StringView logical(path.data(),
                                                        path.size());
        if (listed.find(logical) == listed.end())
            result.removed.emplace_back(logical);
    }
    return result;
}

void Index_dealloc(IndexObject *self) {
    dftracer::utils::dstd::UniquePtr<trace::index::Index>(self->index).reset();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

PyObject *Index_new(PyTypeObject *type, PyObject * /*args*/,
                    PyObject * /*kwds*/) {
    auto *self = (IndexObject *)type->tp_alloc(type, 0);
    if (self) self->index = nullptr;
    return (PyObject *)self;
}

int Index_init(IndexObject *self, PyObject *args, PyObject *kwds) {
    const char *index_path = nullptr;
    if (!parse_kw<"s">(args, kwds, {"index_dir"}, &index_path)) return -1;
    return guard([&] {
        auto opened =
            dftracer::utils::python::core::py_unwrap(trace::index::Index::open(
                trace::index::normalize_index_root(index_path)));
        dftracer::utils::dstd::UniquePtr<trace::index::Index>(self->index)
            .reset();
        self->index = dftracer::utils::dstd::make_unique<trace::index::Index>(
                          std::move(opened))
                          .release();
        return 0;
    });
}

PyObject *Index_find_stale_files(IndexObject *self, PyObject *args,
                                 PyObject *kwds) {
    PyObject *paths_obj = nullptr;
    if (!parse_kw<"O">(args, kwds, {"paths"}, &paths_obj)) return nullptr;
    if (!self->index) {
        PyErr_SetString(PyExc_RuntimeError, "Index not initialised");
        return nullptr;
    }
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return nullptr;

    StaleCheck result;
    if (!run_blocking_r([&] { return stale_check(*self->index, paths); },
                        result))
        return nullptr;

    PyObject *changed = str_list_from(result.changed);
    PyObject *added = str_list_from(result.added);
    PyObject *removed = str_list_from(result.removed);
    PyObject *stale =
        PyBool_FromLong(!result.changed.empty() || !result.added.empty() ||
                        !result.removed.empty());
    PyObject *d = PyDict_New();
    if (!changed || !added || !removed || !stale || !d) {
        Py_XDECREF(changed);
        Py_XDECREF(added);
        Py_XDECREF(removed);
        Py_XDECREF(stale);
        Py_XDECREF(d);
        return nullptr;
    }
    PyDict_SetItemString(d, "changed", changed);
    PyDict_SetItemString(d, "added", added);
    PyDict_SetItemString(d, "removed", removed);
    PyDict_SetItemString(d, "stale", stale);
    Py_DECREF(changed);
    Py_DECREF(added);
    Py_DECREF(removed);
    Py_DECREF(stale);
    return d;
}

PyObject *Index_write_agg_config(IndexObject *self, PyObject *args) {
    if (!self->index) {
        PyErr_SetString(PyExc_RuntimeError, "Index not initialised");
        return nullptr;
    }
    PyObject *config_obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &config_obj)) return nullptr;
    const auto config =
        dftracer::utils::python::frame::agg_config_from_py(config_obj);
    if (!run_blocking([&] {
            dftracer::utils::python::core::py_unwrap(
                trace::index::schemas::dft::agg::tier::write_config(
                    *self->index,
                    trace::index::schemas::dft::agg::tier::make_config(
                        config)));
        }))
        return nullptr;
    Py_RETURN_NONE;
}

// The canonical typed hash the index builds its bloom filters and posting
// lists with, of one Python scalar.
PyObject *hash_value_fn(PyObject *, PyObject *x) {
    std::uint64_t h = 0;
    if (x == Py_None) {
        h = dstd::hash::value_hash_null();
    } else if (PyBool_Check(x)) {
        h = dstd::hash::value_hash_bool(x == Py_True);
    } else if (PyLong_Check(x)) {
        int overflow = 0;
        const long long v = PyLong_AsLongLongAndOverflow(x, &overflow);
        if (overflow == 0) {
            if (v == -1 && PyErr_Occurred()) return nullptr;
            h = dstd::hash::value_hash_int64(v);
        } else {
            const unsigned long long u =
                overflow > 0 ? PyLong_AsUnsignedLongLong(x) : 0;
            if (overflow < 0 || (u == static_cast<unsigned long long>(-1) &&
                                 PyErr_Occurred())) {
                PyErr_Clear();
                PyErr_SetString(PyExc_OverflowError,
                                "int out of range for int64 and uint64");
                return nullptr;
            }
            h = dstd::hash::value_hash_uint64(u);
        }
    } else if (PyFloat_Check(x)) {
        h = dstd::hash::value_hash_double(PyFloat_AS_DOUBLE(x));
    } else if (PyUnicode_Check(x)) {
        Py_ssize_t n = 0;
        const char *text = PyUnicode_AsUTF8AndSize(x, &n);
        if (!text) return nullptr;
        h = dstd::hash::value_hash_string(dftracer::utils::dstd::StringView(
            text, static_cast<std::size_t>(n)));
    } else if (PyBytes_Check(x)) {
        h = dstd::hash::value_hash_string(dftracer::utils::dstd::StringView(
            PyBytes_AS_STRING(x),
            static_cast<std::size_t>(PyBytes_GET_SIZE(x))));
    } else {
        PyErr_Format(PyExc_TypeError,
                     "hash_value takes None, bool, int, float, str or bytes, "
                     "not %.100s",
                     Py_TYPE(x)->tp_name);
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(h);
}

PyMethodDef HASH_METHODS[] = {
    {"_hash_value", hash_value_fn, METH_O,
     "_hash_value(x)\n--\n\nThe canonical typed hash of a scalar."},
    {nullptr, nullptr, 0, nullptr}};

PyObject *Index_summaries(IndexObject *self, PyObject *args, PyObject *kwds) {
    const char *file_path = nullptr;
    if (!parse_kw<"s">(args, kwds, {"file"}, &file_path)) return nullptr;
    if (!self->index) {
        PyErr_SetString(PyExc_RuntimeError, "Index not initialised");
        return nullptr;
    }
    dftracer::utils::dataframe::DataFrame frame;
    if (!run_blocking([&] {
            frame = dftracer::utils::python::core::py_unwrap(
                [&]() -> dftracer::utils::dstd::Result<
                          dftracer::utils::dataframe::DataFrame> {
                    const auto snapshot = self->index->snapshot();
                    if (!snapshot) return snapshot.error();
                    const auto file =
                        snapshot->file(fileio::get_logical_path(file_path));
                    if (!file)
                        return dftracer::utils::common::make_error<
                            "the index holds no entry for {}">(
                            dftracer::utils::common::ErrorCode::NotFound,
                            file_path);
                    const auto rows =
                        trace::index::summary_rows(*snapshot, *file);
                    if (!rows) return rows.error();
                    return trace::index::summary_frame(*rows);
                }());
        }))
        return nullptr;
    return dftracer::utils::python::frame::wrap_dataframe(std::move(frame));
}

PyMethodDef Index_methods[] = {
    {"summaries", DFTU_PYCFUNCTION(Index_summaries),
     METH_VARARGS | METH_KEYWORDS,
     "summaries(file) -> DataFrame\n"
     "One row per stored summary per chunk of the indexed trace `file`:\n"
     "path, chunk, summary, bytes and hash_version."},
    {"find_stale_files", DFTU_PYCFUNCTION(Index_find_stale_files),
     METH_VARARGS | METH_KEYWORDS,
     "find_stale_files(paths) -> dict\n"
     "Stat-only (mtime + size) staleness check of the given trace paths "
     "against the index. Returns {changed, added, removed, stale}."},
    {"write_agg_config", DFTU_PYCFUNCTION(Index_write_agg_config), METH_VARARGS,
     "write_agg_config(aggregation_config) -> None\n"
     "Record the aggregation config a distributed build aggregated its "
     "files with. Each file's dftracer.agg entry comes with its data."},
    {NULL, NULL, 0, NULL}};

}  // namespace

PyTypeObject IndexType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext.Index",
    .tp_basicsize = sizeof(IndexObject),
    .tp_dealloc = (destructor)Index_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Handle to a .dftindex index root.\n\n"
        "Used by the distributed indexer coordinator to check staleness and to "
        "record the aggregation of a distributed build.",
    .tp_methods = Index_methods,
    .tp_init = (initproc)Index_init,
    .tp_new = Index_new,
};

namespace dftracer::utils::python::index {

int init_index(PyObject *m) {
    if (register_type(m, &IndexType, "Index") < 0) return -1;
    if (PyModule_AddFunctions(m, HASH_METHODS) < 0) return -1;
    return 0;
}

}  // namespace dftracer::utils::python::index
