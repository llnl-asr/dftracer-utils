#include <Python.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/lazy_json_line_processor.h>
#include <dftracer/utils/python/pylist_line_processor.h>
#include <dftracer/utils/python/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <structmember.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

std::shared_ptr<dftracer::utils::utilities::reader::internal::Reader> *
cast_reader_handle(dft_reader_handle_t handle) {
    return static_cast<std::shared_ptr<
        dftracer::utils::utilities::reader::internal::Reader> *>(handle);
}

static void Reader_dealloc(ReaderObject *self) {
    if (self->handle) {
        dft_reader_destroy(self->handle);
    }
    Py_XDECREF(self->gz_path);
    Py_XDECREF(self->idx_path);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Reader_new(PyTypeObject *type, PyObject *args,
                            PyObject *kwds) {
    ReaderObject *self;
    self = (ReaderObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->handle = NULL;
        self->gz_path = NULL;
        self->idx_path = NULL;
        self->checkpoint_size = 1024 * 1024;
        self->buffer_size = 1024 * 1024;
    }
    return (PyObject *)self;
}

static int Reader_init(ReaderObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"gz_path", "idx_path", "checkpoint_size",
                                   "indexer", NULL};
    const char *gz_path;
    const char *idx_path = NULL;
    std::size_t checkpoint_size = 1024 * 1024;
    IndexerObject *indexer = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snO", (char **)kwlist,
                                     &gz_path, &idx_path, &checkpoint_size,
                                     &indexer)) {
        return -1;
    }

    self->gz_path = PyUnicode_FromString(gz_path);
    if (!self->gz_path) {
        return -1;
    }

    if (idx_path) {
        self->idx_path = PyUnicode_FromString(idx_path);
    } else {
        PyObject *gz_path_obj = PyUnicode_FromString(gz_path);
        self->idx_path = PyUnicode_FromFormat("%U.idx", gz_path_obj);
        Py_DECREF(gz_path_obj);
    }

    if (!self->idx_path) {
        Py_DECREF(self->gz_path);
        return -1;
    }

    self->checkpoint_size = checkpoint_size;

    if (indexer && indexer->handle) {
        self->handle = dft_reader_create_with_indexer(indexer->handle);
    } else {
        const char *idx_path_str = PyUnicode_AsUTF8(self->idx_path);
        if (!idx_path_str) {
            return -1;
        }
        self->handle =
            dft_reader_create(gz_path, idx_path_str, checkpoint_size);
    }

    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create reader");
        return -1;
    }

    return 0;
}

static PyObject *Reader_get_max_bytes(ReaderObject *self,
                                      PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t max_bytes;
    int result = dft_reader_get_max_bytes(self->handle, &max_bytes);
    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get max bytes");
        return NULL;
    }

    return PyLong_FromSize_t(max_bytes);
}

static PyObject *Reader_get_num_lines(ReaderObject *self,
                                      PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t num_lines;
    int result = dft_reader_get_num_lines(self->handle, &num_lines);
    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get num lines");
        return NULL;
    }

    return PyLong_FromSize_t(num_lines);
}

static PyObject *Reader_reset(ReaderObject *self,
                              PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    dft_reader_reset(self->handle);
    Py_RETURN_NONE;
}

static PyObject *Reader_read_into_buffer(ReaderObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_bytes, end_bytes;
    Py_buffer buffer;

    if (!PyArg_ParseTuple(args, "nny*", &start_bytes, &end_bytes, &buffer)) {
        return NULL;
    }

    if (!PyBuffer_IsContiguous(&buffer, 'C') || buffer.readonly) {
        PyBuffer_Release(&buffer);
        PyErr_SetString(PyExc_ValueError,
                        "Buffer must be contiguous and writable");
        return NULL;
    }

    int bytes_read = dft_reader_read(self->handle, start_bytes, end_bytes,
                                     (char *)buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);

    if (bytes_read < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to read data");
        return NULL;
    }

    return PyLong_FromLong(bytes_read);
}

static PyObject *Reader_read_line_bytes_into_buffer(ReaderObject *self,
                                                    PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_bytes, end_bytes;
    Py_buffer buffer;

    if (!PyArg_ParseTuple(args, "nny*", &start_bytes, &end_bytes, &buffer)) {
        return NULL;
    }

    if (!PyBuffer_IsContiguous(&buffer, 'C') || buffer.readonly) {
        PyBuffer_Release(&buffer);
        PyErr_SetString(PyExc_ValueError,
                        "Buffer must be contiguous and writable");
        return NULL;
    }

    int bytes_read = dft_reader_read_line_bytes(
        self->handle, start_bytes, end_bytes, (char *)buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);

    if (bytes_read < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to read line bytes data");
        return NULL;
    }

    return PyLong_FromLong(bytes_read);
}

