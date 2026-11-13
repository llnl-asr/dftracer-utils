// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/abi_context.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/frame/op_runner.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dlfcn.h>

#include <cstddef>

using namespace dftracer::utils;

namespace dftracer::utils::python::frame {
namespace {

using dftracer::utils::Runtime;
using dftracer::utils::dstd::StringIntern;
using plugins::host::PluginFold;

using build_op_fn = ::dftu_status (*)(::dftu_ctx*, const ::dftu_plugin_host*,
                                      ::dftu_job**);

// The status and message of the one run of a started job.
struct JobRun {
    dftu_status status = DFTU_OK;
    dstd::String message;
};

coro::CoroTask<JobRun> await_job(::dftu_task* started) {
    coro::AbiTask* task = coro::abi_task(started);
    JobRun run;
    run.status = co_await coro::run_abi_task(task);
    if (run.status < 0) run.message = coro::abi_task_error(*task).message();
    co_return run;
}

PyObject* jit_run_op(PyObject*, PyObject* args) {
    const char* so_path = nullptr;
    const char* in_data = nullptr;
    Py_ssize_t in_size = 0;
    Py_ssize_t out_size = 0;
    if (!PyArg_ParseTuple(args, "sy#n", &so_path, &in_data, &in_size,
                          &out_size)) {
        return nullptr;
    }
    if (out_size < 0) {
        PyErr_SetString(PyExc_ValueError, "out_size must be non-negative");
        return nullptr;
    }

    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        PyErr_Format(PyExc_RuntimeError, "cannot load op '%s': %s", so_path,
                     dlerror());
        return nullptr;
    }
    auto build =
        reinterpret_cast<build_op_fn>(dlsym(handle, "dftracer_build_op"));
    if (!build) {
        PyErr_Format(PyExc_RuntimeError,
                     "op '%s' exports no 'dftracer_build_op' symbol", so_path);
        dlclose(handle);
        return nullptr;
    }

    dstd::Vector<char> out_buf(static_cast<std::size_t>(out_size));
    JobRun run;
    bool ran = false;
    {
        // The job host is a real PluginFold with no slice: the job graph only
        // needs the host's task service, never a scan.
        StringIntern intern;
        ::dftu_plugin plugin{};
        plugin.struct_size = sizeof(::dftu_plugin);
        PluginFold fold(&plugin, intern);
        ::dftu_plugin_host& host = fold.host();
        coro::AbiContext ctx;
        ::dftu_job* job = nullptr;
        const dftu_status built = build(ctx.get(), &host, &job);
        if (built < 0) {
            run.status = built;
            run.message.assign(ctx.message().data(), ctx.message().size());
            ran = true;
        }
        const auto* tasks = static_cast<const ::dftu_svc_task*>(
            host.get_service(host.h, DFTU_SVC_TASK));
        if (built >= 0 && job && tasks && tasks->start) {
            ::dftu_task* task = nullptr;
            const dftu_status started = tasks->start(
                ctx.get(), host.h, job, in_data, out_buf.data(), &task);
            if (started < 0) {
                run.status = started;
                run.message.assign(ctx.message().data(), ctx.message().size());
            } else if (task) {
                Py_BEGIN_ALLOW_THREADS;
                auto finished = default_runtime().block_on(
                    coro::named("jit-op", await_job(task)));
                if (finished) {
                    run = std::move(*finished);
                } else {
                    run.status = DFTU_E_CANCELLED;
                    run.message = finished.error().message();
                }
                Py_END_ALLOW_THREADS;
                coro::free_abi_task(task);
            }
            ran = true;
        }
    }
    dlclose(handle);

    if (!ran) {
        PyErr_Format(PyExc_RuntimeError, "op '%s' built no runnable graph",
                     so_path);
        return nullptr;
    }
    if (run.status < 0) {
        PyErr_Format(PyExc_RuntimeError, "op '%s' failed (status %d): %s",
                     so_path, static_cast<int>(run.status),
                     run.message.empty() ? "no message" : run.message.c_str());
        return nullptr;
    }
    return PyBytes_FromStringAndSize(out_buf.data(),
                                     static_cast<Py_ssize_t>(out_size));
}

PyMethodDef op_runner_methods[] = {
    {"jit_run_op", jit_run_op, METH_VARARGS,
     "jit_run_op(so_path: str, in_bytes: bytes, out_size: int) -> bytes\n\n"
     "Load a compiled jit_op .so (exposing dftracer_build_op), build its\n"
     "dftu_job graph on a standalone task host, run it over in_bytes, and\n"
     "return out_size bytes of result. No plugin and no scan are involved."},
    {nullptr, nullptr, 0, nullptr},
};

}  // namespace

int init_op_runner(PyObject* m) {
    return PyModule_AddFunctions(m, op_runner_methods);
}

}  // namespace dftracer::utils::python::frame
