# SPDX-License-Identifier: MIT
"""The comparison behind the pandas and Polars parity suites.

A case runs one method on the same data in the library and in ours, and
``compare`` says how the two results differ: type, shape, dtype kind, values.
Our Series and DataFrame carry no row index, so for pandas ``compare`` also
checks that the pandas result has the default RangeIndex (``index_is_range``);
a result with labels is a difference that waits for the row index step.
"""

import math
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

from dftracer.utils import DataFrame, LazyFrame, Series
from dftracer.utils.enums import DType


class R:
    """An argument built from the receiver: ``R(lambda r: r["a"] > 1)`` is a
    mask of the frame in pandas and in ours alike."""

    def __init__(self, fn: Callable[[Any], Any]) -> None:
        self.fn = fn


def resolve(arg: Any, receiver: Any) -> Any:
    if isinstance(arg, R):
        return arg.fn(receiver)
    if isinstance(arg, list):
        return [resolve(a, receiver) for a in arg]
    if isinstance(arg, tuple):
        return tuple(resolve(a, receiver) for a in arg)
    if isinstance(arg, dict):
        return {k: resolve(v, receiver) for k, v in arg.items()}
    return arg


def ours_kind(dtype: DType) -> str:
    name = DType(dtype).name
    if name == "BOOL":
        return "bool"
    if "INT" in name and "LIST" not in name and "BINARY" not in name:
        return "int"
    if name.startswith("FLOAT") or name.startswith("DECIMAL"):
        return "float"
    if name in ("STRING", "LARGE_STRING"):
        return "str"
    if name.startswith("DATE") or name in ("TIMESTAMP", "TIME32", "TIME64"):
        return "datetime"
    if name == "DURATION":
        return "timedelta"
    return name.lower()


def pandas_kind(pd: Any, dtype: Any) -> str:
    api = pd.api.types
    if api.is_bool_dtype(dtype):
        return "bool"
    if api.is_integer_dtype(dtype):
        return "int"
    if api.is_float_dtype(dtype):
        return "float"
    if api.is_datetime64_any_dtype(dtype):
        return "datetime"
    if api.is_timedelta64_dtype(dtype):
        return "timedelta"
    if api.is_string_dtype(dtype) or api.is_object_dtype(dtype):
        return "str"
    return str(dtype)


def _null(v: Any) -> bool:
    if v is None:
        return True
    try:
        return bool(v != v)
    except (TypeError, ValueError):
        return False


def clean(values: List[Any]) -> List[Any]:
    out = []
    for v in values:
        if hasattr(v, "tolist") and not isinstance(v, (str, bytes)):
            v = v.tolist()
        elif hasattr(v, "item") and not isinstance(v, (str, bytes)):
            try:
                v = v.item()
            except (ValueError, AttributeError):
                pass
        if _null(v) or type(v).__name__ == "NAType" or type(v).__name__ == "NaTType":
            out.append(None)
        elif isinstance(v, (list, tuple)):
            out.append(clean(list(v)))
        else:
            out.append(v)
    return out


