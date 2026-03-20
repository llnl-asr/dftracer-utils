#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

static void TraceReaderIterator_dealloc(TraceReaderIteratorObject *self) {
    if (self->state) {
        // Unblock the producer if it is waiting on a full queue.
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

    return 0;
}
