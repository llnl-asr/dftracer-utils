# SPDX-License-Identifier: MIT
"""Author a DFTracer plugin in Python and get a native one.

Decorate a class with :func:`plugin`; its scan state and its one
:func:`each_event` or :func:`each_batch` method are AST-compiled to a C plugin
against the stable ABI, built to a cached ``.so``, and loaded through
:class:`dftracer.utils.plugins.Plugins` exactly like a hand-written plugin.

A body is a deliberately small subset: conditions, locals, ``raise`` and ``try``,
files and JSON values, then state updates. Anything outside it raises
:class:`JitError` at decoration time, with the line and column, never a silent
miscompile. The guide is ``docs/source/jit.rst``.

Scan state is a :func:`map` descriptor or, with the merge inferred from the update
code, an annotated plain field (``counts: Dict[int, int] = defaultdict(int)``,
``names: Set[str] = set()``, ``total: int = 0``); a persistent key-value field is
``seen: Store[int, int]``, and the settings of a load are ``config: Cfg`` for a
dataclass ``Cfg``. :func:`finalize` and :func:`task` methods run after the scan, and
:func:`sender`, :func:`receiver`, :func:`latest` and :func:`phase` connect plugins.

Every :func:`map` is one named accumulator on the dataframe engine's ``AggState``,
reached through the host's ``DFTU_SVC_AGG`` service: the key tuple names its
grouping columns (an empty tuple makes it a whole-scan scalar) and each value
reduction is one aggregate. The host merges same-named accumulators across worker
slices and finalizes each to a frame.

Example::

    @jit.plugin
    class NameEdges:
        edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)] += 1
"""

from __future__ import annotations

