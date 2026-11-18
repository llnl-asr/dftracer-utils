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
from ..jit_op import Op as Op
from ..jit_op import compile_op as compile_op
from ..jit_op import op as op
from ..jit_op import run_op as run_op
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
from .maps import (
    Column as Column,
)
from .maps import (
    Event as Event,
)
from .maps import (
    Frame as Frame,
)
from .maps import (
    Map as Map,
)
from .maps import (
    Mask as Mask,
)
from .maps import (
    Port as Port,
)
from .maps import (
    Store as Store,
)
from .maps import (
    consume as consume,
)
from .maps import (
    each_batch as each_batch,
)
from .maps import (
    each_event as each_event,
)
from .maps import (
    map as map,
)
from .maps import (
    plugin_config as plugin_config,
)
from .maps import (
    publish as publish,
)
from .maps import (
    record as record,
)
from .monoids import (
    ApproxTopK as ApproxTopK,
)
from .monoids import (
    ArgMax as ArgMax,
)
from .monoids import (
    ArgMin as ArgMin,
)
from .monoids import (
    BottomK as BottomK,
)
from .monoids import (
    Counter as Counter,
)
from .monoids import (
    Distinct as Distinct,
)
from .monoids import (
    ListV as ListV,
)
from .monoids import (
    Max as Max,
)
from .monoids import (
    Mean as Mean,
)
from .monoids import (
    Min as Min,
)
from .monoids import (
    Product as Product,
)
from .monoids import (
    Quantiles as Quantiles,
)
from .monoids import (
    Sample as Sample,
)
from .monoids import (
    SetV as SetV,
)
from .monoids import (
    Stddev as Stddev,
)
from .monoids import (
    Sum as Sum,
)
from .monoids import (
    TopK as TopK,
)
from .monoids import (
    Variance as Variance,
)
from .monoids import (
    active as active,
)
from .monoids import (
    approx_topk as approx_topk,
)
from .monoids import (
    argmax as argmax,
)
from .monoids import (
    argmin as argmin,
)
from .monoids import (
    bitset as bitset,
)
from .monoids import (
    bottomk as bottomk,
)
from .monoids import (
    busy as busy,
)
from .monoids import (
    concurrency as concurrency,
)
from .monoids import (
    corr as corr,
)
from .monoids import (
    count as count,
)
from .monoids import (
    count_valid as count_valid,
)
from .monoids import (
    covar_pop as covar_pop,
)
from .monoids import (
    covar_samp as covar_samp,
)
from .monoids import (
    distinct as distinct,
)
from .monoids import (
    first as first,
)
from .monoids import (
    hist as hist,
)
from .monoids import (
    kurt as kurt,
)
from .monoids import (
    last as last,
)
from .monoids import (
    list as list,
)
from .monoids import (
    max as max,
)
from .monoids import (
    maxf as maxf,
)
from .monoids import (
    mean as mean,
)
from .monoids import (
    min as min,
)
from .monoids import (
    minf as minf,
)
from .monoids import (
    prod as prod,
)
from .monoids import (
    quantiles as quantiles,
)
from .monoids import (
    regr_intercept as regr_intercept,
)
from .monoids import (
    regr_r2 as regr_r2,
)
from .monoids import (
    regr_slope as regr_slope,
)
from .monoids import (
    sample as sample,
)
from .monoids import (
    set as set,
)
from .monoids import (
    skew as skew,
)
from .monoids import (
    std as std,
)
from .monoids import (
    stddev as stddev,
)
from .monoids import (
    sum as sum,
)
from .monoids import (
    sumsq as sumsq,
)
from .monoids import (
    topk as topk,
)
from .monoids import (
    utilization as utilization,
)
from .monoids import (
    var as var,
)
from .monoids import (
    variance as variance,
)
from .package import (
    JitPackage as JitPackage,
)
from .package import (
    build as build,
)
from .package import (
    compile_class as compile_class,
)
from .package import (
    is_jit_plugin as is_jit_plugin,
)
from .package import (
    plugin as plugin,
)
from .package import (
    plugin_result_names as plugin_result_names,
)
from .package import (
    provided_names as provided_names,
)
from .package import (
    series as series,
)
from .prims import (
    abs as abs,
)
from .prims import (
    align_down as align_down,
)
from .prims import (
    align_up as align_up,
)
from .prims import (
    bit_width as bit_width,
)
from .prims import (
    ceil_pow2 as ceil_pow2,
)
from .prims import (
    clamp as clamp,
)
from .prims import (
    clampf as clampf,
)
from .prims import (
    clz as clz,
)
from .prims import (
    copysign as copysign,
)
from .prims import (
    ctz as ctz,
)
from .prims import (
    div_ceil as div_ceil,
)
from .prims import (
    div_round as div_round,
)
from .prims import (
    exp as exp,
)
from .prims import (
    fastrange as fastrange,
)
from .prims import (
    floor_pow2 as floor_pow2,
)
from .prims import (
    fma as fma,
)
from .prims import (
    fmax as fmax,
)
from .prims import (
    fmin as fmin,
)
from .prims import (
    gcd as gcd,
)
from .prims import (
    ilog2 as ilog2,
)
from .prims import (
    isqrt as isqrt,
)
from .prims import (
    lerp as lerp,
)
from .prims import (
    log as log,
)
from .prims import (
    log2 as log2,
)
from .prims import (
    mix64 as mix64,
)
from .prims import (
    mul_hi as mul_hi,
)
from .prims import (
    popcount as popcount,
)
from .prims import (
    rotl as rotl,
)
from .prims import (
    rotr as rotr,
)
from .prims import (
    sqrt as sqrt,
)
from .types import (
    NONE as NONE,
)
from .types import (
    Str as Str,
)
from .types import (
    bytes as bytes,
)
from .types import (
    f32 as f32,
)
from .types import (
    f64 as f64,
)
from .types import (
    i8 as i8,
)
from .types import (
    i16 as i16,
)
from .types import (
    i32 as i32,
)
from .types import (
    i64 as i64,
)
from .types import (
    str_ as str_,
)
from .types import (
    u8 as u8,
)
from .types import (
    u16 as u16,
)
from .types import (
    u32 as u32,
)
from .types import (
    u64 as u64,
)
from .vfold import (
    vfold as vfold,
)

__all__ = [
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
    "op",
    "ops",
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
