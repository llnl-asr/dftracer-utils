// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/formats/emit.h>
#include <dftracer/utils/python/formats/mutable_value.h>
#include <dftracer/utils/python/formats/value.h>

#include <cstring>
#include <type_traits>

namespace dftracer::utils::python::formats {

namespace {

PyObject *g_mapping_abc = nullptr;
PyObject *g_sequence_abc = nullptr;

template <class F>
PyTypeObject g_mutable_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
dstd::String g_mutable_name;

template <class F>
MutableObject<F> *as_mutable(PyObject *o) {
    return reinterpret_cast<MutableObject<F> *>(o);
}

bool is_text_like(PyObject *o) {
    return PyUnicode_Check(o) || PyBytes_Check(o) || PyByteArray_Check(o);
}

// 1 for a Mapping that is not a dict, 0 for something else, -1 on error.
int is_abc(PyObject *o, PyObject *abc) { return PyObject_IsInstance(o, abc); }

template <class F>
PyObject *base_owner(MutableObject<F> *self) {
    return reinterpret_cast<ValueObject<F> *>(self->base)->owner;
}

template <class F>
const typename F::Val &base_val(MutableObject<F> *self) {
    return reinterpret_cast<ValueObject<F> *>(self->base)->val;
}

template <class F>
PyObject *new_mutable(int mode, PyObject *base, PyObject *entries,
                      PyObject *scalar) {
    PyTypeObject *type = &g_mutable_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    auto *object = as_mutable<F>(self);
    object->mode = mode;
    Py_XINCREF(base);
    object->base = base;
    Py_XINCREF(entries);
    object->entries = entries;
    Py_XINCREF(scalar);
    object->scalar = scalar;
    object->dirty = base == nullptr;
    return self;
}

template <class F>
void Mutable_dealloc(PyObject *self) {
    auto *object = as_mutable<F>(self);
    Py_XDECREF(object->base);
    Py_XDECREF(object->entries);
    Py_XDECREF(object->scalar);
    Py_TYPE(self)->tp_free(self);
}

// The dict or list of a container, made from its base on the first use.
template <class F>
bool materialize(MutableObject<F> *self) {
    if (self->entries) return true;
    if (self->mode == MutableObject<F>::Scalar) {
        PyErr_SetString(PyExc_TypeError, "a scalar has no members");
        return false;
    }
    const bool is_object = self->mode == MutableObject<F>::Object;
    PyObject *entries = is_object ? PyDict_New() : PyList_New(0);
    if (!entries) return false;
    if (self->base) {
        PyObject *owner = base_owner<F>(self);
        bool failed = false;
        if (is_object) {
            F::members(base_val<F>(self), [&](dstd::StringView name,
                                              typename F::Val member) {
                if (failed) return;
                PyObject *key = make_str(name);
                PyObject *value = key ? make_value<F>(owner, member) : nullptr;
                // A name that repeats keeps the first, as find does.
                if (!value || (PyDict_Contains(entries, key) == 0 &&
                               PyDict_SetItem(entries, key, value) < 0))
                    failed = true;
                Py_XDECREF(key);
                Py_XDECREF(value);
            });
        } else {
            F::elements(base_val<F>(self), [&](typename F::Val element) {
                if (failed) return;
                PyObject *value = make_value<F>(owner, element);
                if (!value || PyList_Append(entries, value) < 0) failed = true;
                Py_XDECREF(value);
            });
        }
        if (failed) {
            Py_DECREF(entries);
            return false;
        }
    }
    self->entries = entries;
    return true;
}

template <class F>
PyObject *from_input(PyObject *entry);

// A new MutableValue for what an entry holds.
template <class F>
PyObject *wrap_entry(PyObject *entry) {
    if (PyObject_TypeCheck(entry, &g_mutable_type<F>)) {
        Py_INCREF(entry);
        return entry;
    }
    if (value_of<F>(entry)) return make_mutable<F>(entry);
    return from_input<F>(entry);
}

template <class F>
PyObject *from_input(PyObject *entry) {
    PyObject *data = nullptr;
    PyObject *plain = entry;
    if (PyObject_TypeCheck(entry, value_type<JsonFormat>()) ||
        PyObject_TypeCheck(entry, value_type<YamlFormat>()) ||
        PyObject_TypeCheck(entry, mutable_type<JsonFormat>()) ||
        PyObject_TypeCheck(entry, mutable_type<YamlFormat>())) {
        data = data_of(entry, 0);
        if (!data) return nullptr;
        plain = data;
    }
    PyObject *result = nullptr;
    int mapping = 0;
    int sequence = 0;
    if (!PyDict_Check(plain) && !PyList_Check(plain) && !PyTuple_Check(plain) &&
        !is_text_like(plain)) {
        mapping = is_abc(plain, g_mapping_abc);
        if (mapping == 0) sequence = is_abc(plain, g_sequence_abc);
        if (mapping < 0 || sequence < 0) {
            Py_XDECREF(data);
            return nullptr;
        }
    }
    if (PyDict_Check(plain) || mapping) {
        PyObject *copy = PyDict_New();
        if (copy && PyDict_Update(copy, plain) == 0)
            result = new_mutable<F>(MutableObject<F>::Object, nullptr, copy,
                                    nullptr);
        Py_XDECREF(copy);
    } else if (PyList_Check(plain) || PyTuple_Check(plain) || sequence) {
        PyObject *copy = PySequence_List(plain);
        if (copy)
            result =
                new_mutable<F>(MutableObject<F>::Array, nullptr, copy, nullptr);
        Py_XDECREF(copy);
    } else {
        result =
            new_mutable<F>(MutableObject<F>::Scalar, nullptr, nullptr, plain);
    }
    Py_XDECREF(data);
    return result;
}

bool text_of(PyObject *key, dstd::StringView &out) {
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(key, &size);
    if (!text) return false;
    out = dstd::StringView(text, static_cast<std::size_t>(size));
    return true;
}

// The position of an int key in an array of `size` entries, or -1 with the
// Python error set.
Py_ssize_t array_index(PyObject *key, Py_ssize_t size) {
    if (!PyLong_Check(key) || PyBool_Check(key)) {
        PyErr_Format(PyExc_TypeError, "an array index must be int, not %.200s",
                     Py_TYPE(key)->tp_name);
        return -1;
    }
    Py_ssize_t index = PyLong_AsSsize_t(key);
    if (index == -1 && PyErr_Occurred()) return -1;
    if (index < 0) index += size;
    if (index < 0 || index >= size) {
        PyErr_SetString(PyExc_IndexError, "array index out of range");
        return -1;
    }
    return index;
}

template <class F>
PyObject *Mutable_getitem(PyObject *py_self, PyObject *key) {
    auto *self = as_mutable<F>(py_self);
    if (!materialize<F>(self)) return nullptr;
    PyObject *entry = nullptr;
    Py_ssize_t index = 0;
    if (self->mode == MutableObject<F>::Object) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "an object key must be str");
            return nullptr;
        }
        entry = PyDict_GetItemWithError(self->entries, key);
        if (!entry) {
            if (!PyErr_Occurred()) PyErr_SetObject(PyExc_KeyError, key);
            return nullptr;
        }
    } else {
        index = array_index(key, PyList_GET_SIZE(self->entries));
        if (index < 0) return nullptr;
        entry = PyList_GET_ITEM(self->entries, index);
    }
    PyObject *wrapped = wrap_entry<F>(entry);
    if (!wrapped || wrapped == entry) return wrapped;
    int status;
    if (self->mode == MutableObject<F>::Object) {
        status = PyDict_SetItem(self->entries, key, wrapped);
    } else {
        Py_INCREF(wrapped);
        status = PyList_SetItem(self->entries, index, wrapped);
    }
    if (status < 0) {
        Py_DECREF(wrapped);
        return nullptr;
    }
    return wrapped;
}

