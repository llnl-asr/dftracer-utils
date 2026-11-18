// SPDX-License-Identifier: MIT
// The native side of `@dft.udf`.
//
// A JIT-compiled UDF is a shared library that exports one dftu_udf_desc; it
// runs on the engine's workers and never touches Python.
//
// A per-batch Python UDF hands every batch to ONE lane thread. A worker queues
// the batch and waits without the GIL; the lane takes the GIL, calls the
// function once on read-only numpy views of the argument columns, and gives
// the result column back. Only the lane thread calls the function, so on a
// build with the GIL the batches of all workers run one after another. A
// free-threaded build could run several lane loops; the queue and the job
// hand-off already allow it.
//
// The contract for every binding that runs an engine call: release the GIL
// around it (PyEval_SaveThread), because the lane needs the GIL to run a batch
// and numpy drops it inside its own loops. A running batch cannot be cancelled
// or interrupted, since it reads the caller's columns; the run waits for it.

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/c_api_udf.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/frame/udf_python.h>
#include <dlfcn.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

using namespace dftracer::utils;

namespace dftracer::utils::python::frame {
namespace {

const char* const LANE_MODULE = "dftracer.utils._udf_lane";
const char* const JIT_ENTRY = "dftu_jit_udf_desc";
constexpr std::chrono::milliseconds CANCEL_POLL{20};

struct TypeRow {
    dftu_dtype id;
    const char* name;
    std::size_t width;
};

const TypeRow TYPES[] = {
    {DFTU_TYPE_BOOL, "bool", 0},       {DFTU_TYPE_INT8, "int8", 1},
    {DFTU_TYPE_INT16, "int16", 2},     {DFTU_TYPE_INT32, "int32", 4},
    {DFTU_TYPE_INT64, "int64", 8},     {DFTU_TYPE_UINT8, "uint8", 1},
    {DFTU_TYPE_UINT16, "uint16", 2},   {DFTU_TYPE_UINT32, "uint32", 4},
    {DFTU_TYPE_UINT64, "uint64", 8},   {DFTU_TYPE_FLOAT32, "float32", 4},
    {DFTU_TYPE_FLOAT64, "float64", 8},
};

const TypeRow* type_by_id(std::int32_t id) {
    for (const TypeRow& row : TYPES)
        if (row.id == id) return &row;
    return nullptr;
}

const TypeRow* type_by_name(const char* name) {
    for (const TypeRow& row : TYPES)
        if (dstd::StringView(row.name) == dstd::StringView(name)) return &row;
    return nullptr;
}

std::size_t packed_bytes(std::uint64_t rows) {
    return static_cast<std::size_t>((rows + 7) / 8);
}

/// One batch waiting for the lane. The worker that made it owns it and waits
/// for `state` to leave Queued and Running.
struct Job {
    enum class State { Queued, Running, Done, Cancelled };