def close(a: Any, b: Any) -> bool:
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    if a is None or b is None:
        return a is None and b is None
    if isinstance(a, bool) or isinstance(b, bool):
        return bool(a) == bool(b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(a, b, rel_tol=1e-9, abs_tol=1e-9)
    try:
        return bool(a == b)
    except (TypeError, ValueError):
        return False


Norm = Tuple[str, Dict[str, Any]]


def norm_ours(x: Any) -> Norm:
    if isinstance(x, LazyFrame):
        x = x.collect()
    if isinstance(x, Series):
        return "series", {"values": clean(x.to_list()), "kind": ours_kind(x.type)}
    if isinstance(x, DataFrame):
        cols = list(x.columns)
        return "frame", {
            "columns": cols,
            "values": {c: clean(x[c].to_list()) for c in cols},
            "kinds": {c: ours_kind(x[c].type) for c in cols},
            "n": x.shape[0],
        }
    if isinstance(x, (list, tuple)):
        return "seq", {"values": clean(list(x))}
    if isinstance(x, dict):
        return "dict", {"values": {k: clean([v])[0] for k, v in x.items()}}
    if isinstance(x, DType):
        return "dtype", {"kind": ours_kind(x)}
    return "scalar", {"values": clean([x])[0]}


def norm_pandas(pd: Any, x: Any) -> Norm:
    if isinstance(x, pd.Series):
        values = clean(x.tolist())
        kind = pandas_kind(pd, x.dtype)
        if kind == "str" and any(isinstance(v, list) for v in values):
            kind = "list"
        return "series", {"values": values, "kind": kind, "range": _is_range(pd, x.index)}
    if isinstance(x, pd.DataFrame):
        cols = [str(c) for c in x.columns]
        return "frame", {
            "columns": cols,
            "values": {str(c): clean(x[c].tolist()) for c in x.columns},
            "kinds": {str(c): pandas_kind(pd, x[c].dtype) for c in x.columns},
            "n": len(x),
            "range": _is_range(pd, x.index),
        }
    if isinstance(x, (list, tuple)):
        return "seq", {"values": clean(list(x))}
    if getattr(x, "ndim", 0) == 1 and hasattr(x, "tolist") and hasattr(x, "dtype"):
        return "array", {"values": clean(x.tolist())}
    if isinstance(x, dict):
        return "dict", {"values": {k: clean([v])[0] for k, v in x.items()}}
    if (
        isinstance(x, pd.api.extensions.ExtensionDtype)
        or hasattr(x, "kind")
        and hasattr(x, "itemsize")
    ):
        return "dtype", {"kind": pandas_kind(pd, x)}
    return "scalar", {"values": clean([x])[0]}


def _is_range(pd: Any, idx: Any) -> bool:
    return isinstance(idx, pd.RangeIndex) and idx.start == 0 and idx.step == 1


def compare(ours: Any, theirs: Any, theirs_norm: Norm) -> Optional[str]:
    """None when ours matches the library's result (``theirs``, normalized as
    ``theirs_norm``), else the first difference. A message starting ``[index]``
    means values, dtype and shape agree and only the row labels differ."""
    ok, ot = norm_ours(ours), theirs_norm
    if ot[0] == "array" and ok[0] in ("series", "seq"):
        ot = ("array", ot[1])
        return (
            None
            if close(ok[1]["values"], ot[1]["values"])
            else (f"[values] {ok[1]['values']} vs {ot[1]['values']}")
        )
    if ok[0] != ot[0]:
        return f"[type] ours {type(ours).__name__} ({ok[0]}), library {type(theirs).__name__} ({ot[0]})"
    a, b = ok[1], ot[1]
    kind = ok[0]
    if kind == "series":
        if len(a["values"]) != len(b["values"]):
            return f"[shape] length {len(a['values'])} vs {len(b['values'])}"
        if a["kind"] != b["kind"]:
            return f"[dtype] {a['kind']} vs {b['kind']}"
        if not close(a["values"], b["values"]):
            return f"[values] {a['values']} vs {b['values']}"
        if not b.get("range", True):
            return "[index] the library result has row labels"
    elif kind == "frame":
        if a["columns"] != b["columns"]:
            return f"[shape] columns {a['columns']} vs {b['columns']}"
        if a["n"] != b["n"]:
            return f"[shape] rows {a['n']} vs {b['n']}"
        for c in a["columns"]:
            if a["kinds"][c] != b["kinds"][c]:
                return f"[dtype] {c}: {a['kinds'][c]} vs {b['kinds'][c]}"
            if not close(a["values"][c], b["values"][c]):
                return f"[values] {c}: {a['values'][c]} vs {b['values'][c]}"
        if not b.get("range", True):
            return "[index] the library result has row labels"
    elif kind == "dtype":
        if a["kind"] != b["kind"]:
            return f"[dtype] {a['kind']} vs {b['kind']}"
    elif not close(a["values"], b["values"]):
        return f"[values] {a['values']} vs {b['values']}"
    return None


def polars_kind(pl: Any, dtype: Any) -> str:
    if dtype == pl.Boolean:
        return "bool"
    if dtype.is_integer():
        return "int"
    if dtype.is_float() or dtype.is_decimal():
        return "float"
    if dtype in (pl.String, pl.Categorical):
        return "str"
    if dtype == pl.Duration or isinstance(dtype, pl.Duration):
        return "timedelta"
    if dtype.is_temporal():
        return "datetime"
    if isinstance(dtype, pl.List):
        return "list"
    return str(dtype).lower()


def norm_polars(pl: Any, x: Any) -> Norm:
    if isinstance(x, pl.LazyFrame):
        x = x.collect()
    if isinstance(x, pl.Series):
        return "series", {"values": clean(x.to_list()), "kind": polars_kind(pl, x.dtype)}
    if isinstance(x, pl.DataFrame):
        return "frame", {
            "columns": list(x.columns),
            "values": {c: clean(x[c].to_list()) for c in x.columns},
            "kinds": {c: polars_kind(pl, x[c].dtype) for c in x.columns},
            "n": x.height,
        }
    if isinstance(x, (list, tuple)):
        return "seq", {"values": clean(list(x))}
    if isinstance(x, dict):
        return "dict", {"values": {k: clean([v])[0] for k, v in x.items()}}
    if isinstance(x, pl.DataType) or (isinstance(x, type) and issubclass(x, pl.DataType)):
        return "dtype", {"kind": polars_kind(pl, x)}
    return "scalar", {"values": clean([x])[0]}


FINDING_TAGS = ("needs-index", "keyword", "missing", "shape", "semantics", "engine")


def load_findings(name: str) -> Dict[str, str]:
    """The ``kind.method | tag | reason`` lines of a findings file next to the
    tests, as ``{kind.method: "tag: reason"}``."""
    out: Dict[str, str] = {}
    for line in (Path(__file__).with_name(name)).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, tag, reason = (p.strip() for p in line.split(" | ", 2))
        assert tag in FINDING_TAGS, f"{name}: unknown tag {tag!r} in {line!r}"
        assert key not in out, f"{name}: {key} is listed twice"
        out[key] = f"{tag}: {reason}"
    return out
