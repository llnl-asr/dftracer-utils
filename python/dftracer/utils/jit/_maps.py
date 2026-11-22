# SPDX-License-Identifier: MIT
"""Scan state declarations: map, record, Store, ports and the typed event surface."""

from __future__ import annotations

import builtins
import collections
import dataclasses
import inspect
import math
import sys
import types
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    ClassVar,
    Generic,
    Literal,
    Mapping,
    Protocol,
    TypeVar,
    Union,
    cast,
    get_args,
    get_origin,
    get_type_hints,
    overload,
)

from .._c_text import c_str_literal as _c_str_literal
from .._jit_errors import JitError, class_error
from ._channels import PortDecl as _PortDecl
from ._monoids import Monoid, Product, Quantiles
from ._types import (
    K1,
    K2,
    K3,
    K4,
    MISSING,
    SK,
    SV,
    C,
    K,
    N,
    Str,
    TypeTag,
    f64,
    i64,
    int_literal,
    str_,
    u64,
)

if TYPE_CHECKING:
    from ..plugins import JSONValue


V = TypeVar("V", bound="Monoid | type[object]")


class Map(Generic[K, V]):
    __slots__ = ("key_types", "values", "is_product", "value_names")

    def __init__(
        self,
        key_types: tuple[TypeTag[object], ...],
        values: tuple[Monoid, ...],
        is_product: bool,
        value_names: tuple[str, ...] | None = None,
    ) -> None:
        self.key_types = key_types
        self.values = values
        self.is_product = is_product
        self.value_names = value_names

    @overload
    def __getitem__(self, key: K) -> V: ...
    @overload
    def __getitem__(self, key: object) -> V: ...
    def __getitem__(self, key: object) -> V:
        raise NotImplementedError

    @overload
    def __setitem__(self, key: K, value: V) -> None: ...
    @overload
    def __setitem__(self, key: object, value: V) -> None: ...
    def __setitem__(self, key: object, value: V) -> None: ...


AnyMap = Map[Any, Any]


class Event(Protocol):
    """Typed per-event surface for a ``@jit.each_event`` method. Authoring-only:
    the method body is AST-compiled, never executed, so nothing is instantiated."""

    pid: int
    tid: int
    ts: int
    dur: int
    phase: int
    has_dur: bool
    cat: Str
    name: Str
    fhash: Str
    hhash: Str

    def arg_i64(self, name: str) -> int: ...
    def arg_f64(self, name: str) -> float: ...
    def arg_str(self, name: str) -> Str: ...


class Mask(Protocol):
    """A Bool column built from comparisons in an ``@jit.each_batch`` body.
    Authoring-only: the body is AST-compiled, never executed."""

    def __and__(self, other: Mask) -> Mask: ...
    def __or__(self, other: Mask) -> Mask: ...
    def __invert__(self) -> Mask: ...
    def sum(self) -> int: ...
    def count(self) -> int: ...


class Column(Protocol[N]):
    """A numeric column of an ``@jit.each_batch`` body: ``Column[int]`` or
    ``Column[float]``. Arithmetic and comparisons build lazy column
    expressions; ``s[mask]`` filters; a reduction returns a scalar to fold into
    scan state. Authoring-only: the body is AST-compiled, never executed."""

    @overload
    def __add__(self, other: int | Column[int]) -> Column[N]: ...
    @overload
    def __add__(self, other: float | Column[float]) -> Column[float]: ...
    @overload
    def __sub__(self, other: int | Column[int]) -> Column[N]: ...
    @overload
    def __sub__(self, other: float | Column[float]) -> Column[float]: ...
    @overload
    def __mul__(self, other: int | Column[int]) -> Column[N]: ...
    @overload
    def __mul__(self, other: float | Column[float]) -> Column[float]: ...
    @overload
    def __radd__(self, other: int) -> Column[N]: ...
    @overload
    def __radd__(self, other: float) -> Column[float]: ...
    @overload
    def __rsub__(self, other: int) -> Column[N]: ...
    @overload
    def __rsub__(self, other: float) -> Column[float]: ...
    @overload
    def __rmul__(self, other: int) -> Column[N]: ...
    @overload
    def __rmul__(self, other: float) -> Column[float]: ...
    def __truediv__(self, other: float | Column[int] | Column[float]) -> Column[float]: ...
    def __rtruediv__(self, other: float) -> Column[float]: ...
    def __neg__(self) -> Column[N]: ...
    def __abs__(self) -> Column[N]: ...
    def __lt__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __le__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __gt__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __ge__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __eq__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __ne__(self, other: float | Column[int] | Column[float]) -> Mask: ...
    def __getitem__(self, mask: Mask) -> Column[N]: ...
    def abs(self) -> Column[N]: ...
    def clip(self, lo: N, hi: N) -> Column[N]: ...
    def isnull(self) -> Mask: ...
    def notnull(self) -> Mask: ...
    def fill_null(self, value: N) -> Column[N]: ...
    def where(self, mask: Mask, other: N | Column[N]) -> Column[N]: ...
    def sum(self) -> N: ...
    def min(self) -> N: ...
    def max(self) -> N: ...
    def count(self) -> int: ...
    def mean(self) -> float: ...


