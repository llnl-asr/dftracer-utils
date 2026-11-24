# SPDX-License-Identifier: MIT
"""JitPackage, JitPlugin and the compile of a plugin to a shared object."""

from __future__ import annotations

import builtins
from pathlib import Path
from typing import (
    TYPE_CHECKING,
    Callable,
    overload,
)

from .. import _build
from .._expr import Expr
from .._jit_errors import JitError
from ._maps import PortHandle
from ._plugin import (
    JitPlugin,
    build_plugin,
    check_namespace,
    module_of,
    module_rest,
    qualname_of,
    series_impl,
    top_level_package,
)
from ._types import C

if TYPE_CHECKING:
    from typing_extensions import TypeGuard

    from .._expr import Expr
    from .ops import _Result as Result


class JitPackage:
    """The namespace and build settings a group of jit-authored plugins ship
    under.

    Identity for anything a jit plugin declares (a :func:`map` accumulator, a
    :func:`publish` port) is package + module + class + attribute, never a
    typed string: ``<namespace>/<module-without-its-top-package>.<qualname>.<attr>``,
    all lowercase. Bare :func:`plugin` / :func:`series` use a default
    ``JitPackage()``, whose namespace is the decorated class's top-level import
    package - so ``JitPackage("mypkg")`` on code already living in package
    ``mypkg`` produces the identical identity; pass an explicit namespace only
    to ship under a different name, or to name code authored in ``__main__``
    (a script or notebook has no package, and :meth:`build` refuses a
    shippable ``.so`` from one unless a namespace is supplied).

    Also carries the build settings for :meth:`build`: ``cache_dir`` (default
    the module's JIT cache), ``cflags`` (extra compiler flags), and ``out``
    (a fixed output path instead of the content-hashed cache).
    """

    __slots__ = ("namespace", "cache_dir", "cflags", "out")

    def __init__(
        self,
        namespace: str | None = None,
        *,
        cache_dir: str | None = None,
        cflags: list[str] | None = None,
        out: str | None = None,
    ) -> None:
        if namespace is not None:
            namespace = check_namespace("JitPackage", namespace.lower())
        self.namespace = namespace
        self.cache_dir = cache_dir
        self.cflags = builtins.list(cflags) if cflags is not None else None
        self.out = out

    def _namespace_for(self, module: str) -> str:
        return self.namespace if self.namespace is not None else top_level_package(module)

    def identity(self, cls: type, attr: str | None = None) -> str:
        """The qualified identity for ``attr`` on ``cls`` (or for ``cls``
        itself when ``attr`` is None), all lowercase."""
        module = module_of(cls)
        namespace = self._namespace_for(module)
        parts = [p for p in (module_rest(module), qualname_of(cls), attr) if p]
        return (namespace + "/" + ".".join(parts)).lower()

    def shippable(self, cls: type) -> bool:
        """False for a class authored in ``__main__`` with no explicit
        namespace to fall back on - :meth:`build` refuses those."""
        return not (module_of(cls) == "__main__" and self.namespace is None)

    @overload
    def plugin(self, cls: type[C]) -> type[C]: ...
    @overload
    def plugin(self, cls: None = None) -> Callable[[type[C]], type[C]]: ...
    def plugin(self, cls: type | None = None) -> type | Callable[[type], type]:
        """Like :func:`plugin`, but names every map/port under this package."""
        if cls is None:
            return lambda c: build_plugin(c, self)
        return build_plugin(cls, self)

    @overload
    def series(self, fn: Callable[..., Expr]) -> Callable[..., Result]: ...
    @overload
    def series(
        self, fn: None = None, *, module: str | None = None
    ) -> Callable[[Callable[..., Expr]], Callable[..., Result]]: ...
    def series(
        self, fn: Callable[..., Expr] | None = None, *, module: str | None = None
    ) -> Callable[..., Result] | Callable[[Callable[..., Expr]], Callable[..., Result]]:
        """Like :func:`series`, but the default op name comes from this
        package instead of the raw ``__module__``."""
        if fn is None:
            return lambda f: series_impl(f, module, self)
        return series_impl(fn, module, self)

    def build(self, cls: type, *, out: str | None = None) -> str:
        """Compile ``cls`` (a :func:`plugin`-decorated class) to a shippable
        native ``.so`` and return its path.

        Raises :class:`JitError` for a class authored in ``__main__`` unless
        this package (or the one that decorated ``cls``) has an explicit
        namespace: a script or notebook has no import path to derive one
        from."""
        spec = getattr(cls, "_jit_plugin", None)
        if not isinstance(spec, JitPlugin):
            raise TypeError("expected a @jit.plugin-decorated class")
        if not spec.shippable:
            raise JitError(
                f"cannot build a shippable .so for '{cls.__name__}' from __main__; "
                'move it to a module, or pass JitPackage("yourname")'
            )
        return compile_plugin(
            spec, cache_dir=self.cache_dir, extra_cflags=self.cflags, out=out or self.out
        )


DEFAULT_PACKAGE = JitPackage()


# Each expression reads the batch-resolved column for that field at row `i`
# (see COLBUF_HELPERS); a column absent from the batch reads back its old


