#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scalar UDFs a plugin registers, called from Python by name.

``dft.udf`` is the read-only registry, ``dft.call`` the by-name fallback. The
expression a handle builds holds the UDF, so it keeps running after the plugin
object that registered it is gone.
"""

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.plugins import Plugins  # noqa: E402

from .common import find_installed_example_plugin  # noqa: E402

_PLUGIN = find_installed_example_plugin("duration_udfs")

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(
        not _PLUGIN, reason="no duration_udfs plugin installed next to the extension"
    ),
]


def _df():
    return DataFrame.from_arrow(pa.table({"dur": [10, 20, 30, 40]}))


def _dict(df):
    return df.to_arrow().to_pydict()


def test_udf_is_a_read_only_mapping_of_registered_names():
    assert "to_ms" not in dft.udf
    with pytest.raises(KeyError):
        dft.udf["to_ms"]
    plugins = Plugins([_PLUGIN])
    assert {"to_ms", "is_slow"} <= set(dft.udf)
    assert "to_ms" in dft.udf
    assert len(dft.udf) >= 2
    assert dft.udf["to_ms"].name == "to_ms"
    assert dft.udf.get("to_ms") is not None
    assert dft.udf.get("no_such_udf") is None
    del plugins
    assert "to_ms" not in dft.udf


def test_a_name_that_is_not_a_string_is_not_registered():
    assert 1 not in dft.udf
    assert dft.udf.get(1) is None  # type: ignore[arg-type]
    with pytest.raises(KeyError):
        dft.udf[1]  # type: ignore[index]


def test_unknown_name_raises_at_the_lookup_and_the_call():
    with pytest.raises(KeyError):
        dft.udf["no_such_udf"]
    with pytest.raises(KeyError):
        dft.call("no_such_udf", col("dur"))


def test_udf_in_with_columns_and_filter_matches_the_reference():
    plugins = Plugins([_PLUGIN])
    to_ms = dft.udf["to_ms"]
    is_slow = dft.udf["is_slow"]
    out = _df().lazy().with_columns(ms=to_ms(col("dur"))).collect()
    ref = _df().lazy().with_columns(ms=col("dur") / 1000.0).collect()
    assert _dict(out)["ms"] == _dict(ref)["ms"]

    kept = _df().lazy().filter(is_slow(col("dur"), 25)).collect()
    assert _dict(kept) == {"dur": [30, 40]}

    by_name = _df().lazy().filter(dft.call("is_slow", col("dur"), 25)).collect()
    assert _dict(by_name) == _dict(kept)

    both = _df().lazy().with_columns(ms=to_ms(col("dur")) * 2.0).collect()
    assert _dict(both)["ms"] == [0.02, 0.04, 0.06, 0.08]
    del plugins


def test_a_plan_keeps_the_udf_after_the_plugin_object_is_dropped():
    plugins = Plugins([_PLUGIN])
    plan = _df().lazy().with_columns(ms=dft.udf["to_ms"](col("dur")))
    del plugins
    assert "to_ms" not in dft.udf
    assert _dict(plan.collect())["ms"] == [0.01, 0.02, 0.03, 0.04]
