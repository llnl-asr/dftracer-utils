# SPDX-License-Identifier: MIT
"""Helpers that turn trace agg specs and per-shard results into plain values."""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence, cast

from .._agg import UdafAgg
from .._dataframe import DataFrame
from .._expr import Agg, Col, Wildcard

if TYPE_CHECKING:
    from ..schemas import JsonValue

_OCCUPANCY = ("busy", "concurrency", "utilization", "active")
_WILDCARD_OPS = ("sum", "min", "max", "var", "std", "skew", "kurt")


def agg_specs(specs: Sequence[str | Agg]) -> tuple[list[str], list[str], int | None]:
    """Trace agg spec strings, ``F.any`` reductions and the occupancy grid of
    ``specs``: spec strings pass through, a bare-field ``Agg`` becomes
    ``"op:field"``, and ``F.any.<op>()`` becomes a numeric-args reduction."""

    strings: list[str] = []
    wildcard: list[str] = []
    grids = set()
    for s in specs:
        if isinstance(s, str):
            strings.append(s)
            continue
        if not isinstance(s, Agg):
            raise TypeError(
                f"agg() expects a spec string or an Agg (F.dur.sum()), got {type(s).__name__}"
            )
        if isinstance(s, UdafAgg):
            raise TypeError(
                f"TraceViewer.agg() cannot run the udaf {s.name!r}; collect first, "
                "then DataFrame.group_by"
            )
        if isinstance(s.value, Wildcard):
            if s.op == "count":
                strings.append("count")
            elif s.op == "mean" or s.op in _WILDCARD_OPS:
                wildcard.append(s.op)
            else:
                raise ValueError(
                    "F.any supports .count()/.mean()/.sum()/.min()/.max()/.var()/"
                    ".std()/.skew()/.kurt(); name a field for argmax or percentile"
                )
            continue
        if s.value is None:
            strings.append(s.op)
            continue
        if not isinstance(s.value, Col):
            raise TypeError(
                "TraceViewer.agg() needs a bare-field aggregate like F.dur.sum(); "
                "aggregate a computed value with with_column() then group_by()"
            )
        if s.op in ("arg_max", "arg_min"):
            raise ValueError(
                f"{s.op}: a position within a group has no meaning across trace files; "
                "collect the rows into a DataFrame first"
            )
        # The index keeps DDSketches, so a quantile here is the sketch's.
        op = "pct" if s.op == "quantile" else s.op
        spec = f"{'count' if s.op == 'count_valid' else op}:{s.value.name}"
        if s.op == "pct":
            spec += f":{s.param}"
        elif s.op == "quantile":
            spec += f":{s.param}"
        strings.append(spec)
        if s.op in _OCCUPANCY and s.param:
            grids.add(int(s.param))
    if len(grids) > 1:
        raise ValueError("occupancy aggregates in one plan share one resolution")
    return strings, wildcard, (grids.pop() if grids else None)


def stats_row(frame: DataFrame) -> dict[str, JsonValue]:
    row = {c: v[0] for c, v in frame.to_dict().items()} if frame.height else {}
    out = {
        "duration_count": row.get("count", 0),
        "duration_mean_us": row.get("mean_dur", 0.0),
        "duration_stddev_us": row.get("std_dur", 0.0),
        "min_timestamp_us": row.get("min_ts", 0),
        "max_timestamp_us": row.get("max_ts", 0),
    }
    return cast("dict[str, JsonValue]", out)


def export_stats(frame: DataFrame) -> dict[str, JsonValue]:
    return cast("dict[str, JsonValue]", {c: v[0] for c, v in frame.to_dict().items()})


def partial_bytes(frame: DataFrame) -> bytes:
    value = frame.to_dict()["partial"][0]
    if not isinstance(value, (bytes, bytearray)):
        raise TypeError(f"a partial is bytes, got {type(value).__name__}")
    return bytes(value)
