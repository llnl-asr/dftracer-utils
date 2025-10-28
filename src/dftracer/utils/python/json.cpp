#include <dftracer/utils/python/json.h>

#include <cstring>
#include <iostream>

static void JSON_dealloc(JSONObject* self) {
    if (self->doc && self->owns_doc) {
        yyjson_doc_free(self->doc);
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* JSON_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    JSONObject* self;
    self = (JSONObject*)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->doc = nullptr;
        self->root = nullptr;
        self->parsed = false;
        self->json_length = 0;
        self->owns_doc = false;
    }
    return (PyObject*)self;
}

static int JSON_init(JSONObject* self, PyObject* args, PyObject* kwds) {
    const char* json_str;
    if (!PyArg_ParseTuple(args, "s", &json_str)) {
        return -1;
    }

    self->json_length = strlen(json_str);
    if (self->json_length > 0) {
        std::memcpy(self->json_data, json_str, self->json_length);
    }
    self->doc = nullptr;
    self->root = nullptr;
    self->parsed = false;
    self->owns_doc = true;
    return 0;
}

static bool JSON_ensure_parsed(JSONObject* self) {
    if (self->root != nullptr) {
        return true;
    }

    if (self->parsed && self->doc != nullptr) {
        return true;
    }

    if (!self->parsed && self->json_length > 0) {
        // Use YYJSON_READ_INSITU for large-scale processing
        // (zero-copy, in-place modification)
        yyjson_read_err err;
        self->doc = yyjson_read_opts(self->json_data, self->json_length,
                                     YYJSON_READ_INSITU, NULL, &err);
        if (!self->doc) {
            char err_msg[256];
            std::snprintf(err_msg, sizeof(err_msg),
                          "Failed to parse JSON at position %zu: %s (code %u, "
                          "string: %.*s)",
                          err.pos, err.msg, err.code, (int)self->json_length,
                          self->json_data);
            PyErr_SetString(PyExc_ValueError, err_msg);
            return false;
        }
        self->parsed = true;
        return true;
    }

    // If we get here, there's no data to parse
    return false;
}

// Get the root yyjson_val for this JSON object (handles both top-level and
// subtrees)
static yyjson_val* JSON_get_root(JSONObject* self) {
    if (self->root != nullptr) {
        // This is a subtree wrapper - return the wrapped value directly
        return self->root;
    }
    // This is a top-level document - get the root from the doc
    return yyjson_doc_get_root(self->doc);
}

static PyObject* JSON_contains(JSONObject* self, PyObject* key) {
    if (!JSON_ensure_parsed(self)) {
        return NULL;
    }

    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "Key must be a string");
        return NULL;
    }

    const char* key_str = PyUnicode_AsUTF8(key);
    if (!key_str) {
        return NULL;
    }

    yyjson_val* root = JSON_get_root(self);
    if (!yyjson_is_obj(root)) {
        Py_RETURN_FALSE;
    }

    yyjson_val* val = yyjson_obj_get(root, key_str);
    if (val) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static int JSON_contains_sq(PyObject* self_obj, PyObject* key) {
    JSONObject* self = (JSONObject*)self_obj;
    PyObject* result = JSON_contains(self, key);
    if (!result) {
        return -1;
    }

    int is_true = PyObject_IsTrue(result);
    Py_DECREF(result);
    return is_true;
}

