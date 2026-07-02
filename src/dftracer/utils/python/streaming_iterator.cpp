#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/streaming_iterator.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

namespace dftracer::utils::python {

static PyObject* ArrowStreamingIterator_new(PyTypeObject* type,
                                            PyObject* /*args*/,
                                            PyObject* /*kwds*/) {
    ArrowStreamingIteratorObject* self =
        (ArrowStreamingIteratorObject*)type->tp_alloc(type, 0);
    if (self) {
        // Allocate C++ state separately to avoid layout issues
        self->cpp_state = new ArrowStreamingIteratorState();
    }
    return (PyObject*)self;
}

static void ArrowStreamingIterator_dealloc(ArrowStreamingIteratorObject* self) {
    if (self->cpp_state) {
        // Cancel the stream if still running
        if (self->cpp_state->cancel) {
            self->cpp_state->cancel();
        }
        delete self->cpp_state;
        self->cpp_state = nullptr;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* ArrowStreamingIterator_iter(PyObject* self) {
    Py_INCREF(self);
    return self;
}

static PyObject* ArrowStreamingIterator_next(
    ArrowStreamingIteratorObject* self) {
    if (!self->cpp_state || !self->cpp_state->pull_next) {
        PyErr_SetString(PyExc_RuntimeError, "Iterator not initialized");
        return NULL;
    }

    std::optional<ArrowExportResult> result;
    if (!run_blocking_r([&] { return self->cpp_state->pull_next(); }, result))
        return NULL;

    if (!result.has_value()) {
        // Check for error
        if (self->cpp_state->get_error) {
            auto ex = self->cpp_state->get_error();
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (const std::exception& e) {
                    set_typed_py_error(e);
                    return NULL;
                } catch (...) {
                    PyErr_SetString(PyExc_RuntimeError,
                                    "Unknown error in streaming iterator");
                    return NULL;
                }
            }
        }
        // Normal completion
        return NULL;  // StopIteration
    }

    // Wrap the ArrowExportResult in an ArrowBatchCapsule
    ArrowBatchCapsuleObject* obj =
        (ArrowBatchCapsuleObject*)ArrowBatchCapsuleType.tp_alloc(
            &ArrowBatchCapsuleType, 0);
    if (!obj) return NULL;
    obj->result = new ArrowExportResult(std::move(*result));
    return (PyObject*)obj;
}

static PyObject* ArrowStreamingIterator_cancel(
    ArrowStreamingIteratorObject* self, PyObject* Py_UNUSED(args)) {
    if (self->cpp_state && self->cpp_state->cancel) {
        self->cpp_state->cancel();
    }
    Py_RETURN_NONE;
}

static PyMethodDef ArrowStreamingIterator_methods[] = {
    {"cancel", (PyCFunction)ArrowStreamingIterator_cancel, METH_NOARGS,
     "Cancel the streaming iterator."},
    {NULL}};

PyTypeObject ArrowStreamingIteratorType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext._ArrowStreamingIterator",
    sizeof(ArrowStreamingIteratorObject),       /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)ArrowStreamingIterator_dealloc, /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    "Streaming Arrow batch iterator.\n\n"
    "Yields ArrowBatch objects as they become available from the C++ "
    "pipeline.\n"
    "Call cancel() to stop the stream early.", /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    ArrowStreamingIterator_iter,               /* tp_iter */
    (iternextfunc)ArrowStreamingIterator_next, /* tp_iternext */
    ArrowStreamingIterator_methods,            /* tp_methods */
    0,                                         /* tp_members */
    0,                                         /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    0,                                         /* tp_init */
    0,                                         /* tp_alloc */
    ArrowStreamingIterator_new,                /* tp_new */
};

int init_arrow_streaming_iterator(PyObject* m) {
    if (register_type(m, &ArrowStreamingIteratorType,
                      "_ArrowStreamingIterator") < 0)
        return -1;

    return 0;
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