template <class F>
int Mutable_assign(PyObject *py_self, PyObject *key, PyObject *value) {
    auto *self = as_mutable<F>(py_self);
    if (!materialize<F>(self)) return -1;
    self->dirty = true;
    if (self->mode == MutableObject<F>::Object) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "an object key must be str");
            return -1;
        }
        if (!value) {
            if (PyDict_DelItem(self->entries, key) < 0) return -1;
            return 0;
        }
        return PyDict_SetItem(self->entries, key, value);
    }
    const Py_ssize_t index = array_index(key, PyList_GET_SIZE(self->entries));
    if (index < 0) return -1;
    if (!value)
        return PyList_SetSlice(self->entries, index, index + 1, nullptr);
    Py_INCREF(value);
    return PyList_SetItem(self->entries, index, value);
}

template <class F>
PyObject *Mutable_append(PyObject *py_self, PyObject *value) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode != MutableObject<F>::Array) {
        PyErr_SetString(PyExc_TypeError, "only an array has append");
        return nullptr;
    }
    if (!materialize<F>(self) || PyList_Append(self->entries, value) < 0)
        return nullptr;
    self->dirty = true;
    Py_RETURN_NONE;
}

template <class F>
PyObject *Mutable_get(PyObject *py_self, PyObject *key) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode != MutableObject<F>::Object || !PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError,
                        "get takes a str key and only an object has members");
        return nullptr;
    }
    if (!materialize<F>(self)) return nullptr;
    const int found = PyDict_Contains(self->entries, key);
    if (found < 0) return nullptr;
    if (!found) Py_RETURN_NONE;
    return Mutable_getitem<F>(py_self, key);
}

