#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <nanoarrow/nanoarrow.h>

using ArrowExportResult =
    dftracer::utils::utilities::common::arrow::ArrowExportResult;

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
                        "Arrow data already exported via __arrow_c_array__. "
                        "Each batch can only be exported once.");
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
    if (!self->result || !self->result->valid()) return PyLong_FromLong(0);
    return PyLong_FromLongLong(self->result->num_rows());
}

static PyObject *ArrowBatchCapsule_get_num_columns(
    ArrowBatchCapsuleObject *self, void *) {
    if (!self->result || !self->result->valid()) return PyLong_FromLong(0);
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

PyTypeObject ArrowBatchCapsuleType = {
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

static void cancel_and_wait_batch_state(MemoryViewBatchIteratorState *bs) {
    bs->cancelled.store(true, std::memory_order_release);
    if (bs->channel) bs->channel->close();
    if (bs->task_future.valid()) bs->task_future.wait();
}

static void cancel_and_wait_json_dict_state(JsonDictIteratorState *js) {
    js->cancelled.store(true, std::memory_order_release);
    if (js->channel) js->channel->close();
    if (js->task_future.valid()) js->task_future.wait();
}

static void TraceReaderIterator_dealloc(TraceReaderIteratorObject *self) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (self->arrow_state) {
        self->arrow_state->cancelled.store(true, std::memory_order_release);
        if (self->arrow_state->channel) self->arrow_state->channel->close();
        Py_BEGIN_ALLOW_THREADS if (self->arrow_state->task_future.valid()) {
            self->arrow_state->task_future.wait();
        }
        Py_END_ALLOW_THREADS self->arrow_state.reset();
    }
#endif
    if (self->json_dict_state) {
        Py_BEGIN_ALLOW_THREADS cancel_and_wait_json_dict_state(
            self->json_dict_state.get());
        Py_END_ALLOW_THREADS self->json_dict_state.reset();
    }
    if (self->batch_state) {
        Py_BEGIN_ALLOW_THREADS cancel_and_wait_batch_state(
            self->batch_state.get());
        Py_END_ALLOW_THREADS self->batch_state.reset();
    }
    Py_XDECREF(self->current_batch);
    self->current_batch = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TraceReaderIterator_iter(TraceReaderIteratorObject *self) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *TraceReaderIterator_next(TraceReaderIteratorObject *self) {
    if (self->mode == IteratorMode::JSON_DICT) {
        while (true) {
            if (self->json_dict_current_batch) {
                auto &events = self->json_dict_current_batch->events;
                Py_ssize_t n = static_cast<Py_ssize_t>(events.size());
                if (self->json_dict_index < n) {
                    JsonDictValueObject *obj =
                        (JsonDictValueObject *)JsonDictValueType.tp_alloc(
                            &JsonDictValueType, 0);
                    if (!obj) return NULL;
                    new (&obj->batch) std::shared_ptr<JsonDictBatch>(
                        self->json_dict_current_batch);
                    obj->event_index =
                        static_cast<std::size_t>(self->json_dict_index);
                    obj->is_args = false;
                    self->json_dict_index++;
                    return (PyObject *)obj;
                }
                self->json_dict_current_batch.reset();
                self->json_dict_index = 0;
            }

            auto *js = self->json_dict_state.get();
            std::optional<JsonDictBatch> batch;
            Py_BEGIN_ALLOW_THREADS batch = js->channel->blocking_receive();
            Py_END_ALLOW_THREADS

                if (!batch.has_value()) {
                std::lock_guard<std::mutex> lock(js->error_mtx);
                if (js->error) {
                    try {
                        std::rethrow_exception(js->error);
                    } catch (const std::exception &e) {
                        set_typed_py_error(e);
                        return NULL;
                    } catch (...) {
                        PyErr_SetString(PyExc_RuntimeError,
                                        "Unknown error in json dict iterator");
                        return NULL;
                    }
                }
                return NULL;
            }

            auto dequeued_bytes = dftracer::utils::python::byte_size(*batch);
            js->bytes_in_queue.fetch_sub(dequeued_bytes,
                                         std::memory_order_acq_rel);
            self->json_dict_current_batch =
                std::make_shared<JsonDictBatch>(std::move(*batch));
            self->json_dict_index = 0;
        }
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (self->mode == IteratorMode::ARROW) {
        auto *astate = self->arrow_state.get();
        std::optional<ArrowExportResult> batch;
        Py_BEGIN_ALLOW_THREADS batch = astate->channel->blocking_receive();
        Py_END_ALLOW_THREADS

            if (!batch.has_value()) {
            std::lock_guard<std::mutex> lock(astate->error_mtx);
            if (astate->error) {
                try {
                    std::rethrow_exception(astate->error);
                } catch (const std::exception &e) {
                    set_typed_py_error(e);
                    return NULL;
                } catch (...) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "Unknown error in Arrow iterator");
                    return NULL;
                }
            }
            return NULL;
        }

        auto dequeued_bytes = dftracer::utils::python::byte_size(*batch);
        astate->bytes_in_queue.fetch_sub(dequeued_bytes,
                                         std::memory_order_acq_rel);

        ArrowBatchCapsuleObject *obj =
            (ArrowBatchCapsuleObject *)ArrowBatchCapsuleType.tp_alloc(
                &ArrowBatchCapsuleType, 0);
        if (!obj) return NULL;
        obj->result = new ArrowExportResult(std::move(*batch));
        return (PyObject *)obj;
    }
#endif

    using namespace dftracer::utils::python;
    while (true) {
        if (self->current_batch) {
            auto *batch_obj = (MemoryViewBatchObject *)self->current_batch;
            Py_ssize_t n =
                static_cast<Py_ssize_t>(batch_obj->data->num_entries());
            if (self->batch_index < n) {
                PyObject *mv =
                    MemoryViewBatch_item(batch_obj, self->batch_index);
                self->batch_index++;
                return mv;
            }
            Py_DECREF(self->current_batch);
            self->current_batch = NULL;
            self->batch_index = 0;
        }

        auto *bs = self->batch_state.get();
        std::optional<MemoryViewBatchData> batch_data;
        Py_BEGIN_ALLOW_THREADS batch_data = bs->channel->blocking_receive();
        Py_END_ALLOW_THREADS

            if (!batch_data.has_value()) {
            std::lock_guard<std::mutex> lock(bs->error_mtx);
            if (bs->error) {
                try {
                    std::rethrow_exception(bs->error);
                } catch (const std::exception &e) {
                    set_typed_py_error(e);
                    return NULL;
                } catch (...) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "Unknown error in batch iterator");
                    return NULL;
                }
            }
            return NULL;
        }

        auto dequeued_bytes = dftracer::utils::python::byte_size(*batch_data);
        bs->bytes_in_queue.fetch_sub(dequeued_bytes, std::memory_order_acq_rel);

        auto *obj = (MemoryViewBatchObject *)MemoryViewBatchType.tp_alloc(
            &MemoryViewBatchType, 0);
        if (!obj) return NULL;
        obj->data = new MemoryViewBatchData(std::move(*batch_data));
        self->current_batch = (PyObject *)obj;
        self->batch_index = 0;
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
    "Lazy iterator over TraceReader lines or raw chunks",
    0,                                       /* tp_traverse */
    0,                                       /* tp_clear */
    0,                                       /* tp_richcompare */
    0,                                       /* tp_weaklistoffset */
    (getiterfunc)TraceReaderIterator_iter,   /* tp_iter */
    (iternextfunc)TraceReaderIterator_next,  /* tp_iternext */
    0,                                       /* tp_methods */
    0,                                       /* tp_members */
    0,                                       /* tp_getset */
    0,                                       /* tp_base */
    0,                                       /* tp_dict */
    0,                                       /* tp_descr_get */
    0,                                       /* tp_descr_set */
    0,                                       /* tp_dictoffset */
    0,                                       /* tp_init */
    0,                                       /* tp_alloc */
    0,                                       /* tp_new */
};

int init_trace_reader_iterator(PyObject *m) {
    if (register_type(m, &TraceReaderIteratorType, "TraceReaderIterator") < 0)
        return -1;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (register_type(m, &ArrowBatchCapsuleType, "_ArrowBatchCapsule") < 0)
        return -1;
#endif

    return 0;
}