from .._jit_errors import Cancelled as Cancelled
from .._jit_errors import Closed as Closed
from .._jit_errors import Error as Error
from .._jit_errors import Fatal as Fatal
from .._jit_errors import IoError as IoError
from .._jit_errors import JitError as JitError
from .._jit_errors import NotFound as NotFound
from .._jit_errors import ParseError as ParseError
from .._jit_errors import SkipRest as SkipRest
from .._jit_op import JitOpError as JitOpError
from .._jit_op import Op as Op
from .._jit_op import can_fuse as can_fuse
from .._jit_op import compile_op as compile_op
from .._jit_op import op as op
from .._jit_op import run_op as run_op
from .._public import set_home as _set_home
from . import ops as ops
from ._channels import Channel as Channel
from ._channels import Latest as Latest
from ._channels import Phase as Phase
from ._channels import Receiver as Receiver
from ._channels import Sender as Sender
from ._channels import channel as channel
from ._channels import latest as latest
from ._channels import phase as phase
from ._channels import receiver as receiver
from ._channels import sender as sender
from ._flow import File as File
from ._flow import Value as Value
from ._flow import all as all
from ._flow import any as any
from ._flow import emit as emit
from ._flow import finalize as finalize
from ._flow import json as json
from ._flow import open as open
from ._flow import sleep as sleep
from ._flow import task as task
from ._flow import yaml as yaml
from ._maps import (
    Column as Column,
)
from ._maps import (
    Event as Event,
)
from ._maps import (
    Frame as Frame,
)
from ._maps import (
    Map as Map,
)
from ._maps import (
    Mask as Mask,
)
from ._maps import (
    Port as Port,
)
from ._maps import (
    Store as Store,
)
from ._maps import (
    consume as consume,
)
from ._maps import (
    each_batch as each_batch,
)
from ._maps import (
    each_event as each_event,
)
from ._maps import (
    map as map,
)
from ._maps import (
    plugin_config as plugin_config,
)
from ._maps import (
    publish as publish,
)
from ._maps import (
    record as record,
)
from ._monoids import (
    ApproxTopK as ApproxTopK,
)
from ._monoids import (
    ArgMax as ArgMax,
)
from ._monoids import (
    ArgMin as ArgMin,
)
from ._monoids import (
    BottomK as BottomK,
)
from ._monoids import (
    Counter as Counter,
)
from ._monoids import (
    Distinct as Distinct,
)
from ._monoids import (
    ListV as ListV,
)
from ._monoids import (
    Max as Max,
)
from ._monoids import (
    Mean as Mean,
)
from ._monoids import (
    Min as Min,
)
from ._monoids import (
    Product as Product,
)
from ._monoids import (
    Quantiles as Quantiles,
)
from ._monoids import (
    Sample as Sample,
)
from ._monoids import (
    SetV as SetV,
)
from ._monoids import (
    Stddev as Stddev,
)
from ._monoids import (
    Sum as Sum,
)
from ._monoids import (
    TopK as TopK,
)
from ._monoids import (
    Variance as Variance,
)
from ._monoids import (
    active as active,
)
from ._monoids import (
    approx_topk as approx_topk,
)
from ._monoids import (
    argmax as argmax,
)
from ._monoids import (
    argmin as argmin,
)
from ._monoids import (
    bitset as bitset,
)
from ._monoids import (
    bottomk as bottomk,
)
from ._monoids import (
    busy as busy,
)
from ._monoids import (
    concurrency as concurrency,
)
from ._monoids import (
    corr as corr,
)
from ._monoids import (
    count as count,
)
from ._monoids import (
    count_valid as count_valid,
)
from ._monoids import (
    covar_pop as covar_pop,
)
from ._monoids import (
    covar_samp as covar_samp,
)
from ._monoids import (
    distinct as distinct,
)
from ._monoids import (
    first as first,
)
from ._monoids import (
    hist as hist,
)
from ._monoids import (
    kurt as kurt,
)
from ._monoids import (
    last as last,
)
from ._monoids import (
    list as list,
)
from ._monoids import (
    max as max,
)
from ._monoids import (
    maxf as maxf,
)
from ._monoids import (
    mean as mean,
)
from ._monoids import (
    min as min,
)
from ._monoids import (
    minf as minf,
)
from ._monoids import (
    prod as prod,
)
from ._monoids import (
    quantiles as quantiles,
)
from ._monoids import (
    regr_intercept as regr_intercept,
)
from ._monoids import (
    regr_r2 as regr_r2,
)
from ._monoids import (
    regr_slope as regr_slope,
)
from ._monoids import (
    sample as sample,
)
from ._monoids import (
    set as set,
)
from ._monoids import (
    skew as skew,
)
from ._monoids import (
    std as std,
)
from ._monoids import (
    stddev as stddev,
)
from ._monoids import (
    sum as sum,
)
from ._monoids import (
    sumsq as sumsq,
)
from ._monoids import (
    topk as topk,
)
from ._monoids import (
    utilization as utilization,
)
from ._monoids import (
    var as var,
)
from ._monoids import (
    variance as variance,
)
from ._package import (
    JitPackage as JitPackage,
)
from ._package import (
    build as build,
)
from ._package import (
    compile_class as compile_class,
)
from ._package import (
    is_jit_plugin as is_jit_plugin,
)
from ._package import (
    plugin as plugin,
)
from ._package import (
    plugin_result_names as plugin_result_names,
)
from ._package import (
    provided_names as provided_names,
)
from ._package import (
    series as series,
)
from ._prims import (
    abs as abs,
)
from ._prims import (
    align_down as align_down,
)
from ._prims import (
    align_up as align_up,
)
from ._prims import (
    bit_width as bit_width,
)
from ._prims import (
    ceil_pow2 as ceil_pow2,
)
from ._prims import (
    clamp as clamp,
)
from ._prims import (
    clampf as clampf,
)
from ._prims import (
    clz as clz,
)
from ._prims import (
    copysign as copysign,
)
from ._prims import (
    ctz as ctz,
)
from ._prims import (
    div_ceil as div_ceil,
)
from ._prims import (
    div_round as div_round,
)
from ._prims import (
    exp as exp,
)
from ._prims import (
    fastrange as fastrange,
)
from ._prims import (
    floor_pow2 as floor_pow2,
)
from ._prims import (
    fma as fma,
)
from ._prims import (
    fmax as fmax,
)
from ._prims import (
    fmin as fmin,
)
from ._prims import (
    gcd as gcd,
)
from ._prims import (
    ilog2 as ilog2,
)
from ._prims import (
    isqrt as isqrt,
)
from ._prims import (
    lerp as lerp,
)
from ._prims import (
    log as log,
)
from ._prims import (
    log2 as log2,
)
from ._prims import (
    mix64 as mix64,
)
from ._prims import (
    mul_hi as mul_hi,
)
from ._prims import (
    popcount as popcount,
)
from ._prims import (
    rotl as rotl,
)
from ._prims import (
    rotr as rotr,
)
from ._prims import (
    sqrt as sqrt,
)
from ._summary import (
    EQ as EQ,
)
from ._summary import (
    GE as GE,
)
from ._summary import (
    GT as GT,
)
from ._summary import (
    LE as LE,
)
from ._summary import (
    LT as LT,
)
from ._summary import (
    Cond as Cond,
)
from ._summary import (
    summary as summary,
)
from ._types import (
    NONE as NONE,
)
from ._types import (
    Str as Str,
)
from ._types import (
    bytes as bytes,
)
from ._types import (
    f32 as f32,
)
from ._types import (
    f64 as f64,
)
from ._types import (
    i8 as i8,
)
from ._types import (
    i16 as i16,
)
from ._types import (
    i32 as i32,
)
from ._types import (
    i64 as i64,
)
from ._types import (
    str_ as str_,
)
from ._types import (
    u8 as u8,
)
from ._types import (
    u16 as u16,
)
from ._types import (
    u32 as u32,
)
from ._types import (
    u64 as u64,
)
from ._vfold import (
    vfold as vfold,
)