class Frame(Protocol):
    """The scan batch an ``@jit.each_batch`` method receives: ``df["dur"]`` is
    a numeric column. The scan columns ``pid``, ``tid``, ``ts``, ``dur`` and
    ``ph`` are ``Column[int]``; an ``args.<key>`` column is typed
    ``Column[float]``, and a sum of it folds into an int field only when the
    column holds integers at run time."""

    @overload
    def __getitem__(self, name: Literal["pid", "tid", "ts", "dur", "ph"]) -> Column[int]: ...
    @overload
    def __getitem__(self, name: str) -> Column[float]: ...


class _Record:
    __slots__ = ("names", "monoids")

    def __init__(self, names: tuple[str, ...], monoids: tuple[Monoid, ...]) -> None:
        self.names = names
        self.monoids = monoids


def record(cls: type[C]) -> type[C]:
    """Class decorator: turn monoid-valued attributes into a named product value.

    Each attribute (``count = jit.count()``) declares one value component in
    declaration order; pass the class as a :func:`map` ``value``."""
    fields = {k: v for k, v in vars(cls).items() if isinstance(v, Monoid)}
    if not fields:
        raise JitError("@jit.record needs at least one monoid attribute such as jit.count()")
    setattr(cls, "_jit_record", _Record(tuple(fields), tuple(fields.values())))
    return cls


def _value_spec(value: object) -> tuple[tuple[Monoid, ...], bool, tuple[str, ...] | None]:
    if isinstance(value, Monoid):
        return (value,), False, None
    rec = getattr(value, "_jit_record", None)
    if isinstance(rec, _Record):
        return rec.monoids, True, rec.names
    if isinstance(value, dict):
        if not value or not builtins.all(isinstance(v, Monoid) for v in value.values()):
            raise JitError("map value dict must map names to monoids such as jit.count()")
        return tuple(value.values()), True, tuple(value.keys())
    if isinstance(value, tuple) and value and builtins.all(isinstance(v, Monoid) for v in value):
        return value, True, None
    raise JitError("map value must be a monoid, a tuple/dict of monoids, or a @jit.record")


# A dict/tuple of monoids is a product (value type Product); these precede the
# generic `value: V` forms so it is not typed as a bare dict/tuple.
@overload
def map(*, key: TypeTag[K1], value: dict[str, Monoid]) -> Map[tuple[K1], Product]: ...
@overload
def map(*, key: TypeTag[K1], value: tuple[Monoid, ...]) -> Map[tuple[K1], Product]: ...
@overload
def map(*, key: tuple[TypeTag[K1]], value: dict[str, Monoid]) -> Map[tuple[K1], Product]: ...
@overload
def map(*, key: tuple[TypeTag[K1]], value: tuple[Monoid, ...]) -> Map[tuple[K1], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2]], value: dict[str, Monoid]
) -> Map[tuple[K1, K2], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2]], value: tuple[Monoid, ...]
) -> Map[tuple[K1, K2], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3]], value: dict[str, Monoid]
) -> Map[tuple[K1, K2, K3], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3]], value: tuple[Monoid, ...]
) -> Map[tuple[K1, K2, K3], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3], TypeTag[K4]], value: dict[str, Monoid]
) -> Map[tuple[K1, K2, K3, K4], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3], TypeTag[K4]], value: tuple[Monoid, ...]
) -> Map[tuple[K1, K2, K3, K4], Product]: ...
@overload
def map(*, key: TypeTag[K1], value: V) -> Map[tuple[K1], V]: ...
@overload
def map(*, key: tuple[TypeTag[K1]], value: V) -> Map[tuple[K1], V]: ...
@overload
def map(*, key: tuple[TypeTag[K1], TypeTag[K2]], value: V) -> Map[tuple[K1, K2], V]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3]], value: V
) -> Map[tuple[K1, K2, K3], V]: ...
@overload
def map(
    *, key: tuple[TypeTag[K1], TypeTag[K2], TypeTag[K3], TypeTag[K4]], value: V
) -> Map[tuple[K1, K2, K3, K4], V]: ...
@overload
def map(
    *, key: tuple[TypeTag[object], ...], value: dict[str, Monoid]
) -> Map[tuple[object, ...], Product]: ...
@overload
def map(
    *, key: tuple[TypeTag[object], ...], value: tuple[Monoid, ...]
) -> Map[tuple[object, ...], Product]: ...
@overload
def map(*, key: tuple[TypeTag[object], ...], value: V) -> Map[tuple[object, ...], V]: ...
def map(key: tuple[TypeTag[Any], ...] | TypeTag[Any], value: object) -> Map[Any, Any]:
    """Declare a keyed accumulator: a typed-tuple key, and either one value
    reduction, a tuple of reductions (a positional product), a dict of
    name->reduction, or a :func:`record` class (both named products).

    It lowers to the engine's ``AggState`` grouped by the key columns, one
    aggregate per value reduction; the host merges same-named accumulators
    across worker slices and finalizes each to a native DataFrame.

    A single-component key may be passed bare (``key=jit.i64``); it normalizes to
    the 1-tuple ``(jit.i64,)`` and the body may then use a bare subscript
    ``self.m[k]`` as well as ``self.m[(k,)]``. Multi-key maps stay tuple-only.
    An empty key tuple declares a scalar accumulator: one whole-scan row."""
    if isinstance(key, TypeTag):
        key = (key,)
    if not isinstance(key, tuple):
        raise JitError("map key must be a tuple of jit.i64 / jit.str_")
    for k in key:
        if not isinstance(k, TypeTag):
            raise JitError("map key components must be jit.i64 or jit.str_")
        if k.dft == "DFTU_T_BYTES":
            raise JitError(
                "a bytes map key has no column form; group by jit.str_ or jit.i64 instead"
            )
    values, is_product, value_names = _value_spec(value)
    if is_product and builtins.any(isinstance(v, Quantiles) for v in values):
        raise JitError("jit.quantiles is top-level only; it cannot be a product/record component")
    return Map(key, values, is_product, value_names)


