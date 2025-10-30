#ifndef DFTRACER_UTILS_PYTHON_PYLIST_LINE_PROCESSOR_H
#define DFTRACER_UTILS_PYTHON_PYLIST_LINE_PROCESSOR_H

#include <Python.h>
#include <dftracer/utils/core/utils/timer.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>

#include <cstdio>
#include <stdexcept>

class PyListLineProcessor
    : public dftracer::utils::utilities::reader::internal::LineProcessor {
   private:
    PyObject* py_list_;

   public:
    PyListLineProcessor() : py_list_(PyList_New(0)) {
        if (!py_list_) {
            PyErr_SetString(PyExc_MemoryError, "Failed to create Python list");
            throw std::runtime_error("Failed to create Python list");
        }
    }

    ~PyListLineProcessor() { Py_XDECREF(py_list_); }

    bool process(const char* data, std::size_t length) override {
        PyObject* py_line = PyUnicode_FromStringAndSize(data, length);
        if (!py_line) {
            return false;
        }
        int result = PyList_Append(py_list_, py_line);
        Py_DECREF(py_line);
        return result == 0;
    }

    PyObject* get_result() {
        if (!py_list_) {
            Py_RETURN_NONE;
        }
        Py_INCREF(py_list_);
        return py_list_;
    }

    std::size_t size() const { return py_list_ ? PyList_Size(py_list_) : 0; }
};

#endif  // DFTRACER_UTILS_PYTHON_PYLIST_LINE_PROCESSOR_H
