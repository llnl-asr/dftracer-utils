#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/json.h>

using dftracer::utils::utilities::composites::dft::ArgsValueProxy;

PyObject *args_value_to_pyobject(const ArgsValue &v) {
    return std::visit(
        [](const auto &val) -> PyObject * {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                Py_RETURN_NONE;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return PyUnicode_FromStringAndSize(val.data(), val.size());
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return PyLong_FromUnsignedLongLong(val);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return PyLong_FromLongLong(val);
            } else if constexpr (std::is_same_v<T, double>) {
                return PyFloat_FromDouble(val);
            } else if constexpr (std::is_same_v<T, bool>) {
                return PyBool_FromLong(val ? 1 : 0);
            } else {
                Py_RETURN_NONE;
            }
        },
        v);
}

static const ArgsMap &get_map(JsonDictValueObject *self) {
    auto &ev = self->batch->events[self->event_index];
    return self->is_args ? ev.args : ev.top;
}

static void JsonDictValue_dealloc(JsonDictValueObject *self) {
    self->batch.reset();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static Py_ssize_t JsonDictValue_length(JsonDictValueObject *self) {
    const auto &map = get_map(self);
    Py_ssize_t count = 0;
    map.for_each_member([&](std::string_view, ArgsValueProxy) { ++count; });
    if (!self->is_args && get_map(self).exists()) {
        auto &ev = self->batch->events[self->event_index];
        if (ev.args.exists()) ++count;
    }
    return count;
}

static PyObject *JsonDictValue_subscript(JsonDictValueObject *self,
                                         PyObject *key) {
    const char *key_str = PyUnicode_AsUTF8(key);
    if (!key_str) return NULL;

    std::string_view k(key_str);

    if (!self->is_args && k == "args") {
        auto &ev = self->batch->events[self->event_index];
        if (!ev.args.exists()) {
            Py_RETURN_NONE;
        }
        JsonDictValueObject *obj =
            (JsonDictValueObject *)JsonDictValueType.tp_alloc(
                &JsonDictValueType, 0);
        if (!obj) return NULL;
        new (&obj->batch) std::shared_ptr<JsonDictBatch>(self->batch);
        obj->event_index = self->event_index;
        obj->is_args = true;
        return (PyObject *)obj;
    }

    const auto &map = get_map(self);
    auto proxy = map[k];
    if (!proxy.exists()) {
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }

    const auto &raw = map.raw();
    auto it = raw.find(k);
    if (it == raw.end()) {
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    return args_value_to_pyobject(it->second);
}

static PyObject *JsonDictValue_keys(JsonDictValueObject *self,
                                    PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    const auto &map = get_map(self);
    map.for_each_member([&](std::string_view k, ArgsValueProxy) {
        PyObject *key = PyUnicode_FromStringAndSize(k.data(), k.size());
        if (key) {
            PyList_Append(list, key);
            Py_DECREF(key);
        }
    });

    if (!self->is_args) {
        auto &ev = self->batch->events[self->event_index];
        if (ev.args.exists()) {
            PyObject *args_key = PyUnicode_InternFromString("args");
            if (args_key) {
                PyList_Append(list, args_key);
                Py_DECREF(args_key);
            }
        }
    }

    return list;
}

static PyObject *JsonDictValue_values(JsonDictValueObject *self,
                                      PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    const auto &map = get_map(self);
    for (const auto &[k, v] : map.raw()) {
        PyObject *val = args_value_to_pyobject(v);
        if (val) {
            PyList_Append(list, val);
            Py_DECREF(val);
        }
    }

    if (!self->is_args) {
        auto &ev = self->batch->events[self->event_index];
        if (ev.args.exists()) {
            JsonDictValueObject *args_obj =
                (JsonDictValueObject *)JsonDictValueType.tp_alloc(
                    &JsonDictValueType, 0);
            if (args_obj) {
                new (&args_obj->batch)
                    std::shared_ptr<JsonDictBatch>(self->batch);
                args_obj->event_index = self->event_index;
                args_obj->is_args = true;
                PyList_Append(list, (PyObject *)args_obj);
                Py_DECREF(args_obj);
            }
        }
    }

    return list;
}

static PyObject *JsonDictValue_items(JsonDictValueObject *self,
                                     PyObject *Py_UNUSED(ignored)) {
    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    const auto &map = get_map(self);
    for (const auto &[k, v] : map.raw()) {
        PyObject *key = PyUnicode_FromStringAndSize(k.data(), k.size());
        PyObject *val = args_value_to_pyobject(v);
        if (key && val) {
            PyObject *tuple = PyTuple_Pack(2, key, val);
            if (tuple) {
                PyList_Append(list, tuple);
                Py_DECREF(tuple);
            }
        }
        Py_XDECREF(key);
        Py_XDECREF(val);
    }

    if (!self->is_args) {
        auto &ev = self->batch->events[self->event_index];
        if (ev.args.exists()) {
            PyObject *args_key = PyUnicode_InternFromString("args");
            JsonDictValueObject *args_obj =
                (JsonDictValueObject *)JsonDictValueType.tp_alloc(
                    &JsonDictValueType, 0);
            if (args_key && args_obj) {
                new (&args_obj->batch)
                    std::shared_ptr<JsonDictBatch>(self->batch);
                args_obj->event_index = self->event_index;
                args_obj->is_args = true;
                PyObject *tuple =
                    PyTuple_Pack(2, args_key, (PyObject *)args_obj);
                if (tuple) {
                    PyList_Append(list, tuple);
                    Py_DECREF(tuple);
                }
            }
            Py_XDECREF(args_key);
            Py_XDECREF((PyObject *)args_obj);
        }
    }

    return list;
}

static PyObject *JsonDictValue_get(JsonDictValueObject *self, PyObject *args) {
    PyObject *key;
    PyObject *default_val = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &default_val)) return NULL;

    const char *key_str = PyUnicode_AsUTF8(key);
    if (!key_str) return NULL;

    std::string_view k(key_str);

    if (!self->is_args && k == "args") {
        auto &ev = self->batch->events[self->event_index];
        if (!ev.args.exists()) {
            Py_INCREF(default_val);
            return default_val;
        }
        JsonDictValueObject *obj =
            (JsonDictValueObject *)JsonDictValueType.tp_alloc(
                &JsonDictValueType, 0);
        if (!obj) return NULL;
        new (&obj->batch) std::shared_ptr<JsonDictBatch>(self->batch);
        obj->event_index = self->event_index;
        obj->is_args = true;
        return (PyObject *)obj;
    }

    const auto &map = get_map(self);
    auto it = map.raw().find(k);
    if (it == map.raw().end()) {
        Py_INCREF(default_val);
        return default_val;
    }
    return args_value_to_pyobject(it->second);
}

