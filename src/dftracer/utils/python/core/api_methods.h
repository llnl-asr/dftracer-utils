// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_API_METHODS_H
#define DFTRACER_UTILS_PYTHON_CORE_API_METHODS_H

#include <Python.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/core/method.h>

#include <cstddef>

// Expands the rows of api.def: a Row per method for run_op and for the table
// the Python side reads, and the PyMethodDef entries of a class. A class file
// puts its entries in its method table with
//
//   #define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, \
//                     kw1, doc)                                              \
//       API_PICK_##cls(ID_##cls##_##name, #name, args, doc)
//   #include <dftracer/utils/python/core/api.def>
//
// and defines API_PICK_<Class> as API_ENTRY, the other classes as nothing.
namespace dftracer::utils::python::core::api {

enum class Args { None, Scalar, Number2 };
enum class Result { Same, Bool, Int, Float };
enum class NameRule { Keep };

struct Row {
    const char* cls;
    const char* name;
    const char* op;
    Args args;
    Result result;
    NameRule name_rule;
    bool on_expr;
    const char* kw[2];
    const char* doc;
};

#define API_ARGS_NONE Args::None
#define API_ARGS_SCALAR Args::Scalar
#define API_ARGS_NUMBER2 Args::Number2
#define API_RESULT_SAME Result::Same
#define API_RESULT_BOOL Result::Bool
#define API_RESULT_INT Result::Int
#define API_RESULT_FLOAT Result::Float
#define API_NAME_RULE_KEEP NameRule::Keep
#define API_FLAG_NO false
#define API_FLAG_YES true

inline constexpr Row ROWS[] = {
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, \
                  doc)                                                       \
    {#cls,                                                                   \
     #name,                                                                  \
     op,                                                                     \
     API_ARGS_##args,                                                        \
     API_RESULT_##result,                                                    \
     API_NAME_RULE_##name_rule,                                              \
     API_FLAG_##on_expr,                                                     \
     {kw0, kw1},                                                             \
     doc},
#include <dftracer/utils/python/core/api.def>
};

inline constexpr std::size_t ROW_COUNT = sizeof(ROWS) / sizeof(ROWS[0]);

/// Not defined as a constant expression: calling it fails the compile.
std::size_t no_such_row();

/// The index of the row of @p cls and @p name in ROWS.
consteval std::size_t row_id(dstd::StringView cls, dstd::StringView name) {
    for (std::size_t i = 0; i < ROW_COUNT; ++i)
        if (cls == ROWS[i].cls && name == ROWS[i].name) return i;
    return no_such_row();
}

/// Runs row @p id on @p self with the arguments of a METH_VARARGS |
/// METH_KEYWORDS call (both null for Args::NONE). It holds the GIL for the
/// argument parsing and the result, reads the column through a copy taken under
/// the GIL and runs the registry op with the GIL released on a long column.
/// Defined with the class that owns the rows.
PyObject* run_op(std::size_t id, PyObject* self, PyObject* args, PyObject* kw);

/// run_op for a row with Args::NONE, without the argument tuple.
PyObject* run_op_noargs(std::size_t id, PyObject* self);

/// The rows as a tuple of (class, name, op, args, result, name_rule, on_expr,
/// (keywords...), doc) tuples, strings for the tokens.
PyObject* rows_as_tuple();

template <std::size_t Id>
PyObject* entry_noargs(PyObject* self, PyObject*) {
    return run_op_noargs(Id, self);
}

template <std::size_t Id>
PyObject* entry_kw(PyObject* self, PyObject* args, PyObject* kw) {
    return run_op(Id, self, args, kw);
}

}  // namespace dftracer::utils::python::core::api

#define API_ENTRY_NONE(id, name, doc)                                         \
    {name, dftracer::utils::python::core::api::entry_noargs<id>, METH_NOARGS, \
     doc},
#define API_ENTRY_SCALAR(id, name, doc)                                        \
    {name, DFTU_PYCFUNCTION(dftracer::utils::python::core::api::entry_kw<id>), \
     METH_VARARGS | METH_KEYWORDS, doc},
#define API_ENTRY_NUMBER2(id, name, doc) API_ENTRY_SCALAR(id, name, doc)
#define API_ENTRY(id, name, args, doc) API_ENTRY_##args(id, name, doc)

#endif  // DFTRACER_UTILS_PYTHON_CORE_API_METHODS_H
