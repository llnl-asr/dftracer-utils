// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/any.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/task_handle.h>

#include <chrono>
#include <future>

static void TaskHandle_dealloc(TaskHandleObject *self) {
    self->future.~shared_future();
    self->typed_future.~shared_future();
    self->name.~basic_string();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TaskHandle_new(PyTypeObject *type, PyObject * /*args*/,
                                PyObject * /*kwds*/) {
    TaskHandleObject *self = (TaskHandleObject *)type->tp_alloc(type, 0);
    if (self) {
        new (&self->future) dftracer::utils::dstd::SharedFuture<void>();
        new (&self->typed_future)
            dftracer::utils::dstd::SharedFuture<dftracer::utils::dstd::Any>();
        new (&self->name) dftracer::utils::dstd::String();
        self->has_typed_future = false;
        self->task_id = -1;
    }
    return (PyObject *)self;
}

static PyObject *TaskHandle_get(TaskHandleObject *self,
                                PyObject *Py_UNUSED(ignored)) {
    if (!self->future.valid()) {
        Py_RETURN_NONE;
    }
    if (self->has_typed_future) {
        dftracer::utils::dstd::Any result;
        if (!run_blocking_r([&] { return self->typed_future.get(); }, result))
            return NULL;
        if (result.has_value()) {
            // Not a PyObject* falls through to None
            if (PyObject **obj =
                    dftracer::utils::dstd::any_cast<PyObject *>(&result)) {
                if (*obj) {
                    Py_INCREF(*obj);
                    return *obj;
                }
            }
        }
        Py_RETURN_NONE;
    }

    // Void task: .get() returns void and rethrows stored exceptions.
    if (!run_blocking([&] { self->future.get(); })) return NULL;
    Py_RETURN_NONE;
}

static PyObject *TaskHandle_wait(TaskHandleObject *self,
                                 PyObject *Py_UNUSED(ignored)) {
    if (!self->future.valid()) {
        Py_RETURN_NONE;
    }
    // Use .get() (not .wait()) so stored exceptions are rethrown.
    if (!run_blocking([&] { self->future.get(); })) return NULL;
    Py_RETURN_NONE;
}

static PyObject *TaskHandle_done(TaskHandleObject *self,
                                 PyObject *Py_UNUSED(ignored)) {
    if (!self->future.valid()) {
        Py_RETURN_FALSE;
    }
    bool is_done = self->future.wait_for(std::chrono::seconds(0)) ==
                   std::future_status::ready;
    return PyBool_FromLong(is_done ? 1 : 0);
}

static PyObject *TaskHandle_get_name(TaskHandleObject *self, void *) {
    return PyUnicode_FromStringAndSize(
        self->name.data(), static_cast<Py_ssize_t>(self->name.size()));
}

static PyObject *TaskHandle_get_task_id(TaskHandleObject *self, void *) {
    return PyLong_FromLongLong(static_cast<long long>(self->task_id));
}

static PyMethodDef TaskHandle_methods[] = {
    {"get", DFTU_PYCFUNCTION(TaskHandle_get), METH_NOARGS,
     "Block until task completes and return result.\n"
     "Raises RuntimeError if the task failed."},
    {"wait", DFTU_PYCFUNCTION(TaskHandle_wait), METH_NOARGS,
     "Block until task completes.\n"
     "Raises RuntimeError if the task failed."},
    {"done", DFTU_PYCFUNCTION(TaskHandle_done), METH_NOARGS,
     "Return True if task has completed."},
    {NULL, NULL, 0, NULL}};

static PyGetSetDef TaskHandle_getsetters[] = {
    {"name", (getter)TaskHandle_get_name, NULL, "Task name", NULL},
    {"task_id", (getter)TaskHandle_get_task_id, NULL, "Task identifier", NULL},
    {NULL, NULL, NULL, NULL, NULL}};

PyTypeObject TaskHandleType = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name =
        "dftracer_utils_ext.TaskHandle",
    .tp_basicsize = sizeof(TaskHandleObject),
    .tp_dealloc = (destructor)TaskHandle_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = "Handle to an async task submitted to Runtime",
    .tp_methods = TaskHandle_methods,
    .tp_getset = TaskHandle_getsetters,
    .tp_new = TaskHandle_new,
};

int dftracer::utils::python::init_task_handle(PyObject *m) {
    if (register_type(m, &TaskHandleType, "TaskHandle") < 0) return -1;
    return 0;
}
