#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/python/memoryview_batch.h>

#include <cstring>

namespace dftracer::utils::python {

static void MemoryViewBatch_dealloc(MemoryViewBatchObject *self) {
    delete self->data;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int MemoryViewBatch_getbuffer(MemoryViewBatchObject *self,
                                     Py_buffer *view, int flags) {
    if (!self->data || self->data->buffer.empty()) {
        PyErr_SetString(PyExc_BufferError, "MemoryViewBatch has no data");
        return -1;
    }
    return PyBuffer_FillInfo(view, (PyObject *)self, self->data->buffer.data(),
                             static_cast<Py_ssize_t>(self->data->buffer.size()),
                             1, flags);
}

static Py_ssize_t MemoryViewBatch_length(MemoryViewBatchObject *self) {
    if (!self->data) return 0;
    return static_cast<Py_ssize_t>(self->data->num_entries());
}

PyObject *MemoryViewBatch_item(MemoryViewBatchObject *self, Py_ssize_t i) {
    if (!self->data) {
        PyErr_SetString(PyExc_IndexError, "MemoryViewBatch has no data");
        return NULL;
    }
    Py_ssize_t n = static_cast<Py_ssize_t>(self->data->num_entries());
    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "MemoryViewBatch index out of range");
        return NULL;
    }

    PyObject *full = PyMemoryView_FromObject((PyObject *)self);
    if (!full) return NULL;

    Py_ssize_t start = static_cast<Py_ssize_t>(self->data->offsets[i]);
    Py_ssize_t stop = start + static_cast<Py_ssize_t>(self->data->lengths[i]);
    PyObject *lo = PyLong_FromSsize_t(start);
    PyObject *hi = PyLong_FromSsize_t(stop);
    PyObject *slice = (lo && hi) ? PySlice_New(lo, hi, NULL) : NULL;
    Py_XDECREF(lo);
    Py_XDECREF(hi);
    if (!slice) {
        Py_DECREF(full);
        return NULL;
    }
    PyObject *entry = PyObject_GetItem(full, slice);  // sub-view, shares buffer
    Py_DECREF(slice);
    Py_DECREF(full);
    return entry;
}

static PyBufferProcs MemoryViewBatch_as_buffer = {
    (getbufferproc)MemoryViewBatch_getbuffer,
    NULL,
};

static PySequenceMethods MemoryViewBatch_as_sequence = {
    (lenfunc)MemoryViewBatch_length,
    NULL,
    NULL,
    (ssizeargfunc)MemoryViewBatch_item,
};

static PyObject *MemoryViewBatch_get_num_entries(MemoryViewBatchObject *self,
                                                 void *) {
    if (!self->data) return PyLong_FromLong(0);
    return PyLong_FromSsize_t(
        static_cast<Py_ssize_t>(self->data->num_entries()));
}

static PyObject *MemoryViewBatch_get_num_bytes(MemoryViewBatchObject *self,
                                               void *) {
    if (!self->data) return PyLong_FromLong(0);
    return PyLong_FromSsize_t(
        static_cast<Py_ssize_t>(self->data->buffer.size()));
}

static PyGetSetDef MemoryViewBatch_getsetters[] = {
    {"num_entries", (getter)MemoryViewBatch_get_num_entries, NULL,
     "Number of entries", NULL},
    {"num_bytes", (getter)MemoryViewBatch_get_num_bytes, NULL,
     "Total buffer size in bytes", NULL},
    {NULL}};

PyTypeObject MemoryViewBatchType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext._MemoryViewBatch",
    .tp_basicsize = sizeof(MemoryViewBatchObject),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)MemoryViewBatch_dealloc,
    .tp_as_sequence = &MemoryViewBatch_as_sequence,
    .tp_as_buffer = &MemoryViewBatch_as_buffer,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Zero-copy batch of byte entries backed by a contiguous buffer",
    .tp_getset = MemoryViewBatch_getsetters,
};

int init_memoryview_batch(PyObject *m) {
    if (PyType_Ready(&MemoryViewBatchType) < 0) return -1;
    Py_INCREF(&MemoryViewBatchType);
    if (PyModule_AddObject(m, "_MemoryViewBatch",
                           (PyObject *)&MemoryViewBatchType) < 0) {
        Py_DECREF(&MemoryViewBatchType);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python
