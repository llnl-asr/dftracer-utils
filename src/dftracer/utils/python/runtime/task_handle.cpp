// SPDX-License-Identifier: MIT
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/no_destructor.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/runtime/task_handle.h>

#include <condition_variable>
#include <mutex>
#include <new>
#include <utility>

namespace {

using dftracer::utils::python::runtime::AsyncState;
using StatePtr = dftracer::utils::dstd::SharedPtr<AsyncState>;

struct TaskHandleObject {
    PyObject_HEAD StatePtr state;
    // The converted result, kept so a second get() returns it again.
    PyObject *value;
};

PyTypeObject TaskHandleType;

bool python_is_finalizing() {
#if PY_VERSION_HEX >= 0x030D0000
    return Py_IsFinalizing();
#else
    return _Py_IsFinalizing();
#endif
}

void call_callback(PyObject *fn, PyObject *handle) {
    PyObject *result = PyObject_CallFunctionObjArgs(fn, handle, nullptr);
    if (result)
        Py_DECREF(result);
    else
        PyErr_WriteUnraisable(fn);
}

}  // namespace

namespace dftracer::utils::python::runtime {

namespace {

// Workers that may still take the GIL to run callbacks. The exit hook stops new
// ones and waits for those inside a callback, so none reaches
// PyGILState_Ensure once the interpreter finalizes.
struct CallbackGate {
    std::mutex mutex;
    std::condition_variable idle;
    bool closing{false};
    int active{0};
};

CallbackGate &callback_gate() {
    static dstd::NoDestructor<CallbackGate> gate;
    return *gate;
}

}  // namespace

void AsyncState::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ended_.wait(lock, [this] { return done(); });
}

bool AsyncState::add_callback(PyObject *handle, PyObject *fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done()) return false;
    if (!owner_) {
        Py_INCREF(handle);
        owner_ = handle;
    }
    Py_INCREF(fn);
    callbacks_.push_back(fn);
    return true;
}

void AsyncState::finish() {
    dstd::Vector<PyObject *> pending;
    PyObject *owner = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(callbacks_);
        owner = std::exchange(owner_, nullptr);
        done_.store(true, std::memory_order_release);
    }
    ended_.notify_all();
    if (pending.empty()) return;
    CallbackGate &gate = callback_gate();
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        if (gate.closing || python_is_finalizing()) return;
        ++gate.active;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    for (PyObject *fn : pending) {
        call_callback(fn, owner);
        Py_DECREF(fn);
    }
    Py_DECREF(owner);
    PyGILState_Release(gil);
    std::lock_guard<std::mutex> lock(gate.mutex);
    --gate.active;
    gate.idle.notify_all();
}

