// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/store.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>

#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

using dftracer::utils::store::Batch;
using dftracer::utils::store::CopyInfo;
using dftracer::utils::store::Cursor;
using dftracer::utils::store::FoldKind;
using dftracer::utils::store::Namespace;
using dftracer::utils::store::NamespaceSpec;
using dftracer::utils::store::PackOptions;
using dftracer::utils::store::ScopeKey;
using dftracer::utils::store::Snapshot;
using dftracer::utils::store::SourceStat;
using dftracer::utils::store::Store;
using dftracer::utils::store::Value;

using namespace dftracer::utils;

namespace {

struct StoreObject {
    PyObject_HEAD dstd::Optional<Store> store;
};

struct NamespaceObject {
    PyObject_HEAD Namespace ns;
};

struct BatchObject {
    PyObject_HEAD dstd::Optional<Store> store;
    dstd::Optional<Batch> batch;
};

struct SnapshotObject {
    PyObject_HEAD dstd::Optional<Snapshot> snapshot;
};

struct CursorObject {
    PyObject_HEAD dstd::Optional<Cursor> cursor;
    std::atomic<bool> busy;
    bool started;
};

PyTypeObject StoreType = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject NamespaceType = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject BatchType = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject SnapshotType = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject CursorType = {PyVarObject_HEAD_INIT(NULL, 0)};

template <class T>
void destroy(T &member) {
    member.~T();
}

template <class T>
T take(dstd::Expected<T, dstd::Error> &&result) {
    if (!result) throw common::DFTUtilsException(result.error());
    return std::move(*result);
}

void check(const dstd::Expected<void, dstd::Error> &result) {
    if (!result) throw common::DFTUtilsException(result.error());
}

template <class F>
bool guarded(F &&body) {
    try {
        body();
        return true;
    } catch (const std::exception &e) {
        dftracer::utils::python::set_typed_py_error(e);
    } catch (...) {
        PyErr_SetString(dftracer::utils::python::g_dft_error,
                        "unknown C++ exception");
    }
    return false;
}

bool as_u64(PyObject *obj, std::uint64_t &out) {
    const unsigned long long value = PyLong_AsUnsignedLongLong(obj);
    if (value == static_cast<unsigned long long>(-1) && PyErr_Occurred()) {
        return false;
    }
    out = value;
    return true;
}

class Bytes {
   public:
    Bytes() = default;
    Bytes(const Bytes &) = delete;
    Bytes &operator=(const Bytes &) = delete;
    Bytes(Bytes &&other) noexcept : view_(other.view_), held_(other.held_) {
        other.held_ = false;
    }
    Bytes &operator=(Bytes &&) = delete;
    ~Bytes() {
        if (held_) PyBuffer_Release(&view_);
    }

    bool acquire(PyObject *obj) {
        if (PyObject_GetBuffer(obj, &view_, PyBUF_SIMPLE) < 0) return false;
        held_ = true;
        return true;
    }

    dstd::ByteView view() const {
        return dstd::ByteView(static_cast<const unsigned char *>(view_.buf),
                              static_cast<std::size_t>(view_.len));
    }

