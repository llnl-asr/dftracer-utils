// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/python/core/arrow_batch_capsule.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <nanoarrow/nanoarrow.h>

using Batch = dftracer::utils::arrow::Batch;

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

    if (!self->result || !self->result->is_valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Arrow data already exported via __arrow_c_array__. "
                        "Each batch can only be exported once.");
        return NULL;
    }

    auto *schema = dftracer::utils::dstd::make_unique<ArrowSchema>().release();
    auto *array = dftracer::utils::dstd::make_unique<ArrowArray>().release();

    self->result->release_into(schema, array);

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
    if (!self->result || !self->result->is_valid()) return PyLong_FromLong(0);
    return PyLong_FromLongLong(self->result->num_rows());
}

static PyObject *ArrowBatchCapsule_get_num_columns(
    ArrowBatchCapsuleObject *self, void *) {
    if (!self->result || !self->result->is_valid()) return PyLong_FromLong(0);
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

int dftracer::utils::python::core::init_arrow_batch_capsule(PyObject *m) {
    return register_type(m, &ArrowBatchCapsuleType, "_ArrowBatchCapsule");
}
