#ifndef DFTRACER_UTILS_PYTHON_JSON_H
#define DFTRACER_UTILS_PYTHON_JSON_H

#include <Python.h>
#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

typedef struct {
    PyObject_HEAD mutable yyjson_doc* doc;
    yyjson_val* root;
    mutable bool parsed;
    std::size_t json_length;
    bool owns_doc;
    char json_data[];
} JSONObject;

extern PyTypeObject JSONType;

extern PyMethodDef JSON_methods[];
extern PySequenceMethods JSON_as_sequence;
extern PyMappingMethods JSON_as_mapping;

int init_json(PyObject* m);

PyObject* JSON_from_data(const char* data, size_t length);

// Create a JSON object wrapping a yyjson_val subtree
PyObject* JSON_from_yyjson_val(yyjson_doc* doc, yyjson_val* root);

#endif  // DFTRACER_UTILS_PYTHON_JSON_H
