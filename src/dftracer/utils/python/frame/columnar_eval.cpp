// SPDX-License-Identifier: MIT
// Binding for the native columnar expression engine. Python serializes its
// expression tree to a flat post-order AST; this rebuilds it as a
// dataframe::Expr and evaluates it - all the compiler work (type inference,
// CSE, lowering, fused chunked + parallel evaluation) lives in vec, so C/C++
// consumers get the same engine. This TU also installs the runtime-backed
// parallel backend so vec's parallelism seam fans out across the coroutine
// pool.

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udf.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/scalar_helpers.h>
#include <dftracer/utils/python/core/seq_helpers.h>
#include <dftracer/utils/python/frame/columnar_eval.h>
#include <dftracer/utils/python/frame/series.h>
#include <dftracer/utils/runtime/runtime.h>

#include <cstdint>
#include <utility>

using dftracer::utils::python::core::release_gil;

using namespace dftracer::utils;

namespace {

// AST node opcodes (mirror the serializer in expr.py).
enum {
    AstCol = 0,
    AstLitI = 1,
    AstLitF = 2,
    AstBin = 3,
    AstPrim = 4,
    AstCmp = 5,
    AstLogical = 6,
    AstNot = 7,
    AstCast = 8,
    AstUnary = 9,
    AstClip = 10,
    AstFillna = 11,
    AstStrPred = 12,
    AstStrMap = 13,
    AstStrLen = 14,
    AstStrFind = 15,
    AstStrReplace = 16,
    AstStrSlice = 17,
    AstIsIn = 18,
    AstSelect = 19,
    AstIsNull = 20,
    AstCmpExpr = 21,
    AstStrFn = 22,  // (fn, text, text2, i0, i1, operand count)
    AstStrPredCol = 23,
    AstStrReplaceCol = 24,
    AstLitS = 25,
    AstUdf = 26,  // (udf capsule, operand count)
};

const char* const UDF_CAPSULE = "dftu.udf";

void free_udf_capsule(PyObject* capsule) {
    dftracer::utils::dstd::UniquePtr<dataframe::Udf> owner(
        static_cast<dataframe::Udf*>(
            PyCapsule_GetPointer(capsule, UDF_CAPSULE)));
}

const char* const UDAF_CAPSULE = "dftu.udaf";

using UdafHold = dftracer::utils::dstd::SharedPtr<const dataframe::UdafRecord>;

void free_udaf_capsule(PyObject* capsule) {
    dftracer::utils::dstd::UniquePtr<UdafHold> owner(
        static_cast<UdafHold*>(PyCapsule_GetPointer(capsule, UDAF_CAPSULE)));
}

// A borrowed view of a Python str; valid while `o` lives, which the ast list
// guarantees for the duration of build_expr.
bool py_str_view(PyObject* o, dftracer::utils::dstd::StringView* out) {
    Py_ssize_t n = 0;
    const char* s = PyUnicode_AsUTF8AndSize(o, &n);
    if (!s) return false;
    *out = dftracer::utils::dstd::StringView(s, static_cast<std::size_t>(n));
    return true;
}

// The is_in value list as a Series: all strs (String); otherwise numbers,
// where a bool is 0 or 1. Ints alone are Int64, or Uint64 when one is above
// the int64 range; a float among them makes Float64. An empty list is an
// empty Int64, which matches nothing.
bool py_values_series(PyObject* list, dataframe::Series* out) {
    dftracer::utils::python::core::FastSeq items;
    if (!items.open(list, "is_in values must be a sequence")) return false;
    const auto n = static_cast<std::size_t>(items.size());
    bool all_str = n > 0;
    bool any_str = false;
    bool any_float = false;
    for (Py_ssize_t i = 0; i < items.size(); ++i) {
        PyObject* v = items.item(i);
        const bool str = PyUnicode_Check(v);
        all_str = all_str && str;
        any_str = any_str || str;
        if (PyFloat_Check(v)) {
            any_float = true;
        } else if (!str && !PyLong_Check(v)) {
            PyErr_SetString(PyExc_TypeError,
                            "is_in values must be strings, numbers or bools");
            return false;
        }
    }
    if (all_str) {
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> values;
        values.reserve(n);
        for (Py_ssize_t i = 0; i < items.size(); ++i) {
            dftracer::utils::dstd::StringView s;
            if (!py_str_view(items.item(i), &s)) return false;
            values.emplace_back(s);
        }
        *out = dataframe::Series::strings(values);
        return true;
    }
    if (any_str) {
        PyErr_SetString(PyExc_TypeError,
                        "is_in values must be all strings or all numbers");
        return false;
    }
    if (any_float) {
        dftracer::utils::dstd::Vector<double> values;
        values.reserve(n);
        for (Py_ssize_t i = 0; i < items.size(); ++i) {
            values.push_back(PyFloat_AsDouble(items.item(i)));
            if (PyErr_Occurred()) return false;
        }
        *out = dataframe::Series::flat_f64(values.data(),
                                           static_cast<std::int64_t>(n));
        return true;
    }
    dftracer::utils::dstd::Vector<std::int64_t> values;
    values.reserve(n);
    bool wide = false;
    for (Py_ssize_t i = 0; i < items.size() && !wide; ++i) {
        int overflow = 0;
        const long long x =
            PyLong_AsLongLongAndOverflow(items.item(i), &overflow);
        if (overflow < 0) {
            PyErr_SetString(PyExc_OverflowError,
                            "is_in value is below the int64 range");
            return false;
        }
        if (overflow > 0) wide = true;
        if (x == -1 && PyErr_Occurred()) return false;
        values.push_back(static_cast<std::int64_t>(x));
    }
    if (!wide) {
        *out = dataframe::Series::flat_i64(values.data(),
                                           static_cast<std::int64_t>(n));
        return true;
    }
    dftracer::utils::dstd::Vector<std::uint64_t> unsigned_values;
    unsigned_values.reserve(n);
    for (Py_ssize_t i = 0; i < items.size(); ++i) {
        const unsigned long long x = PyLong_AsUnsignedLongLong(items.item(i));
        if (PyErr_Occurred()) {
            PyErr_Clear();
            PyErr_SetString(PyExc_OverflowError,
                            "is_in values mix negative ints and ints above "
                            "the int64 range, or one is above the uint64 "
                            "range");
            return false;
        }
        unsigned_values.push_back(static_cast<std::uint64_t>(x));
    }
    *out = dataframe::Series::flat(dataframe::TypeId::Uint64,
                                   unsigned_values.data(),
                                   static_cast<std::int64_t>(n));
    return true;
}

// Rebuild a dataframe::Expr from a post-order list of (op, arg0[, arg1])
// tuples.
bool build_expr(PyObject* ast, dataframe::Expr* out) {
    if (!PyList_Check(ast)) {
        PyErr_SetString(PyExc_TypeError, "ast must be a list");
        return false;
    }
    dftracer::utils::dstd::Vector<dataframe::Expr> stack;
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
            case AstCol:
                stack.push_back(dataframe::expr_col(
                    static_cast<std::int32_t>(PyLong_AsLong(arg(1)))));
                break;
            case AstLitI:
                stack.push_back(dataframe::expr_lit(
                    static_cast<std::int64_t>(PyLong_AsLongLong(arg(1)))));
                break;
            case AstLitF:
                stack.push_back(dataframe::expr_lit(PyFloat_AsDouble(arg(1))));
                break;
            case AstLitS: {
                dftracer::utils::dstd::StringView v;
                if (!py_str_view(arg(1), &v)) return false;
                stack.push_back(dataframe::expr_lit_str(v));
                break;
            }
            case AstUdf: {
                auto* udf = static_cast<dataframe::Udf*>(
                    PyCapsule_GetPointer(arg(1), UDF_CAPSULE));
                if (!udf) return false;
                const long count = PyLong_AsLong(arg(2));
                if (count < 0 ||
                    static_cast<std::size_t>(count) > stack.size()) {
                    PyErr_SetString(PyExc_ValueError,
                                    "a udf call needs its operands");
                    return false;
                }
                dftracer::utils::dstd::Vector<dataframe::Expr> operands(
                    stack.end() - count, stack.end());
                stack.resize(stack.size() - static_cast<std::size_t>(count));
                stack.push_back((*udf)(operands));
                break;
            }
            case AstBin: {
                dataframe::Expr b = pop(), a = pop();
                stack.push_back(dataframe::expr_binary(
                    static_cast<dataframe::BinaryOp>(PyLong_AsLong(arg(1))), a,
                    b));
                break;
            }
            case AstPrim: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_prim(
                    static_cast<dataframe::PrimOp>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AstCmp: {
                dftu_scalar rhs{};
                if (PyUnicode_Check(arg(2))) {
                    // expr_cmp copies the text into the node, so the borrow
                    // ends with this call.
                    dftracer::utils::dstd::StringView s;
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
            case AstCmpExpr: {
                dataframe::Expr b = pop(), a = pop();
                stack.push_back(dataframe::expr_cmp_expr(
                    static_cast<dataframe::CmpOp>(PyLong_AsLong(arg(1))), a,
                    b));
                break;
            }
            case AstLogical: {
                dataframe::Expr b = pop(), a = pop();
                stack.push_back(dataframe::expr_logical(
                    static_cast<dataframe::LogicalOp>(PyLong_AsLong(arg(1))), a,
                    b));
                break;
            }
            case AstNot: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_not(a));
                break;
            }
            case AstCast: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_cast(
                    static_cast<dataframe::TypeId>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AstUnary: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_unary(
                    static_cast<dataframe::UnaryOp>(PyLong_AsLong(arg(1))), a));
                break;
            }
            case AstClip: {
                dftu_scalar lo{}, hi{};
                if (!py_to_scalar(arg(1), &lo) || !py_to_scalar(arg(2), &hi))
                    return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_clip(a, lo, hi));
                break;
            }
            case AstFillna: {
                dftu_scalar fill{};
                if (!py_to_scalar(arg(1), &fill)) return false;
                dataframe::Expr a = pop();
                const bool nan =
                    PyTuple_GET_SIZE(t) > 2 && PyObject_IsTrue(arg(2)) == 1;
                stack.push_back(dataframe::expr_fillna(a, fill, nan));
                break;
            }
            case AstStrPred: {
                dftracer::utils::dstd::StringView pattern;
                if (!py_str_view(arg(2), &pattern)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_pred(
                    static_cast<dataframe::StrPredOp>(PyLong_AsLong(arg(1))), a,
                    pattern));
                break;
            }
            case AstStrPredCol: {
                if (stack.size() < 2) {
                    PyErr_SetString(PyExc_ValueError,
                                    "string predicate needs its operands");
                    return false;
                }
                dataframe::Expr needle = pop();
                dataframe::Expr a = pop();
                auto pred = dataframe::expr_str_pred_col(
                    static_cast<dataframe::StrPredOp>(PyLong_AsLong(arg(1))), a,
                    needle);
                if (!pred) {
                    dftracer::utils::python::core::set_py_error(pred.error());
                    return false;
                }
                stack.push_back(std::move(*pred));
                break;
            }
            case AstStrReplaceCol: {
                if (stack.size() < 3) {
                    PyErr_SetString(PyExc_ValueError,
                                    "replace needs its operands");
                    return false;
                }
                dataframe::Expr to = pop();
                dataframe::Expr from = pop();
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_replace_col(a, from, to));
                break;
            }
            case AstStrMap: {
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_map(
                    static_cast<dataframe::StrMapOp>(PyLong_AsLong(arg(1))),
                    a));
                break;
            }
            case AstStrFn: {
                dftracer::utils::dstd::StringView text, text2;
                if (PyTuple_GET_SIZE(t) != 7 || !py_str_view(arg(2), &text) ||
                    !py_str_view(arg(3), &text2))
                    return false;
                const long fn = PyLong_AsLong(arg(1));
                const long long i0 = PyLong_AsLongLong(arg(4));
                const long long i1 = PyLong_AsLongLong(arg(5));
                const long nargs = PyLong_AsLong(arg(6));
                if (PyErr_Occurred()) return false;
                if (nargs < 1 || nargs > 2 ||
                    stack.size() < static_cast<std::size_t>(nargs)) {
                    PyErr_SetString(PyExc_ValueError,
                                    "string method needs its operands");
                    return false;
                }
                dataframe::Expr b;
                if (nargs == 2) b = pop();
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_fn(
                    static_cast<dataframe::StrFn>(fn), a,
                    nargs == 2 ? &b : nullptr, text, text2, i0, i1));
                break;
            }
            case AstStrLen: {
                dataframe::Expr a = pop();
                stack.push_back(
                    dataframe::expr_str_size(a, PyLong_AsLong(arg(1)) != 0));
                break;
            }
            case AstStrFind: {
                dftracer::utils::dstd::StringView needle;
                if (!py_str_view(arg(1), &needle)) return false;
                dataframe::Expr a = pop();
                dataframe::Expr found = dataframe::expr_str_find(a, needle);
                // A third item asks for null where the needle is absent.
                if (PyTuple_GET_SIZE(t) > 2 && PyObject_IsTrue(arg(2))) {
                    found = dataframe::expr_select(
                        dataframe::expr_cmp(
                            dataframe::CmpOp::Ge, found,
                            dataframe::to_scalar<std::int64_t>(0)),
                        found,
                        dataframe::expr_lit_null(dataframe::TypeId::Int64));
                }
                stack.push_back(std::move(found));
                break;
            }
            case AstStrReplace: {
                dftracer::utils::dstd::StringView from, to;
                if (!py_str_view(arg(1), &from) || !py_str_view(arg(2), &to))
                    return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_replace(
                    a, from, to, PyLong_AsLong(arg(3)) != 0));
                break;
            }
            case AstStrSlice: {
                const long long start = PyLong_AsLongLong(arg(1));
                const long long len = PyLong_AsLongLong(arg(2));
                if (PyErr_Occurred()) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_str_slice(a, start, len));
                break;
            }
            case AstIsIn: {
                dataframe::Series values;
                if (!py_values_series(arg(1), &values)) return false;
                dataframe::Expr a = pop();
                stack.push_back(dataframe::expr_is_in(a, std::move(values)));
                break;
            }
            case AstSelect: {
                if (stack.size() < 3) {
                    PyErr_SetString(PyExc_ValueError,
                                    "select needs three operands");
                    return false;
                }
                dataframe::Expr b = pop(), a = pop(), cond = pop();
                stack.push_back(dataframe::expr_select(cond, a, b));
                break;
            }
            case AstIsNull: {
                if (stack.empty()) {
                    PyErr_SetString(PyExc_ValueError,
                                    "is_null needs an operand");
                    return false;
                }
                const long null = PyLong_AsLong(arg(1));
                if (null == -1 && PyErr_Occurred()) return false;
                dataframe::Expr a = pop();
                stack.push_back(null >= 2
                                    ? dataframe::expr_is_na(a, null == 2)
                                    : dataframe::expr_is_null(a, null != 0));
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
    const auto ran = dftracer::utils::default_runtime().parallel_for(
        n, grain, [&](std::int64_t b, std::int64_t e) { body(bctx, b, e); });
    if (!ran) {
        dftracer::utils::dstd::fatal(
            "parallel_for: the runtime stopped during a fork-join");
    }
}

}  // namespace