class Port(Protocol):
    """Authoring-only handle for a batch-scoped inter-plugin port.

    A :func:`publish` port is written per event with ``self.<port> += <expr>`` and
    the host publishes the per-batch total for a later plugin's consume port wired
    to it; a :func:`consume` port reads that value as a plain scalar
    (``self.<port>``, 0 when no producer published this batch). Never instantiated."""

    def __iadd__(self, x: float) -> Port: ...


class Store(Generic[SK, SV]):
    """A persistent key-value field of a plugin, lowered to the plugin store ABI.

    Declare it by annotation only, ``seen: jit.Store[int, int]``; the field name
    is the namespace name and the host registers it as
    ``plugin.<plugin name>.<field name>``, so the same plugin keeps one
    namespace across runs. The plugin name is the stem of its ``.so``: the class
    identity for a plugin built from the cache, the file name for ``build(out=)``.
    Keys and values go through the :mod:`dftracer.utils.store` encoding, so
    ``Store.namespace(name, 1, durable=True, shared=True)`` of the same index
    root reads and writes it from Python as a ``Namespace[K, V]``.

    ``K`` is an ``int``, a ``str`` or a ``Tuple`` of those; ``V`` is an ``int``
    or a ``float``. Authoring-only, never instantiated: the body of
    ``@jit.each_event`` is compiled, and each use below is an explicit key.

    ``get`` reads from a snapshot taken when the batch starts, so it sees what
    earlier folds committed and not what this fold's ``put`` calls wrote, which
    land when the batch ends. A ``put`` of a key that other workers or later runs
    wrote also replaces it (last writer wins), so keep per-scan totals in plain
    state and use a ``Store`` to carry a value to the next run. The store is
    lent only inside a fold, to the one index root of the scan. A
    :class:`~dftracer.utils.Series` or any other batch value cannot be a
    stored value."""

    def get(self, key: SK, default: SV) -> SV:
        """The value of ``key`` as of this batch's snapshot, or ``default``."""
        raise NotImplementedError

    def put(self, key: SK, value: SV) -> None:
        """Write ``value`` under ``key`` when the batch ends."""
        raise NotImplementedError


class StoreField:
    """A :class:`Store` field: key components (``"i64"`` or ``"str"``), the value
    family (``"i64"`` or ``"f64"``) and its index among the plugin's stores."""

    __slots__ = ("name", "key_kinds", "value_kind", "index", "is_tuple")

    def __init__(
        self,
        name: str,
        key_kinds: tuple[str, ...],
        value_kind: str,
        index: int,
        is_tuple: bool,
    ) -> None:
        self.name = name
        self.key_kinds = key_kinds
        self.value_kind = value_kind
        self.index = index
        self.is_tuple = is_tuple


