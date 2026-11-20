// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_TRACE_VIEWER_H
#define DFTRACER_UTILS_PYTHON_TRACE_TRACE_VIEWER_H

#include <Python.h>
#include <dftracer/utils/python/core/type_helpers.h>

namespace dftracer::utils::trace::views {
class LiveView;
class View;
}  // namespace dftracer::utils::trace::views

// TraceViewer: the native handle behind the Python TraceViewer, one C++
// trace::views::View. The trace builders return a new handle; lazy()
// hands its plan to Python as a LazyFrame, and with_lazy() takes one back
// after the Python side appended generic ops. Terminals return LazyFrame
// plans (or run at once, for the ones with no scan to share).
typedef struct {
    PyObject_HEAD PyObject *weaklist;
    dftracer::utils::trace::views::View *tv;
} TraceViewerObject;

extern PyTypeObject TraceViewerType;

// _LiveTraceViewer: the native handle behind the Python LiveTraceViewer, one
// C++ trace::views::LiveView. refresh() returns a TraceViewer.
typedef struct {
    PyObject_HEAD dftracer::utils::trace::views::LiveView *live;
} LiveTraceViewerObject;

extern PyTypeObject LiveTraceViewerType;

namespace dftracer::utils::python::trace {

int init_trace_viewer(PyObject *m);

// The TraceViewer type and the slot its factories allocate, for _bind_types.
TypeBinding trace_viewer_binding();

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_TRACE_VIEWER_H
