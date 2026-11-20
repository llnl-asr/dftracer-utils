// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/core/api_methods.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

namespace dftracer::utils::python::core::api {

PyObject* accessor_series(PyObject* accessor) {
    return reinterpret_cast<frame::series_detail::AccessorObject*>(accessor)
        ->series;
}

}  // namespace dftracer::utils::python::core::api

namespace dftracer::utils::python::frame::series_detail {

namespace {

PyTypeObject StrAccessorType;
PyTypeObject DtAccessorType;
PyTypeObject ListAccessorType;

PyObject* Accessor_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    PyObject* series = nullptr;
    if (!kwds && PyTuple_GET_SIZE(args) == 1)
        series = PyTuple_GET_ITEM(args, 0);
    else if (!parse_kw<"O">(args, kwds, {"series"}, &series))
        return nullptr;
    if (!as_native<Series>(series)) return nullptr;
    auto* self = reinterpret_cast<AccessorObject*>(type->tp_alloc(type, 0));
    if (!self) return nullptr;
    Py_INCREF(series);
    self->series = series;
    return reinterpret_cast<PyObject*>(self);
}

void Accessor_dealloc(AccessorObject* self) {
    if (self->weaklist)
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    Py_XDECREF(self->series);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* Accessor_get_series(PyObject* self, void*) {
    PyObject* series = core::api::accessor_series(self);
    Py_INCREF(series);
    return series;
}

PyGetSetDef Accessor_getset[] = {{"_s", Accessor_get_series, nullptr,
                                  "The Series this accessor reads.", nullptr},
                                 {nullptr, nullptr, nullptr, nullptr, nullptr}};

#undef API_PICK_Series
#undef API_PICK_Str
#undef API_PICK_Dt
#undef API_PICK_List
#define API_PICK_Series(id, name, args, doc)
#define API_PICK_Str(id, name, args, doc) API_ACC_ENTRY(id, name, args, doc)
#define API_PICK_Dt(id, name, args, doc)
#define API_PICK_List(id, name, args, doc)
PyMethodDef Str_methods[] = {
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, \
                  err, doc)                                                  \
    API_PICK_##cls(core::api::row_id(#cls, #name), #name, args, doc)
#include <dftracer/utils/python/core/api.def>
    {nullptr, nullptr, 0, nullptr}};

#undef API_PICK_Str
#undef API_PICK_Dt
#define API_PICK_Str(id, name, args, doc)
#define API_PICK_Dt(id, name, args, doc) API_ACC_ENTRY(id, name, args, doc)
PyMethodDef Dt_methods[] = {
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, \
                  err, doc)                                                  \
    API_PICK_##cls(core::api::row_id(#cls, #name), #name, args, doc)
#include <dftracer/utils/python/core/api.def>
    {nullptr, nullptr, 0, nullptr}};

#undef API_PICK_Dt
#undef API_PICK_List
#define API_PICK_Dt(id, name, args, doc)
#define API_PICK_List(id, name, args, doc) API_ACC_ENTRY(id, name, args, doc)
PyMethodDef List_methods[] = {
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, \
                  err, doc)                                                  \
    API_PICK_##cls(core::api::row_id(#cls, #name), #name, args, doc)
#include <dftracer/utils/python/core/api.def>
    {nullptr, nullptr, 0, nullptr}};

void make_type(PyTypeObject& t, const char* name, const char* doc,
               PyMethodDef* methods) {
    t = {};
    t.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    t.tp_name = name;
    t.tp_basicsize = sizeof(AccessorObject);
    t.tp_weaklistoffset = WEAKLIST_OFFSET;
    t.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    t.tp_doc = doc;
    t.tp_new = Accessor_new;
    t.tp_dealloc = reinterpret_cast<destructor>(Accessor_dealloc);
    t.tp_methods = methods;
    t.tp_getset = Accessor_getset;
}

}  // namespace

int init_accessors(PyObject* m) {
    make_type(StrAccessorType, "dftracer_utils_ext.StrAccessor",
              "The string methods of a Series: Series.str.", Str_methods);
    make_type(DtAccessorType, "dftracer_utils_ext.DtAccessor",
              "The temporal methods of a Series: Series.dt.", Dt_methods);
    make_type(ListAccessorType, "dftracer_utils_ext.ListAccessor",
              "The list methods of a Series: Series.list.", List_methods);
    if (register_type(m, &StrAccessorType, "StrAccessor") < 0) return -1;
    if (register_type(m, &DtAccessorType, "DtAccessor") < 0) return -1;
    return register_type(m, &ListAccessorType, "ListAccessor");
}

}  // namespace dftracer::utils::python::frame::series_detail
