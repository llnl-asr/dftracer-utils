// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/formats/emit.h>
#include <dftracer/utils/python/formats/mutable_value.h>
#include <dftracer/utils/python/formats/value.h>

#include <memory>
#include <new>
#include <type_traits>

namespace dftracer::utils::python::formats {

namespace {

constexpr const char *JSON_KIND_NAMES[] = {"object", "array", "string", "int",
                                           "float",  "bool",  "null"};

PyObject *expected(const char *what, const char *got) {
    PyErr_Format(PyExc_TypeError, "expected %s, got %s", what, got);
    return nullptr;
}

}  // namespace

int JsonFormat::kind(const Val &v) {
    switch (v.type()) {
        case Val::Type::Object:
            return Object;
        case Val::Type::Array:
            return Array;
        case Val::Type::String:
            return String;
        case Val::Type::Int:
        case Val::Type::Uint:
            return Int;
        case Val::Type::Double:
            return Float;
        case Val::Type::Bool:
            return Bool;
        case Val::Type::Null:
            break;
    }
    return Null;
}

PyObject *JsonFormat::scalar(const Val &v) {
    switch (v.type()) {
        case Val::Type::String:
            return make_str(v.as_string());
        case Val::Type::Int:
            return PyLong_FromLongLong(v.as_int());
        case Val::Type::Uint:
            return PyLong_FromUnsignedLongLong(v.as_uint());
        case Val::Type::Double:
            return PyFloat_FromDouble(v.as_double());
        case Val::Type::Bool:
            return PyBool_FromLong(v.as_bool());
        default:
            Py_RETURN_NONE;
    }
}

PyObject *JsonFormat::as_int(const Val &v) {
    if (v.type() == Val::Type::Int || v.type() == Val::Type::Uint)
        return scalar(v);
    return expected("an int", JSON_KIND_NAMES[kind(v)]);
}

PyObject *JsonFormat::as_float(const Val &v) {
    switch (v.type()) {
        case Val::Type::Int:
            return PyFloat_FromDouble(static_cast<double>(v.as_int()));
        case Val::Type::Uint:
            return PyFloat_FromDouble(static_cast<double>(v.as_uint()));
        case Val::Type::Double:
            return PyFloat_FromDouble(v.as_double());
        default:
            return expected("a number", JSON_KIND_NAMES[kind(v)]);
    }
}

PyObject *JsonFormat::as_bool(const Val &v) {
    if (v.type() == Val::Type::Bool) return scalar(v);
    return expected("a bool", JSON_KIND_NAMES[kind(v)]);
}

PyObject *JsonFormat::as_str(const Val &v) {
    if (v.type() == Val::Type::String) return scalar(v);
    return expected("a string", JSON_KIND_NAMES[kind(v)]);
}

int YamlFormat::kind(const Val &v) {
    if (v.is_object()) return Object;
    if (v.is_array()) return Array;
    if (v.is_null()) return Null;
    return Scalar;
}

PyObject *YamlFormat::scalar(const Val &v) {
    const auto text = v.as_string();
    if (!text) Py_RETURN_NONE;
    return make_str(*text);
}

namespace {

// The text of a YAML scalar for an as_ reader, or null with the Python error
// set: a TypeError for a container and a ValueError for a null.
PyObject *yaml_text_error(const YamlFormat::Val &v, const char *what) {
    if (v.is_object() || v.is_array())
        return expected(what, v.is_object() ? "object" : "array");
    PyErr_Format(PyExc_ValueError, "%s is not %s",
                 v.is_null() ? "null" : dstd::String(v.raw()).c_str(), what);
    return nullptr;
}

}  // namespace

PyObject *YamlFormat::as_int(const Val &v) {
    if (const auto n = v.as_int()) return PyLong_FromLongLong(*n);
    if (const auto n = v.as_uint()) return PyLong_FromUnsignedLongLong(*n);
    return yaml_text_error(v, "an int");
}

PyObject *YamlFormat::as_float(const Val &v) {
    if (const auto n = v.as_double()) return PyFloat_FromDouble(*n);
    return yaml_text_error(v, "a number");
}

PyObject *YamlFormat::as_bool(const Val &v) {
    if (const auto b = v.as_bool()) return PyBool_FromLong(*b);
    return yaml_text_error(v, "a bool");
}

PyObject *YamlFormat::as_str(const Val &v) {
    if (v.is_null() || v.is_object() || v.is_array())
        return yaml_text_error(v, "a string");
    return scalar(v);
}

namespace {

template <class F>
PyTypeObject g_value_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
PyTypeObject g_document_type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = nullptr};
template <class F>
dstd::String g_value_name;
template <class F>
dstd::String g_document_name;
template <class F>
PyObject *g_kinds[F::KIND_COUNT] = {};

template <class F>
const typename F::Val &val_of(PyObject *self) {
    return reinterpret_cast<ValueObject<F> *>(self)->val;
}

template <class F>
PyObject *owner_of(PyObject *self) {
    return reinterpret_cast<ValueObject<F> *>(self)->owner;
}

template <class F>
PyObject *child(PyObject *self, const typename F::Val &v) {
    return make_value<F>(owner_of<F>(self), v);
}

template <class F>
void Value_dealloc(PyObject *self) {
    Py_XDECREF(owner_of<F>(self));
    std::destroy_at(&reinterpret_cast<ValueObject<F> *>(self)->elements);
    Py_TYPE(self)->tp_free(self);
}

template <class F>
void Document_dealloc(PyObject *self) {
    std::destroy_at(&reinterpret_cast<DocumentObject<F> *>(self)->doc);
    std::destroy_at(&reinterpret_cast<DocumentObject<F> *>(self)->source);
    Py_TYPE(self)->tp_free(self);
}

template <class F>
Py_ssize_t count(const typename F::Val &v) {
    Py_ssize_t n = 0;
    if (F::is_object(v))
        F::members(v, [&](dstd::StringView, typename F::Val) { ++n; });
    else
        F::elements(v, [&](typename F::Val) { ++n; });
    return n;
}

template <class F>
Py_ssize_t Value_length(PyObject *self) {
    const auto &v = val_of<F>(self);
    if (!F::is_object(v) && !F::is_array(v)) {
        PyErr_SetString(PyExc_TypeError, "a scalar has no length");
        return -1;
    }
    const auto &elements = reinterpret_cast<ValueObject<F> *>(self)->elements;
    if (elements) return static_cast<Py_ssize_t>(elements->size());
    return count<F>(v);
}

template <class F>
int Value_bool(PyObject *self) {
    const auto &v = val_of<F>(self);
    if (F::is_null(v)) return 0;
    if (F::is_object(v) || F::is_array(v)) return count<F>(v) > 0;
    return 1;
}

template <class F>
PyObject *Value_getitem(PyObject *self, PyObject *key) {
    const auto &v = val_of<F>(self);
    if (PyUnicode_Check(key)) {
        if (!F::is_object(v)) {
            PyErr_SetString(PyExc_TypeError, "only an object has string keys");
            return nullptr;
        }
        Py_ssize_t size = 0;
        const char *text = PyUnicode_AsUTF8AndSize(key, &size);
        if (!text) return nullptr;
        const auto found =
            F::find(v, dstd::StringView(text, static_cast<std::size_t>(size)));
        if (!found) {
            PyErr_SetObject(PyExc_KeyError, key);
            return nullptr;
        }
        return child<F>(self, *found);
    }
    if (PyLong_Check(key) && !PyBool_Check(key)) {
        if (!F::is_array(v)) {
            PyErr_SetString(PyExc_TypeError, "only an array has int indexes");
            return nullptr;
        }
        Py_ssize_t index = PyLong_AsSsize_t(key);
        if (index == -1 && PyErr_Occurred()) return nullptr;
        auto &table = reinterpret_cast<ValueObject<F> *>(self)->elements;
        if (!table) {
            auto made = dstd::make_unique<dstd::Vector<typename F::Val>>();
            F::elements(v, [&](typename F::Val e) { made->push_back(e); });
            table = std::move(made);
        }
        const Py_ssize_t size = static_cast<Py_ssize_t>(table->size());
        if (index < 0) index += size;
        if (index < 0 || index >= size) {
            PyErr_SetString(PyExc_IndexError, "array index out of range");
            return nullptr;
        }
        return child<F>(self, (*table)[static_cast<std::size_t>(index)]);
    }
    PyErr_Format(PyExc_TypeError, "a key must be str or int, not %.200s",
                 Py_TYPE(key)->tp_name);
    return nullptr;
}

template <class F>
int Value_contains(PyObject *self, PyObject *key) {
    const auto &v = val_of<F>(self);
    if (!F::is_object(v) || !PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError,
                        "only an object holds str keys, and `in` takes a str");
        return -1;
    }
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(key, &size);
    if (!text) return -1;
    return F::find(v, dstd::StringView(text, static_cast<std::size_t>(size)))
                   .has_value()
               ? 1
               : 0;
}