template <class F>
PyObject *Mutable_items(PyObject *py_self, PyObject *) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode != MutableObject<F>::Object) {
        PyErr_SetString(PyExc_TypeError, "only an object has items");
        return nullptr;
    }
    if (!materialize<F>(self)) return nullptr;
    PyObject *keys = PyDict_Keys(self->entries);
    if (!keys) return nullptr;
    PyObject *list = PyList_New(0);
    for (Py_ssize_t i = 0; list && i < PyList_GET_SIZE(keys); ++i) {
        PyObject *key = PyList_GET_ITEM(keys, i);
        PyObject *value = Mutable_getitem<F>(py_self, key);
        PyObject *pair = value ? PyTuple_Pack(2, key, value) : nullptr;
        Py_XDECREF(value);
        if (!pair || PyList_Append(list, pair) < 0) {
            Py_CLEAR(list);
        }
        Py_XDECREF(pair);
    }
    Py_DECREF(keys);
    if (!list) return nullptr;
    PyObject *it = PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

template <class F>
PyObject *Mutable_iter(PyObject *py_self) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode != MutableObject<F>::Array) {
        PyErr_SetString(PyExc_TypeError,
                        "only an array is iterated; use items() for an object");
        return nullptr;
    }
    if (!materialize<F>(self)) return nullptr;
    PyObject *list = PyList_New(0);
    for (Py_ssize_t i = 0; list && i < PyList_GET_SIZE(self->entries); ++i) {
        PyObject *index = PyLong_FromSsize_t(i);
        PyObject *item = index ? Mutable_getitem<F>(py_self, index) : nullptr;
        Py_XDECREF(index);
        if (!item || PyList_Append(list, item) < 0) {
            Py_CLEAR(list);
        }
        Py_XDECREF(item);
    }
    if (!list) return nullptr;
    PyObject *it = PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

template <class F>
Py_ssize_t Mutable_length(PyObject *py_self) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode == MutableObject<F>::Scalar) {
        PyErr_SetString(PyExc_TypeError, "a scalar has no length");
        return -1;
    }
    if (self->entries) return PyObject_Length(self->entries);
    return PyObject_Length(self->base);
}

