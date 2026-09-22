"""The dataframe op registry, as callables.

Every engine column op and reducer lives in one name-keyed registry, alongside
any user ops authored with :func:`dftracer.utils.jit.series`. A built-in's
registry key (the ABI truth, e.g. from :func:`names`/:func:`info`, or what a C
plugin types) carries the host's `dftu.` prefix: `dftu.series.add`. `ops` is
already that namespace, so the attribute path elides the redundant prefix::

    from dftracer.utils.jit import ops

    ops.series.add(a, b)             # a built-in, dftu. elided
    ops.run("dftu.series.add", a, b) # run() / get() / info() take the full key
    ops.get("dftu.series.mul")(a, b) # bound callable
    ops.list()                       # every registry key (built-in + user)
    ops.info("dftu.series.compare")  # {name, kind, arity, signature}
    ops.mymod.zscore(a)              # a user op: its own literal path, unprefixed

`ops.dftu.series.add` does not resolve - one spelling per surface. A user op
(`@jit.series`) is reached by its own literal path either way, since it is
never under `dftu.`.

Column arguments are Series; any other operands (a scalar, an op-enum int, a
string, an int, a char) follow the op's signature. The call returns a Series for
a column op, or a Python scalar for a reducer.
"""

from typing import TYPE_CHECKING, Callable, Dict, List, Union

from .. import dftracer_utils_ext as _ext
from ..series import Series, _unwrap, _wrap

if TYPE_CHECKING:
    from ..columnar import Expr

__all__ = ["run", "get", "list", "names", "info"]

# A column argument is a Series; any other operand follows the op's signature.
Operand = Union[Series, int, float, str]
# A column op returns a Series; a reducer returns a Python scalar.
Result = Union[Series, int, float]


class _UserOp:
    """A user op authored with @jit.series: an Expr over N placeholder columns
    (__x0__ .. __xN-1__), applied to the call's Series arguments."""

    __slots__ = ("name", "n_args", "expr")

    def __init__(self, name: str, n_args: int, expr: "Expr") -> None:
        self.name = name
        self.n_args = n_args
        self.expr = expr


_USER: Dict[str, _UserOp] = {}


def _register_user(name: str, n_args: int, expr: "Expr") -> None:
    """Register a user op; rejects a name already used by a built-in or user op
    (no silent shadowing)."""
    if name in _USER or name in _ext.op_list():
        raise ValueError(f"op '{name}' is already registered")
    _USER[name] = _UserOp(name, n_args, expr)


def run(name: str, *args: Operand) -> Result:
    """Run the op `name` on `args` (Series unwrapped in, result wrapped out).
    Resolves a user op (@jit.series) before the built-in registry."""
    uop = _USER.get(name)
    if uop is not None:
        if len(args) != uop.n_args:
            raise TypeError(f"op '{name}' takes {uop.n_args} argument(s), got {len(args)}")
        source = {f"__x{i}__": a for i, a in enumerate(args)}
        return uop.expr.apply(source)
    return _wrap(_ext.op_run(name, *(_unwrap(a) for a in args)))


def get(name: str) -> Callable[..., Result]:
    """The op `name` as a bound callable, so `ops.get("dftu.series.mul")(a, b)` runs it."""

    def call(*args: Operand) -> Result:
        return run(name, *args)

    call.__name__ = name
    call.__qualname__ = f"ops.{name}"
    return call


def names() -> List[str]:
    """Every registered op's registry key (built-in and user): the full
    `dftu.<family>.<op>` form for a built-in, the literal path for a user op."""
    return [*_ext.op_list(), *_USER]


# ops.list() reads as the natural discovery call; it shadows the builtin only as
# a module attribute, never inside this module.
list = names  # noqa: A001


def info(name: str) -> Dict[str, object]:
    """{name, kind, arity, signature} for a registered op."""
    uop = _USER.get(name)
    if uop is not None:
        return {
            "name": name,
            "kind": "series",
            "arity": uop.n_args,
            "signature": "user (expr)",
        }
    return _ext.op_info(name)


_HOST_PREFIX = "dftu."


def _resolve(path: str) -> Union[str, None]:
    """The registry key `path` reaches as an attribute path: a user op's own
    literal path, else the built-in at `dftu.<path>` (the host prefix elided
    from the attribute surface). None if neither is registered - in
    particular, a path that already starts with "dftu." never resolves here,
    so the un-elided spelling does not also work."""
    if path in _USER:
        return path
    host_key = _HOST_PREFIX + path
    if host_key in _ext.op_list():
        return host_key
    return None


def _is_module(name: str) -> bool:
    """True if `name` is an attribute-path prefix, i.e. some user op is
    `name.<op>` or some built-in is `dftu.name.<op>` (e.g. "series" for the
    built-in "dftu.series.add")."""
    prefix = name + "."
    host_prefix = _HOST_PREFIX + prefix
    return any(k.startswith(prefix) for k in _USER) or any(
        k.startswith(host_prefix) for k in _ext.op_list()
    )


class _ModuleNs:
    """A module namespace: ``ops.series.add(a, b)`` runs the built-in op
    ``dftu.series.add`` (the host prefix elided); ``ops.stats.zscore(a, b)``
    runs the user op ``stats.zscore`` (its own literal path)."""

    __slots__ = ("_prefix",)

    def __init__(self, prefix: str) -> None:
        self._prefix = prefix

    def __getattr__(self, name: str) -> Union[Callable[..., Result], "_ModuleNs"]:
        full = f"{self._prefix}.{name}"
        key = _resolve(full)
        # A dotted module (`series.add`) nests one namespace per segment.
        if key is None and _is_module(full):
            return _ModuleNs(full)
        return get(key if key is not None else full)


def __getattr__(name: str):
    # A user op's literal path, or a built-in with "dftu." elided -> a bound
    # callable; ops.<module> -> a namespace.
    key = _resolve(name)
    if key is not None:
        return get(key)
    if _is_module(name):
        return _ModuleNs(name)
    raise AttributeError(f"module {__name__!r} has no op or attribute {name!r}")