static PyObject* yyjson_val_to_python(yyjson_val* val) {
    if (yyjson_is_null(val)) {
        Py_RETURN_NONE;
    } else if (yyjson_is_bool(val)) {
        if (yyjson_get_bool(val)) {
            Py_RETURN_TRUE;
        } else {
            Py_RETURN_FALSE;
        }
    } else if (yyjson_is_uint(val)) {
        return PyLong_FromUnsignedLongLong(yyjson_get_uint(val));
    } else if (yyjson_is_int(val)) {
        return PyLong_FromLongLong(yyjson_get_int(val));
    } else if (yyjson_is_real(val)) {
        return PyFloat_FromDouble(yyjson_get_real(val));
    } else if (yyjson_is_str(val)) {
        return PyUnicode_FromString(yyjson_get_str(val));
    } else if (yyjson_is_arr(val)) {
        std::size_t idx, max;
        yyjson_val* item;
        PyObject* list = PyList_New(0);
        if (!list) return NULL;

        yyjson_arr_foreach(val, idx, max, item) {
            PyObject* py_item = yyjson_val_to_python(item);
            if (!py_item) {
                Py_DECREF(list);
                return NULL;
            }
            if (PyList_Append(list, py_item) < 0) {
                Py_DECREF(py_item);
                Py_DECREF(list);
                return NULL;
            }
            Py_DECREF(py_item);
        }
        return list;
    } else if (yyjson_is_obj(val)) {
        std::size_t idx, max;
        yyjson_val *key_val, *val_val;
        PyObject* dict = PyDict_New();
        if (!dict) return NULL;

        yyjson_obj_foreach(val, idx, max, key_val, val_val) {
            const char* key_str = yyjson_get_str(key_val);
            PyObject* py_key = PyUnicode_FromString(key_str);
            PyObject* py_val = yyjson_val_to_python(val_val);

            if (!py_key || !py_val) {
                Py_XDECREF(py_key);
                Py_XDECREF(py_val);
                Py_DECREF(dict);
                return NULL;
            }

            if (PyDict_SetItem(dict, py_key, py_val) < 0) {
                Py_DECREF(py_key);
                Py_DECREF(py_val);
                Py_DECREF(dict);
                return NULL;
            }

            Py_DECREF(py_key);
            Py_DECREF(py_val);
        }
        return dict;
    }

    Py_RETURN_NONE;
}

static PyObject* JSON_getitem(JSONObject* self, PyObject* key) {
    if (!JSON_ensure_parsed(self)) {
        return NULL;
    }

    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "Key must be a string");
        return NULL;
    }

    const char* key_str = PyUnicode_AsUTF8(key);
    if (!key_str) {
        return NULL;
    }

    yyjson_val* root = JSON_get_root(self);
    if (!yyjson_is_obj(root)) {
        PyErr_SetString(PyExc_TypeError, "JSON root is not an object");
        return NULL;
    }

    yyjson_val* val = yyjson_obj_get(root, key_str);
    if (!val) {
        PyErr_SetString(PyExc_KeyError, key_str);
        return NULL;
    }

    // If the value is an object or array, return a lazy wrapper
    if (yyjson_is_obj(val) || yyjson_is_arr(val)) {
        return JSON_from_yyjson_val(self->doc, val);
    }

    return yyjson_val_to_python(val);
}

static PyObject* JSON_keys(JSONObject* self, PyObject* Py_UNUSED(ignored)) {
    if (!JSON_ensure_parsed(self)) {
        return NULL;
    }

    yyjson_val* root = JSON_get_root(self);
    if (!yyjson_is_obj(root)) {
        return PyList_New(0);
    }

    PyObject* keys = PyList_New(0);
    if (!keys) return NULL;

    std::size_t idx, max;
    yyjson_val *key_val, *val_val;
    yyjson_obj_foreach(root, idx, max, key_val, val_val) {
        const char* key_str = yyjson_get_str(key_val);
        PyObject* py_key = PyUnicode_FromString(key_str);
        if (!py_key) {
            Py_DECREF(keys);
            return NULL;
        }
        if (PyList_Append(keys, py_key) < 0) {
            Py_DECREF(py_key);
            Py_DECREF(keys);
            return NULL;
        }
        Py_DECREF(py_key);
    }

    return keys;
}

static PyObject* JSON_get(JSONObject* self, PyObject* args) {
    PyObject* key;
    PyObject* default_value = Py_None;

    if (!PyArg_ParseTuple(args, "O|O", &key, &default_value)) {
        return NULL;
    }

    if (!JSON_ensure_parsed(self)) {
        return NULL;
    }

    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "Key must be a string");
        return NULL;
    }

    const char* key_str = PyUnicode_AsUTF8(key);
    if (!key_str) {
        return NULL;
    }

    yyjson_val* root = JSON_get_root(self);
    if (!yyjson_is_obj(root)) {
        Py_INCREF(default_value);
        return default_value;
    }

    yyjson_val* val = yyjson_obj_get(root, key_str);
    if (!val) {
        Py_INCREF(default_value);
        return default_value;
    }

    // If the value is an object or array, return a lazy wrapper
    if (yyjson_is_obj(val) || yyjson_is_arr(val)) {
        return JSON_from_yyjson_val(self->doc, val);
    }

    return yyjson_val_to_python(val);
}