    PyObject* fn = nullptr;
    const dftu_series* const* args = nullptr;
    std::uint32_t arg_count = 0;
    std::uint64_t rows = 0;
    const TypeRow* result = nullptr;
    State state = State::Queued;
    dftu_status status = DFTU_OK;
    dstd::String message;
    dftu_series* out = nullptr;
};

const char* const SERIES_CAPSULE = "dftu.series";

void free_series_capsule(PyObject* capsule) {
    dftu_series_free(static_cast<dftu_series*>(
        PyCapsule_GetPointer(capsule, SERIES_CAPSULE)));
}

/// Takes the text of the pending Python error, as "Type: message", and clears
/// it. The GIL must be held.
dstd::String take_error_text() {
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* trace = nullptr;
#if PY_VERSION_HEX >= 0x030c0000
    value = PyErr_GetRaisedException();
    if (value != nullptr) type = reinterpret_cast<PyObject*>(Py_TYPE(value));
    Py_XINCREF(type);
#else
    PyErr_Fetch(&type, &value, &trace);
    PyErr_NormalizeException(&type, &value, &trace);
#endif
    dstd::String text;
    const char* type_name = type != nullptr
                                ? reinterpret_cast<PyTypeObject*>(type)->tp_name
                                : "Exception";
    PyObject* str = value != nullptr ? PyObject_Str(value) : nullptr;
    const char* detail = str != nullptr ? PyUnicode_AsUTF8(str) : nullptr;
    if (detail == nullptr) PyErr_Clear();
    text = dstd::fmt<"{}: {}">(type_name, detail != nullptr ? detail : "");
    Py_XDECREF(str);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(trace);
    return text;
}

/// Runs one batch on the calling thread, which must hold the GIL: the
/// arguments become numpy views through dftracer.utils._udf_lane.call, and
/// the result is copied into a new column. Sets the job's status.
void run_batch(Job& job) {
    PyObject* specs = PyList_New(static_cast<Py_ssize_t>(job.arg_count));
    PyObject* helper = nullptr;
    PyObject* result = nullptr;
    bool failed = specs == nullptr;
    for (std::uint32_t i = 0; !failed && i < job.arg_count; ++i) {
        const dftu_series* column = job.args[i];
        const TypeRow* type = type_by_id(dftu_series_type(column));
        if (type == nullptr) {
            job.message = "an argument column has a type a python udf rejects";
            job.status = DFTU_E_INVALID;
            Py_DECREF(specs);
            return;
        }
        // The array behind the argument owns a share of the column, so a view
        // the function keeps never outlives the memory it reads.
        dftu_series* share = dftu_series_chunk(column, 0);
        if (share == nullptr) {
            job.message = "an argument column could not be shared";
            job.status = DFTU_E_INTERNAL;
            Py_DECREF(specs);
            return;
        }
        const void* data = dftu_series_data(share);
        PyObject* keep =
            PyCapsule_New(share, SERIES_CAPSULE, free_series_capsule);
        if (keep == nullptr) {
            dftu_series_free(share);
            failed = true;
            break;
        }
        PyObject* item =
            Py_BuildValue("(sKN)", type->name,
                          static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(data)),
                          keep);
        if (item == nullptr) {
            failed = true;
            break;
        }
        PyList_SET_ITEM(specs, static_cast<Py_ssize_t>(i), item);
    }
    if (!failed) {
        helper = PyImport_ImportModule(LANE_MODULE);
        failed = helper == nullptr;
    }
    if (!failed) {
        result = PyObject_CallMethod(helper, "call", "OKsO", job.fn,
                                     static_cast<unsigned long long>(job.rows),
                                     job.result->name, specs);
        failed = result == nullptr;
    }
    Py_XDECREF(helper);
    Py_XDECREF(specs);
    if (failed) {
        job.message = PyErr_Occurred() ? take_error_text()
                                       : dstd::String("python call failed");
        job.status = DFTU_E_INVALID;
        return;
    }
    Py_buffer buffer;
    if (PyObject_GetBuffer(result, &buffer, PyBUF_SIMPLE) < 0) {
        job.message = take_error_text();
        job.status = DFTU_E_INVALID;
        Py_DECREF(result);
        return;
    }
    const std::size_t want = job.result->id == DFTU_TYPE_BOOL
                                 ? packed_bytes(job.rows)
                                 : job.rows * job.result->width;
    if (static_cast<std::size_t>(buffer.len) != want) {
        job.message = dstd::fmt<"result holds {} bytes, expected {}">(
            static_cast<std::size_t>(buffer.len), want);
        job.status = DFTU_E_INTERNAL;
    } else {
        job.out =
            dftu_series_new_flat(job.result->id, buffer.buf,
                                 static_cast<std::int64_t>(job.rows), nullptr);
        if (job.out == nullptr) {
            job.message = "the result column could not be built";
            job.status = DFTU_E_NO_MEMORY;
        }
    }
    PyBuffer_Release(&buffer);
    Py_DECREF(result);
}

/// The one Python lane. Workers queue Jobs; the lane thread runs them.
class Lane {
   public:
    /// Counts one more batch UDF and starts the lane thread when none runs.
    /// The caller holds the GIL; it is released while an old lane thread that
    /// is leaving is joined.
    bool acquire() {
        dstd::Thread old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (is_closed_) return false;
            ++users_;
            if (is_alive_) {
                is_stopping_ = false;
                return true;
            }
            old = std::move(thread_);
        }
        if (old.joinable()) {
            PyThreadState* saved = PyEval_SaveThread();
            old.join();
            PyEval_RestoreThread(saved);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_alive_) {
            is_alive_ = true;
            is_stopping_ = false;
            ++threads_started_;
            thread_ = dstd::Thread([this] { loop(); }, "dftu-pylane");
        } else {
            is_stopping_ = false;
        }
        return true;
    }