class PlainField:
    """A plain-container scan-state field: ``kind`` is ``dict``, ``set``, ``list``
    or ``scalar``; ``elem`` is the value (dict, scalar) or element (set, list)
    family; ``factory`` is whether a dict is a ``defaultdict``."""

    __slots__ = ("name", "kind", "key_types", "elem", "factory")

    def __init__(
        self,
        name: str,
        kind: str,
        key_types: tuple[TypeTag[object], ...],
        elem: str,
        factory: bool,
    ) -> None:
        self.name = name
        self.kind = kind
        self.key_types = key_types
        self.elem = elem
        self.factory = factory


_STATE_SKIP = frozenset({"config", "plan_duql"})


def _state_elem(tp: object) -> str | None:
    if tp is int:
        return "i64"
    if tp is float:
        return "f64"
    if tp is str or tp is Str:
        return "str"
    return None


def _state_key(tp: object) -> tuple[str, ...] | None:
    """The key components (``"i64"`` or ``"str"``) of a state key annotation: an
    ``int``, a ``str`` or a non-empty ``Tuple`` of those."""
    parts: tuple[object, ...] = (tp,)
    if get_origin(tp) is tuple:
        parts = get_args(tp)
        if not parts or parts == ((),) or parts[-1] is Ellipsis:
            return None
    kinds: list[str] = []
    for part in parts:
        kind = _state_elem(part)
        if kind is None or kind == "f64":
            return None
        kinds.append(kind)
    return tuple(kinds)


def resolve_hint(cls: type, attr: str, tp: object) -> object:
    """``tp`` with a string annotation evaluated in the class's module."""
    if not isinstance(tp, str):
        return tp
    module = sys.modules.get(getattr(cls, "__module__", ""))
    scope = dict(vars(module)) if module is not None else {}
    try:
        return eval(tp, scope, dict(vars(cls)))  # noqa: S307 - the author's own annotation
    except Exception as exc:
        raise class_error(
            cls,
            f"cannot resolve the annotation '{tp}' of '{attr}' ({exc}); "
            "declare the types it names at module scope",
            attr,
        ) from None


def _state_default(cls: type, field: PlainField, declared_default: bool) -> None:
    """Refuse a default that is not the empty state a worker starts from."""
    val = cls.__dict__.get(field.name, MISSING)
    if field.kind == "dict":
        if declared_default and not isinstance(val, collections.defaultdict):
            raise class_error(
                cls,
                f"'{field.name}' is a DefaultDict; its default must be defaultdict(int) "
                "or defaultdict(float)",
                field.name,
            )
        if isinstance(val, collections.defaultdict):
            want = int if field.elem == "i64" else float
            if val.default_factory is not want or val:
                raise class_error(
                    cls,
                    f"'{field.name}' must start as defaultdict({want.__name__}) with no "
                    "items; a worker slice starts empty",
                    field.name,
                )
        elif val is not MISSING and (type(val) is not dict or val):
            raise class_error(
                cls, f"'{field.name}' must start as {{}} or as an empty defaultdict", field.name
            )
        return
    if val is MISSING:
        return
    empty: object = {"set": builtins.set, "list": builtins.list}.get(field.kind)
    if empty is not None:
        if type(val) is not empty or val:
            raise class_error(
                cls, f"'{field.name}' must start empty; a worker slice starts empty", field.name
            )
        return
    if isinstance(val, bool) or not isinstance(val, (int, float)) or val != 0:
        raise class_error(
            cls,
            f"'{field.name}' must start at 0, because a non-zero start would be added once per "
            "worker; use ClassVar[int] for a constant, or config for a setting",
            field.name,
        )


_BATCH_TYPES = frozenset({"Series", "DataFrame", "LazyFrame", "Expr"})


def _is_batch_type(tp: object) -> bool:
    base = get_origin(tp) or tp
    return (
        isinstance(base, type)
        and base.__name__ in _BATCH_TYPES
        and base.__module__.startswith("dftracer.utils")
    )


def _escape_error(cls: type, attr: str, tp: object) -> JitError:
    return class_error(
        cls,
        f"'{attr}: {tp!r}' keeps a Series, DataFrame or Expr on self, but a batch value is valid "
        "only inside one step call and cannot outlive it; fold it into scan state (a Dict, Set, "
        "List or number) instead",
        attr,
    )


