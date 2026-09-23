// Binding for the native columnar expression engine. Python serializes its
// expression tree to a flat post-order AST; this rebuilds it as a
// dataframe::Expr and evaluates it - all the compiler work (type inference,
// CSE, lowering, fused chunked + parallel evaluation) lives in vec, so C/C++
// consumers get the same engine. This TU also installs the runtime-backed
// parallel backend so vec's parallelism seam fans out across the coroutine
// pool.

#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/columnar_eval.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_scalar_helpers.h>
#include <dftracer/utils/python/series.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace {

// AST node opcodes (mirror the serializer in columnar.py).
enum {
    AST_COL = 0,
    AST_LIT_I = 1,
    AST_LIT_F = 2,
    AST_BIN = 3,
    AST_PRIM = 4,
    AST_CMP = 5,
    AST_LOGICAL = 6,
    AST_NOT = 7,
    AST_CAST = 8,
    AST_UNARY = 9,
    AST_CLIP = 10,
    AST_FILLNA = 11,
    AST_STR_PRED = 12,
    AST_STR_MAP = 13,
    AST_STR_LEN = 14,
    AST_STR_FIND = 15,
    AST_STR_REPLACE = 16,
    AST_STR_SLICE = 17,
    AST_IS_IN = 18,
    AST_SELECT = 19,
    AST_IS_NULL = 20,
};

// A borrowed view of a Python str; valid while `o` lives, which the ast list
// guarantees for the duration of build_expr.
bool py_str_view(PyObject* o, std::string_view* out) {
    Py_ssize_t n = 0;
    const char* s = PyUnicode_AsUTF8AndSize(o, &n);
    if (!s) return false;
    *out = std::string_view(s, static_cast<std::size_t>(n));
    return true;
}

// The is_in value list as a Series: all ints (Int64) or all strs (String).
bool py_values_series(PyObject* list, dataframe::Series* out) {
    PyObject* seq = PySequence_Fast(list, "is_in values must be a sequence");
    if (!seq) return false;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    bool all_str = n > 0;
    bool all_int = n > 0;
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* v = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyUnicode_Check(v)) all_str = false;
        if (PyBool_Check(v) || !PyLong_Check(v)) all_int = false;
    }
    if (all_str) {
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(n));
        for (Py_ssize_t i = 0; i < n; ++i) {
            std::string_view s;
            if (!py_str_view(PySequence_Fast_GET_ITEM(seq, i), &s)) {
                Py_DECREF(seq);
                return false;
            }
            values.emplace_back(s);
        }
        Py_DECREF(seq);
        *out = dataframe::Series::strings(values);
        return true;
    }
    if (all_int) {
        std::vector<std::int64_t> values;
        values.reserve(static_cast<std::size_t>(n));
        for (Py_ssize_t i = 0; i < n; ++i) {
            values.push_back(static_cast<std::int64_t>(
                PyLong_AsLongLong(PySequence_Fast_GET_ITEM(seq, i))));
            if (PyErr_Occurred()) {
                Py_DECREF(seq);
                return false;
            }
        }
        Py_DECREF(seq);
        *out = dataframe::Series::flat_i64(values.data(),
                                           static_cast<std::int64_t>(n));
        return true;
    }
    Py_DECREF(seq);
    PyErr_SetString(PyExc_TypeError,
                    "is_in values must be a non-empty list of all ints or all "
                    "strings");
    return false;
}