template <class F>
PyObject *Value_get(PyObject *self, PyObject *key) {
    const auto &v = val_of<F>(self);
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "a key must be str");
        return nullptr;
    }
    if (!F::is_object(v)) {
        PyErr_SetString(PyExc_TypeError, "only an object has members");
        return nullptr;
    }
    Py_ssize_t size = 0;
    const char *text = PyUnicode_AsUTF8AndSize(key, &size);
    if (!text) return nullptr;
    const auto found =
        F::find(v, dstd::StringView(text, static_cast<std::size_t>(size)));
    if (!found) Py_RETURN_NONE;
    return child<F>(self, *found);
}

template <class F>
PyObject *Value_items(PyObject *self, PyObject *) {
    const auto &v = val_of<F>(self);
    if (!F::is_object(v)) {
        PyErr_SetString(PyExc_TypeError, "only an object has items");
        return nullptr;
    }
    PyObject *list = PyList_New(0);
    if (!list) return nullptr;
    bool failed = false;
    F::members(v, [&](dstd::StringView name, typename F::Val member) {
        if (failed) return;
        PyObject *key = make_str(name);
        PyObject *value = key ? child<F>(self, member) : nullptr;
        PyObject *pair = value ? PyTuple_Pack(2, key, value) : nullptr;
        Py_XDECREF(key);
        Py_XDECREF(value);
        if (!pair || PyList_Append(list, pair) < 0) failed = true;
        Py_XDECREF(pair);
    });
    PyObject *it = failed ? nullptr : PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

template <class F>
PyObject *Value_iter(PyObject *self) {
    const auto &v = val_of<F>(self);
    if (!F::is_array(v)) {
        PyErr_SetString(PyExc_TypeError,
                        "only an array is iterated; use items() for an object");
        return nullptr;
    }
    PyObject *list = PyList_New(0);
    if (!list) return nullptr;
    bool failed = false;
    F::elements(v, [&](typename F::Val element) {
        if (failed) return;
        PyObject *item = child<F>(self, element);
        if (!item || PyList_Append(list, item) < 0) failed = true;
        Py_XDECREF(item);
    });
    PyObject *it = failed ? nullptr : PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

template <class F>
PyObject *Value_as_int(PyObject *self, PyObject *) {
    return F::as_int(val_of<F>(self));
}
template <class F>
PyObject *Value_as_float(PyObject *self, PyObject *) {
    return F::as_float(val_of<F>(self));
}
template <class F>
PyObject *Value_as_bool(PyObject *self, PyObject *) {
    return F::as_bool(val_of<F>(self));
}
template <class F>
PyObject *Value_as_str(PyObject *self, PyObject *) {
    return F::as_str(val_of<F>(self));
}
template <class F>
PyObject *Value_to_python(PyObject *self, PyObject *) {
    return to_python<F>(val_of<F>(self), 0);
}
template <class F>
PyObject *Value_mutable(PyObject *self, PyObject *) {
    return make_mutable<F>(self);
}

template <class F>
PyObject *Value_get_kind(PyObject *self, void *) {
    PyObject *kind = g_kinds<F>[F::kind(val_of<F>(self))];
    if (!kind) {
        PyErr_SetString(PyExc_RuntimeError, "the Kind class is not bound");
        return nullptr;
    }
    Py_INCREF(kind);
    return kind;
}

template <class F>
PyObject *Value_get_raw(PyObject *self, void *) {
    const RawSpan &raw = reinterpret_cast<ValueObject<F> *>(self)->raw;
    if (raw.data) return make_str(dstd::StringView(raw.data, raw.size));
    return raw_text<F>(val_of<F>(self));
}

template <class F>
PyObject *Value_repr(PyObject *self) {
    const char *name = Py_TYPE(self)->tp_name;
    PyObject *kind = Value_get_kind<F>(self, nullptr);
    if (!kind) return nullptr;
    PyObject *kind_name = PyObject_GetAttrString(kind, "name");
    Py_DECREF(kind);
    if (!kind_name) return nullptr;
    PyObject *text = PyUnicode_FromFormat("<%s kind=%U>", name, kind_name);
    Py_DECREF(kind_name);
    return text;
}

template <class F>
PyMethodDef g_value_methods[] = {
    {"get", DFTU_PYCFUNCTION(Value_get<F>), METH_O,
     "get(key) -> Value | None\n\nThe member of an object, or None."},
    {"items", DFTU_PYCFUNCTION(Value_items<F>), METH_NOARGS,
     "items() -> Iterator[tuple[str, Value]]\n\nThe members of an object."},
    {"as_int", DFTU_PYCFUNCTION(Value_as_int<F>), METH_NOARGS,
     "as_int() -> int"},
    {"as_float", DFTU_PYCFUNCTION(Value_as_float<F>), METH_NOARGS,
     "as_float() -> float"},
    {"as_bool", DFTU_PYCFUNCTION(Value_as_bool<F>), METH_NOARGS,
     "as_bool() -> bool"},
    {"as_str", DFTU_PYCFUNCTION(Value_as_str<F>), METH_NOARGS,
     "as_str() -> str"},
    {"to_python", DFTU_PYCFUNCTION(Value_to_python<F>), METH_NOARGS,
     "to_python() -> Data\n\nThe plain dict, list, str, int, float, bool or "
     "None under the value."},
    {"mutable", DFTU_PYCFUNCTION(Value_mutable<F>), METH_NOARGS,
     "mutable() -> MutableValue\n\nAn overlay of changes on this value; the "
     "value does not change."},
    {nullptr, nullptr, 0, nullptr}};

template <class F>
PyGetSetDef g_value_getset[] = {
    {"kind", Value_get_kind<F>, nullptr, "The kind of the value.", nullptr},
    {"raw", Value_get_raw<F>, nullptr,
     "The source text of a record or a document root, else the text the "
     "parsed value writes as.",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

template <class F>
PyMappingMethods g_value_mapping = {Value_length<F>, Value_getitem<F>, nullptr};
template <class F>
PySequenceMethods g_value_sequence = {};
template <class F>
PyNumberMethods g_value_number = {};

}  // namespace

template <class F>
PyTypeObject *value_type() {
    return &g_value_type<F>;
}

template <class F>
PyTypeObject *document_type() {
    return &g_document_type<F>;
}

template <class F>
PyObject *make_value(PyObject *owner, const typename F::Val &val, RawSpan raw) {
    PyTypeObject *type = &g_value_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    auto *object = reinterpret_cast<ValueObject<F> *>(self);
    Py_XINCREF(owner);
    object->owner = owner;
    object->raw = raw;
    new (&object->elements) dstd::UniquePtr<dstd::Vector<typename F::Val>>();
    new (&object->val) typename F::Val(val);
    return self;
}

template <class F>
PyObject *make_document(typename F::Doc &&doc, dstd::String &&source) {
    PyTypeObject *type = &g_document_type<F>;
    PyObject *self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    new (&reinterpret_cast<DocumentObject<F> *>(self)->doc)
        dstd::UniquePtr<typename F::Doc>(
            dstd::make_unique<typename F::Doc>(std::move(doc)));
    new (&reinterpret_cast<DocumentObject<F> *>(self)->source)
        dstd::String(std::move(source));
    return self;
}

template <class F>
const RawSpan *raw_of(PyObject *o) {
    if (!PyObject_TypeCheck(o, &g_value_type<F>)) return nullptr;
    const RawSpan *raw = &reinterpret_cast<ValueObject<F> *>(o)->raw;
    return raw->data ? raw : nullptr;
}

template <class F>
const typename F::Val *value_of(PyObject *o) {
    if (!PyObject_TypeCheck(o, &g_value_type<F>)) return nullptr;
    return &reinterpret_cast<ValueObject<F> *>(o)->val;
}

template <class F>
PyObject *to_python(const typename F::Val &v, int depth) {
    if (depth > MAX_EMIT_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "nested too deeply");
        return nullptr;
    }
    if (F::is_object(v)) {
        PyObject *dict = PyDict_New();
        if (!dict) return nullptr;
        bool failed = false;
        F::members(v, [&](dstd::StringView name, typename F::Val member) {
            if (failed) return;
            PyObject *key = make_str(name);
            PyObject *value = key ? to_python<F>(member, depth + 1) : nullptr;
            if (!value || PyDict_SetItem(dict, key, value) < 0) failed = true;
            Py_XDECREF(key);
            Py_XDECREF(value);
        });
        if (failed) {
            Py_DECREF(dict);
            return nullptr;
        }
        return dict;
    }
    if (F::is_array(v)) {
        PyObject *list = PyList_New(0);
        if (!list) return nullptr;
        bool failed = false;
        F::elements(v, [&](typename F::Val element) {
            if (failed) return;
            PyObject *item = to_python<F>(element, depth + 1);
            if (!item || PyList_Append(list, item) < 0) failed = true;
            Py_XDECREF(item);
        });
        if (failed) {
            Py_DECREF(list);
            return nullptr;
        }
        return list;
    }
    return F::scalar(v);
}

template <class S, class D>
void emit_value(Emitter<D> &e, const typename S::Val &v) {
    if (S::is_object(v)) {
        e.begin_object();
        S::members(v, [&](dstd::StringView name, typename S::Val member) {
            e.key(name);
            emit_value<S, D>(e, member);
        });
        e.end();
    } else if (S::is_array(v)) {
        e.begin_array();
        S::elements(
            v, [&](typename S::Val element) { emit_value<S, D>(e, element); });
        e.end();
    } else if constexpr (std::is_same_v<S, JsonFormat>) {
        switch (v.type()) {
            case S::Val::Type::String:
                e.str(v.as_string());
                break;
            case S::Val::Type::Int:
                e.integer(v.as_int());
                break;
            case S::Val::Type::Uint:
                e.unsigned_integer(v.as_uint());
                break;
            case S::Val::Type::Double:
                e.real(v.as_double());
                break;
            case S::Val::Type::Bool:
                e.boolean(v.as_bool());
                break;
            default:
                e.null();
        }
    } else {
        if (const auto text = v.as_string())
            e.str(*text);
        else
            e.null();
    }
}

template <class F>
PyObject *raw_text(const typename F::Val &v) {
    if constexpr (std::is_same_v<F, YamlFormat>) {
        if (!F::is_object(v) && !F::is_array(v)) {
            if (const auto text = v.as_string()) return make_str(*text);
            return make_str("null");
        }
    }
    dstd::String out;
    {
        Emitter<F> e(out, 2);
        emit_value<F, F>(e, v);
    }
    return make_str(out);
}

// The enum class of the kinds of format F, added to `m`; the members are kept
// for Value.kind.
template <class F>
int init_kind(PyObject *m, dstd::StringView module) {
    PyObject *enum_module = PyImport_ImportModule("enum");
    if (!enum_module) return -1;
    PyObject *enum_class = PyObject_GetAttrString(enum_module, "Enum");
    Py_DECREF(enum_module);
    if (!enum_class) return -1;
    PyObject *names = PyList_New(F::KIND_COUNT);
    for (int i = 0; names && i < F::KIND_COUNT; ++i) {
        PyObject *pair = Py_BuildValue("(si)", F::KIND_NAMES[i], i);
        if (!pair)
            Py_CLEAR(names);
        else
            PyList_SET_ITEM(names, i, pair);
    }
    PyObject *args = names ? Py_BuildValue("(sO)", "Kind", names) : nullptr;
    PyObject *kwargs =
        args ? Py_BuildValue(
                   "{s:s,s:s}", "module",
                   dstd::String(module.substr(0, module.size() - 1)).c_str(),
                   "qualname", "Kind")
             : nullptr;
    PyObject *kind_class =
        kwargs ? PyObject_Call(enum_class, args, kwargs) : nullptr;
    Py_DECREF(enum_class);
    Py_XDECREF(names);
    Py_XDECREF(args);
    Py_XDECREF(kwargs);
    if (!kind_class) return -1;
    for (int i = 0; i < F::KIND_COUNT; ++i) {
        PyObject *member = PyObject_CallFunction(kind_class, "i", i);
        if (!member) {
            Py_DECREF(kind_class);
            return -1;
        }
        g_kinds<F>[i] = member;
    }
    dstd::String attribute(F::TYPE_PREFIX);
    attribute += "Kind";
    const int added = PyModule_AddObject(m, attribute.c_str(), kind_class);
    if (added < 0) Py_DECREF(kind_class);
    return added;
}

template <class F>
int init_value(PyObject *m) {
    const dstd::String module =
        dstd::String("dftracer.utils.formats.") + F::NAME + ".";
    if (init_kind<F>(m, module) < 0) return -1;
    g_value_name<F> = module + "Value";
    PyTypeObject &type = g_value_type<F>;
    type.tp_name = g_value_name<F>.c_str();
    type.tp_basicsize = sizeof(ValueObject<F>);
    type.tp_dealloc = Value_dealloc<F>;
    type.tp_repr = Value_repr<F>;
    type.tp_flags = Py_TPFLAGS_DEFAULT;
    type.tp_doc =
        "A parsed value: a view with no copy that keeps its document alive.\n"
        "It never changes; mutable() gives an overlay of changes.";
    g_value_number<F>.nb_bool = Value_bool<F>;
    type.tp_as_number = &g_value_number<F>;
    g_value_sequence<F>.sq_contains = Value_contains<F>;
    type.tp_as_sequence = &g_value_sequence<F>;
    type.tp_as_mapping = &g_value_mapping<F>;
    type.tp_iter = Value_iter<F>;
    type.tp_methods = g_value_methods<F>;
    type.tp_getset = g_value_getset<F>;
    dstd::String attribute(F::TYPE_PREFIX);
    attribute += "Value";
    if (register_type(m, &type, attribute.c_str()) < 0) return -1;

    g_document_name<F> = module + "_Document";
    PyTypeObject &document = g_document_type<F>;
    document.tp_name = g_document_name<F>.c_str();
    document.tp_basicsize = sizeof(DocumentObject<F>);
    document.tp_dealloc = Document_dealloc<F>;
    document.tp_flags = Py_TPFLAGS_DEFAULT;
    return PyType_Ready(&document);
}

template PyTypeObject *value_type<JsonFormat>();
template PyTypeObject *value_type<YamlFormat>();
template PyTypeObject *document_type<JsonFormat>();
template PyTypeObject *document_type<YamlFormat>();
template PyObject *make_value<JsonFormat>(PyObject *, const JsonFormat::Val &,
                                          RawSpan);
template PyObject *make_value<YamlFormat>(PyObject *, const YamlFormat::Val &,
                                          RawSpan);
template PyObject *make_document<JsonFormat>(JsonFormat::Doc &&,
                                             dstd::String &&);
template PyObject *make_document<YamlFormat>(YamlFormat::Doc &&,
                                             dstd::String &&);
template const RawSpan *raw_of<JsonFormat>(PyObject *);
template const RawSpan *raw_of<YamlFormat>(PyObject *);
template const JsonFormat::Val *value_of<JsonFormat>(PyObject *);
template const YamlFormat::Val *value_of<YamlFormat>(PyObject *);
template PyObject *to_python<JsonFormat>(const JsonFormat::Val &, int);
template PyObject *to_python<YamlFormat>(const YamlFormat::Val &, int);
template PyObject *raw_text<JsonFormat>(const JsonFormat::Val &);
template PyObject *raw_text<YamlFormat>(const YamlFormat::Val &);
template int init_value<JsonFormat>(PyObject *);
template int init_value<YamlFormat>(PyObject *);
template void emit_value<JsonFormat, JsonFormat>(Emitter<JsonFormat> &,
                                                 const JsonFormat::Val &);
template void emit_value<JsonFormat, YamlFormat>(Emitter<YamlFormat> &,
                                                 const JsonFormat::Val &);
template void emit_value<YamlFormat, JsonFormat>(Emitter<JsonFormat> &,
                                                 const YamlFormat::Val &);
template void emit_value<YamlFormat, YamlFormat>(Emitter<YamlFormat> &,
                                                 const YamlFormat::Val &);

}  // namespace dftracer::utils::python::formats
