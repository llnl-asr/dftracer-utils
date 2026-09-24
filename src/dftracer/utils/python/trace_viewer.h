#ifndef DFTRACER_UTILS_PYTHON_TRACE_VIEWER_H
#define DFTRACER_UTILS_PYTHON_TRACE_VIEWER_H

#include <Python.h>

namespace dftracer::utils::trace::views {
class View;
}

// _TraceViewer: the native handle behind the Python TraceViewer, one C++
// trace::views::View. The trace builders return a new handle; lazy()
// hands its plan to Python as a _LazyFrame, and with_lazy() takes one back
// after the Python side appended generic ops. Terminals return _LazyFrame
// plans (or run at once, for the ones with no scan to share).
typedef struct {
    PyObject_HEAD dftracer::utils::trace::views::View *tv;
} TraceViewerObject;

extern PyTypeObject TraceViewerType;

namespace dftracer::utils::python {

int init_trace_viewer(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_TRACE_VIEWER_H
