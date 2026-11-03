// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/python/index.h>
#include <dftracer/utils/python/py_agg_config.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>

#include <cstdint>

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
    const index::Index &index,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> &paths) {
    const index::Snapshot snapshot =
        dftracer::utils::common::unwrap(index.snapshot());

    StaleCheck result;
    dftracer::utils::dstd::StringViewSet listed;
    listed.reserve(paths.size());
    for (const dftracer::utils::dstd::String &path : paths) {
        const dftracer::utils::dstd::String logical =
            index::store::internal::get_logical_path(path);
        listed.insert(dftracer::utils::dstd::String(logical));
        dftracer::utils::common::InputFileStat st;
        if (!dftracer::utils::common::stat_input(path, st)) st = {};
        const index::SourceStat now{static_cast<std::uint64_t>(st.mtime),
                                    st.size};
        switch (snapshot.freshness(logical, now)) {
            case index::Freshness::Current:
                break;
            case index::Freshness::Missing:
                result.added.push_back(path);
                break;
            case index::Freshness::Stale:
            case index::Freshness::Failed:
                result.changed.push_back(path);
                break;
        }
    }

    for (const index::FileRef &file :
         dftracer::utils::common::unwrap(snapshot.files())) {
        const dftracer::utils::dstd::StringView path = file.path();
        const dftracer::utils::dstd::StringView logical(path.data(),
                                                        path.size());
        if (listed.find(logical) == listed.end())
            result.removed.emplace_back(logical);
    }
    return result;
}

void Index_dealloc(IndexObject *self) {
    delete self->index;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

PyObject *Index_new(PyTypeObject *type, PyObject * /*args*/,
                    PyObject * /*kwds*/) {
    auto *self = (IndexObject *)type->tp_alloc(type, 0);
    if (self) self->index = nullptr;
    return (PyObject *)self;
}

int Index_init(IndexObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"index_path", NULL};
    const char *index_path = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s",
                                     const_cast<char **>(kwlist), &index_path))
        return -1;
    try {
        auto opened = dftracer::utils::common::unwrap(index::Index::open(
            index::store::internal::normalize_index_root(index_path)));
        delete self->index;
        self->index = new index::Index(std::move(opened));
    } catch (const std::exception &e) {
        python::set_typed_py_error(e);
        return -1;
    }
    return 0;
}

PyObject *Index_find_stale_files(IndexObject *self, PyObject *args,
                                 PyObject *kwds) {
    static const char *kwlist[] = {"paths", NULL};
    PyObject *paths_obj = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                     const_cast<char **>(kwlist), &paths_obj))
        return nullptr;
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
    const auto config = python::agg_config_from_py(config_obj);
    if (!run_blocking([&] {
            index::schemas::dft::agg::tier::write_config(
                *self->index,
                index::schemas::dft::agg::tier::Config{
                    config.time_interval_us, config.params_hash(),
                    config.group_by_file, config.extra_group_keys});
        }))
        return nullptr;
    Py_RETURN_NONE;
}

PyMethodDef Index_methods[] = {
    {"find_stale_files", DFTU_PYCFUNCTION(Index_find_stale_files),
     METH_VARARGS | METH_KEYWORDS,
     "find_stale_files(paths) -> dict\n"
     "Stat-only (mtime + size) staleness check of the given trace paths "
     "against the index. Returns {changed, added, removed, stale}."},
    {"write_agg_config", DFTU_PYCFUNCTION(Index_write_agg_config), METH_VARARGS,
     "write_agg_config(aggregation_config) -> None\n"
     "Record the aggregation config a distributed build aggregated its "
     "files with. Each file's dftracer.agg entry comes with its data."},
    {NULL}};

}  // namespace

PyTypeObject IndexType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.Index",
    sizeof(IndexObject),
    0,
    (destructor)Index_dealloc,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Py_TPFLAGS_DEFAULT,
    "Handle to a .dftindex index root.\n\n"
    "Used by the distributed indexer coordinator to check staleness and to "
    "record the aggregation of a distributed build.",
    0,
    0,
    0,
    0,
    0,
    0,
    Index_methods,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    (initproc)Index_init,
    0,
    Index_new,
};

namespace dftracer::utils::python {

int init_index(PyObject *m) {
    if (register_type(m, &IndexType, "Index") < 0) return -1;
    return 0;
}

}  // namespace dftracer::utils::python
