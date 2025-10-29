#ifndef DFTRACER_UTILS_PYTHON_LAZY_JSON_LINE_PROCESSOR_H
#define DFTRACER_UTILS_PYTHON_LAZY_JSON_LINE_PROCESSOR_H

#include <Python.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/reader/line_processor.h>

class PyLazyJSONLineProcessor : public dftracer::utils::LineProcessor {
   public:
    PyLazyJSONLineProcessor() : result_list(nullptr) {
        result_list = PyList_New(0);
        if (!result_list) {
            PyErr_NoMemory();
        }
    }

    ~PyLazyJSONLineProcessor() { Py_XDECREF(result_list); }

    bool process(const char* data, std::size_t length) override {
        if (!result_list) return false;

        const char* trimmed;
        std::size_t trimmed_length;
        if (!dftracer::utils::json_trim_and_validate(data, length, trimmed,
                                                     trimmed_length)) {
            return true;
        }

        PyObject* json_obj = JSON_from_data(trimmed, trimmed_length);
        if (!json_obj) {
            PyErr_Clear();
            return true;
        }

        int result = PyList_Append(result_list, json_obj);
        Py_DECREF(json_obj);

        return result == 0;
    }

    PyObject* get_result() {
        if (!result_list) {
            Py_RETURN_NONE;
        }
        Py_INCREF(result_list);
        return result_list;
    }

    std::size_t size() const {
        return result_list ? PyList_Size(result_list) : 0;
    }

   private:
    PyObject* result_list;
};

#endif  // DFTRACER_UTILS_PYTHON_LAZY_JSON_LINE_PROCESSOR_H