   private:
    Py_buffer view_{};
    bool held_ = false;
};

bool path_text(PyObject *obj, dstd::String &out) {
    PyObject *path = PyOS_FSPath(obj);
    if (!path) return false;
    if (!PyUnicode_Check(path)) {
        Py_DECREF(path);
        PyErr_SetString(PyExc_TypeError, "a path must be str or PathLike");
        return false;
    }
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(path, &size);
    if (!text) {
        Py_DECREF(path);
        return false;
    }
    out.assign(text, static_cast<std::size_t>(size));
    Py_DECREF(path);
    return true;
}

bool parse_scope(PyObject *obj, ScopeKey &out) {
    if (obj == Py_None) {
        return guarded([&] { out = ScopeKey::root(); });
    }
    dstd::String one;
    if (path_text(obj, one)) {
        return guarded([&] {
            out = ScopeKey::file(dstd::StringView(one.data(), one.size()));
        });
    }
    if (!PyErr_ExceptionMatches(PyExc_TypeError)) return false;
    PyErr_Clear();
    PyObject *seq = PySequence_Fast(
        obj, "scope must be None, a path or a sequence of paths");
    if (!seq) return false;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
    if (count == 0) {
        Py_DECREF(seq);
        PyErr_SetString(PyExc_ValueError,
                        "a scope of several files needs at least one path");
        return false;
    }
    dstd::Vector<dstd::String> paths;
    bool ok = guarded([&] { paths.reserve(static_cast<std::size_t>(count)); });
    for (Py_ssize_t i = 0; ok && i < count; ++i) {
        dstd::String text;
        ok = path_text(PySequence_Fast_GET_ITEM(seq, i), text) &&
             guarded([&] { paths.push_back(std::move(text)); });
    }
    Py_DECREF(seq);
    if (!ok) return false;
    return guarded([&] {
        dstd::Vector<dstd::StringView> views;
        views.reserve(paths.size());
        for (const dstd::String &path : paths) {
            views.emplace_back(path.data(), path.size());
        }
        out = ScopeKey::files(
            dstd::Span<const dstd::StringView>(views.data(), views.size()));
    });
}

bool parse_sources(PyObject *obj, dstd::Vector<SourceStat> &out) {
    if (obj == Py_None) return true;
    PyObject *seq = PySequence_Fast(
        obj, "sources must be a sequence of (mtime, size, hash)");
    if (!seq) return false;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
    bool ok = true;
    for (Py_ssize_t i = 0; ok && i < count; ++i) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 3) {
            PyErr_SetString(PyExc_TypeError,
                            "a source must be an (mtime, size, hash) tuple");
            ok = false;
            break;
        }
        SourceStat stat;
        ok = as_u64(PyTuple_GET_ITEM(item, 0), stat.mtime) &&
             as_u64(PyTuple_GET_ITEM(item, 1), stat.size) &&
             as_u64(PyTuple_GET_ITEM(item, 2), stat.hash) &&
             guarded([&] { out.push_back(stat); });
    }
    Py_DECREF(seq);
    return ok;
}

PyObject *bytes_of(dstd::ByteView view) {
    return PyBytes_FromStringAndSize(
        reinterpret_cast<const char *>(view.data()),
        static_cast<Py_ssize_t>(view.size()));
}

Namespace *namespace_of(PyObject *obj) {
    if (!PyObject_TypeCheck(obj, &NamespaceType)) {
        PyErr_SetString(PyExc_TypeError, "ns must be a store namespace");
        return nullptr;
    }
    return &reinterpret_cast<NamespaceObject *>(obj)->ns;
}

Store *store_of(StoreObject *self) {
    if (!self->store) {
        PyErr_SetString(dftracer::utils::python::g_dft_error,
                        "the store is not open");
        return nullptr;
    }
    return &*self->store;
}

Batch *batch_of(BatchObject *self) {
    if (!self->batch) {
        PyErr_SetString(dftracer::utils::python::g_dft_error,
                        "the batch is already committed or discarded");
        return nullptr;
    }
    return &*self->batch;
}

Snapshot *snapshot_of(SnapshotObject *self) {
    if (!self->snapshot) {
        PyErr_SetString(dftracer::utils::python::g_dft_error,
                        "the snapshot is not open");
        return nullptr;
    }
    return &*self->snapshot;
}

/* Store */

PyObject *Store_new(PyTypeObject *type, PyObject *, PyObject *) {
    auto *self = reinterpret_cast<StoreObject *>(type->tp_alloc(type, 0));
    if (self) new (&self->store) dstd::Optional<Store>();
    return reinterpret_cast<PyObject *>(self);
}

