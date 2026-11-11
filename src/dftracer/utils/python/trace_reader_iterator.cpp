// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <nanoarrow/nanoarrow.h>

using ArrowExportResult = dftracer::utils::trace::arrow::ArrowExportResult;

static void release_arrow_schema(PyObject *capsule) {
    auto *schema = static_cast<ArrowSchema *>(
        PyCapsule_GetPointer(capsule, "arrow_schema"));
    if (schema && schema->release) {
        schema->release(schema);
    }
    dftracer::utils::dstd::UniquePtr<ArrowSchema>(schema).reset();
}

static void release_arrow_array(PyObject *capsule) {
    auto *array =
        static_cast<ArrowArray *>(PyCapsule_GetPointer(capsule, "arrow_array"));
    if (array && array->release) {
        array->release(array);
    }
    dftracer::utils::dstd::UniquePtr<ArrowArray>(array).reset();
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

    auto *schema = dftracer::utils::dstd::make_unique<ArrowSchema>().release();
    auto *array = dftracer::utils::dstd::make_unique<ArrowArray>().release();

    self->result->release_schema().move(schema);
    self->result->release_array().move(array);

    PyObject *schema_capsule =
        PyCapsule_New(schema, "arrow_schema", release_arrow_schema);
    if (!schema_capsule) {
        if (schema->release) schema->release(schema);
        dftracer::utils::dstd::UniquePtr<ArrowSchema>(schema).reset();
        if (array->release) array->release(array);
        dftracer::utils::dstd::UniquePtr<ArrowArray>(array).reset();
        return NULL;
    }

    PyObject *array_capsule =
        PyCapsule_New(array, "arrow_array", release_arrow_array);
    if (!array_capsule) {
        Py_DECREF(schema_capsule);
        if (array->release) array->release(array);
        dftracer::utils::dstd::UniquePtr<ArrowArray>(array).reset();
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
    {"__arrow_c_array__", DFTU_PYCFUNCTION(ArrowBatchCapsule_arrow_c_array),
     METH_VARARGS,
     "Export as Arrow C Data Interface PyCapsule pair (schema, array)"},
    {NULL, NULL, 0, NULL}};

static PyGetSetDef ArrowBatchCapsule_getsetters[] = {
    {"num_rows", (getter)ArrowBatchCapsule_get_num_rows, NULL, "Number of rows",
     NULL},
    {"num_columns", (getter)ArrowBatchCapsule_get_num_columns, NULL,
     "Number of columns", NULL},
    {NULL, NULL, NULL, NULL, NULL}};

PyTypeObject ArrowBatchCapsuleType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext._ArrowBatchCapsule",
    .tp_basicsize = sizeof(ArrowBatchCapsuleObject),
    .tp_dealloc = (destructor)ArrowBatchCapsule_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Internal Arrow batch wrapper implementing __arrow_c_array__ protocol",
    .tp_methods = ArrowBatchCapsule_methods,
    .tp_getset = ArrowBatchCapsule_getsetters,
};

#endif  // DFTRACER_UTILS_ENABLE_ARROW

static void cancel_and_wait_batch_state(MemoryViewBatchIteratorState *bs) {
    bs->cancelled.store(true, std::memory_order_release);
    if (bs->channel) bs->channel->close();
    if (bs->task_future.valid()) bs->task_future.wait();
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
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (self->mode == IteratorMode::Arrow) {
        auto *astate = self->arrow_state.get();
        dftracer::utils::dstd::Optional<ArrowExportResult> batch;
        Py_BEGIN_ALLOW_THREADS batch = astate->channel->blocking_receive();
        Py_END_ALLOW_THREADS

            if (!batch.has_value()) {
            std::lock_guard<std::mutex> lock(astate->error_mtx);
            if (astate->error) {
                try {
                    std::rethrow_exception(astate->error);
                } catch (const std::exception &e) {
                    dftracer::utils::python::set_typed_py_error(e);
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
                static_cast<Py_ssize_t>(batch_obj->data->entry_count());
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
        dftracer::utils::dstd::Optional<MemoryViewBatchData> batch_data;
        Py_BEGIN_ALLOW_THREADS batch_data = bs->channel->blocking_receive();
        Py_END_ALLOW_THREADS

            if (!batch_data.has_value()) {
            std::lock_guard<std::mutex> lock(bs->error_mtx);
            if (bs->error) {
                try {
                    std::rethrow_exception(bs->error);
                } catch (const std::exception &e) {
                    dftracer::utils::python::set_typed_py_error(e);
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
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext.TraceReaderIterator",
    .tp_basicsize = sizeof(TraceReaderIteratorObject),
    .tp_dealloc = (destructor)TraceReaderIterator_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Lazy iterator over TraceReader lines or raw chunks",
    .tp_iter = (getiterfunc)TraceReaderIterator_iter,
    .tp_iternext = (iternextfunc)TraceReaderIterator_next,
};

int dftracer::utils::python::init_trace_reader_iterator(PyObject *m) {
    if (register_type(m, &TraceReaderIteratorType, "TraceReaderIterator") < 0)
        return -1;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (register_type(m, &ArrowBatchCapsuleType, "_ArrowBatchCapsule") < 0)
        return -1;
#endif

    return 0;
}