// Rebuild a dataframe::Expr from a post-order list of (op, arg0[, arg1])
// tuples.
bool build_expr(PyObject* ast, dataframe::Expr* out) {
    if (!PyList_Check(ast)) {
        PyErr_SetString(PyExc_TypeError, "ast must be a list");
        return false;
    }
    std::vector<dataframe::Expr> stack;
    Py_ssize_t n = PyList_GET_SIZE(ast);
    for (Py_ssize_t k = 0; k < n; ++k) {
        PyObject* t = PyList_GET_ITEM(ast, k);
        if (!PyTuple_Check(t) || PyTuple_GET_SIZE(t) < 1) {
            PyErr_SetString(PyExc_TypeError, "each ast node is a tuple");
            return false;
        }
        int op = static_cast<int>(PyLong_AsLong(PyTuple_GET_ITEM(t, 0)));
        auto arg = [&](int i) { return PyTuple_GET_ITEM(t, i); };
        auto pop = [&]() {
            dataframe::Expr e = std::move(stack.back());
            stack.pop_back();
            return e;
        };
        switch (op) {
            case AST_COL:
                stack.push_back(dataframe::expr_col(
                    static_cast<std::int32_t>(PyLong_AsLong(arg(1)))));
                break;
            case AST_LIT_I:
                stack.push_back(dataframe::expr_lit(
                    static_cast<std::int64_t>(PyLong_AsLongLong(arg(1)))));
                break;
            case AST_LIT_F:
                stack.push_back(dataframe::expr_lit(PyFloat_AsDouble(arg(1))));
                break;
            case AST_BIN: {
                dataframe::Expr b = pop(), a = pop();
                stack.push_back(dataframe::expr_binary(
                    static_cast<dataframe::BinaryOp>(PyLong_AsLong(arg(1))), a,
                    b));
                break;
            }
            case AST_PRIM: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_prim(
                    static_cast<dataframe::PrimOp>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AST_CMP: {
                dftu_scalar rhs{};
                if (PyUnicode_Check(arg(2))) {
                    // expr_cmp copies the text into the node, so the borrow
                    // ends with this call.
                    std::string_view s;
                    if (!py_str_view(arg(2), &s)) return false;
                    rhs.kind = DFTU_SCALAR_TAG_STR;
                    rhs.len = static_cast<std::uint32_t>(s.size());
                    rhs.value.s = s.data();
                } else if (!py_to_scalar(arg(2), &rhs)) {
                    return false;
                }
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_cmp(
                    static_cast<dataframe::CmpOp>(PyLong_AsLong(arg(1))), a,
                    rhs));
                break;
            }
            case AST_LOGICAL: {
                dataframe::Expr b = pop(), a = pop();
                stack.push_back(dataframe::expr_logical(
                    static_cast<dataframe::LogicalOp>(PyLong_AsLong(arg(1))), a,
                    b));
                break;
            }
            case AST_NOT: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_not(a));
                break;
            }
            case AST_CAST: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_cast(
                    static_cast<dataframe::TypeId>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AST_UNARY: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_unary(
                    static_cast<dataframe::UnaryOp>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AST_CLIP: {
                dftu_scalar lo{}, hi{};
                if (!py_to_scalar(arg(1), &lo) || !py_to_scalar(arg(2), &hi))
                    return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_clip(a, lo, hi));
                break;
            }
            case AST_FILLNA: {
                dftu_scalar fill{};
                if (!py_to_scalar(arg(1), &fill)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_fillna(a, fill));
                break;
            }
            case AST_STR_PRED: {
                std::string_view pattern;
                if (!py_str_view(arg(2), &pattern)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_pred(
                    static_cast<dataframe::StrPredOp>(PyLong_AsLong(arg(1))), a,
                    pattern));
                break;
            }
            case AST_STR_MAP: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_map(
                    static_cast<dataframe::StrMapOp>(PyLong_AsLong(arg(1))),
                    a));
                break;
            }
            case AST_STR_LEN: {
                dataframe::Expr a = pop();
                stack.push_back(
                    dataframe::expr_str_len(a, PyLong_AsLong(arg(1)) != 0));
                break;
            }
            case AST_STR_FIND: {
                std::string_view needle;
                if (!py_str_view(arg(1), &needle)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_find(a, needle));
                break;
            }
            case AST_STR_REPLACE: {
                std::string_view from, to;
                if (!py_str_view(arg(1), &from) || !py_str_view(arg(2), &to))
                    return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_replace(
                    a, from, to, PyLong_AsLong(arg(3)) != 0));
                break;
            }
            case AST_STR_SLICE: {
                const long long start = PyLong_AsLongLong(arg(1));
                const long long len = PyLong_AsLongLong(arg(2));
                if (PyErr_Occurred()) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_slice(a, start, len));
                break;
            }
            case AST_IS_IN: {
                dataframe::Series values;
                if (!py_values_series(arg(1), &values)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_is_in(a, std::move(values)));
                break;
            }
            case AST_SELECT: {
                if (stack.size() < 3) {
                    PyErr_SetString(PyExc_ValueError,
                                    "select needs three operands");
                    return false;
                }
                dataframe::Expr b = pop(), a = pop(), cond = pop();
                stack.push_back(dataframe::expr_select(cond, a, b));
                break;
            }
            case AST_IS_NULL: {
                if (stack.empty()) {
                    PyErr_SetString(PyExc_ValueError,
                                    "is_null needs an operand");
                    return false;
                }
                const long null = PyLong_AsLong(arg(1));
                if (null == -1 && PyErr_Occurred()) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_is_null(a, null != 0));
                break;
            }
            default:
                PyErr_Format(PyExc_ValueError, "bad ast op %d", op);
                return false;
        }
        if (PyErr_Occurred()) return false;
    }
    if (stack.size() != 1) {
        PyErr_SetString(PyExc_ValueError,
                        "ast did not reduce to one expression");
        return false;
    }
    *out = std::move(stack.back());
    return true;
}

// The runtime-backed backend for vec's parallel_for seam: fan chunks across the
// process default runtime (lazily created, so import stays cheap).
void runtime_backend(void*, std::int64_t n, std::int64_t grain,
                     void (*body)(void*, std::int64_t, std::int64_t),
                     void* bctx) {
    dftracer::utils::default_runtime().parallel_for(
        n, grain, [&](std::int64_t b, std::int64_t e) { body(bctx, b, e); });
}

}  // namespace

