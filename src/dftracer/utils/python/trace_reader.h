#ifndef DFTRACER_UTILS_PYTHON_TRACE_READER_H
#define DFTRACER_UTILS_PYTHON_TRACE_READER_H

#include <Python.h>

#include <cstddef>

typedef struct {
    PyObject_HEAD PyObject *file_path;
    PyObject *index_dir;
    std::size_t checkpoint_size;
    int auto_build_index;
    std::size_t index_threshold;
    int has_index;
} TraceReaderObject;

extern PyTypeObject TraceReaderType;
int init_trace_reader(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_TRACE_READER_H