    /// One batch UDF is gone for good; the lane thread leaves after the last.
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_ > 0) --users_;
        if (users_ == 0) {
            is_stopping_ = true;
            wake_.notify_all();
        }
    }

    /// Stops the lane for the rest of the process: queued batches fail with
    /// DFTU_E_CANCELLED and later ones too. Joins the thread without the GIL.
    void close() {
        dstd::Thread old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_closed_ = true;
            is_stopping_ = true;
            wake_.notify_all();
            old = std::move(thread_);
        }
        PyThreadState* saved =
            PyGILState_Check() ? PyEval_SaveThread() : nullptr;
        old.join();
        if (saved != nullptr) PyEval_RestoreThread(saved);
    }

    /// Queues `job` and waits for it without the GIL. Polls the cancel of
    /// `ctx`: a batch still queued is cancelled, a running one is waited for
    /// because it reads the caller's columns.
    dftu_status run(dftu_ctx* ctx, Job& job) {
        PyThreadState* saved =
            PyGILState_Check() ? PyEval_SaveThread() : nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (is_closed_ || !is_alive_ || is_stopping_) {
                job.state = Job::State::Cancelled;
            } else {
                queue_.push_back(&job);
                wake_.notify_one();
            }
            while (job.state == Job::State::Queued ||
                   job.state == Job::State::Running) {
                done_.wait_for(lock, CANCEL_POLL);
                if (job.state == Job::State::Queued && dftu_check(ctx) < 0)
                    cancel_locked(job);
            }
        }
        if (saved != nullptr) PyEval_RestoreThread(saved);
        if (job.state == Job::State::Cancelled)
            return dftu_fail(ctx, DFTU_E_CANCELLED,
                             "the python udf lane cancelled the batch");
        if (job.status < 0)
            return dftu_fail(ctx, job.status, job.message.c_str());
        return DFTU_OK;
    }

    void stats(std::uint64_t* started, std::uint64_t* alive,
               std::uint64_t* batches, std::uint64_t* users) {
        std::lock_guard<std::mutex> lock(mutex_);
        *started = threads_started_;
        *alive = is_alive_ ? 1 : 0;
        *batches = batches_;
        *users = users_;
    }

   private:
    void cancel_locked(Job& job) {
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i] == &job) {
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        job.state = Job::State::Cancelled;
    }

    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock,
                       [this] { return !queue_.empty() || is_stopping_; });
            if (is_stopping_) {
                for (Job* job : queue_) job->state = Job::State::Cancelled;
                queue_ = dstd::Vector<Job*>();
                is_alive_ = false;
                done_.notify_all();
                return;
            }
            Job* job = queue_.front();
            queue_.erase(queue_.begin());
            if (queue_.empty()) queue_ = dstd::Vector<Job*>();
            job->state = Job::State::Running;
            lock.unlock();
            {
                PyGILState_STATE gil = PyGILState_Ensure();
                run_batch(*job);
                PyGILState_Release(gil);
            }
            lock.lock();
            job->state = Job::State::Done;
            ++batches_;
            done_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    dstd::Vector<Job*> queue_;
    dstd::Thread thread_;
    std::uint64_t users_ = 0;
    std::uint64_t threads_started_ = 0;
    std::uint64_t batches_ = 0;
    bool is_alive_ = false;
    bool is_stopping_ = false;
    bool is_closed_ = false;
};

Lane& lane() { return dstd::Singleton<Lane>::get(); }

/// What `self` of a batch UDF holds: the function and the declared result.
struct BatchUdf {
    PyObject* fn;
    const TypeRow* result;
};

/// What `bound` of a batch UDF holds.
struct BatchBound {
    const BatchUdf* udf;
};

