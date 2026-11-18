# SPDX-License-Identifier: MIT
"""The comparison behind the pandas and Polars parity suites.

A case runs one method on the same data in the library and in ours, and
``compare`` says how the two results differ: type, shape, dtype kind, values.
Our Series and DataFrame carry no row index, so for pandas ``compare`` also
checks that the pandas result has the default RangeIndex (``index_is_range``);
a result with labels is a difference that waits for the row index step.

The comparison is exact: NaN and null are separate tokens, a bool never equals
an int, an int never equals a float, a float must match to a relative 1e-9,
and a dtype is its Arrow spelling (width, signedness and temporal unit).
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


def arrow_kind(arrow_type: Any) -> str:
    """The comparison spelling of an Arrow type: width, signedness and
    temporal unit stay; strings fold to ``str``, nested types to ``list``."""
    text = {"double": "float64", "float": "float32", "halffloat": "float16"}.get(
        str(arrow_type), str(arrow_type)
    )
    if text.startswith("dictionary<"):
        return arrow_kind(text.split("values=")[1].split(",")[0])
    if text in ("string", "large_string", "string_view"):
        return "str"
    if text.startswith(("list", "large_list", "fixed_size_list")):
        return "list"
    if text.startswith("timestamp"):
        text = text.split(",")[0].rstrip("]") + "]"
    return text


def ours_kind(x: Any) -> str:
    return arrow_kind(x.to_arrow().type)


_PANDAS_KINDS = {
    "boolean": "bool",
    "string": "str",
    "object": "str",
    "str": "str",
}


def pandas_kind(pd: Any, dtype: Any) -> str:
    name = str(dtype)
    low = name.lower()
    if low in _PANDAS_KINDS:
        return _PANDAS_KINDS[low]
    if low.startswith("datetime64"):
        return "timestamp[" + low.split("[")[1].split(",")[0].rstrip("]") + "]"
    if low.startswith("timedelta64"):
        return "duration[" + low.split("[")[1].rstrip("]") + "]"
    return low


class _Nan:
    """The token of a floating point NaN, which is a value and not a null."""

    def __repr__(self) -> str:
        return "nan"


NAN = _Nan()


def _is_nan(v: Any) -> bool:
    return isinstance(v, float) and v != v


def _is_null(v: Any) -> bool:
    return v is None or type(v).__name__ in ("NAType", "NaTType")


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
        if _is_null(v):
            out.append(None)
        elif _is_nan(v):
            out.append(NAN)
        elif isinstance(v, (list, tuple)):
            out.append(clean(list(v)))
        else:
            out.append(v)
    return out


def _category(v: Any) -> str:
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    return type(v).__name__


def close(a: Any, b: Any) -> bool:
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    if a is None or b is None:
        return a is None and b is None
    if a is NAN or b is NAN:
        return a is b
    if _category(a) != _category(b):
        return False
    if isinstance(a, float):
        return a == b or math.isclose(a, b, rel_tol=1e-9, abs_tol=0.0)
    try:
        return bool(a == b)
    except (TypeError, ValueError):
        return False


Norm = Tuple[str, Dict[str, Any]]


def norm_ours(x: Any) -> Norm:
    if isinstance(x, LazyFrame):
        x = x.collect()
    if isinstance(x, Series):
        return "series", {"values": clean(x.to_list()), "kind": ours_kind(x)}
    if isinstance(x, DataFrame):
        cols = list(x.columns)
        return "frame", {
            "columns": cols,
            "values": {c: clean(x[c].to_list()) for c in cols},
            "kinds": {c: ours_kind(x[c]) for c in cols},
            "n": x.shape[0],
        }
    if isinstance(x, (list, tuple)):
        return "seq", {"values": clean(list(x))}
    if isinstance(x, dict):
        return "dict", {"values": {k: clean([v])[0] for k, v in x.items()}}
    if isinstance(x, DType):
        name = DType(x).name.lower()
        return "dtype", {"kind": "str" if name.endswith("string") else name}
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


def _order_key(v: Any) -> Tuple[int, str]:
    return (v is None, repr(v))


def unordered(norm: Norm) -> Norm:
    """The result with its rows in a canonical order, for a method whose row
    order the library leaves unspecified (``unique`` without
    ``maintain_order``, an inner join)."""
    kind, d = norm
    if kind == "series":
        return kind, {**d, "values": sorted(d["values"], key=_order_key)}
    if kind == "frame":
        cols = d["columns"]
        rows = sorted(
            zip(*(d["values"][c] for c in cols)), key=lambda r: [_order_key(v) for v in r]
        )
        return kind, {**d, "values": {c: [r[i] for r in rows] for i, c in enumerate(cols)}}
    return norm


def compare(ours: Any, theirs: Any, theirs_norm: Norm, is_unordered: bool = False) -> Optional[str]:
    """None when ours matches the library's result (``theirs``, normalized as
    ``theirs_norm``), else the first difference. A message starting ``[index]``
    means values, dtype and shape agree and only the row labels differ.
    ``is_unordered`` compares the rows as a multiset."""
    ok, ot = norm_ours(ours), theirs_norm
    if is_unordered:
        ok, ot = unordered(ok), unordered(ot)
    if ot[0] == "array" and ok[0] in ("series", "seq"):
        if len(ok[1]["values"]) != len(ot[1]["values"]):
            return f"[shape] length {len(ok[1]['values'])} vs {len(ot[1]['values'])}"
        if not close(ok[1]["values"], ot[1]["values"]):
            return f"[values] {ok[1]['values']} vs {ot[1]['values']}"
        return None
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
    if dtype in (pl.String, pl.Categorical):
        return "str"
    if isinstance(dtype, pl.Datetime) or dtype == pl.Datetime:
        return f"timestamp[{getattr(dtype, 'time_unit', None) or 'us'}]"
    if isinstance(dtype, pl.Duration) or dtype == pl.Duration:
        return f"duration[{getattr(dtype, 'time_unit', None) or 'us'}]"
    if dtype == pl.Date:
        return "date32[day]"
    if dtype == pl.Time:
        return "time64[ns]"
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


def load_findings(name: str, pandas_version: str = "") -> Dict[str, str]:
    """The ``kind.method#N[input] | tag | reason`` lines of a findings file
    next to the tests, as ``{case id: "tag: reason"}``. A case id written
    ``pandas2:kind.method#N[input]`` counts only under pandas 2 (``pandas3:``
    under pandas 3), and ``pandas<3.1:kind.method#N[input]`` only under a
    pandas older than 3.1; a plain line applies to every version."""
    have = tuple(int(p) for p in pandas_version.split(".")[:2] if p.isdigit())
    out: Dict[str, str] = {}
    for line in (Path(__file__).with_name(name)).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, tag, reason = (p.strip() for p in line.split(" | ", 2))
        assert tag in FINDING_TAGS, f"{name}: unknown tag {tag!r} in {line!r}"
        if ":" in key.split("#")[0]:
            only, key = key.split(":", 1)
            assert only.startswith("pandas"), f"{name}: unknown prefix in {line!r}"
            bound = only[len("pandas") :]
            if bound.startswith("<"):
                below = tuple(int(p) for p in bound[1:].split("."))
                if not have or have >= below:
                    continue
            elif not have or have[0] != int(bound):
                continue
        assert key not in out, f"{name}: {key} is listed twice"
        out[key] = f"{tag}: {reason}"
    return out