def _plain_field(cls: type, attr: str, tp: object) -> PlainField:
    if _is_batch_type(tp):
        raise _escape_error(cls, attr, tp)
    origin = get_origin(tp)
    args = get_args(tp)
    scalar = _state_elem(tp)
    if scalar in ("i64", "f64"):
        field = PlainField(attr, "scalar", (), scalar, False)
        _state_default(cls, field, False)
        return field
    if origin in (dict, collections.defaultdict):
        keys = _state_key(args[0]) if len(args) == 2 else None
        elem = _state_elem(args[1]) if len(args) == 2 else None
        if keys is None or elem not in ("i64", "f64"):
            raise class_error(
                cls,
                f"'{attr}' needs Dict[K, V] with K an int, a str or a Tuple of those and V an "
                f"int or a float, got {tp!r}",
                attr,
            )
        types = tuple(str_ if k == "str" else i64 for k in keys)
        field = PlainField(attr, "dict", types, elem, False)
        _state_default(cls, field, origin is collections.defaultdict)
        field.factory = isinstance(cls.__dict__.get(attr), collections.defaultdict)
        return field
    if origin in (builtins.set, builtins.list):
        elem = _state_elem(args[0]) if len(args) == 1 else None
        if elem not in ("i64", "str"):
            raise class_error(cls, f"'{attr}' needs an int or str element type, got {tp!r}", attr)
        kind = "set" if origin is builtins.set else "list"
        field = PlainField(attr, kind, (), elem, False)
        _state_default(cls, field, False)
        return field
    raise class_error(
        cls,
        f"'{attr}: {tp!r}' is not scan state; a state field is a Dict, DefaultDict, Set or List "
        "of int, float or str, a Tuple-keyed Dict, an int, a float, or a jit.Store[K, V]",
        attr,
    )


def _store_field(cls: type, attr: str, tp: object, index: int) -> StoreField:
    if attr in cls.__dict__:
        raise class_error(cls, f"'{attr}' is a jit.Store; declare it by annotation only", attr)
    if not attr or builtins.any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in attr):
        raise class_error(
            cls,
            f"the jit.Store field '{attr}' names a store namespace, so it must be lowercase "
            "ASCII [a-z0-9_]",
            attr,
        )
    args = get_args(tp)
    if len(args) == 2 and _is_batch_type(args[1]):
        raise _escape_error(cls, attr, tp)
    keys = _state_key(args[0]) if len(args) == 2 else None
    value = _state_elem(args[1]) if len(args) == 2 else None
    if keys is None or value not in ("i64", "f64"):
        raise class_error(
            cls,
            f"'{attr}' needs jit.Store[K, V] with K an int, a str or a Tuple of those and V "
            f"an int or a float, got {tp!r}",
            attr,
        )
    return StoreField(attr, keys, value, index, get_origin(args[0]) is tuple)


def own_annotations(cls: type) -> dict[str, object]:
    get = getattr(inspect, "get_annotations", None)
    if get is None:
        return dict(cls.__dict__.get("__annotations__", {}))
    if sys.version_info >= (3, 14):
        # Python 3.14 evaluates annotations lazily (PEP 649), so a class has no
        # "__annotations__" entry in its __dict__.
        import annotationlib

        return dict(get(cls, format=annotationlib.Format.FORWARDREF))
    return dict(get(cls))


def state_fields(cls: type) -> tuple[dict[str, PlainField], dict[str, StoreField]]:
    """The plain-container scan-state and :class:`Store` fields ``cls`` declares
    by annotation."""
    plains: dict[str, PlainField] = {}
    stores: dict[str, StoreField] = {}
    own = own_annotations(cls)
    hints = type_hints(cls)
    for attr in own:
        if attr in _STATE_SKIP or isinstance(
            cls.__dict__.get(attr), (Map, PortHandle, EachEvent, _PortDecl)
        ):
            continue
        tp = resolve_hint(cls, attr, hints.get(attr, own[attr]))
        if get_origin(tp) is ClassVar or tp is ClassVar:
            continue
        if get_origin(tp) is Store:
            stores[attr] = _store_field(cls, attr, tp, len(stores))
        else:
            plains[attr] = _plain_field(cls, attr, tp)
    return plains, stores


PORT_ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789._/-")

# The registry namespace reserved for host ops (see _HOST_NAME_PREFIX below);
# not available as a JitPackage namespace or an explicit consume() wire id.
RESERVED_NAMESPACE = "dftu"


class PortHandle:
    __slots__ = ("is_f64", "name", "role")

    def __init__(self, name: str | None, is_f64: bool, role: str) -> None:
        self.name = name
        self.is_f64 = is_f64
        self.role = role


def _check_port_name(fn: str, name: object) -> str:
    if not isinstance(name, str) or not name:
        raise JitError(f"jit.{fn} needs a non-empty port name such as 'com.example.edges'")
    if name == RESERVED_NAMESPACE or name.startswith(RESERVED_NAMESPACE + "."):
        raise JitError(
            f"jit.{fn} port name '{name}' uses the reserved dftu. namespace; pick your own, "
            "e.g. 'com.example.edges'"
        )
    if builtins.any(c not in PORT_ID_CHARS for c in name):
        raise JitError(f"jit.{fn} port name '{name}' must be lowercase ASCII [a-z0-9._/-]")
    return name


def _port_is_f64(fn: str, of: TypeTag[object]) -> bool:
    if of is f64:
        return True
    if of is u64 or of is i64:
        return False
    raise JitError(f"jit.{fn}(of=...) must be jit.u64, jit.i64, or jit.f64")


