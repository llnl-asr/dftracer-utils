#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/task_handle.h>

#include <any>
#include <chrono>
#include <future>
#include <string>

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
        new (&self->future) std::shared_future<void>();
        new (&self->typed_future) std::shared_future<std::any>();
        new (&self->name) std::string();
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
        std::any result;
        try {
            Py_BEGIN_ALLOW_THREADS result = self->typed_future.get();
            Py_END_ALLOW_THREADS
        } catch (const std::exception &e) {
            PyErr_SetString(PyExc_RuntimeError, e.what());
            return NULL;
        } catch (...) {
            PyErr_SetString(PyExc_RuntimeError, "Unknown error in task");
            return NULL;
        }
        if (result.has_value()) {
            try {
                PyObject *obj = std::any_cast<PyObject *>(result);
                if (obj) {
                    Py_INCREF(obj);
                    return obj;
                }
            } catch (const std::bad_any_cast &) {
                // Not a PyObject* — fall through to None
            }
        }
        Py_RETURN_NONE;
    }

    // Void task: .get() returns void and rethrows stored exceptions.
    try {
        Py_BEGIN_ALLOW_THREADS self->future.get();
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "Unknown error in task");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *TaskHandle_wait(TaskHandleObject *self,
                                 PyObject *Py_UNUSED(ignored)) {
    if (!self->future.valid()) {
        Py_RETURN_NONE;
    }
    // Use .get() (not .wait()) so stored exceptions are rethrown.
    try {
        Py_BEGIN_ALLOW_THREADS self->future.get();
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "Unknown error in task");
        return NULL;
    }
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
    {"get", (PyCFunction)TaskHandle_get, METH_NOARGS,
     "Block until task completes and return result.\n"
     "Raises RuntimeError if the task failed."},
    {"wait", (PyCFunction)TaskHandle_wait, METH_NOARGS,
     "Block until task completes.\n"
     "Raises RuntimeError if the task failed."},
    {"done", (PyCFunction)TaskHandle_done, METH_NOARGS,
     "Return True if task has completed."},
    {NULL}};

static PyGetSetDef TaskHandle_getsetters[] = {
    {"name", (getter)TaskHandle_get_name, NULL, "Task name", NULL},
    {"task_id", (getter)TaskHandle_get_task_id, NULL, "Task identifier", NULL},
    {NULL}};

PyTypeObject TaskHandleType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.TaskHandle",
    sizeof(TaskHandleObject),                       /* tp_basicsize */
    0,                                              /* tp_itemsize */
    (destructor)TaskHandle_dealloc,                 /* tp_dealloc */
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
    "Handle to an async task submitted to Runtime", /* tp_doc */
    0,                                              /* tp_traverse */
    0,                                              /* tp_clear */
    0,                                              /* tp_richcompare */
    0,                                              /* tp_weaklistoffset */
    0,                                              /* tp_iter */
    0,                                              /* tp_iternext */
    TaskHandle_methods,                             /* tp_methods */
    0,                                              /* tp_members */
    TaskHandle_getsetters,                          /* tp_getset */
    0,                                              /* tp_base */
    0,                                              /* tp_dict */
    0,                                              /* tp_descr_get */
    0,                                              /* tp_descr_set */
    0,                                              /* tp_dictoffset */
    0,                                              /* tp_init */
    0,                                              /* tp_alloc */
    TaskHandle_new,                                 /* tp_new */
};

PyObject *create_task_handle(dftracer::utils::TaskHandle handle) {
    TaskHandleObject *obj =
        (TaskHandleObject *)TaskHandleType.tp_alloc(&TaskHandleType, 0);
    if (!obj) return NULL;
    new (&obj->future) std::shared_future<void>(std::move(handle.future));
    new (&obj->typed_future) std::shared_future<std::any>();
    new (&obj->name) std::string(std::move(handle.name));
    obj->has_typed_future = false;
    obj->task_id = handle.id;
    return (PyObject *)obj;
}

PyObject *create_typed_task_handle(std::shared_future<void> void_future,
                                   std::shared_future<std::any> typed_future,
                                   dftracer::utils::TaskIndex id,
                                   std::string name) {
    TaskHandleObject *obj =
        (TaskHandleObject *)TaskHandleType.tp_alloc(&TaskHandleType, 0);
    if (!obj) return NULL;
    new (&obj->future) std::shared_future<void>(std::move(void_future));
    new (&obj->typed_future)
        std::shared_future<std::any>(std::move(typed_future));
    new (&obj->name) std::string(std::move(name));
    obj->has_typed_future = true;
    obj->task_id = id;
    return (PyObject *)obj;
}

int init_task_handle(PyObject *m) {
    if (PyType_Ready(&TaskHandleType) < 0) return -1;
    Py_INCREF(&TaskHandleType);
    if (PyModule_AddObject(m, "TaskHandle", (PyObject *)&TaskHandleType) < 0) {
        Py_DECREF(&TaskHandleType);
        return -1;
    }
    return 0;
}