template <class F>
int Mutable_bool(PyObject *py_self) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode == MutableObject<F>::Scalar) {
        if (self->scalar) return PyObject_IsTrue(self->scalar);
        return PyObject_IsTrue(self->base);
    }
    const Py_ssize_t size = Mutable_length<F>(py_self);
    return size < 0 ? -1 : size > 0;
}

// as_int, as_float, as_bool and as_str of a scalar: what was set is checked
// here, and the base Value answers for what was not.
template <class F>
PyObject *scalar_as(PyObject *py_self, const char *method) {
    auto *self = as_mutable<F>(py_self);
    if (self->mode != MutableObject<F>::Scalar) {
        PyErr_Format(PyExc_TypeError, "%s needs a scalar", method);
        return nullptr;
    }
    if (!self->scalar) return PyObject_CallMethod(self->base, method, nullptr);
    PyObject *s = self->scalar;
    const dstd::StringView name(method);
    if (name == "as_int" && PyLong_Check(s) && !PyBool_Check(s)) {
        Py_INCREF(s);
        return s;
    }
    if (name == "as_float" &&
        (PyFloat_Check(s) || (PyLong_Check(s) && !PyBool_Check(s)))) {
        return PyNumber_Float(s);
    }
    if (name == "as_bool" && PyBool_Check(s)) {
        Py_INCREF(s);
        return s;
    }
    if (name == "as_str" && PyUnicode_Check(s)) {
        Py_INCREF(s);
        return s;
    }
    PyErr_Format(PyExc_TypeError, "%s of a %.200s", method,
                 Py_TYPE(s)->tp_name);
    return nullptr;
}

template <class F>
PyObject *Mutable_as_int(PyObject *self, PyObject *) {
    return scalar_as<F>(self, "as_int");
}
template <class F>
PyObject *Mutable_as_float(PyObject *self, PyObject *) {
    return scalar_as<F>(self, "as_float");
}
template <class F>
PyObject *Mutable_as_bool(PyObject *self, PyObject *) {
    return scalar_as<F>(self, "as_bool");
}
template <class F>
PyObject *Mutable_as_str(PyObject *self, PyObject *) {
    return scalar_as<F>(self, "as_str");
}
template <class F>
PyObject *Mutable_to_python(PyObject *self, PyObject *) {
    return data_of(self, 0);
}

template <class F>
PyObject *Mutable_repr(PyObject *self) {
    const char *mode[] = {"scalar", "object", "array"};
    return PyUnicode_FromFormat("<%s %s>", Py_TYPE(self)->tp_name,
                                mode[as_mutable<F>(self)->mode]);
}

template <class F>
PyMethodDef g_mutable_methods[] = {
    {"append", DFTU_PYCFUNCTION(Mutable_append<F>), METH_O,
     "append(value) -> None\n\nAdd an element at the end of an array."},
    {"get", DFTU_PYCFUNCTION(Mutable_get<F>), METH_O,
     "get(key) -> MutableValue | None\n\nThe member of an object, or None."},
    {"items", DFTU_PYCFUNCTION(Mutable_items<F>), METH_NOARGS,
     "items() -> Iterator[tuple[str, MutableValue]]"},
    {"as_int", DFTU_PYCFUNCTION(Mutable_as_int<F>), METH_NOARGS,
     "as_int() -> int"},
    {"as_float", DFTU_PYCFUNCTION(Mutable_as_float<F>), METH_NOARGS,
     "as_float() -> float"},
    {"as_bool", DFTU_PYCFUNCTION(Mutable_as_bool<F>), METH_NOARGS,
     "as_bool() -> bool"},
    {"as_str", DFTU_PYCFUNCTION(Mutable_as_str<F>), METH_NOARGS,
     "as_str() -> str"},
    {"to_python", DFTU_PYCFUNCTION(Mutable_to_python<F>), METH_NOARGS,
     "to_python() -> Data\n\nThe plain data with the changes applied."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
PyMappingMethods g_mutable_mapping = {Mutable_length<F>, Mutable_getitem<F>,
                                      Mutable_assign<F>};
template <class F>
PyNumberMethods g_mutable_number = {};

// True when nothing was set, deleted or appended in the value or below it.
template <class S>
bool is_clean(MutableObject<S> *self) {
    if (self->dirty || !self->base) return false;
    if (!self->entries) return true;
    PyObject *item = nullptr;
    if (self->mode == MutableObject<S>::Object) {
        PyObject *key = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(self->entries, &pos, &key, &item))
            if (PyObject_TypeCheck(item, &g_mutable_type<S>) &&
                !is_clean<S>(as_mutable<S>(item)))
                return false;
        return true;
    }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(self->entries); ++i) {
        item = PyList_GET_ITEM(self->entries, i);
        if (PyObject_TypeCheck(item, &g_mutable_type<S>) &&
            !is_clean<S>(as_mutable<S>(item)))
            return false;
    }
    return true;
}