def publish(of: TypeTag[object] = u64) -> Port:
    """Declare a batch-scoped publish port.

    Its wire id is derived from the attribute it is assigned to (package +
    module + class + attribute, see :class:`JitPackage`), resolved when the
    enclosing class is built by :func:`plugin`. In ``each_event`` accumulate a
    per-batch total with ``self.<port> += <expr>`` (a u64 sum by default,
    ``of=jit.f64`` for a double sum). The total is published once per batch and
    reset for the next batch; ``of`` picks the wire width (u64/i64 as an 8-byte
    int, f64 as a double)."""
    return cast(Port, PortHandle(None, _port_is_f64("publish", of), "publish"))


def consume(producer: Port | str, of: TypeTag[object] = u64) -> Port:
    """Declare a batch-scoped consume port wired to ``producer``.

    ``producer`` is the other plugin's :func:`publish` attribute itself
    (``consume(Other.total)``), never a typed string - identity comes from the
    producer's object, and it must already be built (the producer class must be
    declared and decorated before this one). A string is accepted only for the
    C-interop case where the producer is a hand-written plugin with no Python
    object to reference. In ``each_event`` read the value a producer published
    for the current batch as a plain scalar ``self.<port>`` (0 when no producer
    published this batch). ``of`` must match the producer's width."""
    is_f64 = _port_is_f64("consume", of)
    if isinstance(producer, str):
        _check_port_name("consume", producer)
        return cast(Port, PortHandle(producer, is_f64, "consume"))
    port = cast(PortHandle, producer)
    if not isinstance(port, PortHandle) or port.role != "publish":
        raise JitError("jit.consume(...) needs a jit.publish attribute or an explicit wire id")
    if port.name is None:
        raise JitError(
            "jit.consume(...) references a jit.publish attribute whose plugin is not "
            "built yet; declare and decorate the producer class before this one"
        )
    if port.is_f64 != is_f64:
        raise JitError(f"jit.consume(of=...) does not match the producer's width for '{port.name}'")
    return cast(Port, PortHandle(port.name, is_f64, "consume"))


# ---- dataclass config ------------------------------------------------------

_CFG_KINDS: dict[type, str] = {bool: "bool", int: "int", float: "float", str: "str"}
_CFG_NATIVE_KIND = {
    "bool": "DFTU_VAL_BOOL",
    "int": "DFTU_VAL_I64",
    "float": "DFTU_VAL_F64",
    "str": "DFTU_VAL_STR",
}


class CfgField:
    """One field of a plugin's config dataclass: a value of ``kind`` (int,
    float, bool or str) that is optionally None, with the dataclass default."""

    __slots__ = ("name", "kind", "optional", "default", "required")

    def __init__(
        self,
        name: str,
        kind: str,
        optional: bool,
        default: int | float | bool | str | None,
        required: bool,
    ) -> None:
        self.name = name
        self.kind = kind
        self.optional = optional
        self.default = default
        self.required = required

    def value_kind(self) -> str:
        # An optional field may be sent as null, so only the key is declared.
        return "DFTU_CONFIG_ANY" if self.optional else _CFG_NATIVE_KIND[self.kind]

    def members(self) -> list[str]:
        """The DftuCfg members of one loaded instance. A prefix per member kind
        keeps every name apart: ``v_`` value, ``l_`` string length, ``h_`` set."""
        n = self.name
        if self.kind == "str":
            out = [f"char* v_{n};", f"uint32_t l_{n};"]
        elif self.kind == "float":
            out = [f"double v_{n};"]
        else:
            out = [f"int64_t v_{n};"]
        if self.optional:
            out.append(f"int h_{n};")
        return out

    def locals_(self) -> list[str]:
        """The on_batch locals the body reads (``_cfg_``, ``_has_``); a string's
        interned id is ``_cfgs_`` and is declared where it is resolved."""
        n = self.name
        out: list[str] = []
        if self.kind != "str":
            ctype = "double" if self.kind == "float" else "int64_t"
            out += [f"    const {ctype} _cfg_{n} = _c->v_{n};", f"    (void)_cfg_{n};"]
        if self.optional:
            out += [f"    const int _has_{n} = _c->h_{n};", f"    (void)_has_{n};"]
        return out

    def _c_default(self) -> str:
        d = self.default
        if d is None:
            return "0.0" if self.kind == "float" else "0"
        if self.kind == "float":
            return repr(float(d))
        if self.kind == "bool":
            return "1" if d else "0"
        return int_literal(int(d))

    def factory_reads(self) -> list[str]:
        """The factory statements that load the field into the instance's
        DftuCfg, falling back to the dataclass default when the key is absent
        and to unset when it is null."""
        n = self.name
        key = _c_str_literal(n)
        out = ["    {", f"        const dftu_value* v = dftu_obj_get(config, {key});"]
        if self.optional:
            out += [
                "        int is_null = v != NULL && v->kind == DFTU_VAL_NULL;",
                f"        _c->h_{n} = is_null ? 0 : (v != NULL ? 1 : {0 if self.default is None else 1});",
            ]
            # An optional key is declared with any kind so it may be null, which
            # leaves the host no type check to make; make it here.
            if self.kind == "str":
                ok = "v->kind == DFTU_VAL_STR"
            else:
                ok = (
                    "v->kind == DFTU_VAL_BOOL || v->kind == DFTU_VAL_I64 || v->kind == DFTU_VAL_F64"
                )
            out += [
                f"        if (v != NULL && !is_null && !({ok})) {{",
                "            destroy(inst);",
                "            return dftu_fail(ctx, DFTU_E_INVALID,",
                f"                             {_c_str_literal(f'config key {n!r} has the wrong type')});",
                "        }",
            ]
        else:
            out.append("        int is_null = 0;")
        if self.kind == "str":
            default = "" if self.default is None else str(self.default)
            blen = len(default.encode("utf-8"))
            out += [
                "        uint32_t len = 0;",
                "        const char* text = is_null ? NULL : dftu_as_str(v, &len);",
                f"        if (!text && !is_null) {{ text = {_c_str_literal(default)}; len = {blen}; }}",
                "        char* copy = (char*)malloc((size_t)len + 1);",
                "        if (!copy) {",
                "            destroy(inst);",
                '            return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        }",
                "        if (len) memcpy(copy, text, len);",
                "        copy[len] = 0;",
                f"        _c->v_{n} = copy;",
                f"        _c->l_{n} = len;",
            ]
        else:
            fn = {"float": "dftu_as_f64", "bool": "dftu_as_bool"}.get(self.kind, "dftu_as_i64")
            out.append(f"        _c->v_{n} = is_null ? 0 : {fn}(v, {self._c_default()});")
        out.append("    }")
        return out