void Store_dealloc(StoreObject *self) {
    destroy(self->store);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

int Store_init(StoreObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"path", NULL};
    PyObject *path = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                     const_cast<char **>(kwlist), &path)) {
        return -1;
    }
    dstd::String dir;
    if (!path_text(path, dir)) return -1;
    dstd::Optional<Store> opened;
    int unused = 0;
    if (!run_blocking_r(
            [&] {
                opened.emplace(take(
                    Store::open(dstd::StringView(dir.data(), dir.size()))));
                return 0;
            },
            unused)) {
        return -1;
    }
    self->store = std::move(opened);
    return 0;
}

PyObject *Store_namespace(StoreObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"name",    "format_version", "fold",
                                   "durable", "shared",         NULL};
    const char *name = NULL;
    PyObject *version_arg = NULL;
    int fold = 0;
    int durable = 0;
    int shared = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Oipp",
                                     const_cast<char **>(kwlist), &name,
                                     &version_arg, &fold, &durable, &shared)) {
        return NULL;
    }
    std::uint64_t format_version = 0;
    if (version_arg && !as_u64(version_arg, format_version)) return NULL;
    if (format_version > UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "format_version does not fit in 32 bits");
        return NULL;
    }
    Store *store = store_of(self);
    if (!store) return NULL;
    if (fold < static_cast<int>(FoldKind::None) ||
        fold > static_cast<int>(FoldKind::Concat)) {
        PyErr_SetString(PyExc_ValueError, "unknown fold kind");
        return NULL;
    }
    NamespaceSpec spec;
    spec.name = name;
    spec.format_version = static_cast<std::uint32_t>(format_version);
    spec.fold = static_cast<FoldKind>(fold);
    spec.durable = durable != 0;
    spec.shared = shared != 0;
    Namespace added;
    if (!run_blocking_r([&] { return take(store->ns(spec)); }, added)) {
        return NULL;
    }
    auto *obj = reinterpret_cast<NamespaceObject *>(
        NamespaceType.tp_alloc(&NamespaceType, 0));
    if (!obj) return NULL;
    new (&obj->ns) Namespace(std::move(added));
    return reinterpret_cast<PyObject *>(obj);
}

PyObject *Store_batch(StoreObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", "params_hash", "sources",
                                   NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    PyObject *hash_arg = NULL;
    PyObject *sources_arg = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|OO",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg, &hash_arg, &sources_arg)) {
        return NULL;
    }
    std::uint64_t params_hash = 0;
    if (hash_arg && !as_u64(hash_arg, params_hash)) return NULL;
    Store *store = store_of(self);
    if (!store) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    dstd::Vector<SourceStat> sources;
    if (!parse_sources(sources_arg, sources)) return NULL;
    dstd::Optional<Batch> batch;
    if (!run_blocking_r(
            [&] {
                return dstd::Optional<Batch>(
                    store->batch(*ns, scope, params_hash,
                                 sources.empty() ? nullptr : &sources));
            },
            batch)) {
        return NULL;
    }
    auto *obj =
        reinterpret_cast<BatchObject *>(BatchType.tp_alloc(&BatchType, 0));
    if (!obj) return NULL;
    new (&obj->store) dstd::Optional<Store>(*store);
    new (&obj->batch) dstd::Optional<Batch>(std::move(batch));
    return reinterpret_cast<PyObject *>(obj);
}