@overload
def series(fn: Callable[..., Expr]) -> Callable[..., Result]: ...
@overload
def series(
    fn: None = None, *, module: str | None = None
) -> Callable[[Callable[..., Expr]], Callable[..., Result]]: ...
def series(
    fn: Callable[..., Expr] | None = None, *, module: str | None = None
) -> Callable[..., Result] | Callable[[Callable[..., Expr]], Callable[..., Result]]:
    """Author a reusable column op from an expression over its arguments.

    The body builds a lazy column expression from its Series arguments using the
    columnar DSL (arithmetic, comparisons, clip/cast/fillna, prims), e.g.::

        @jit.series
        def doubled(dur):
            return dur * 2

    It returns a callable, so ``doubled(s)`` applies it via the engine's Expr
    evaluator - no compile step, and it fuses with built-in Expr math.

    It also registers in dftracer.utils.jit.ops under ``<module>.<name>``, where
    the module defaults to the defining function's :class:`JitPackage` (a bare
    ``fn.__module__`` for a default package); pass ``module="stats"`` to choose
    it explicitly, or use :meth:`JitPackage.series` for a named package. The
    registry is shared with the engine's built-in ops, so a user op never takes
    a bare name, and the host's ``dftu.`` namespace is refused. Reach it as
    ``ops.run("stats.<name>", s)``, ``ops.stats.<name>(s)`` and
    ``s.ops.stats.<name>()``. Arguments are positional column operands."""
    if fn is None:
        return lambda f: series_impl(f, module, DEFAULT_PACKAGE)
    return series_impl(fn, module, DEFAULT_PACKAGE)


@overload
def plugin(cls: type[C]) -> type[C]: ...
@overload
def plugin(cls: None = None) -> Callable[[type[C]], type[C]]: ...
def plugin(cls: type | None = None) -> type | Callable[[type], type]:
    """Class decorator: AST-compile the state, the ``each_event`` or ``each_batch``
    body and any ``finalize`` and ``task`` methods to a C plugin.

    Attaches the emitted source under ``cls._jit_plugin``; the native ``.so`` is
    built lazily on first load. The batch columns the body reads are declared
    for you, from the same AST pass that emits it, so the scan materializes
    only those. Raises :class:`JitError` for any construct outside the
    supported subset.
    """
    if cls is None:
        return lambda c: build_plugin(c, DEFAULT_PACKAGE)
    return build_plugin(cls, DEFAULT_PACKAGE)


def plugin_result_names(obj: object) -> dict[str, str]:
    """``{wire id: attribute name}`` for a ``@jit.plugin`` class's maps, so a
    host can rewrite its package-qualified result keys back to the attribute
    name callers index ``results[...]`` with."""
    spec = getattr(obj, "_jit_plugin", None)
    if isinstance(spec, JitPlugin):
        return spec.result_names
    return {}


def compile_plugin(
    spec: JitPlugin,
    *,
    cache_dir: str | Path | None = None,
    extra_cflags: list[str] | None = None,
    out: str | None = None,
) -> str:
    """Build ``spec`` to a native ``.so`` and return its path.

    With ``cache_dir``/``extra_cflags``/``out`` unset, this is a content-hashed
    cache build (an unchanged plugin never rebuilds); pass them (see
    :class:`JitPackage`) for a fixed output path or extra compiler flags.
    Raises :class:`JitError` if the compile fails.
    """
    try:
        return _build.compile_source(
            spec.source,
            spec.name,
            out=out,
            cache=cache_dir,
            extra_flags=builtins.list(extra_cflags) if extra_cflags else (),
        )
    except _build.BuildError as exc:
        raise JitError(f"jit compile failed:\n{exc}", locate=False) from exc


def compile_class(obj: object) -> str:
    """Compile a ``@jit.plugin`` class to a native ``.so`` and return the path."""
    spec = getattr(obj, "_jit_plugin", None)
    if not isinstance(spec, JitPlugin):
        raise TypeError("expected a @jit.plugin-decorated class")
    return compile_plugin(spec)


def build(cls: type, *, out: str | None = None) -> str:
    """Compile ``cls`` to a shippable native ``.so`` under the default package.

    Equivalent to ``JitPackage().build(cls, out=out)``; raises :class:`JitError`
    for a class authored in ``__main__`` (pass a :class:`JitPackage` there
    instead - see :meth:`JitPackage.build`)."""
    return DEFAULT_PACKAGE.build(cls, out=out)


def is_jit_plugin(obj: object) -> TypeGuard[type]:
    """True if ``obj`` is a ``@jit.plugin``-decorated class."""
    return isinstance(getattr(obj, "_jit_plugin", None), JitPlugin)


def provided_names(cls: type) -> dict[str, str]:
    """``{attribute: qualified identity}`` for every name ``cls`` provides: each
    :func:`map` accumulator and each :func:`publish` port, already resolved
    under whichever :class:`JitPackage` decorated it."""
    spec = getattr(cls, "_jit_plugin", None)
    if not isinstance(spec, JitPlugin):
        raise TypeError("expected a @jit.plugin-decorated class")
    names: dict[str, str] = {attr: qid for qid, attr in spec.result_names.items()}
    for attr, val in vars(cls).items():
        if isinstance(val, PortHandle) and val.role == "publish" and val.name is not None:
            names[attr] = val.name
    return names