template <class S>
PyObject *mutable_data(MutableObject<S> *self, int depth) {
    if (!self->entries) {
        if (self->mode == MutableObject<S>::Scalar && self->scalar)
            return data_of(self->scalar, depth + 1);
        return to_python<S>(base_val<S>(self), depth + 1);
    }
    if (self->mode == MutableObject<S>::Object) {
        PyObject *dict = PyDict_New();
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t pos = 0;
        while (dict && PyDict_Next(self->entries, &pos, &key, &item)) {
            PyObject *value = data_of(item, depth + 1);
            if (!value || PyDict_SetItem(dict, key, value) < 0) Py_CLEAR(dict);
            Py_XDECREF(value);
        }
        return dict;
    }
    PyObject *list = PyList_New(0);
    for (Py_ssize_t i = 0; list && i < PyList_GET_SIZE(self->entries); ++i) {
        PyObject *value = data_of(PyList_GET_ITEM(self->entries, i), depth + 1);
        if (!value || PyList_Append(list, value) < 0) Py_CLEAR(list);
        Py_XDECREF(value);
    }
    return list;
}

// The (key, value) pairs of a Mapping as a fast sequence whose items are all
// tuples of two, or null with a TypeError set: items() may give anything.
PyObject *mapping_pairs(PyObject *mapping) {
    PyObject *items = PyMapping_Items(mapping);
    if (!items) return nullptr;
    PyObject *fast = PySequence_Fast(items, "items() must give a sequence");
    Py_DECREF(items);
    if (!fast) return nullptr;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(fast); ++i) {
        PyObject *pair = PySequence_Fast_GET_ITEM(fast, i);
        if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
            PyErr_SetString(
                PyExc_TypeError,
                "items() of a mapping must give (key, value) pairs");
            Py_DECREF(fast);
            return nullptr;
        }
    }
    return fast;
}

template <class D>
bool emit_mapping_items(Emitter<D> &e, PyObject *items, int depth) {
    e.begin_object();
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(items); ++i) {
        PyObject *pair = PySequence_Fast_GET_ITEM(items, i);
        PyObject *key = PyTuple_GET_ITEM(pair, 0);
        dstd::StringView name;
        if (!PyUnicode_Check(key)) {
            PyErr_Format(PyExc_TypeError, "a key must be str, not %.200s",
                         Py_TYPE(key)->tp_name);
            return false;
        }
        if (!text_of(key, name)) return false;
        e.key(name);
        if (!emit_py<D>(e, PyTuple_GET_ITEM(pair, 1), depth + 1)) return false;
    }
    e.end();
    return true;
}