PyObject *Store_flush(StoreObject *self, PyObject *) {
    Store *store = store_of(self);
    if (!store) return NULL;
    if (!run_blocking([&] {
            check(dftracer::utils::default_runtime().block_on(store->flush()));
        })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *Store_refresh(StoreObject *self, PyObject *) {
    Store *store = store_of(self);
    if (!store) return NULL;
    if (!run_blocking([&] { check(store->refresh()); })) return NULL;
    Py_RETURN_NONE;
}

PyObject *Store_close(StoreObject *self, PyObject *) {
    Store *store = store_of(self);
    if (!store) return NULL;
    if (!run_blocking([&] {
            check(dftracer::utils::default_runtime().block_on(store->close()));
        })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *Store_drop(StoreObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg)) {
        return NULL;
    }
    Store *store = store_of(self);
    if (!store) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    if (!run_blocking([&] { check(store->drop(*ns, scope)); })) return NULL;
    Py_RETURN_NONE;
}

PyObject *Store_pack(StoreObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"small_file_bytes", "dead_ratio",
                                   "keep_seconds", NULL};
    PyObject *small_arg = Py_None;
    PyObject *ratio_arg = Py_None;
    PyObject *keep_arg = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOO",
                                     const_cast<char **>(kwlist), &small_arg,
                                     &ratio_arg, &keep_arg)) {
        return NULL;
    }
    Store *store = store_of(self);
    if (!store) return NULL;
    PackOptions options;
    if (small_arg != Py_None) {
        options.small_file_bytes = PyLong_AsUnsignedLongLong(small_arg);
        if (PyErr_Occurred()) return NULL;
    }
    if (ratio_arg != Py_None) {
        options.dead_ratio = PyFloat_AsDouble(ratio_arg);
        if (PyErr_Occurred()) return NULL;
    }
    if (keep_arg != Py_None) {
        options.keep_seconds = PyLong_AsUnsignedLongLong(keep_arg);
        if (PyErr_Occurred()) return NULL;
    }
    dftracer::utils::store::PackReport report;
    if (!run_blocking_r(
            [&] {
                return take(dftracer::utils::default_runtime().block_on(
                    store->pack(options)));
            },
            report)) {
        return NULL;
    }
    return Py_BuildValue("(KKKKK)",
                         static_cast<unsigned long long>(report.files_packed),
                         static_cast<unsigned long long>(report.copies_written),
                         static_cast<unsigned long long>(report.copies_skipped),
                         static_cast<unsigned long long>(report.files_deleted),
                         static_cast<unsigned long long>(report.bytes_deleted));
}

PyObject *Store_snapshot(StoreObject *self, PyObject *) {
    Store *store = store_of(self);
    if (!store) return NULL;
    dstd::Optional<Snapshot> view;
    if (!run_blocking_r(
            [&] { return dstd::Optional<Snapshot>(take(store->snapshot())); },
            view)) {
        return NULL;
    }
    auto *obj = reinterpret_cast<SnapshotObject *>(
        SnapshotType.tp_alloc(&SnapshotType, 0));
    if (!obj) return NULL;
    new (&obj->snapshot) dstd::Optional<Snapshot>(std::move(view));
    return reinterpret_cast<PyObject *>(obj);
}

PyMethodDef Store_methods[] = {
    {"namespace", DFTU_PYCFUNCTION(Store_namespace),
     METH_VARARGS | METH_KEYWORDS,
     "namespace(name, format_version=0, fold=0, durable=False, shared=False)\n"
     "\n"
     "Register a namespace with this process's writer and return it."},
    {"batch", DFTU_PYCFUNCTION(Store_batch), METH_VARARGS | METH_KEYWORDS,
     "batch(ns, scope, params_hash=0, sources=None)\n"
     "\n"
     "Start a batch of writes for one scope."},
    {"flush", DFTU_PYCFUNCTION(Store_flush), METH_NOARGS,
     "Make the committed batches visible to readers."},
    {"refresh", DFTU_PYCFUNCTION(Store_refresh), METH_NOARGS,
     "Look for new data of other processes now."},
    {"close", DFTU_PYCFUNCTION(Store_close), METH_NOARGS,
     "Flush and close this process's writer."},
    {"drop", DFTU_PYCFUNCTION(Store_drop), METH_VARARGS | METH_KEYWORDS,
     "drop(ns, scope)\n"
     "\n"
     "Hide the whole scope."},
    {"pack", DFTU_PYCFUNCTION(Store_pack), METH_VARARGS | METH_KEYWORDS,
     "pack(small_file_bytes=None, dead_ratio=None, keep_seconds=None)\n"
     "\n"
     "Rewrite the chosen data files. Returns the counts of files packed,\n"
     "copies written, copies skipped, files deleted and bytes deleted."},
    {"snapshot", DFTU_PYCFUNCTION(Store_snapshot), METH_NOARGS,
     "A point-in-time view of every data file of the store."},
    {NULL}};