static int JsonDictValue_contains(JsonDictValueObject *self, PyObject *key) {
    const char *key_str = PyUnicode_AsUTF8(key);
    if (!key_str) return -1;

    std::string_view k(key_str);

    if (!self->is_args && k == "args") {
        auto &ev = self->batch->events[self->event_index];
        return ev.args.exists() ? 1 : 0;
    }

    const auto &map = get_map(self);
    return map[k].exists() ? 1 : 0;
}

static PyObject *JsonDictValue_to_dict(JsonDictValueObject *self,
                                       PyObject *Py_UNUSED(ignored)) {
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    const auto &map = get_map(self);
    for (const auto &[k, v] : map.raw()) {
        PyObject *key = PyUnicode_FromStringAndSize(k.data(), k.size());
        PyObject *val = args_value_to_pyobject(v);
        if (!key || !val) {
            Py_XDECREF(key);
            Py_XDECREF(val);
            Py_DECREF(dict);
            return NULL;
        }
        PyDict_SetItem(dict, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
    }

    if (!self->is_args) {
        auto &ev = self->batch->events[self->event_index];
        if (ev.args.exists()) {
            PyObject *args_dict = PyDict_New();
            if (!args_dict) {
                Py_DECREF(dict);
                return NULL;
            }
            for (const auto &[k, v] : ev.args.raw()) {
                PyObject *key = PyUnicode_FromStringAndSize(k.data(), k.size());
                PyObject *val = args_value_to_pyobject(v);
                if (!key || !val) {
                    Py_XDECREF(key);
                    Py_XDECREF(val);
                    Py_DECREF(args_dict);
                    Py_DECREF(dict);
                    return NULL;
                }
                PyDict_SetItem(args_dict, key, val);
                Py_DECREF(key);
                Py_DECREF(val);
            }
            PyDict_SetItemString(dict, "args", args_dict);
            Py_DECREF(args_dict);
        }
    }

    return dict;
}

static PyMappingMethods JsonDictValue_as_mapping = {
    (lenfunc)JsonDictValue_length,
    (binaryfunc)JsonDictValue_subscript,
    NULL,
};

static PySequenceMethods JsonDictValue_as_sequence = {
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, (objobjproc)JsonDictValue_contains,
    NULL, NULL,
};

static PyMethodDef JsonDictValue_methods[] = {
    {"keys", (PyCFunction)JsonDictValue_keys, METH_NOARGS,
     "Return list of keys."},
    {"values", (PyCFunction)JsonDictValue_values, METH_NOARGS,
     "Return list of values."},
    {"items", (PyCFunction)JsonDictValue_items, METH_NOARGS,
     "Return list of (key, value) pairs."},
    {"get", (PyCFunction)JsonDictValue_get, METH_VARARGS,
     "Get value by key with optional default."},
    {"to_dict", (PyCFunction)JsonDictValue_to_dict, METH_NOARGS,
     "Convert to a regular Python dict."},
    {NULL}};

PyTypeObject JsonDictValueType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.JsonDictValue",
    sizeof(JsonDictValueObject),       /* tp_basicsize */
    0,                                 /* tp_itemsize */
    (destructor)JsonDictValue_dealloc, /* tp_dealloc */
    0,                                 /* tp_vectorcall_offset */
    0,                                 /* tp_getattr */
    0,                                 /* tp_setattr */
    0,                                 /* tp_as_async */
    0,                                 /* tp_repr */
    0,                                 /* tp_as_number */
    &JsonDictValue_as_sequence,        /* tp_as_sequence */
    &JsonDictValue_as_mapping,         /* tp_as_mapping */
    0,                                 /* tp_hash */
    0,                                 /* tp_call */
    0,                                 /* tp_str */
    0,                                 /* tp_getattro */
    0,                                 /* tp_setattro */
    0,                                 /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                /* tp_flags */
    "Zero-copy wrapper over a parsed DFTracer JSON event.\n"
    "Supports dict-like access: event['name'], event['args']['ret'].\n"
    "Call .to_dict() to materialize a regular Python dict.",
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    JsonDictValue_methods, /* tp_methods */
};

int init_json_dict_value(PyObject *m) {
    if (PyType_Ready(&JsonDictValueType) < 0) return -1;
    Py_INCREF(&JsonDictValueType);
    if (PyModule_AddObject(m, "JsonDictValue", (PyObject *)&JsonDictValueType) <
        0) {
        Py_DECREF(&JsonDictValueType);
        return -1;
    }
    return 0;
}
