#ifndef DFTRACER_UTILS_PYTHON_PLUGIN_HOST_H
#define DFTRACER_UTILS_PYTHON_PLUGIN_HOST_H

#include <Python.h>
#include <dftracer/utils/plugins/plugins.h>

// PluginHostObject backs the Python `Plugins` type: the C++ plugins::Plugins
// set. The set is built eagerly at construction (dlopen, ABI gate, capability
// resolution), so a load/symbol/ABI/capability failure raises ImportError from
// the constructor. run() drives every plugin as a fold over one fused parallel
// scan of a trace set and returns its named results; the scan counters are
// returned alongside them, not stored on this object.

typedef struct {
    PyObject_HEAD PyObject
        *runtime_obj;  // RuntimeObject* or NULL (uses default)
    void *host_ptr;    // python::PluginHostState*
} PluginHostObject;

extern PyTypeObject PluginHostType;

namespace dftracer::utils::python {

int init_plugin_host(PyObject *m);

// Read a PluginHostObject's named results as a {name: pyarrow|bytes} dict, once
// its session has executed. NULL with a Python error set on failure.
PyObject *plugin_host_results_dict(PyObject *host);

// The built plugin set behind a PluginHostObject. NULL with a Python error set
// if construction failed to leave a built set (should not happen post-init).
const plugins::Plugins *plugin_host_plugins(PyObject *host);

// Where a co-scan should deposit that host's named results; owned by the host
// object, so it outlives the session. NULL with a Python error set.
plugins::NamedResultRegistry *plugin_host_results(PyObject *host);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_PLUGIN_HOST_H