void drop_batch_udf(const void* raw) {
    auto* udf = static_cast<BatchUdf*>(const_cast<void*>(raw));
    if (Py_IsInitialized()) {
        PyGILState_STATE gil = PyGILState_Ensure();
        Py_DECREF(udf->fn);
        PyGILState_Release(gil);
    }
    dstd::UniquePtr<BatchUdf> owner(udf);
    lane().release();
}

dftu_status batch_bind(dftu_ctx* ctx, void* self, const dftu_type_info* args,
                       std::uint32_t n, const dftu_scalar* const* /*consts*/,
                       dftu_type_info* out, void** bound) {
    const auto* udf = static_cast<const BatchUdf*>(self);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (type_by_id(args[i].id) == nullptr) {
            const dstd::String why = dstd::fmt<
                "argument {} is not a number or bool column; a per-batch "
                "python udf takes numeric and bool columns, not strings">(i +
                                                                          1);
            return dftu_fail(ctx, DFTU_E_INVALID, why.c_str());
        }
    }
    out->struct_size = sizeof(dftu_type_info);
    out->id = udf->result->id;
    auto held = dstd::make_unique<BatchBound>(BatchBound{udf});
    *bound = held.release();
    return DFTU_OK;
}

dftu_status batch_exec(dftu_ctx* ctx, void* bound,
                       const dftu_series* const* args, std::uint32_t n,
                       std::uint64_t rows, dftu_series** out) {
    const BatchUdf* udf = static_cast<const BatchBound*>(bound)->udf;
    if (!Py_IsInitialized())
        return dftu_fail(ctx, DFTU_E_CANCELLED, "python is not running");
    if (dftu_check(ctx) < 0)
        return dftu_fail(ctx, DFTU_E_CANCELLED, "cancelled");
    Job job;
    job.fn = udf->fn;
    job.args = args;
    job.arg_count = n;
    job.rows = rows;
    job.result = udf->result;
    const dftu_status status = lane().run(ctx, job);
    if (status < 0) return status;
    *out = job.out;
    return DFTU_OK;
}

void batch_free_bound(void* bound) {
    dstd::UniquePtr<BatchBound> owner(static_cast<BatchBound*>(bound));
}

PyObject* raise_error(PyObject* type, dstd::CStringView message) {
    PyErr_SetString(type, message.c_str());
    return nullptr;
}

// udf_register_batch(name, fn, arg_count, returns, volatile) -> None
PyObject* udf_register_batch(PyObject* /*self*/, PyObject* args) {
    const char* name = nullptr;
    PyObject* fn = nullptr;
    int arg_count = 0;
    const char* returns = nullptr;
    int is_volatile = 0;
    if (!PyArg_ParseTuple(args, "sOisp", &name, &fn, &arg_count, &returns,
                          &is_volatile))
        return nullptr;
    if (!PyCallable_Check(fn)) {
        PyErr_SetString(PyExc_TypeError, "a udf function must be callable");
        return nullptr;
    }
    const TypeRow* result = type_by_name(returns);
    if (result == nullptr) {
        PyErr_SetString(PyExc_ValueError, "unknown udf result type");
        return nullptr;
    }
    if (!lane().acquire()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "the python udf lane is closed for this process");
        return nullptr;
    }
    auto udf = dstd::make_unique<BatchUdf>(BatchUdf{fn, result});
    Py_INCREF(fn);
    dftu_udf_desc desc{};
    desc.struct_size = sizeof(dftu_udf_desc);
    desc.name = name;
    desc.flags = is_volatile ? DFTU_FN_VOLATILE : 0;
    desc.min_args = arg_count;
    desc.max_args = arg_count;
    desc.bind = &batch_bind;
    desc.exec = &batch_exec;
    desc.free_bound = &batch_free_bound;
    BatchUdf* raw = udf.get();
    // On any failure below `keep` drops with the record and runs
    // drop_batch_udf, which gives back the lane count and the reference.
    dstd::SharedPtr<const void> keep = dstd::adopt_shared_ptr(
        static_cast<const void*>(udf.release()), &drop_batch_udf);
    auto added = dataframe::add_udf(desc, raw, keep);
    if (!added)
        return raise_error(PyExc_ValueError,
                           dstd::String(added.error().message()));
    Py_RETURN_NONE;
}