def _cfg_kind(tp: object) -> tuple[str, bool] | None:
    """(kind, is_optional) for a supported config field type, else None."""
    if isinstance(tp, type) and tp in _CFG_KINDS:
        return _CFG_KINDS[tp], False
    union_type = getattr(types, "UnionType", None)
    origin = getattr(tp, "__origin__", None)
    if origin is Union or (union_type is not None and isinstance(tp, union_type)):
        args = [a for a in getattr(tp, "__args__", ()) if a is not type(None)]
        if len(args) == 1 and isinstance(args[0], type) and args[0] in _CFG_KINDS:
            return _CFG_KINDS[args[0]], True
    return None


def type_hints(obj: type) -> dict[str, object]:
    try:
        return get_type_hints(obj)
    # Python 3.9 and older raise KeyError for a class whose module is not in
    # sys.modules, such as one loaded with importlib.util.module_from_spec.
    except (NameError, TypeError, KeyError):
        return own_annotations(obj)


def config_fields(cls: type) -> dict[str, CfgField]:
    """The fields of the dataclass ``cls`` declares as ``config: Cfg``; empty
    when it declares none. Raises :class:`JitError` at the offending annotation
    for a config type that is not a dataclass or a field type that has no
    native form."""
    if "config" not in own_annotations(cls):
        return {}
    cfg_type = type_hints(cls).get("config")
    if not (isinstance(cfg_type, type) and dataclasses.is_dataclass(cfg_type)):
        raise class_error(
            cls,
            f"@jit.plugin config must be annotated with a dataclass, got {cfg_type!r}",
            "config",
        )
    hints = type_hints(cfg_type)
    fields: dict[str, CfgField] = {}
    for f in dataclasses.fields(cfg_type):
        if not f.name.isascii():
            raise class_error(cfg_type, f"config field '{f.name}' must have an ASCII name", f.name)
        tp = hints.get(f.name, f.type)
        kind = _cfg_kind(tp)
        if kind is None:
            raise class_error(
                cfg_type,
                f"config field '{f.name}' has type {tp!r}; a config field is an int, float, "
                "bool or str, or Optional of one",
                f.name,
            )
        if f.default_factory is not dataclasses.MISSING:
            raise class_error(
                cfg_type,
                f"config field '{f.name}' uses default_factory; give it a plain default",
                f.name,
            )
        has_default = f.default is not dataclasses.MISSING
        if (
            has_default
            and not (f.default is None and kind[1])
            and not _cfg_value_ok(kind[0], f.default)
        ):
            raise class_error(
                cfg_type,
                f"config field '{f.name}' has default {f.default!r}, not a {kind[0]}",
                f.name,
            )
        default = cast("int | float | bool | str | None", f.default) if has_default else None
        if kind[0] == "float" and isinstance(default, float) and not math.isfinite(default):
            raise class_error(cfg_type, f"config field '{f.name}' default must be finite", f.name)
        fields[f.name] = CfgField(f.name, kind[0], kind[1], default, not has_default)
    return fields