namespace dftracer::utils::python {

bool build_expr_from_ast(PyObject* ast, dataframe::Expr* out) {
    return build_expr(ast, out);
}

// vec_eval(ast, [Series, ...]) -> Series
PyObject* vec_eval(PyObject* /*self*/, PyObject* args) {
    PyObject* ast = nullptr;
    PyObject* cols_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &ast, &cols_obj)) return nullptr;

    dataframe::Expr expr;
    if (!build_expr(ast, &expr)) return nullptr;

    PyObject* seq = PySequence_Fast(cols_obj, "columns must be a sequence");
    if (!seq) return nullptr;
    Py_ssize_t nc = PySequence_Fast_GET_SIZE(seq);
    std::vector<const dataframe::Series*> inputs;
    inputs.reserve(static_cast<std::size_t>(nc));
    for (Py_ssize_t i = 0; i < nc; ++i) {
        const dataframe::Series* c =
            unwrap_vec_column(PySequence_Fast_GET_ITEM(seq, i));
        if (!c) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "columns must be SeriesList");
            return nullptr;
        }
        inputs.push_back(c);
    }
    Py_DECREF(seq);

    try {
        return wrap_vec_column(dataframe::eval(expr, inputs));
    } catch (const std::exception& ex) {
        PyErr_SetString(PyExc_RuntimeError, ex.what());
        return nullptr;
    }
}

// vec_eval_many([ast, ...], [Series, ...]) -> [Series, ...]
// Compiles all roots into one program so CSE spans them (a subexpression shared
// across outputs is computed once), sharing the same input column list.
PyObject* vec_eval_many(PyObject* /*self*/, PyObject* args) {
    PyObject* asts_obj = nullptr;
    PyObject* cols_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &asts_obj, &cols_obj)) return nullptr;

    PyObject* aseq = PySequence_Fast(asts_obj, "asts must be a sequence");
    if (!aseq) return nullptr;
    Py_ssize_t ne = PySequence_Fast_GET_SIZE(aseq);
    std::vector<dataframe::Expr> roots(static_cast<std::size_t>(ne));
    for (Py_ssize_t i = 0; i < ne; ++i) {
        if (!build_expr(PySequence_Fast_GET_ITEM(aseq, i),
                        &roots[static_cast<std::size_t>(i)])) {
            Py_DECREF(aseq);
            return nullptr;
        }
    }
    Py_DECREF(aseq);

    PyObject* seq = PySequence_Fast(cols_obj, "columns must be a sequence");
    if (!seq) return nullptr;
    Py_ssize_t nc = PySequence_Fast_GET_SIZE(seq);
    std::vector<const dataframe::Series*> inputs;
    inputs.reserve(static_cast<std::size_t>(nc));
    for (Py_ssize_t i = 0; i < nc; ++i) {
        const dataframe::Series* c =
            unwrap_vec_column(PySequence_Fast_GET_ITEM(seq, i));
        if (!c) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError, "columns must be Series");
            return nullptr;
        }
        inputs.push_back(c);
    }
    Py_DECREF(seq);

    try {
        std::vector<dataframe::Series> outs =
            dataframe::eval_many(roots, inputs);
        PyObject* list = PyList_New(static_cast<Py_ssize_t>(outs.size()));
        if (!list) return nullptr;
        for (std::size_t i = 0; i < outs.size(); ++i) {
            PyObject* s = wrap_vec_column(std::move(outs[i]));
            if (!s) {
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
        }
        return list;
    } catch (const std::exception& ex) {
        PyErr_SetString(PyExc_RuntimeError, ex.what());
        return nullptr;
    }
}

int init_columnar_eval(PyObject* m) {
    dataframe::set_parallel_backend(runtime_backend, nullptr);
    static PyMethodDef def = {
        "vec_eval", vec_eval, METH_VARARGS,
        "vec_eval(ast, columns) -> Series: compile (type inference + CSE) "
        "and fused chunked evaluation of a columnar expression."};
    PyObject* fn = PyCFunction_NewEx(&def, nullptr, nullptr);
    if (!fn) return -1;
    if (PyModule_AddObject(m, "vec_eval", fn) < 0) {
        Py_DECREF(fn);
        return -1;
    }
    static PyMethodDef def_many = {
        "vec_eval_many", vec_eval_many, METH_VARARGS,
        "vec_eval_many(asts, columns) -> [Series]: compile several roots into "
        "one CSE'd program over a shared input list and evaluate in one pass."};
    PyObject* fn_many = PyCFunction_NewEx(&def_many, nullptr, nullptr);
    if (!fn_many) return -1;
    if (PyModule_AddObject(m, "vec_eval_many", fn_many) < 0) {
        Py_DECREF(fn_many);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python

#else   // !DFTRACER_UTILS_ENABLE_ARROW

namespace dftracer::utils::python {
int init_columnar_eval(PyObject*) { return 0; }
bool build_expr_from_ast(PyObject*, dftracer::utils::dataframe::Expr*) {
    return false;
}
}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
