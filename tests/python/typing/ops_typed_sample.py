"""Typed DataFrame-ops sample: ty checks this clean.

Every enum arg is a valid Literal and every window spec is a well-formed typed
tuple, proving the static surface accepts correct usage with no annotations.
"""

from typing import cast

from dftracer.utils import DataFrame

df = cast(DataFrame, None)

df.window(
    partition_by=["pid"],
    order_by=["ts"],
    specs=[
        ("row_number", "rn"),
        ("rank", "rk"),
        ("dense_rank", "dr"),
        ("lag", "dur", 1, "prev"),
        ("lead", "dur", 1, "next"),
        ("running_sum", "dur", "cum"),
        ("running_count", "dur", "n"),
        ("delta", "dur", "d"),
        ("rate", "bytes", "ts", "bw"),
        ("rate", "bytes", "ts", "bw", True),
        ("sessionize", "ts", 15.0, "sess"),
        ("sessionize", "ts", 15.0, "sess", "end"),
        ("sessionize", "ts", 15.0, "sess", None, 60.0),
        ("frame_sum", "dur", 1, 1, "fs"),
        ("frame_max", "dur", None, 0, "peak"),
        ("ntile", 4, "q"),
        ("first_value", "dur", "fv"),
        ("last_value", "dur", "lv"),
        ("nth_value", "dur", 2, "nv"),
    ],
)

df.gap_fill(["pid"], "ts", 10, "v", "none")
df.gap_fill(["pid"], "ts", 10, "v", "locf", start=0, end=30)
df.gap_fill(["pid"], "ts", 10, ["a", "b"], "linear")

df.join(df, "k")
df.join(df, ["a", "b"], "left")
df.join(df, "k", "semi")
df.join(df, 1, "inner")

df.asof(df, "ts")
df.asof(df, "ts", by="k", direction="nearest", tolerance=3)

df.interval(df, "p", "lo", "hi", outer=True)