template <class D>
bool emit_int(Emitter<D> &e, PyObject *o) {
    int overflow = 0;
    const long long s = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow == 0) {
        if (s == -1 && PyErr_Occurred()) return false;
        e.integer(s);
        return true;
    }
    if (overflow < 0) {
        PyErr_SetString(PyExc_OverflowError, "int out of range for 64 bits");
        return false;
    }
    const unsigned long long u = PyLong_AsUnsignedLongLong(o);
    if (u == static_cast<unsigned long long>(-1) && PyErr_Occurred())
        return false;
    e.unsigned_integer(u);
    return true;
}

}  // namespace

template <class F>
PyTypeObject *mutable_type() {
    return &g_mutable_type<F>;
}

template <class F>
PyObject *make_mutable(PyObject *value) {
    const auto *val = value_of<F>(value);
    const int mode = F::is_object(*val)  ? MutableObject<F>::Object
                     : F::is_array(*val) ? MutableObject<F>::Array
                                         : MutableObject<F>::Scalar;
    return new_mutable<F>(mode, value, nullptr, nullptr);
}

template <class S>
const RawSpan *unchanged_raw(PyObject *py_self) {
    if (!PyObject_TypeCheck(py_self, &g_mutable_type<S>)) return nullptr;
    auto *self = as_mutable<S>(py_self);
    return is_clean<S>(self) ? raw_of<S>(self->base) : nullptr;
}

template <class S, class D>
bool emit_mutable(Emitter<D> &e, PyObject *py_self, int depth) {
    auto *self = as_mutable<S>(py_self);
    if (is_clean<S>(self)) return emit_py<D>(e, self->base, depth + 1);
    if (!self->entries) {
        if (self->mode == MutableObject<S>::Scalar && self->scalar)
            return emit_py<D>(e, self->scalar, depth + 1);
        emit_value<S, D>(e, base_val<S>(self));
        return true;
    }
    if (self->mode == MutableObject<S>::Object) {
        e.begin_object();
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(self->entries, &pos, &key, &item)) {
            dstd::StringView name;
            if (!text_of(key, name)) return false;
            e.key(name);
            if (!emit_py<D>(e, item, depth + 1)) return false;
        }
        e.end();
        return true;
    }
    e.begin_array();
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(self->entries); ++i)
        if (!emit_py<D>(e, PyList_GET_ITEM(self->entries, i), depth + 1))
            return false;
    e.end();
    return true;
}

PyObject *data_of(PyObject *o, int depth) {
    if (depth > MAX_EMIT_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nested too deeply");
        return nullptr;
    }
    if (o == Py_None || PyBool_Check(o) || PyLong_Check(o) ||
        PyFloat_Check(o) || PyUnicode_Check(o)) {
        Py_INCREF(o);
        return o;
    }
    if (const auto *v = value_of<JsonFormat>(o))
        return to_python<JsonFormat>(*v, depth);
    if (const auto *v = value_of<YamlFormat>(o))
        return to_python<YamlFormat>(*v, depth);
    if (PyObject_TypeCheck(o, &g_mutable_type<JsonFormat>))
        return mutable_data<JsonFormat>(as_mutable<JsonFormat>(o), depth);
    if (PyObject_TypeCheck(o, &g_mutable_type<YamlFormat>))
        return mutable_data<YamlFormat>(as_mutable<YamlFormat>(o), depth);
    const int mapping = is_abc(o, g_mapping_abc);
    if (mapping < 0) return nullptr;
    if (mapping) {
        PyObject *items = mapping_pairs(o);
        if (!items) return nullptr;
        PyObject *dict = PyDict_New();
        for (Py_ssize_t i = 0; dict && i < PySequence_Fast_GET_SIZE(items);
             ++i) {
            PyObject *pair = PySequence_Fast_GET_ITEM(items, i);
            PyObject *key = PyTuple_GET_ITEM(pair, 0);
            PyObject *value =
                PyUnicode_Check(key)
                    ? data_of(PyTuple_GET_ITEM(pair, 1), depth + 1)
                    : nullptr;
            if (!PyUnicode_Check(key))
                PyErr_Format(PyExc_TypeError, "a key must be str, not %.200s",
                             Py_TYPE(key)->tp_name);
            if (!value || PyDict_SetItem(dict, key, value) < 0) Py_CLEAR(dict);
            Py_XDECREF(value);
        }
        Py_DECREF(items);
        return dict;
    }
    const int sequence = is_text_like(o) ? 0 : is_abc(o, g_sequence_abc);
    if (sequence < 0) return nullptr;
    if (sequence) {
        PyObject *fast = PySequence_Fast(o, "a sequence");
        if (!fast) return nullptr;
        PyObject *list = PyList_New(0);
        for (Py_ssize_t i = 0; list && i < PySequence_Fast_GET_SIZE(fast);
             ++i) {
            PyObject *value =
                data_of(PySequence_Fast_GET_ITEM(fast, i), depth + 1);
            if (!value || PyList_Append(list, value) < 0) Py_CLEAR(list);
            Py_XDECREF(value);
        }
        Py_DECREF(fast);
        return list;
    }
    PyErr_Format(PyExc_TypeError, "a value of type %.200s is not supported",
                 Py_TYPE(o)->tp_name);
    return nullptr;
}