namespace {

PyObject *close_task_callbacks(PyObject *, PyObject *) {
    CallbackGate &gate = callback_gate();
    Py_BEGIN_ALLOW_THREADS {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.closing = true;
        gate.idle.wait(lock, [&] { return gate.active == 0; });
    }
    Py_END_ALLOW_THREADS Py_RETURN_NONE;
}

PyMethodDef task_handle_module_methods[] = {
    {"_close_task_callbacks", close_task_callbacks, METH_NOARGS,
     "Drop the done callbacks that have not started and wait for those "
     "running. Called at interpreter exit."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

}  // namespace dftracer::utils::python::runtime

namespace {

PyObject *TaskHandle_add_done_callback(TaskHandleObject *self, PyObject *fn) {
    if (!PyCallable_Check(fn)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return nullptr;
    }
    PyObject *handle = reinterpret_cast<PyObject *>(self);
    if (!self->state->add_callback(handle, fn)) call_callback(fn, handle);
    Py_RETURN_NONE;
}

PyObject *TaskHandle_cancel(TaskHandleObject *self,
                            PyObject *Py_UNUSED(ignored)) {
    self->state->cancel();
    Py_RETURN_NONE;
}

void TaskHandle_dealloc(TaskHandleObject *self) {
    Py_XDECREF(self->value);
    self->state.~StatePtr();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject *>(self));
}

PyObject *TaskHandle_get(TaskHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    if (!self->value) {
        AsyncState *state = self->state.get();
        Py_BEGIN_ALLOW_THREADS state->wait();
        Py_END_ALLOW_THREADS
            // Another thread may have converted it while this one waited.
            if (!self->value) {
            PyObject *converted = state->to_python();
            if (!converted) return nullptr;
            self->value = converted;
        }
    }
    Py_INCREF(self->value);
    return self->value;
}

PyObject *TaskHandle_wait(TaskHandleObject *self,
                          PyObject *Py_UNUSED(ignored)) {
    PyObject *value = TaskHandle_get(self, nullptr);
    if (!value) return nullptr;
    Py_DECREF(value);
    Py_RETURN_NONE;
}

PyObject *TaskHandle_done(TaskHandleObject *self,
                          PyObject *Py_UNUSED(ignored)) {
    return PyBool_FromLong(self->state->done() ? 1 : 0);
}

PyObject *TaskHandle_get_name(TaskHandleObject *self, void *) {
    const auto &name = self->state->name();
    return PyUnicode_FromStringAndSize(name.data(),
                                       static_cast<Py_ssize_t>(name.size()));
}

PyObject *TaskHandle_get_task_id(TaskHandleObject *self, void *) {
    return PyLong_FromLongLong(static_cast<long long>(self->state->id()));
}

PyMethodDef TaskHandle_methods[] = {
    {"get", DFTU_PYCFUNCTION(TaskHandle_get), METH_NOARGS,
     "get() -> result\n"
     "--\n"
     "\n"
     "Block, with the GIL released, until the task ends. Return its result\n"
     "or raise its error. A second call returns the same result.\n"},
    {"wait", DFTU_PYCFUNCTION(TaskHandle_wait), METH_NOARGS,
     "wait() -> None\n"
     "--\n"
     "\n"
     "Block until the task ends. Raise its error.\n"},
    {"done", DFTU_PYCFUNCTION(TaskHandle_done), METH_NOARGS,
     "done() -> bool\n"
     "--\n"
     "\n"
     "Return True if the task has ended, without blocking.\n"},
    {"add_done_callback", DFTU_PYCFUNCTION(TaskHandle_add_done_callback),
     METH_O,
     "add_done_callback(fn) -> None\n"
     "--\n"
     "\n"
     "Call fn(handle) once when the task ends: on the calling thread if it\n"
     "already has, otherwise on the pool worker that finished it, which\n"
     "takes the GIL for the call. A slow callback holds that worker. An\n"
     "exception of fn is reported as unraisable.\n"},
    {"cancel", DFTU_PYCFUNCTION(TaskHandle_cancel), METH_NOARGS,
     "cancel() -> None\n"
     "--\n"
     "\n"
     "Ask the task to stop. get() then raises its cancelled error.\n"},
    {nullptr, nullptr, 0, nullptr}};

PyGetSetDef TaskHandle_getsetters[] = {
    {"name", reinterpret_cast<getter>(TaskHandle_get_name), nullptr,
     "Task name", nullptr},
    {"task_id", reinterpret_cast<getter>(TaskHandle_get_task_id), nullptr,
     "Task identifier", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

}  // namespace

namespace dftracer::utils::python::runtime {

PyObject *new_task_handle(dstd::SharedPtr<AsyncState> state) {
    auto *self = reinterpret_cast<TaskHandleObject *>(
        TaskHandleType.tp_alloc(&TaskHandleType, 0));
    if (!self) return nullptr;
    new (&self->state) StatePtr(std::move(state));
    self->value = nullptr;
    return reinterpret_cast<PyObject *>(self);
}

int init_task_handle(PyObject *m) {
    TaskHandleType = {};
    TaskHandleType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    TaskHandleType.tp_name = "dftracer_utils_ext.TaskHandle";
    TaskHandleType.tp_basicsize = sizeof(TaskHandleObject);
    TaskHandleType.tp_dealloc =
        reinterpret_cast<destructor>(TaskHandle_dealloc);
    TaskHandleType.tp_flags = Py_TPFLAGS_DEFAULT;
    TaskHandleType.tp_doc =
        "Handle to a native task that runs on the Runtime pool.\n"
        "\n"
        "Made by build_async() and collect_async(); not constructible.";
    TaskHandleType.tp_methods = TaskHandle_methods;
    TaskHandleType.tp_getset = TaskHandle_getsetters;
    if (register_type(m, &TaskHandleType, "TaskHandle") < 0) return -1;
    return PyModule_AddFunctions(
        m, dftracer::utils::python::runtime::task_handle_module_methods);
}

}  // namespace dftracer::utils::python::runtime
