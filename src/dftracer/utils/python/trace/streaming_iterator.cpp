// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/runtime_mixin.h>
#include <dftracer/utils/python/core/type_helpers.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/frame/dataframe.h>
#include <dftracer/utils/python/trace/streaming_iterator.h>
#include <dftracer/utils/python/trace/trace_reader_iterator.h>

namespace dftracer::utils::python::trace {

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

// The next Arrow batch of a pull_next producer, wrapped in a capsule.
static PyObject* next_batch(ArrowStreamingIteratorObject* self) {
    dstd::Optional<Batch> result;
    if (!run_blocking_r([&] { return self->cpp_state->pull_next(); }, result))
        return NULL;

    if (!result.has_value()) {
        // Check for error
        if (self->cpp_state->get_error) {
            if (auto error = self->cpp_state->get_error()) {
                core::set_py_error(*error);
                return NULL;
            }
        }
        // Normal completion
        return NULL;  // StopIteration
    }

    // Wrap the Batch in an ArrowBatchCapsule
    ArrowBatchCapsuleObject* obj =
        (ArrowBatchCapsuleObject*)ArrowBatchCapsuleType.tp_alloc(
            &ArrowBatchCapsuleType, 0);
    if (!obj) return NULL;
    obj->result = new Batch(std::move(*result));
    return (PyObject*)obj;
}

static PyObject* ArrowStreamingIterator_next(
    ArrowStreamingIteratorObject* self) {
    if (!self->cpp_state || !self->cpp_state->pull_df) {
        if (self->cpp_state && self->cpp_state->pull_next)
            return next_batch(self);
        PyErr_SetString(PyExc_RuntimeError, "Iterator not initialized");
        return NULL;
    }

    // Native-DataFrame path: pull a DataFrame chunk (GIL released) and wrap it,
    // so the stream never crosses Arrow.
    dstd::Optional<dftracer::utils::dataframe::DataFrame> df;
    if (!run_blocking_r([&] { return self->cpp_state->pull_df(); }, df))
        return NULL;
    if (!df.has_value()) {
        if (self->cpp_state->get_error) {
            if (auto error = self->cpp_state->get_error()) {
                core::set_py_error(*error);
                return NULL;
            }
        }
        return NULL;  // StopIteration
    }
    return frame::wrap_dataframe(std::move(*df));
}

static PyObject* ArrowStreamingIterator_cancel(
    ArrowStreamingIteratorObject* self, PyObject* Py_UNUSED(args)) {
    if (self->cpp_state && self->cpp_state->cancel) {
        self->cpp_state->cancel();
    }
    Py_RETURN_NONE;
}

static PyMethodDef ArrowStreamingIterator_methods[] = {
    {"cancel", DFTU_PYCFUNCTION(ArrowStreamingIterator_cancel), METH_NOARGS,
     "Cancel the streaming iterator."},
    {NULL, NULL, 0, NULL}};

PyTypeObject ArrowStreamingIteratorType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext._ArrowStreamingIterator",
    .tp_basicsize = sizeof(ArrowStreamingIteratorObject),
    .tp_dealloc = (destructor)ArrowStreamingIterator_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Streaming Arrow batch iterator.\n\n"
        "Yields ArrowBatch objects as they become available from the C++ "
        "pipeline.\n"
        "Call cancel() to stop the stream early.",
    .tp_iter = ArrowStreamingIterator_iter,
    .tp_iternext = (iternextfunc)ArrowStreamingIterator_next,
    .tp_methods = ArrowStreamingIterator_methods,
    .tp_new = ArrowStreamingIterator_new,
};

int init_arrow_streaming_iterator(PyObject* m) {
    if (register_type(m, &ArrowStreamingIteratorType,
                      "_ArrowStreamingIterator") < 0)
        return -1;

    return 0;
}

}  // namespace dftracer::utils::python::trace
