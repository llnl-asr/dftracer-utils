#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/python/op_runner.h>
#include <dlfcn.h>

#include <cstddef>
#include <vector>

namespace dftracer::utils::python {
namespace {

namespace coro = dftracer::utils::coro;
using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::PluginFold;

// The compose host is a real PluginFold with a do-nothing slice: the op graph
// only needs the host's compose extension and task machinery, never a scan.
struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

using build_op_fn = ::dftu_op* (*)(const ::dftu_plugin_host*);

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

    std::vector<char> out_buf(static_cast<std::size_t>(out_size));
    int rc = -1;
    bool ran = false;
    {
        StringIntern intern;
        ::dftu_plugin* plugin =
            dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
        {
            PluginFold fold(plugin, intern);
            ::dftu_plugin_host& host = fold.host();
            ::dftu_op* op = build(&host);
            const auto* compose = static_cast<const ::dftu_svc_compose*>(
                host.get_service(host.h, DFTU_SVC_COMPOSE));
            if (op && compose && compose->run) {
                ::dftu_task* task =
                    compose->run(host.h, op, in_data, out_buf.data(), &rc);
                if (task) {
                    Py_BEGIN_ALLOW_THREADS;
                    Runtime rt(1);
                    rt.scope("jit-op", [&](CoroScope&) -> coro::CoroTask<void> {
                          co_await *reinterpret_cast<coro::CoroTask<void>*>(
                              task);
                      }).wait();
                    rt.shutdown();
                    Py_END_ALLOW_THREADS;
                }
                ran = true;
            }
        }
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
    dlclose(handle);

    if (!ran) {
        PyErr_Format(PyExc_RuntimeError, "op '%s' built no runnable graph",
                     so_path);
        return nullptr;
    }
    if (rc != 0) {
        PyErr_Format(PyExc_RuntimeError, "op '%s' failed (rc=%d)", so_path, rc);
        return nullptr;
    }
    return PyBytes_FromStringAndSize(out_buf.data(),
                                     static_cast<Py_ssize_t>(out_size));
}

PyMethodDef op_runner_methods[] = {
    {"jit_run_op", jit_run_op, METH_VARARGS,
     "jit_run_op(so_path: str, in_bytes: bytes, out_size: int) -> bytes\n\n"
     "Load a compiled jit_op .so (exposing dftracer_build_op), build its\n"
     "dftu_op graph on a standalone compose host, run it over in_bytes, and\n"
     "return out_size bytes of result. No plugin and no scan are involved."},
    {nullptr, nullptr, 0, nullptr},
};

}  // namespace

int init_op_runner(PyObject* m) {
    return PyModule_AddFunctions(m, op_runner_methods);
}

}  // namespace dftracer::utils::python