/* Namespace */

void Namespace_dealloc(NamespaceObject *self) {
    destroy(self->ns);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *Namespace_name(NamespaceObject *self, void *) {
    return PyUnicode_FromStringAndSize(
        self->ns.spec.name.data(),
        static_cast<Py_ssize_t>(self->ns.spec.name.size()));
}

PyObject *Namespace_format_version(NamespaceObject *self, void *) {
    return PyLong_FromUnsignedLong(self->ns.spec.format_version);
}

PyObject *Namespace_fold(NamespaceObject *self, void *) {
    return PyLong_FromLong(static_cast<long>(self->ns.spec.fold));
}

PyObject *Namespace_shared(NamespaceObject *self, void *) {
    return PyBool_FromLong(self->ns.spec.shared ? 1 : 0);
}

PyObject *Namespace_durable(NamespaceObject *self, void *) {
    return PyBool_FromLong(self->ns.spec.durable ? 1 : 0);
}

PyGetSetDef Namespace_getsetters[] = {
    {"name", reinterpret_cast<getter>(Namespace_name), NULL, "Namespace name.",
     NULL},
    {"format_version", reinterpret_cast<getter>(Namespace_format_version), NULL,
     "Format version of the stored values.", NULL},
    {"fold", reinterpret_cast<getter>(Namespace_fold), NULL,
     "Fold kind number; 0 for none.", NULL},
    {"durable", reinterpret_cast<getter>(Namespace_durable), NULL,
     "Whether the namespace is durable.", NULL},
    {"shared", reinterpret_cast<getter>(Namespace_shared), NULL,
     "Whether each scope holds the keys of every writer.", NULL},
    {NULL}};

/* Batch */

void Batch_dealloc(BatchObject *self) {
    destroy(self->batch);
    destroy(self->store);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *Batch_write(BatchObject *self, PyObject *args, bool fold) {
    PyObject *key_arg = NULL;
    PyObject *value_arg = NULL;
    if (!PyArg_ParseTuple(args, "OO", &key_arg, &value_arg)) return NULL;
    Batch *batch = batch_of(self);
    if (!batch) return NULL;
    Bytes key;
    Bytes value;
    if (!key.acquire(key_arg) || !value.acquire(value_arg)) return NULL;
    if (!guarded([&] {
            if (fold) {
                batch->fold(key.view(), value.view());
            } else {
                batch->put(key.view(), value.view());
            }
        })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *Batch_put(BatchObject *self, PyObject *args) {
    return Batch_write(self, args, false);
}

PyObject *Batch_fold(BatchObject *self, PyObject *args) {
    return Batch_write(self, args, true);
}

PyObject *Batch_erase(BatchObject *self, PyObject *args) {
    PyObject *key_arg = NULL;
    if (!PyArg_ParseTuple(args, "O", &key_arg)) return NULL;
    Batch *batch = batch_of(self);
    if (!batch) return NULL;
    Bytes key;
    if (!key.acquire(key_arg)) return NULL;
    if (!guarded([&] { batch->erase(key.view()); })) return NULL;
    Py_RETURN_NONE;
}

PyObject *Batch_erase_range(BatchObject *self, PyObject *args) {
    PyObject *begin_arg = NULL;
    PyObject *end_arg = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &begin_arg, &end_arg)) return NULL;
    Batch *batch = batch_of(self);
    if (!batch) return NULL;
    Bytes begin;
    Bytes end;
    if (!begin.acquire(begin_arg)) return NULL;
    if (end_arg != Py_None && !end.acquire(end_arg)) return NULL;
    if (!guarded([&] { batch->erase_range(begin.view(), end.view()); })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *Batch_commit(BatchObject *self, PyObject *) {
    if (!batch_of(self)) return NULL;
    Batch owned = std::move(*self->batch);
    self->batch.reset();
    Store store = *self->store;
    if (!run_blocking([&] {
            check(dftracer::utils::default_runtime().block_on(
                store.commit(std::move(owned))));
        })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *Batch_discard(BatchObject *self, PyObject *) {
    self->batch.reset();
    Py_RETURN_NONE;
}

PyMethodDef Batch_methods[] = {
    {"put", DFTU_PYCFUNCTION(Batch_put), METH_VARARGS,
     "put(key, value)\n\nAdd a key and value; the bytes are copied."},
    {"fold", DFTU_PYCFUNCTION(Batch_fold), METH_VARARGS,
     "fold(key, value)\n\nAdd a fold input to key (fold namespaces only)."},
    {"erase", DFTU_PYCFUNCTION(Batch_erase), METH_VARARGS,
     "erase(key)\n\nHide key in the copy of this batch's scope."},
    {"erase_range", DFTU_PYCFUNCTION(Batch_erase_range), METH_VARARGS,
     "erase_range(begin, end=None)\n\n"
     "Hide every key in [begin, end); None means to the last key."},
    {"commit", DFTU_PYCFUNCTION(Batch_commit), METH_NOARGS,
     "Commit and consume the batch."},
    {"discard", DFTU_PYCFUNCTION(Batch_discard), METH_NOARGS,
     "Drop the batch without writing it."},
    {NULL}};

/* Snapshot */

void Snapshot_dealloc(SnapshotObject *self) {
    destroy(self->snapshot);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *Snapshot_get(SnapshotObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", "key", NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    PyObject *key_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg, &key_arg)) {
        return NULL;
    }
    Snapshot *snapshot = snapshot_of(self);
    if (!snapshot) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    Bytes key;
    if (!key.acquire(key_arg)) return NULL;
    dstd::Optional<Value> found;
    if (!run_blocking_r(
            [&] { return take(snapshot->get(*ns, scope, key.view())); },
            found)) {
        return NULL;
    }
    if (!found) Py_RETURN_NONE;
    return bytes_of(found->bytes());
}

PyObject *Snapshot_multi_get(SnapshotObject *self, PyObject *args,
                             PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", "keys", NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    PyObject *keys_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg, &keys_arg)) {
        return NULL;
    }
    Snapshot *snapshot = snapshot_of(self);
    if (!snapshot) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    PyObject *seq =
        PySequence_Fast(keys_arg, "keys must be a sequence of bytes-like");
    if (!seq) return NULL;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
    dstd::Vector<Bytes> held;
    held.reserve(static_cast<std::size_t>(count));
    dstd::Vector<dstd::ByteView> views;
    views.reserve(static_cast<std::size_t>(count));
    for (Py_ssize_t i = 0; i < count; ++i) {
        Bytes key;
        if (!key.acquire(PySequence_Fast_GET_ITEM(seq, i))) {
            Py_DECREF(seq);
            return NULL;
        }
        views.push_back(key.view());
        held.push_back(std::move(key));
    }
    Py_DECREF(seq);
    dstd::Vector<dstd::Optional<Value>> found;
    if (!run_blocking_r(
            [&] {
                return take(
                    snapshot->multi_get(*ns, scope,
                                        dstd::Span<const dstd::ByteView>(
                                            views.data(), views.size())));
            },
            found)) {
        return NULL;
    }
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(found.size()));
    if (!list) return NULL;
    for (std::size_t i = 0; i < found.size(); ++i) {
        PyObject *item = NULL;
        if (found[i]) {
            item = bytes_of(found[i]->bytes());
            if (!item) {
                Py_DECREF(list);
                return NULL;
            }
        } else {
            Py_INCREF(Py_None);
            item = Py_None;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
    }
    return list;
}

PyObject *Snapshot_scan(SnapshotObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", "prefix", NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    PyObject *prefix_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|O",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg, &prefix_arg)) {
        return NULL;
    }
    Snapshot *snapshot = snapshot_of(self);
    if (!snapshot) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    Bytes prefix;
    if (prefix_arg && !prefix.acquire(prefix_arg)) return NULL;
    dstd::Optional<Cursor> cursor;
    if (!run_blocking_r(
            [&] {
                return dstd::Optional<Cursor>(
                    snapshot->scan(*ns, scope, prefix.view()));
            },
            cursor)) {
        return NULL;
    }
    auto *obj =
        reinterpret_cast<CursorObject *>(CursorType.tp_alloc(&CursorType, 0));
    if (!obj) return NULL;
    new (&obj->cursor) dstd::Optional<Cursor>(std::move(cursor));
    new (&obj->busy) std::atomic<bool>(false);
    obj->started = false;
    return reinterpret_cast<PyObject *>(obj);
}

PyObject *Snapshot_copy_info(SnapshotObject *self, PyObject *args,
                             PyObject *kwds) {
    static const char *kwlist[] = {"ns", "scope", NULL};
    PyObject *ns_arg = NULL;
    PyObject *scope_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO",
                                     const_cast<char **>(kwlist), &ns_arg,
                                     &scope_arg)) {
        return NULL;
    }
    Snapshot *snapshot = snapshot_of(self);
    if (!snapshot) return NULL;
    Namespace *ns = namespace_of(ns_arg);
    if (!ns) return NULL;
    ScopeKey scope;
    if (!parse_scope(scope_arg, scope)) return NULL;
    CopyInfo info;
    if (!run_blocking_r([&] { return snapshot->copy_info(*ns, scope); },
                        info)) {
        return NULL;
    }
    PyObject *sources =
        PyList_New(static_cast<Py_ssize_t>(info.sources.size()));
    if (!sources) return NULL;
    for (std::size_t i = 0; i < info.sources.size(); ++i) {
        PyObject *item = Py_BuildValue(
            "(KKK)", static_cast<unsigned long long>(info.sources[i].mtime),
            static_cast<unsigned long long>(info.sources[i].size),
            static_cast<unsigned long long>(info.sources[i].hash));
        if (!item) {
            Py_DECREF(sources);
            return NULL;
        }
        PyList_SET_ITEM(sources, static_cast<Py_ssize_t>(i), item);
    }
    PyObject *result = Py_BuildValue(
        "(iKKN)", static_cast<int>(info.state),
        static_cast<unsigned long long>(info.generation),
        static_cast<unsigned long long>(info.params_hash), sources);
    return result;
}

PyMethodDef Snapshot_methods[] = {
    {"get", DFTU_PYCFUNCTION(Snapshot_get), METH_VARARGS | METH_KEYWORDS,
     "get(ns, scope, key)\n\nThe value of key as bytes, or None."},
    {"multi_get", DFTU_PYCFUNCTION(Snapshot_multi_get),
     METH_VARARGS | METH_KEYWORDS,
     "multi_get(ns, scope, keys)\n\nOne bytes-or-None per key, in order."},
    {"scan", DFTU_PYCFUNCTION(Snapshot_scan), METH_VARARGS | METH_KEYWORDS,
     "scan(ns, scope, prefix=b'')\n\n"
     "An iterator of (key, value) in key order over the keys with prefix."},
    {"copy_info", DFTU_PYCFUNCTION(Snapshot_copy_info),
     METH_VARARGS | METH_KEYWORDS,
     "copy_info(ns, scope)\n\n"
     "(state, generation, params_hash, sources) of the current copy."},
    {NULL}};

/* Cursor */

void Cursor_dealloc(CursorObject *self) {
    destroy(self->cursor);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

// The key and value are views that die at the next advance, and they are
// copied to bytes after the GIL is back. One thread at a time may use a
// cursor, which keeps that copy free.
struct CursorUse {
    explicit CursorUse(CursorObject *cursor) : cursor_(cursor) {
        bool idle = false;
        held_ = cursor_->busy.compare_exchange_strong(idle, true);
    }
    ~CursorUse() {
        if (held_) cursor_->busy.store(false);
    }
    CursorUse(const CursorUse &) = delete;
    CursorUse &operator=(const CursorUse &) = delete;
    bool held() const { return held_; }

   private:
    CursorObject *cursor_;
    bool held_;
};

PyObject *Cursor_iternext(CursorObject *self) {
    if (!self->cursor) return NULL;
    CursorUse use(self);
    if (!use.held()) {
        PyErr_SetString(dftracer::utils::python::g_dft_error,
                        "the cursor is in use by another thread");
        return NULL;
    }
    Cursor &cursor = *self->cursor;
    dstd::ByteView key;
    dstd::ByteView value;
    int has_entry = 0;
    if (!run_blocking_r(
            [&] {
                if (self->started) {
                    if (!cursor.at_end()) cursor.next();
                } else {
                    self->started = true;
                }
                if (cursor.at_end()) {
                    if (cursor.error()) {
                        throw common::DFTUtilsException(*cursor.error());
                    }
                    return 0;
                }
                key = cursor.key();
                value = take(cursor.value());
                return 1;
            },
            has_entry)) {
        return NULL;
    }
    if (!has_entry) return NULL;
    PyObject *key_bytes = bytes_of(key);
    if (!key_bytes) return NULL;
    PyObject *value_bytes = bytes_of(value);
    if (!value_bytes) {
        Py_DECREF(key_bytes);
        return NULL;
    }
    return Py_BuildValue("(NN)", key_bytes, value_bytes);
}

template <class Obj>
void fill_type(PyTypeObject &type, const char *name, destructor dealloc,
               const char *doc, PyMethodDef *methods = NULL,
               PyGetSetDef *getsets = NULL) {
    type.tp_name = name;
    type.tp_basicsize = sizeof(Obj);
    type.tp_dealloc = dealloc;
    type.tp_flags = Py_TPFLAGS_DEFAULT;
    type.tp_doc = doc;
    type.tp_methods = methods;
    type.tp_getset = getsets;
}

}  // namespace

int dftracer::utils::python::init_store(PyObject *m) {
    fill_type<StoreObject>(
        StoreType, "dftracer_utils_ext.Store",
        reinterpret_cast<destructor>(Store_dealloc),
        "Store(path)\n"
        "--\n"
        "\n"
        "A key-value store in the directory path, created when it does not\n"
        "exist. Use dftracer.utils.Store, which wraps it.\n",
        Store_methods);
    StoreType.tp_new = Store_new;
    StoreType.tp_init = reinterpret_cast<initproc>(Store_init);

    fill_type<NamespaceObject>(NamespaceType,
                               "dftracer_utils_ext.StoreNamespace",
                               reinterpret_cast<destructor>(Namespace_dealloc),
                               "A namespace registered with Store.namespace.",
                               NULL, Namespace_getsetters);

    fill_type<BatchObject>(BatchType, "dftracer_utils_ext.StoreBatch",
                           reinterpret_cast<destructor>(Batch_dealloc),
                           "The writes of one commit.", Batch_methods);

    fill_type<SnapshotObject>(SnapshotType, "dftracer_utils_ext.StoreSnapshot",
                              reinterpret_cast<destructor>(Snapshot_dealloc),
                              "A point-in-time view of a store.",
                              Snapshot_methods);

    fill_type<CursorObject>(CursorType, "dftracer_utils_ext.StoreCursor",
                            reinterpret_cast<destructor>(Cursor_dealloc),
                            "An iterator of (key, value) over a scan.");
    CursorType.tp_iter = PyObject_SelfIter;
    CursorType.tp_iternext = reinterpret_cast<iternextfunc>(Cursor_iternext);

    if (register_type(m, &StoreType, "Store") < 0) return -1;
    if (register_type(m, &NamespaceType, "StoreNamespace") < 0) return -1;
    if (register_type(m, &BatchType, "StoreBatch") < 0) return -1;
    if (register_type(m, &SnapshotType, "StoreSnapshot") < 0) return -1;
    if (register_type(m, &CursorType, "StoreCursor") < 0) return -1;
    return 0;
}
