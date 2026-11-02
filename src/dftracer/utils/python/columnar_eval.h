// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_COLUMNAR_EVAL_H
#define DFTRACER_UTILS_PYTHON_COLUMNAR_EVAL_H

#include <Python.h>

namespace dftracer::utils::dataframe {
class Expr;
}

namespace dftracer::utils::python {

// Register the `vec_eval(program, columns)` module function, which fuses and
// evaluates a compiled columnar expression in one parallel chunked pass.
// Returns 0 on success, -1 with the Python error set on failure.
int init_columnar_eval(PyObject* m);

// Rebuild a dataframe::Expr from a Python post-order AST list (the columnar.py
// serialization). Returns false with a Python error set on a malformed AST.
// Only defined in the arrow-enabled build (returns false otherwise).
bool build_expr_from_ast(PyObject* ast, dftracer::utils::dataframe::Expr* out);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_COLUMNAR_EVAL_H