template <class D>
bool emit_py(Emitter<D> &e, PyObject *o, int depth) {
    if (depth > MAX_EMIT_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nested too deeply");
        return false;
    }
    if (o == Py_None) {
        e.null();
        return true;
    }
    if (PyBool_Check(o)) {
        e.boolean(o == Py_True);
        return true;
    }
    if (PyLong_Check(o)) return emit_int(e, o);
    if (PyFloat_Check(o)) {
        e.real(PyFloat_AS_DOUBLE(o));
        return true;
    }
    if (PyUnicode_Check(o)) {
        dstd::StringView text;
        if (!text_of(o, text)) return false;
        e.str(text);
        return true;
    }
    if (const auto *v = value_of<JsonFormat>(o)) {
        if constexpr (std::is_same_v<D, JsonFormat>) {
            const RawSpan *raw = raw_of<JsonFormat>(o);
            if (raw && std::memchr(raw->data, '\n', raw->size) == nullptr) {
                e.raw_json(dstd::StringView(raw->data, raw->size));
                return true;
            }
        }
        emit_value<JsonFormat, D>(e, *v);
        return true;
    }
    if (const auto *v = value_of<YamlFormat>(o)) {
        emit_value<YamlFormat, D>(e, *v);
        return true;
    }
    if (PyObject_TypeCheck(o, &g_mutable_type<JsonFormat>))
        return emit_mutable<JsonFormat, D>(e, o, depth);
    if (PyObject_TypeCheck(o, &g_mutable_type<YamlFormat>))
        return emit_mutable<YamlFormat, D>(e, o, depth);
    if (PyDict_Check(o)) {
        e.begin_object();
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(o, &pos, &key, &item)) {
            dstd::StringView name;
            if (!PyUnicode_Check(key)) {
                PyErr_Format(PyExc_TypeError, "a key must be str, not %.200s",
                             Py_TYPE(key)->tp_name);
                return false;
            }
            if (!text_of(key, name)) return false;
            e.key(name);
            if (!emit_py<D>(e, item, depth + 1)) return false;
        }
        e.end();
        return true;
    }
    if (PyList_Check(o) || PyTuple_Check(o)) {
        e.begin_array();
        for (Py_ssize_t i = 0;
             i < (PyList_Check(o) ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o));
             ++i) {
            PyObject *item = PyList_Check(o) ? PyList_GET_ITEM(o, i)
                                             : PyTuple_GET_ITEM(o, i);
            if (!emit_py<D>(e, item, depth + 1)) return false;
        }
        e.end();
        return true;
    }
    const int mapping = is_abc(o, g_mapping_abc);
    if (mapping < 0) return false;
    if (mapping) {
        PyObject *items = mapping_pairs(o);
        if (!items) return false;
        const bool ok = emit_mapping_items<D>(e, items, depth);
        Py_DECREF(items);
        return ok;
    }
    const int sequence = is_text_like(o) ? 0 : is_abc(o, g_sequence_abc);
    if (sequence < 0) return false;
    if (sequence) {
        PyObject *fast = PySequence_Fast(o, "a sequence");
        if (!fast) return false;
        bool ok = true;
        e.begin_array();
        for (Py_ssize_t i = 0; ok && i < PySequence_Fast_GET_SIZE(fast); ++i)
            ok = emit_py<D>(e, PySequence_Fast_GET_ITEM(fast, i), depth + 1);
        if (ok) e.end();
        Py_DECREF(fast);
        return ok;
    }
    PyErr_Format(PyExc_TypeError, "a value of type %.200s is not supported",
                 Py_TYPE(o)->tp_name);
    return false;
}