static PyObject *Reader_read_lines_into_buffer(ReaderObject *self,
                                               PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_line, end_line;
    Py_buffer buffer;

    if (!PyArg_ParseTuple(args, "nny*", &start_line, &end_line, &buffer)) {
        return NULL;
    }

    if (!PyBuffer_IsContiguous(&buffer, 'C') || buffer.readonly) {
        PyBuffer_Release(&buffer);
        PyErr_SetString(PyExc_ValueError,
                        "Buffer must be contiguous and writable");
        return NULL;
    }

    std::size_t bytes_written;
    int result =
        dft_reader_read_lines(self->handle, start_line, end_line,
                              (char *)buffer.buf, buffer.len, &bytes_written);
    PyBuffer_Release(&buffer);

    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to read lines data");
        return NULL;
    }

    return PyLong_FromSize_t(bytes_written);
}

static PyObject *Reader_read(ReaderObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_bytes, end_bytes;
    if (!PyArg_ParseTuple(args, "nn", &start_bytes, &end_bytes)) {
        return NULL;
    }

    std::size_t buffer_size = self->buffer_size;
    char *buffer = (char *)PyMem_RawMalloc(buffer_size);
    if (!buffer) {
        return PyErr_NoMemory();
    }

    PyObject *result = PyBytes_FromStringAndSize("", 0);
    if (!result) {
        PyMem_RawFree(buffer);
        return NULL;
    }

    int bytes_read = dft_reader_read(self->handle, start_bytes, end_bytes,
                                     buffer, buffer_size);
    if (bytes_read > 0) {
        PyObject *chunk = PyBytes_FromStringAndSize(buffer, bytes_read);
        if (!chunk) {
            PyMem_RawFree(buffer);
            Py_DECREF(result);
            return NULL;
        }

        PyBytes_ConcatAndDel(&result, chunk);
        if (!result) {
            PyMem_RawFree(buffer);
            return NULL;
        }
    }

    PyMem_RawFree(buffer);

    if (bytes_read < 0) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_RuntimeError, "Failed to read data");
        return NULL;
    }

    return result;
}

