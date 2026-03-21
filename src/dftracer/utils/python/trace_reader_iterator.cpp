#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <nanoarrow/nanoarrow.h>

using ArrowExportResult =
    dftracer::utils::utilities::common::arrow::ArrowExportResult;

typedef struct {
    PyObject_HEAD ArrowExportResult *result;  // owned, allocated with new
} ArrowBatchCapsuleObject;

static void release_arrow_schema(PyObject *capsule) {
    auto *schema = static_cast<ArrowSchema *>(
        PyCapsule_GetPointer(capsule, "arrow_schema"));
    if (schema && schema->release) {
        schema->release(schema);
    }
    delete schema;
}

static void release_arrow_array(PyObject *capsule) {
    auto *array =
        static_cast<ArrowArray *>(PyCapsule_GetPointer(capsule, "arrow_array"));
    if (array && array->release) {
        array->release(array);
    }
    delete array;
}

static PyObject *ArrowBatchCapsule_arrow_c_array(ArrowBatchCapsuleObject *self,
                                                 PyObject *args) {
    PyObject *requested_schema = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &requested_schema)) return NULL;

    if (!self->result || !self->result->valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "ArrowBatchCapsule has no valid data");
        return NULL;
    }

    auto *schema = new ArrowSchema;
    auto *array = new ArrowArray;

    self->result->release_schema().move(schema);
    self->result->release_array().move(array);

    PyObject *schema_capsule =
        PyCapsule_New(schema, "arrow_schema", release_arrow_schema);
    if (!schema_capsule) {
        if (schema->release) schema->release(schema);
        delete schema;
        if (array->release) array->release(array);
        delete array;
        return NULL;
    }

    PyObject *array_capsule =
        PyCapsule_New(array, "arrow_array", release_arrow_array);
    if (!array_capsule) {
        Py_DECREF(schema_capsule);
        if (array->release) array->release(array);
        delete array;
        return NULL;
    }

    PyObject *tuple = PyTuple_Pack(2, schema_capsule, array_capsule);
    Py_DECREF(schema_capsule);
    Py_DECREF(array_capsule);
    return tuple;
}

static PyObject *ArrowBatchCapsule_get_num_rows(ArrowBatchCapsuleObject *self,
                                                void *) {
    if (!self->result) return PyLong_FromLong(0);
    return PyLong_FromLongLong(self->result->num_rows());
}

static PyObject *ArrowBatchCapsule_get_num_columns(
    ArrowBatchCapsuleObject *self, void *) {
    if (!self->result) return PyLong_FromLong(0);
    return PyLong_FromLongLong(self->result->num_columns());
}

static void ArrowBatchCapsule_dealloc(ArrowBatchCapsuleObject *self) {
    delete self->result;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMethodDef ArrowBatchCapsule_methods[] = {
    {"__arrow_c_array__", (PyCFunction)ArrowBatchCapsule_arrow_c_array,
     METH_VARARGS,
     "Export as Arrow C Data Interface PyCapsule pair (schema, array)"},
    {NULL}};

static PyGetSetDef ArrowBatchCapsule_getsetters[] = {
    {"num_rows", (getter)ArrowBatchCapsule_get_num_rows, NULL, "Number of rows",
     NULL},
    {"num_columns", (getter)ArrowBatchCapsule_get_num_columns, NULL,
     "Number of columns", NULL},
    {NULL}};

static PyTypeObject ArrowBatchCapsuleType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext._ArrowBatchCapsule",
    sizeof(ArrowBatchCapsuleObject),       /* tp_basicsize */
    0,                                     /* tp_itemsize */
    (destructor)ArrowBatchCapsule_dealloc, /* tp_dealloc */
    0,                                     /* tp_vectorcall_offset */
    0,                                     /* tp_getattr */
    0,                                     /* tp_setattr */
    0,                                     /* tp_as_async */
    0,                                     /* tp_repr */
    0,                                     /* tp_as_number */
    0,                                     /* tp_as_sequence */
    0,                                     /* tp_as_mapping */
    0,                                     /* tp_hash */
    0,                                     /* tp_call */
    0,                                     /* tp_str */
    0,                                     /* tp_getattro */
    0,                                     /* tp_setattro */
    0,                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                    /* tp_flags */
    "Internal Arrow batch wrapper implementing __arrow_c_array__ protocol",
    0,                                     /* tp_traverse */
    0,                                     /* tp_clear */
    0,                                     /* tp_richcompare */
    0,                                     /* tp_weaklistoffset */
    0,                                     /* tp_iter */
    0,                                     /* tp_iternext */
    ArrowBatchCapsule_methods,             /* tp_methods */
    0,                                     /* tp_members */
    ArrowBatchCapsule_getsetters,          /* tp_getset */
};

#endif                                     // DFTRACER_UTILS_ENABLE_ARROW

static void TraceReaderIterator_dealloc(TraceReaderIteratorObject *self) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (self->arrow_state) {
        self->arrow_state->cancelled.store(true, std::memory_order_release);
        self->arrow_state->cv_producer.notify_all();
        self->arrow_state.reset();
    }