def _cfg_value_ok(kind: str, value: object) -> bool:
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "str":
        return isinstance(value, str)
    if isinstance(value, bool):
        return False
    return isinstance(value, (int, float)) if kind == "float" else isinstance(value, int)


def _check_cfg_value(
    plugin: str, field: CfgField, value: JSONValue
) -> int | float | bool | str | None:
    if value is None and field.optional:
        return None
    if not _cfg_value_ok(field.kind, value):
        expected = f"Optional[{field.kind}]" if field.optional else field.kind
        raise JitError(
            f"{plugin} config field '{field.name}' expects {expected}, got "
            f"{type(value).__name__} ({value!r})"
        )
    if field.kind == "float":
        return float(cast(float, value))
    return cast("int | bool | str", value)


def plugin_config(cls: type, config: Mapping[str, JSONValue] | None) -> dict[str, JSONValue] | None:
    """The load config for ``cls``, checked against its config dataclass.

    A plugin without a ``config: Cfg`` annotation gets ``config`` back as is.
    Otherwise every key must be a field, a required field must be present, a
    value must have its field's type (an int is accepted for a float), and the
    dataclass default fills an absent field; an optional field that is None is
    sent as null, which is how a caller turns an Optional field off even when its
    default is a value. Raises :class:`JitError` naming the field."""
    fields = getattr(cls, "_jit_config", None)
    if fields is None:
        return dict(config) if config is not None else None
    given = dict(config) if config is not None else {}
    name = cls.__name__
    for key in given:
        if key not in fields:
            raise JitError(
                f"{name} has no config field '{key}'; its fields are {', '.join(fields) or 'none'}"
            )
    out: dict[str, JSONValue] = {}
    for fname, field in fields.items():
        if fname in given:
            value = given[fname]
        elif field.required:
            raise JitError(f"{name} config field '{fname}' is required and was not given")
        else:
            value = field.default
        out[fname] = _check_cfg_value(name, field, value)
    return out


class EachEvent:
    __slots__ = ("fn", "raw")

    def __init__(self, fn: Callable[..., object], raw: bool = False) -> None:
        self.fn = fn
        self.raw = raw


@overload
def each_event(fn: Callable[..., None]) -> EachEvent: ...
@overload
def each_event(*, raw: Literal[False] = ...) -> Callable[[Callable[..., None]], EachEvent]: ...
@overload
def each_event(*, raw: Literal[True]) -> Callable[[Callable[..., str]], EachEvent]: ...
def each_event(
    fn: Callable[..., Any] | None = None, *, raw: bool = False
) -> EachEvent | Callable[[Callable[..., Any]], EachEvent]:
    """Mark the one per-event method to AST-compile.

    With ``raw=True`` the method instead returns a C++ string spliced verbatim
    into the per-row loop; available C names are ``host``, ``df``, ``e``, ``i``,
    and, for each declared accumulator ``m``, its row buffers ``_n_m``,
    ``_cap_m``, ``_k<i>_m``, ``_v<c>_m`` (plus ``_o<c>_m`` for a product and
    ``_b<c>_m`` for a by-reading reduction). ``e`` is a per-row shim populated
    from every fixed scan column (``e->pid`` etc), since a raw body cannot be
    re-lowered against the batch-resolved columns a compiled body uses. A raw
    body appends at most ``RAW_ROWS_PER_EVENT`` rows per event per
    accumulator."""
    if fn is None:
        return lambda f: EachEvent(f, raw)
    return EachEvent(fn, raw)


class EachBatch:
    __slots__ = ("fn",)

    def __init__(self, fn: Callable[..., object]) -> None:
        self.fn = fn


def each_batch(fn: Callable[..., None]) -> EachBatch:
    """Mark the one per-batch method of a :func:`vfold` or a :func:`plugin`.

    It takes ``(self, df)`` where ``df`` is the current scan batch as columns.
    On a :func:`vfold` its body folds column reductions into the class's scalar
    accumulators, e.g. ``self.total += df["dur"].sum()``. On a :func:`plugin`
    the body is a sequence of column expressions (arithmetic, comparisons,
    masks, ``s[mask]``, ``where``, ``abs``, ``clip``, ``isnull``,
    ``fill_null``) whose reductions (``sum``, ``min``, ``max``, ``count``,
    ``mean``) fold into scalar state fields or key-less ``jit.map``
    descriptors; annotate ``df`` as :class:`Frame`."""
    return EachBatch(fn)
