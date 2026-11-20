# SPDX-License-Identifier: MIT
"""No ``Any`` in a public signature of ``python/dftracer``; ``object`` only where
a dunder takes what Python hands it or where a baseline records a signature that
still takes any value.

Public means a name with no leading underscore in a module and a class that have
none. A signature is a parameter or return annotation, or an annotated
attribute. Skipped: the implementation of an overloaded function (callers see
the overloads) and the result type of a ``Callable[...]`` (the callee's
business); the parameters of a callable are scanned. The scan reads the source with ``ast``, string annotations included.
"""

import ast
from collections import Counter
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2] / "python" / "dftracer"

# Where ``Any`` stays, each with the reason.
_ANY_ALLOWED = {
    # A dataclass_transform field specifier stands for the annotated type: the
    # value it returns is assigned to an attribute of any type.
    ("utils/schemas.py", "field->"),
}

# Public ``object`` mentions left per file, each opaque: a native handle or
# capsule (the extension stub), a type annotation or TypeTag parameter (jit
# maps), an operand or user value that is checked at run time (``wrap``,
# ``as_expr``, ``Series.from_list`` and ``is_in``, array-likes of
# ``DataFrame.from_dict``, ``GroupBy.pipe`` arguments, ``Session.attach``
# plugin results), the opaque value of a Task handle and the codec.
# The count may only go down; lower it when a signature gets a precise type.
_OBJECT_BASELINE = {
    "utils/dataframe.py": 1,
    "utils/dftracer_utils_ext.pyi": 12,
    "utils/expr.py": 3,
    "utils/groupby.py": 3,
    "utils/jit/maps.py": 12,
    "utils/jit/monoids.py": 1,
    "utils/jit/ops.py": 1,
    "utils/jit/package.py": 3,
    "utils/jit_op.py": 1,
    "utils/runtime.py": 2,
    "utils/series.py": 2,
    "utils/store/codec.py": 2,
    "utils/trace/viewer.py": 1,
}


def _is_callable_subscript(node):
    value = node.value if isinstance(node, ast.Subscript) else None
    return (isinstance(value, ast.Name) and value.id == "Callable") or (
        isinstance(value, ast.Attribute) and value.attr == "Callable"
    )


def _mentions(node):
    found = []
    if isinstance(node, ast.Name) and node.id in ("Any", "object"):
        found.append(node.id)
    elif isinstance(node, ast.Attribute) and node.attr == "Any":
        found.append("Any")
    elif isinstance(node, ast.Constant) and isinstance(node.value, str):
        try:
            found += _mentions(ast.parse(node.value, mode="eval"))
        except SyntaxError:
            pass
    if _is_callable_subscript(node):
        # The parameters of a callable are part of the signature; its result is not.
        elts = getattr(node.slice, "elts", [])
        for child in [node.value, *elts[:-1]]:
            found += _mentions(child)
    else:
        for child in ast.iter_child_nodes(node):
            found += _mentions(child)
    return found


def _is_overload(node):
    return any(
        (isinstance(d, ast.Name) and d.id == "overload")
        or (isinstance(d, ast.Attribute) and d.attr == "overload")
        for d in node.decorator_list
    )


def _is_dunder(name):
    return name.startswith("__") and name.endswith("__")


def _scan(body, prefix, path, out):
    overloaded = {
        n.name
        for n in body
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and _is_overload(n)
    }
    for n in body:
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if n.name.startswith("_") and not _is_dunder(n.name):
                continue
            if n.name in overloaded and not _is_overload(n):
                continue
            a = n.args
            every = a.posonlyargs + a.args + a.kwonlyargs + [x for x in (a.vararg, a.kwarg) if x]
            for arg in every:
                if arg.annotation is not None:
                    for m in _mentions(arg.annotation):
                        out.append((path, f"{prefix}{n.name}({arg.arg})", m, n.name))
            if n.returns is not None:
                for m in _mentions(n.returns):
                    out.append((path, f"{prefix}{n.name}->", m, n.name))
        elif isinstance(n, ast.ClassDef):
            if not n.name.startswith("_"):
                _scan(n.body, f"{prefix}{n.name}.", path, out)
        elif isinstance(n, ast.AnnAssign):
            if isinstance(n.target, ast.Name) and not n.target.id.startswith("_"):
                for m in _mentions(n.annotation):
                    out.append((path, f"{prefix}{n.target.id}:", m, n.target.id))
        elif isinstance(n, ast.If):
            _scan(n.body, prefix, path, out)
            _scan(n.orelse, prefix, path, out)


def _public_signature_mentions():
    out = []
    for path in sorted([*_ROOT.rglob("*.py"), *_ROOT.rglob("*.pyi")]):
        parts = path.relative_to(_ROOT).with_suffix("").parts
        if any(p.startswith("_") and p != "__init__" for p in parts):
            continue
        rel = path.relative_to(_ROOT).as_posix()
        _scan(ast.parse(path.read_text(encoding="utf-8")).body, "", rel, out)
    return out


def test_no_any_in_a_public_signature():
    bad = [
        f"{path}: {qual} takes or returns Any"
        for path, qual, what, _name in _public_signature_mentions()
        if what == "Any" and (path, qual) not in _ANY_ALLOWED
    ]
    assert not bad, "\n".join(bad)


def test_object_in_public_signatures_only_shrinks():
    counts = Counter(
        path
        for path, _qual, what, name in _public_signature_mentions()
        if what == "object" and not _is_dunder(name)
    )
    over = {
        p: (n, _OBJECT_BASELINE.get(p, 0))
        for p, n in counts.items()
        if n > _OBJECT_BASELINE.get(p, 0)
    }
    assert not over, f"object in a public signature (found, allowed) per file: {over}"
    stale = {p: (counts.get(p, 0), n) for p, n in _OBJECT_BASELINE.items() if counts.get(p, 0) < n}
    assert not stale, f"lower _OBJECT_BASELINE (found, listed) per file: {stale}"
