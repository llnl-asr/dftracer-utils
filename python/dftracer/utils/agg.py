# SPDX-License-Identifier: MIT
"""The aggregate builders: ``count()``, user-defined aggregates (``udaf``), the
pandas ``agg`` argument forms and the helpers that name and split the
aggregates of a ``select`` or a group-by. The :class:`~dftracer.utils.expr.Agg`
class itself is in :mod:`dftracer.utils.expr`, because ``Expr`` builds it."""

from __future__ import annotations

import copy
from typing import TYPE_CHECKING, Dict, Iterator, List, Mapping, Optional, Sequence, Tuple, Union

from . import dftracer_utils_ext as _ext
from .expr import AGG_CODE, Agg, AggLeaf, Col, Expr, Lit, emit_ast, wrap

if TYPE_CHECKING:
    from .dataframe import DataFrame
    from .lazyframe import LazyFrame

__all__ = ["Udaf", "count", "udaf"]


def select_aggs(items: Sequence[object]) -> "List[Agg]":
    """The aggregates of a ``select(col("a").sum(), ...)``, named as polars
    names them: an unaliased aggregate of a bare column is output under that
    column's name; two outputs under one name are refused."""
    out: List[Agg] = []
    seen: set = set()
    for item in items:
        if not isinstance(item, Agg):
            raise TypeError("select: mix of aggregates and columns")
        if item._out is None and isinstance(item.value, Col):
            item = item.alias(item.value.name)
        if item.out in seen:
            raise ValueError(f"select: two outputs named {item.out!r}; use .alias(name)")
        seen.add(item.out)
        out.append(item)
    return out


def split_aggs(expr: Expr) -> "Tuple[Expr, List[Agg]]":
    """The expression with every aggregate leaf replaced by a column
    reference ``__dftu_agg_<i>__``, and the aggregates so aliased, in order."""
    aggs: List[Agg] = []

    def rewrite(e: Expr) -> Expr:
        if isinstance(e, AggLeaf):
            aggs.append(e.agg.alias(f"__dftu_agg_{len(aggs)}__"))
            return Col(aggs[-1].out)
        if isinstance(e, Col) or isinstance(e, Lit):
            return e
        node = copy.copy(e)
        for key, value in list(vars(node).items()):
            if isinstance(value, Expr):
                setattr(node, key, rewrite(value))
        return node

    return rewrite(expr), aggs


def has_agg_leaf(expr: Expr) -> bool:
    if isinstance(expr, AggLeaf):
        return True
    return any(has_agg_leaf(v) for v in vars(expr).values() if isinstance(v, Expr))


class Udaf:
    """A registered user-defined aggregate, called over expressions inside
    ``group_by(...).agg``: ``udaf["p50"](col("dur"))`` is the :class:`Agg` that
    runs ``p50`` per group. The engine looks the name up when the group-by runs,
    so the plugin that registered it must still be loaded then."""

    __slots__ = ("_name", "_handle", "_min_args", "_max_args", "_flags")

    def __init__(
        self, name: str, handle: "_ext._UdafHandle", min_args: int, max_args: int, flags: int
    ) -> None:
        self._name = name
        self._handle = handle
        self._min_args = min_args
        self._max_args = max_args
        self._flags = flags

    @property
    def name(self) -> str:
        return self._name

    @property
    def min_args(self) -> int:
        return self._min_args

    @property
    def max_args(self) -> int:
        return self._max_args

    @property
    def flags(self) -> int:
        """The ``DFTU_AGG_*`` flag bits the UDAF registered with."""
        return self._flags

    def __call__(self, *args: Expr) -> "Agg":
        if not self._min_args <= len(args) <= self._max_args:
            want = (
                str(self._min_args)
                if self._min_args == self._max_args
                else f"{self._min_args} to {self._max_args}"
            )
            raise TypeError(f"udaf {self._name!r} takes {want} arguments, got {len(args)}")
        return UdafAgg(self._name, self._handle, tuple(wrap(a) for a in args))

    def __repr__(self) -> str:
        return f"Udaf({self._name!r})"