__all__ = [
    "Cond",
    "EQ",
    "GE",
    "GT",
    "LE",
    "LT",
    "summary",
    "JitError",
    "Error",
    "NotFound",
    "IoError",
    "ParseError",
    "Fatal",
    "SkipRest",
    "Cancelled",
    "task",
    "finalize",
    "emit",
    "sleep",
    "all",
    "any",
    "open",
    "json",
    "yaml",
    "File",
    "Value",
    "Op",
    "JitOpError",
    "compile_class",
    "is_jit_plugin",
    "plugin_config",
    "plugin_result_names",
    "provided_names",
    "op",
    "can_fuse",
    "compile_op",
    "run_op",
    "plugin",
    "JitPackage",
    "build",
    "map",
    "publish",
    "consume",
    "Port",
    "Sender",
    "Receiver",
    "Channel",
    "Latest",
    "Phase",
    "sender",
    "receiver",
    "channel",
    "latest",
    "phase",
    "Closed",
    "count",
    "sum",
    "min",
    "max",
    "minf",
    "maxf",
    "distinct",
    "set",
    "list",
    "mean",
    "variance",
    "var",
    "stddev",
    "std",
    "quantiles",
    "skew",
    "kurt",
    "sumsq",
    "hist",
    "busy",
    "concurrency",
    "utilization",
    "active",
    "first",
    "last",
    "count_valid",
    "argmin",
    "argmax",
    "topk",
    "bottomk",
    "approx_topk",
    "sample",
    "bitset",
    "prod",
    "corr",
    "covar_pop",
    "covar_samp",
    "regr_slope",
    "regr_intercept",
    "regr_r2",
    "record",
    "each_event",
    "vfold",
    "each_batch",
    "series",
    "ilog2",
    "bit_width",
    "clz",
    "ctz",
    "popcount",
    "ceil_pow2",
    "floor_pow2",
    "rotl",
    "rotr",
    "abs",
    "clamp",
    "div_ceil",
    "div_round",
    "align_up",
    "align_down",
    "isqrt",
    "gcd",
    "mul_hi",
    "mix64",
    "fastrange",
    "fmin",
    "fmax",
    "clampf",
    "sqrt",
    "log2",
    "log",
    "exp",
    "fma",
    "lerp",
    "copysign",
    "Map",
    "Store",
    "Event",
    "Frame",
    "Column",
    "Mask",
    "Str",
    "Counter",
    "Sum",
    "Min",
    "Max",
    "Distinct",
    "SetV",
    "ListV",
    "Mean",
    "Variance",
    "Stddev",
    "Quantiles",
    "ArgMin",
    "ArgMax",
    "TopK",
    "BottomK",
    "ApproxTopK",
    "Sample",
    "Product",
    "i64",
    "str_",
    "bytes",
    "i8",
    "i16",
    "i32",
    "u8",
    "u16",
    "u32",
    "u64",
    "f32",
    "f64",
    "NONE",
]

JitError.__module__ = __name__
for _runtime_exc in (Error, NotFound, Closed, IoError, ParseError, Fatal, SkipRest, Cancelled):
    _runtime_exc.__module__ = __name__

_set_home(__name__, globals(), __all__)