// udf_register_library(path) -> None
PyObject* udf_register_library(PyObject* /*self*/, PyObject* arg) {
    const char* path = PyUnicode_AsUTF8(arg);
    if (path == nullptr) return nullptr;
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* why = dlerror();
        return raise_error(PyExc_OSError,
                           dstd::fmt<"cannot load the udf library {}: {}">(
                               path, why != nullptr ? why : "unknown"));
    }
    dstd::SharedPtr<const void> keep = dstd::adopt_shared_ptr(
        static_cast<const void*>(handle),
        [](const void* h) { dlclose(const_cast<void*>(h)); });
    using Entry = const dftu_udf_desc* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(handle, JIT_ENTRY));
    if (entry == nullptr)
        return raise_error(PyExc_OSError,
                           dstd::fmt<"{} does not export {}">(path, JIT_ENTRY));
    const dftu_udf_desc* desc = entry();
    if (desc == nullptr)
        return raise_error(PyExc_OSError,
                           dstd::fmt<"{} gave no udf descriptor">(path));
    auto added = dataframe::add_udf(*desc, nullptr, keep);
    if (!added)
        return raise_error(PyExc_ValueError,
                           dstd::String(added.error().message()));
    Py_RETURN_NONE;
}

// udf_unregister(name) -> bool
PyObject* udf_unregister(PyObject* /*self*/, PyObject* arg) {
    Py_ssize_t n = 0;
    const char* name = PyUnicode_AsUTF8AndSize(arg, &n);
    if (name == nullptr) return nullptr;
    const bool removed = dataframe::unregister_udf(
        dstd::StringView(name, static_cast<std::size_t>(n)));
    return PyBool_FromLong(removed ? 1 : 0);
}

// udf_lane_stats() -> {"threads_started", "threads_alive", "batches", "users"}
PyObject* udf_lane_stats(PyObject* /*self*/, PyObject* /*unused*/) {
    std::uint64_t started = 0, alive = 0, batches = 0, users = 0;
    lane().stats(&started, &alive, &batches, &users);
    return Py_BuildValue("{s:K,s:K,s:K,s:K}", "threads_started",
                         static_cast<unsigned long long>(started),
                         "threads_alive",
                         static_cast<unsigned long long>(alive), "batches",
                         static_cast<unsigned long long>(batches), "users",
                         static_cast<unsigned long long>(users));
}

// udf_lane_shutdown() -> None
PyObject* udf_lane_shutdown(PyObject* /*self*/, PyObject* /*unused*/) {
    lane().close();
    Py_RETURN_NONE;
}

}  // namespace

int init_udf_python(PyObject* m) {
    static PyMethodDef defs[] = {
        {"udf_register_library", udf_register_library, METH_O,
         "udf_register_library(path) -> None: load the shared library `path` "
         "that a jit udf compiled to and register the udf it exports. The "
         "library stays loaded while the udf or a plan that holds it lives."},
        {"udf_register_batch", udf_register_batch, METH_VARARGS,
         "udf_register_batch(name, fn, arg_count, returns, volatile) -> None: "
         "register `fn`, called once per batch on the python lane with "
         "read-only numpy views of the argument columns, as a scalar udf that "
         "gives the dtype named by `returns`."},
        {"udf_unregister", udf_unregister, METH_O,
         "udf_unregister(name) -> bool: remove `name` from the udf registry. "
         "A plan that holds the udf keeps it."},
        {"udf_lane_stats", udf_lane_stats, METH_NOARGS,
         "udf_lane_stats() -> dict: lane threads started and alive, batches "
         "run and registered batch udfs."},
        {"udf_lane_shutdown", udf_lane_shutdown, METH_NOARGS,
         "udf_lane_shutdown() -> None: stop the python lane for the rest of "
         "the process; queued and later batches fail as cancelled."},
    };
    for (PyMethodDef& def : defs) {
        PyObject* fn = PyCFunction_NewEx(&def, nullptr, nullptr);
        if (fn == nullptr) return -1;
        if (PyModule_AddObject(m, def.ml_name, fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }
    return 0;
}

}  // namespace dftracer::utils::python::frame