static PyObject *Reader_read_lines(ReaderObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_line, end_line;
    if (!PyArg_ParseTuple(args, "nn", &start_line, &end_line)) {
        return NULL;
    }

    if (start_line < 1) {
        PyErr_SetString(PyExc_ValueError,
                        "start_line must be >= 1 (1-based indexing)");
        return NULL;
    }
    if (end_line < start_line) {
        PyErr_SetString(PyExc_ValueError, "end_line must be >= start_line");
        return NULL;
    }

    try {
        PyListLineProcessor processor;
        (*cast_reader_handle(self->handle))
            ->read_lines_with_processor(start_line, end_line, processor);
        return processor.get_result();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *Reader_read_line_bytes(ReaderObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_bytes, end_bytes;
    if (!PyArg_ParseTuple(args, "nn", &start_bytes, &end_bytes)) {
        return NULL;
    }

    try {
        PyListLineProcessor processor;
        (*cast_reader_handle(self->handle))
            ->read_line_bytes_with_processor(start_bytes, end_bytes, processor);
        return processor.get_result();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *Reader_read_line_bytes_json(ReaderObject *self,
                                             PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_bytes, end_bytes;
    if (!PyArg_ParseTuple(args, "nn", &start_bytes, &end_bytes)) {
        return NULL;
    }

    try {
        PyLazyJSONLineProcessor processor;
        (*cast_reader_handle(self->handle))
            ->read_line_bytes_with_processor(start_bytes, end_bytes, processor);
        return processor.get_result();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *Reader_read_lines_json(ReaderObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Reader not initialized");
        return NULL;
    }

    std::size_t start_line, end_line;
    if (!PyArg_ParseTuple(args, "nn", &start_line, &end_line)) {
        return NULL;
    }

    if (start_line < 1) {
        PyErr_SetString(PyExc_ValueError,
                        "start_line must be >= 1 (1-based indexing)");
        return NULL;
    }
    if (end_line < start_line) {
        PyErr_SetString(PyExc_ValueError, "end_line must be >= start_line");
        return NULL;
    }

    try {
        PyLazyJSONLineProcessor processor;
        (*cast_reader_handle(self->handle))
            ->read_lines_with_processor(start_line, end_line, processor);
        return processor.get_result();
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *Reader_gz_path(ReaderObject *self, void *closure) {
    Py_INCREF(self->gz_path);
    return self->gz_path;
}

static PyObject *Reader_idx_path(ReaderObject *self, void *closure) {
    Py_INCREF(self->idx_path);
    return self->idx_path;
}

static PyObject *Reader_checkpoint_size(ReaderObject *self, void *closure) {
    return PyLong_FromSize_t(self->checkpoint_size);
}

static PyObject *Reader_buffer_size(ReaderObject *self, void *closure) {
    return PyLong_FromSize_t(self->buffer_size);
}

static PyObject *Reader_enter(ReaderObject *self,
                              PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Reader_exit(ReaderObject *self, PyObject *args) {
    Py_RETURN_NONE;
}

static int Reader_set_buffer_size(ReaderObject *self, PyObject *value,
                                  void *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete buffer_size attribute");
        return -1;
    }

    if (!PyLong_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Buffer size must be an integer");
        return -1;
    }

    std::size_t new_size = PyLong_AsSize_t(value);
    if (PyErr_Occurred()) {
        return -1;
    }

    if (new_size == 0) {
        PyErr_SetString(PyExc_ValueError, "Buffer size must be greater than 0");
        return -1;
    }

    self->buffer_size = new_size;
    return 0;
}

static PyMethodDef Reader_methods[] = {
    {"get_max_bytes", (PyCFunction)Reader_get_max_bytes, METH_NOARGS,
     "Get the maximum byte position available in the file"},
    {"get_num_lines", (PyCFunction)Reader_get_num_lines, METH_NOARGS,
     "Get the total number of lines in the file"},
    {"reset", (PyCFunction)Reader_reset, METH_NOARGS,
     "Reset the reader to initial state"},

    {"read", (PyCFunction)Reader_read, METH_VARARGS,
     "Read raw bytes and return as bytes (start_bytes, end_bytes)"},
    {"read_lines", (PyCFunction)Reader_read_lines, METH_VARARGS,
     "Read lines and return as list[str] (start_line, end_line)"},
    {"read_line_bytes", (PyCFunction)Reader_read_line_bytes, METH_VARARGS,
     "Read line bytes and return as list[str] (start_bytes, end_bytes)"},
    {"read_line_bytes_json", (PyCFunction)Reader_read_line_bytes_json,
     METH_VARARGS,
     "Read line bytes and return as list[JSON] (start_bytes, "
     "end_bytes)"},
    {"read_lines_json", (PyCFunction)Reader_read_lines_json, METH_VARARGS,
     "Read lines and return as list[JSON] (start_line, end_line)"},

    {"__enter__", (PyCFunction)Reader_enter, METH_NOARGS,
     "Enter the runtime context for the with statement"},
    {"__exit__", (PyCFunction)Reader_exit, METH_VARARGS,
     "Exit the runtime context for the with statement"},
    {NULL}};

static PyGetSetDef Reader_getsetters[] = {
    {"gz_path", (getter)Reader_gz_path, NULL, "Path to the gzip file", NULL},
    {"idx_path", (getter)Reader_idx_path, NULL, "Path to the index file", NULL},
    {"checkpoint_size", (getter)Reader_checkpoint_size, NULL,
     "Checkpoint size in bytes", NULL},
    {"buffer_size", (getter)Reader_buffer_size, (setter)Reader_set_buffer_size,
     "Internal buffer size for read operations", NULL},
    {NULL}};

PyTypeObject ReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0) "reader.Reader", /* tp_name */
    sizeof(ReaderObject),                           /* tp_basicsize */
    0,                                              /* tp_itemsize */
    (destructor)Reader_dealloc,                     /* tp_dealloc */
    0,                                              /* tp_vectorcall_offset */
    0,                                              /* tp_getattr */
    0,                                              /* tp_setattr */
    0,                                              /* tp_as_async */
    0,                                              /* tp_repr */
    0,                                              /* tp_as_number */
    0,                                              /* tp_as_sequence */
    0,                                              /* tp_as_mapping */
    0,                                              /* tp_hash */
    0,                                              /* tp_call */
    0,                                              /* tp_str */
    0,                                              /* tp_getattro */
    0,                                              /* tp_setattro */
    0,                                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,       /* tp_flags */
    "Reader objects with zero-copy buffer operations", /* tp_doc */
    0,                                                 /* tp_traverse */
    0,                                                 /* tp_clear */
    0,                                                 /* tp_richcompare */
    0,                                                 /* tp_weaklistoffset */
    0,                                                 /* tp_iter */
    0,                                                 /* tp_iternext */
    Reader_methods,                                    /* tp_methods */
    0,                                                 /* tp_members */
    Reader_getsetters,                                 /* tp_getset */
    0,                                                 /* tp_base */
    0,                                                 /* tp_dict */
    0,                                                 /* tp_descr_get */
    0,                                                 /* tp_descr_set */
    0,                                                 /* tp_dictoffset */
    (initproc)Reader_init,                             /* tp_init */
    0,                                                 /* tp_alloc */
    Reader_new,                                        /* tp_new */
};

int init_reader(PyObject *m) {
    if (PyType_Ready(&ReaderType) < 0) return -1;

    Py_INCREF(&ReaderType);
    if (PyModule_AddObject(m, "Reader", (PyObject *)&ReaderType) < 0) {
        Py_DECREF(&ReaderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