#endif
    if (self->state) {
        self->state->cancelled.store(true, std::memory_order_release);
        self->state->cv_producer.notify_all();
        self->state.reset();
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TraceReaderIterator_iter(TraceReaderIteratorObject *self) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *TraceReaderIterator_next(TraceReaderIteratorObject *self) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (self->mode == IteratorMode::ARROW) {
        auto *astate = self->arrow_state.get();
        ArrowIteratorState::BatchItem batch;
        {
            Py_BEGIN_ALLOW_THREADS std::unique_lock<std::mutex> lock(
                astate->mtx);
            astate->cv_consumer.wait(
                lock, [astate] { return !astate->queue.empty(); });
            batch = std::move(astate->queue.front());
            astate->queue.pop();
            Py_END_ALLOW_THREADS
        }
        astate->cv_producer.notify_one();

        if (!batch.has_value()) {
            if (astate->error) {
                try {
                    std::rethrow_exception(astate->error);
                } catch (const std::exception &e) {
                    PyErr_SetString(PyExc_RuntimeError, e.what());
                    return NULL;
                } catch (...) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "Unknown error in Arrow iterator");
                    return NULL;
                }
            }
            return NULL;  // StopIteration
        }

        ArrowBatchCapsuleObject *obj =
            (ArrowBatchCapsuleObject *)ArrowBatchCapsuleType.tp_alloc(
                &ArrowBatchCapsuleType, 0);
        if (!obj) return NULL;
        obj->result = new ArrowExportResult(std::move(*batch));
        return (PyObject *)obj;
    }
#endif

    auto *state = self->state.get();
    std::optional<std::string> item;

    {
        Py_BEGIN_ALLOW_THREADS std::unique_lock<std::mutex> lock(state->mtx);
        state->cv_consumer.wait(lock,
                                [state] { return !state->queue.empty(); });
        item = std::move(state->queue.front());
        state->queue.pop();
        Py_END_ALLOW_THREADS
    }
    state->cv_producer.notify_one();

    if (!item.has_value()) {
        if (state->error) {
            try {
                std::rethrow_exception(state->error);
            } catch (const std::exception &e) {
                PyErr_SetString(PyExc_RuntimeError, e.what());
                return NULL;
            } catch (...) {
                PyErr_SetString(PyExc_RuntimeError,
                                "Unknown error in TraceReaderIterator");
                return NULL;
            }
        }
        return NULL;
    }

    switch (self->mode) {
        case IteratorMode::LINES:
            return PyUnicode_FromStringAndSize(
                item->data(), static_cast<Py_ssize_t>(item->size()));
        case IteratorMode::JSON: {
            const char *trimmed;
            std::size_t trimmed_length;
            if (!dftracer::utils::json_trim_and_validate(
                    item->data(), item->size(), trimmed, trimmed_length)) {
                // Skip non-JSON lines (e.g. "[" or "]" array delimiters)
                return TraceReaderIterator_next(self);
            }
            PyObject *json_obj = JSON_from_data(trimmed, trimmed_length);
            if (!json_obj) {
                // Skip unparseable lines
                PyErr_Clear();
                return TraceReaderIterator_next(self);
            }
            return json_obj;
        }
        case IteratorMode::RAW:
        default:
            return PyBytes_FromStringAndSize(
                item->data(), static_cast<Py_ssize_t>(item->size()));
    }
}

PyTypeObject TraceReaderIteratorType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.TraceReaderIterator",
    sizeof(TraceReaderIteratorObject),       /* tp_basicsize */
    0,                                       /* tp_itemsize */
    (destructor)TraceReaderIterator_dealloc, /* tp_dealloc */
    0,                                       /* tp_vectorcall_offset */
    0,                                       /* tp_getattr */
    0,                                       /* tp_setattr */
    0,                                       /* tp_as_async */
    0,                                       /* tp_repr */
    0,                                       /* tp_as_number */
    0,                                       /* tp_as_sequence */
    0,                                       /* tp_as_mapping */
    0,                                       /* tp_hash */
    0,                                       /* tp_call */
    0,                                       /* tp_str */
    0,                                       /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                      /* tp_flags */
    "Lazy iterator over TraceReader lines or raw chunks", /* tp_doc */
    0,                                                    /* tp_traverse */
    0,                                                    /* tp_clear */
    0,                                                    /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    (getiterfunc)TraceReaderIterator_iter,  /* tp_iter */
    (iternextfunc)TraceReaderIterator_next, /* tp_iternext */
    0,                                      /* tp_methods */
    0,                                      /* tp_members */
    0,                                      /* tp_getset */
    0,                                      /* tp_base */
    0,                                      /* tp_dict */
    0,                                      /* tp_descr_get */
    0,                                      /* tp_descr_set */
    0,                                      /* tp_dictoffset */
    0,                                      /* tp_init */
    0,                                      /* tp_alloc */
    0,                                      /* tp_new */
};

int init_trace_reader_iterator(PyObject *m) {
    if (PyType_Ready(&TraceReaderIteratorType) < 0) return -1;

    Py_INCREF(&TraceReaderIteratorType);
    if (PyModule_AddObject(m, "TraceReaderIterator",
                           (PyObject *)&TraceReaderIteratorType) < 0) {
        Py_DECREF(&TraceReaderIteratorType);
        Py_DECREF(m);
        return -1;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (PyType_Ready(&ArrowBatchCapsuleType) < 0) return -1;
    Py_INCREF(&ArrowBatchCapsuleType);
    if (PyModule_AddObject(m, "_ArrowBatchCapsule",
                           (PyObject *)&ArrowBatchCapsuleType) < 0) {
        Py_DECREF(&ArrowBatchCapsuleType);
        return -1;
    }
#endif

    return 0;
}