namespace dftracer::utils::python::frame {

bool build_expr_from_ast(PyObject* ast, dataframe::Expr* out) {
    return build_expr(ast, out);
}

// udf_lookup(name) -> capsule holding a dataframe::Udf. The capsule keeps the
// UDF, and so its plugin library, alive for as long as an expression holds it.
static PyObject* udf_lookup(PyObject* /*self*/, PyObject* arg) {
    Py_ssize_t n = 0;
    const char* name = PyUnicode_AsUTF8AndSize(arg, &n);
    if (!name) return nullptr;
    auto found = dataframe::udf(
        dftracer::utils::dstd::StringView(name, static_cast<std::size_t>(n)));
    if (!found) {
        const dftracer::utils::dstd::String message(found.error().message());
        PyErr_SetString(PyExc_LookupError, message.c_str());
        return nullptr;
    }
    auto held =
        dftracer::utils::dstd::make_unique<dataframe::Udf>(std::move(*found));
    PyObject* capsule =
        PyCapsule_New(held.get(), UDF_CAPSULE, free_udf_capsule);
    if (capsule) held.release();
    return capsule;
}

static PyObject* udf_names(PyObject* /*self*/, PyObject* /*unused*/) {
    PyObject* list = PyList_New(0);
    if (!list) return nullptr;
    for (const auto& name : dataframe::udf_names()) {
        PyObject* item = PyUnicode_FromStringAndSize(
            name.data(), static_cast<Py_ssize_t>(name.size()));
        if (!item || PyList_Append(list, item) < 0) {
            Py_XDECREF(item);
            Py_DECREF(list);
            return nullptr;
        }
        Py_DECREF(item);
    }
    return list;
}

// udaf_lookup(name) -> (capsule, min_args, max_args, flags). The capsule holds
// the UDAF record, and so its plugin library, for as long as an aggregate made
// from it lives.
static PyObject* udaf_lookup(PyObject* /*self*/, PyObject* arg) {
    Py_ssize_t n = 0;
    const char* name = PyUnicode_AsUTF8AndSize(arg, &n);
    if (!name) return nullptr;
    auto record = dataframe::find_udaf(
        dftracer::utils::dstd::StringView(name, static_cast<std::size_t>(n)));
    if (!record) {
        PyErr_Format(PyExc_LookupError, "no udaf named %s", name);
        return nullptr;
    }
    const dftu_udaf_desc& desc = record->desc();
    const int min_args = desc.min_args;
    const int max_args = desc.max_args;
    const unsigned long flags = static_cast<unsigned long>(desc.flags);
    auto held = dftracer::utils::dstd::make_unique<UdafHold>(std::move(record));
    PyObject* capsule =
        PyCapsule_New(held.get(), UDAF_CAPSULE, free_udaf_capsule);
    if (!capsule) return nullptr;
    held.release();
    return Py_BuildValue("(Niik)", capsule, min_args, max_args, flags);
}

static PyObject* udaf_names(PyObject* /*self*/, PyObject* /*unused*/) {
    PyObject* list = PyList_New(0);
    if (!list) return nullptr;
    for (const auto& name : dataframe::udaf_names()) {
        PyObject* item = PyUnicode_FromStringAndSize(
            name.data(), static_cast<Py_ssize_t>(name.size()));
        if (!item || PyList_Append(list, item) < 0) {
            Py_XDECREF(item);
            Py_DECREF(list);
            return nullptr;
        }
        Py_DECREF(item);
    }
    return list;
}

// A share of the Series behind each item of `obj`, as pointers into `copies`,
// which must outlive their use; `msg` is the TypeError for an item that is not
// a Series. The caller evaluates with the GIL released, and `s.name = x` swaps
// the name of a Series object's own handle, so the evaluation reads the shares.
static bool columns_from_seq(
    PyObject* obj, const char* msg,
    dftracer::utils::python::core::FastSeq& held,
    dftracer::utils::dstd::Vector<dataframe::Series>& copies,
    dftracer::utils::dstd::Vector<const dataframe::Series*>& out) {
    if (!held.open(obj, "columns must be a sequence")) return false;
    copies.reserve(static_cast<std::size_t>(held.size()));
    out.reserve(static_cast<std::size_t>(held.size()));
    for (Py_ssize_t i = 0; i < held.size(); ++i) {
        const dataframe::Series* c = unwrap_vec_column(held.item(i));
        if (!c) {
            PyErr_SetString(PyExc_TypeError, msg);
            return false;
        }
        copies.push_back(c->share());
        out.push_back(&copies.back());
    }
    return true;
}

// vec_eval(ast, [Series, ...]) -> Series
static PyObject* vec_eval(PyObject* /*self*/, PyObject* args) {
    PyObject* ast = nullptr;
    PyObject* cols_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &ast, &cols_obj)) return nullptr;

    dataframe::Expr expr;
    if (!build_expr(ast, &expr)) return nullptr;

    dftracer::utils::python::core::FastSeq held;
    dftracer::utils::dstd::Vector<dataframe::Series> copies;
    dftracer::utils::dstd::Vector<const dataframe::Series*> inputs;
    if (!columns_from_seq(cols_obj, "columns must be SeriesList", held, copies,
                          inputs))
        return nullptr;

    // A per-batch Python udf runs on the Python lane, which needs the GIL.
    auto out = release_gil([&] { return dataframe::eval(expr, inputs); });
    if (!out) {
        dftracer::utils::python::core::set_py_error(out.error());
        return nullptr;
    }
    return wrap_vec_column(std::move(*out));
}