template <class F>
int init_mutable_value(PyObject *m) {
    if (!g_mapping_abc) {
        PyObject *abc = PyImport_ImportModule("collections.abc");
        if (!abc) return -1;
        g_mapping_abc = PyObject_GetAttrString(abc, "Mapping");
        g_sequence_abc = PyObject_GetAttrString(abc, "Sequence");
        Py_DECREF(abc);
        if (!g_mapping_abc || !g_sequence_abc) return -1;
    }
    g_mutable_name<F> =
        dstd::String("dftracer.utils.formats.") + F::NAME + ".MutableValue";
    PyTypeObject &type = g_mutable_type<F>;
    type.tp_name = g_mutable_name<F>.c_str();
    type.tp_basicsize = sizeof(MutableObject<F>);
    type.tp_dealloc = Mutable_dealloc<F>;
    type.tp_repr = Mutable_repr<F>;
    type.tp_flags = Py_TPFLAGS_DEFAULT;
    type.tp_doc =
        "The changes made to a Value, kept apart from it. A read or a\n"
        "change of a member or an element is on the overlay; the Value\n"
        "never changes, and the parts left alone are written as they are.";
    g_mutable_number<F>.nb_bool = Mutable_bool<F>;
    type.tp_as_number = &g_mutable_number<F>;
    type.tp_as_mapping = &g_mutable_mapping<F>;
    type.tp_iter = Mutable_iter<F>;
    type.tp_methods = g_mutable_methods<F>;
    dstd::String attribute(F::TYPE_PREFIX);
    attribute += "MutableValue";
    return register_type(m, &type, attribute.c_str());
}

template PyTypeObject *mutable_type<JsonFormat>();
template PyTypeObject *mutable_type<YamlFormat>();
template const RawSpan *unchanged_raw<JsonFormat>(PyObject *);
template const RawSpan *unchanged_raw<YamlFormat>(PyObject *);
template PyObject *make_mutable<JsonFormat>(PyObject *);
template PyObject *make_mutable<YamlFormat>(PyObject *);
template int init_mutable_value<JsonFormat>(PyObject *);
template int init_mutable_value<YamlFormat>(PyObject *);
template bool emit_mutable<JsonFormat, JsonFormat>(Emitter<JsonFormat> &,
                                                   PyObject *, int);
template bool emit_mutable<JsonFormat, YamlFormat>(Emitter<YamlFormat> &,
                                                   PyObject *, int);
template bool emit_mutable<YamlFormat, JsonFormat>(Emitter<JsonFormat> &,
                                                   PyObject *, int);
template bool emit_mutable<YamlFormat, YamlFormat>(Emitter<YamlFormat> &,
                                                   PyObject *, int);
template bool emit_py<JsonFormat>(Emitter<JsonFormat> &, PyObject *, int);
template bool emit_py<YamlFormat>(Emitter<YamlFormat> &, PyObject *, int);

}  // namespace dftracer::utils::python::formats