static PyObject* JSON_iter(JSONObject* self) {
    if (!JSON_ensure_parsed(self)) {
        return NULL;
    }

    yyjson_val* root = yyjson_doc_get_root(self->doc);
    if (!yyjson_is_obj(root)) {
        return PyObject_GetIter(PyList_New(0));
    }

    return PyObject_GetIter(JSON_keys(self, NULL));
}

static PyObject* JSON_str(JSONObject* self) {
    if (self->root != nullptr) {
        char* json_str = yyjson_val_write(self->root, 0, NULL);
        if (!json_str) {
            return PyUnicode_FromString("{}");
        }
        PyObject* result = PyUnicode_FromString(json_str);
        free(json_str);
        return result;
    }
    if (self->json_length > 0) {
        return PyUnicode_FromStringAndSize(self->json_data, self->json_length);
    }
    return PyUnicode_FromString("{}");
}

static PyObject* JSON_repr(JSONObject* self) {
    PyObject* str_obj = JSON_str(self);
    if (!str_obj) return NULL;
    PyObject* result = PyUnicode_FromFormat("JSON(%U)", str_obj);
    Py_DECREF(str_obj);
    return result;
}

PyMethodDef JSON_methods[] = {{"__contains__", (PyCFunction)JSON_contains,
                               METH_O, "Check if key exists in JSON object"},
                              {"keys", (PyCFunction)JSON_keys, METH_NOARGS,
                               "Get all keys from JSON object"},
                              {"get", (PyCFunction)JSON_get, METH_VARARGS,
                               "Get value by key with optional default"},
                              {NULL}};

PySequenceMethods JSON_as_sequence = {
    .sq_contains = JSON_contains_sq,
};

PyMappingMethods JSON_as_mapping = {
    .mp_subscript = (binaryfunc)JSON_getitem,
};

PyTypeObject JSONType = {
    PyVarObject_HEAD_INIT(NULL, 0) "json.JSON", /* tp_name */
    sizeof(JSONObject),                         /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)JSON_dealloc,                   /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)JSON_repr,                        /* tp_repr */
    0,                                          /* tp_as_number */
    &JSON_as_sequence,                          /* tp_as_sequence */
    &JSON_as_mapping,                           /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    (reprfunc)JSON_str,                         /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    "Lazy JSON object that parses on demand",   /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)JSON_iter,                     /* tp_iter */
    0,                                          /* tp_iternext */
    JSON_methods,                               /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)JSON_init,                        /* tp_init */
    0,                                          /* tp_alloc */
    JSON_new,                                   /* tp_new */
};

int init_json(PyObject* m) {
    if (PyType_Ready(&JSONType) < 0) return -1;

    Py_INCREF(&JSONType);
    if (PyModule_AddObject(m, "JSON", (PyObject*)&JSONType) < 0) {
        Py_DECREF(&JSONType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}

PyObject* JSON_from_data(const char* data, size_t length) {
    JSONObject* self =
        (JSONObject*)PyObject_MALLOC(sizeof(JSONObject) + length + 1);
    if (!self) {
        return PyErr_NoMemory();
    }

    PyObject_INIT(self, &JSONType);

    self->doc = nullptr;
    self->root = nullptr;
    self->parsed = false;
    self->json_length = length;
    self->owns_doc = true;

    std::memcpy(self->json_data, data, length);
    self->json_data[length] = '\0';

    return (PyObject*)self;
}

// Create a JSON object wrapping a yyjson_val
// subtree (lazy wrapper for nested objects/arrays)
PyObject* JSON_from_yyjson_val(yyjson_doc* doc, yyjson_val* root) {
    JSONObject* self = (JSONObject*)PyObject_MALLOC(sizeof(JSONObject));
    if (!self) {
        return PyErr_NoMemory();
    }

    PyObject_INIT(self, &JSONType);

    self->doc = doc;         // Share the document (don't copy)
    self->root = root;       // Point to the subtree
    self->parsed = true;     // Already parsed (just wrapping a subtree)
    self->json_length = 0;   // No raw JSON data
    self->owns_doc = false;  // Don't free the doc (it's owned by parent)

    return (PyObject*)self;
}