// vec_eval_many([ast, ...], [Series, ...]) -> [Series, ...]
// Compiles all roots into one program so CSE spans them (a subexpression shared
// across outputs is computed once), sharing the same input column list.
static PyObject* vec_eval_many(PyObject* /*self*/, PyObject* args) {
    PyObject* asts_obj = nullptr;
    PyObject* cols_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &asts_obj, &cols_obj)) return nullptr;

    dftracer::utils::dstd::Vector<dataframe::Expr> roots;
    if (!dftracer::utils::python::core::parse_seq<dataframe::Expr>(
            asts_obj, "asts must be a sequence", roots,
            [](PyObject* item,
               dftracer::utils::dstd::Vector<dataframe::Expr>& o) {
                dataframe::Expr root;
                if (!build_expr(item, &root)) return false;
                o.push_back(std::move(root));
                return true;
            }))
        return nullptr;

    dftracer::utils::python::core::FastSeq held;
    dftracer::utils::dstd::Vector<dataframe::Series> copies;
    dftracer::utils::dstd::Vector<const dataframe::Series*> inputs;
    if (!columns_from_seq(cols_obj, "columns must be Series", held, copies,
                          inputs))
        return nullptr;

    auto res = release_gil([&] { return dataframe::eval_many(roots, inputs); });
    if (!res) {
        dftracer::utils::python::core::set_py_error(res.error());
        return nullptr;
    }
    dftracer::utils::dstd::Vector<dataframe::Series>& outs = *res;
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
    static PyMethodDef def_udf = {
        "udf_lookup", udf_lookup, METH_O,
        "udf_lookup(name) -> capsule: the registered scalar UDF `name`, for "
        "the udf node of a columnar ast. Raises LookupError for an unknown "
        "name."};
    PyObject* fn_udf = PyCFunction_NewEx(&def_udf, nullptr, nullptr);
    if (!fn_udf) return -1;
    if (PyModule_AddObject(m, "udf_lookup", fn_udf) < 0) {
        Py_DECREF(fn_udf);
        return -1;
    }
    static PyMethodDef def_names = {
        "udf_names", udf_names, METH_NOARGS,
        "udf_names() -> [str]: the names of the registered scalar UDFs."};
    PyObject* fn_names = PyCFunction_NewEx(&def_names, nullptr, nullptr);
    if (!fn_names) return -1;
    if (PyModule_AddObject(m, "udf_names", fn_names) < 0) {
        Py_DECREF(fn_names);
        return -1;
    }
    static PyMethodDef def_udaf = {
        "udaf_lookup", udaf_lookup, METH_O,
        "udaf_lookup(name) -> (capsule, min_args, max_args, flags): the "
        "registered UDAF `name`. Raises LookupError for an unknown name."};
    PyObject* fn_udaf = PyCFunction_NewEx(&def_udaf, nullptr, nullptr);
    if (!fn_udaf) return -1;
    if (PyModule_AddObject(m, "udaf_lookup", fn_udaf) < 0) {
        Py_DECREF(fn_udaf);
        return -1;
    }
    static PyMethodDef def_udaf_names = {
        "udaf_names", udaf_names, METH_NOARGS,
        "udaf_names() -> [str]: the names of the registered UDAFs."};
    PyObject* fn_udaf_names =
        PyCFunction_NewEx(&def_udaf_names, nullptr, nullptr);
    if (!fn_udaf_names) return -1;
    if (PyModule_AddObject(m, "udaf_names", fn_udaf_names) < 0) {
        Py_DECREF(fn_udaf_names);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python::frame