class _UdafRegistry(Mapping[str, Udaf]):
    """The registered user-defined aggregates by name, read only:
    ``udaf[name]``, ``udaf.get(name)``, ``name in udaf``, iteration and ``len``.
    An unknown name raises ``KeyError`` here, not when an aggregate runs."""

    def __getitem__(self, name: str) -> Udaf:
        if not isinstance(name, str):
            raise KeyError(name)
        try:
            handle, min_args, max_args, flags = _ext.udaf_lookup(name)
        except LookupError:
            raise KeyError(name) from None
        return Udaf(name, handle, min_args, max_args, flags)

    def __iter__(self) -> Iterator[str]:
        return iter(_ext.udaf_names())

    def __len__(self) -> int:
        return len(_ext.udaf_names())

    def __repr__(self) -> str:
        return f"udaf({list(self)!r})"


udaf: Mapping[str, Udaf] = _UdafRegistry()


class UdafAgg(Agg):
    """A call of a registered UDAF over ``args``; the group-by finds the UDAF
    again by ``name``."""

    def __init__(
        self,
        name: str,
        handle: "_ext._UdafHandle",
        args: Tuple[Expr, ...],
        out: "Optional[str]" = None,
    ) -> None:
        super().__init__("udaf", args[0] if args else None, out)
        self.name = name
        self.handle = handle
        self.args = args

    def alias(self, name: str) -> "Agg":
        return UdafAgg(self.name, self.handle, self.args, name)

    @property
    def out(self) -> str:
        if self._out is not None:
            return self._out
        if self.args and isinstance(self.args[0], Col):
            return f"{self.name}_{self.args[0].name}"
        return self.name

    def _spec(self, names: List[str]) -> tuple:
        asts: List[List[tuple]] = []
        for arg in self.args:
            ast: List[tuple] = []
            emit_ast(arg, names.index, ast)
            asts.append(ast)
        return (
            AGG_CODE["udaf"],
            asts[0] if asts else None,
            self.out,
            0.0,
            None,
            (self.name, asts[1:]),
        )


def count() -> Agg:
    """The group row count (``count()`` -> column ``count``)."""
    return Agg("count", None)


def to_agg(spec: object) -> Agg:
    if isinstance(spec, Agg):
        return spec
    if isinstance(spec, str):
        if ":" in spec:
            op, column = spec.split(":", 1)
            if ":" in column:
                column, out = column.split(":", 1)
                return Agg(op, Col(column), out)
            return Agg(op, Col(column), f"{op}_{column}")
        return Agg(spec, None, spec)  # e.g. "count"
    raise TypeError(f"agg spec must be an Agg or string, got {type(spec).__name__}")


# The pandas aliases for the engine's one-column aggregates, as ``agg("x")``,
# ``agg(["x", ...])``, ``agg({"col": "x"})`` and ``agg(out=("col", "x"))``
# name them. The pandas names whose engine aggregate is a sketch (median,
# quantile -> pct; nunique -> distinct) are NOT aliased: pandas gives the exact
# value and the sketch an approximate one, so those are spelled out as
# ``col("x").quantile(0.5)`` / ``.distinct()`` by the caller who accepts that,
# or taken exactly through ``GroupBy.median() / quantile(q) / nunique()``.
PANDAS_AGG_NAMES = {
    "count": "count_valid",
    "size": "count",
    "skew": "skew_unbiased",
    "kurt": "kurt_unbiased",
}


_PANDAS_AGG_APPROXIMATE = {
    "median": "quantile(0.5, 'linear')",
    "quantile": "quantile(q, 'linear')",
    "nunique": "distinct()",
}


