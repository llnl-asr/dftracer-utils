#ifndef DFTRACER_UTILS_PYTHON_READER_H
#define DFTRACER_UTILS_PYTHON_READER_H

#include <Python.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>

#include <cstddef>
#include <cstdint>

typedef struct {
    PyObject_HEAD dft_reader_handle_t handle;
    PyObject *gz_path;
    PyObject *idx_path;
    std::size_t checkpoint_size;
    std::size_t buffer_size;
} ReaderObject;

extern PyTypeObject ReaderType;

int init_reader(PyObject *m);

#endif