def _agg_op_name(name: str) -> str:
    if name in ("median", "quantile"):
        raise ValueError(
            f"agg({name!r}): a quantile needs its level; spell it "
            f"col(...).{_PANDAS_AGG_APPROXIMATE[name]}, or use the group-by method .{name}()"
        )
    if name in _PANDAS_AGG_APPROXIMATE:
        raise ValueError(
            f"agg({name!r}): the engine's {_PANDAS_AGG_APPROXIMATE[name]} is a sketch "
            f"(approximate), not the exact pandas value; call col(...).{_PANDAS_AGG_APPROXIMATE[name]} "
            f"to use it knowingly, or the exact group-by method .{name}()"
        )
    return PANDAS_AGG_NAMES.get(name, name)


def _named_agg(column: str, name: str, out: str) -> Agg:
    """``(column, name)`` as an :class:`Agg` under ``out``, pandas dict / tuple
    style: ``name`` is an engine aggregate or one of its pandas aliases."""
    op = _agg_op_name(name)
    if op == "count":
        return Agg("count", None, out)
    return Agg(op, Col(column), out)


def reduce_specs(frame: "Union[DataFrame, LazyFrame]", agg: str, keys: Sequence[str]) -> List[str]:
    """The group-by spec strings of the engine aggregate ``agg`` over the
    non-key columns of ``frame``, a frame or a plan."""
    if isinstance(frame, _ext.LazyFrame):
        return _ext.LazyFrame._reduce_specs(frame, agg, list(keys))
    return _ext.DataFrame._reduce_specs(frame, agg, list(keys))


def agg_specs_from_pandas_forms(
    frame: "Union[DataFrame, LazyFrame]",
    keys: Sequence[str],
    func: object,
    named: "Dict[str, object]",
) -> "List[Union[str, Agg]]":
    """Lower the pandas ``agg`` argument forms to engine specs, flat names.
    ``frame`` is a DataFrame or LazyFrame;
    only its ``reduce_specs`` is used.

    - a string: broadcast over every eligible non-key column, each under its
      own name (``agg("sum")``);
    - a list of strings: every (column, name) pair as ``<column>_<name>``;
    - a dict ``{column: name | [names]}`` as ``<column>_<name>`` (a single
      name keeps the column's own name, as pandas does);
    - keywords ``out=("column", name)`` (also ``pd.NamedAgg``);
    - :class:`Agg` expressions and ``"op:column"`` strings pass through.
    """
    if func is None and not named:
        raise TypeError("agg() needs at least one aggregate")
    specs: "List[Union[str, Agg]]" = []
    if isinstance(func, str):
        specs.extend(reduce_specs(frame, _agg_op_name(func), keys))
    elif isinstance(func, Agg):
        specs.append(func)
    elif isinstance(func, dict):
        for column, names in func.items():
            if isinstance(names, (str, Agg)):
                if isinstance(names, Agg):
                    specs.append(names)
                else:
                    specs.append(_named_agg(column, names, column))
            else:
                for name in names:
                    specs.append(_named_agg(column, name, f"{column}_{name}"))
    elif isinstance(func, (list, tuple)):
        for item in func:
            if isinstance(item, Agg) or (isinstance(item, str) and ":" in item):
                specs.append(item)
                continue
            if not isinstance(item, str):
                raise TypeError(f"agg() list items must be names or Agg, got {type(item).__name__}")
            for spec in reduce_specs(frame, _agg_op_name(item), keys):
                column = spec.split(":")[1]
                specs.append(_named_agg(column, item, f"{column}_{item}"))
    elif func is not None:
        raise TypeError(f"agg() takes a name, a list, a dict or an Agg, got {type(func).__name__}")
    for out, spec in named.items():
        if isinstance(spec, Agg):
            specs.append(spec.alias(out))
        elif isinstance(spec, tuple) and len(spec) == 2 and isinstance(spec[1], str):
            specs.append(_named_agg(str(spec[0]), spec[1], out))
        elif hasattr(spec, "column") and hasattr(spec, "aggfunc"):
            specs.append(_named_agg(str(spec.column), str(spec.aggfunc), out))
        else:
            raise TypeError(f"agg() keyword {out!r} must be (column, name) or an Agg")
    return specs
