#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""User-defined aggregates a plugin registers, called from Python in group_by.

``dft.udaf`` is the read-only registry. Calling a ``Udaf`` with column
expressions gives an ``Agg`` that ``group_by(...).agg`` runs per group.
"""

import math

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402
from dftracer.utils.plugins import Plugins  # noqa: E402

from .common import find_installed_example_plugin  # noqa: E402

_PLUGIN = find_installed_example_plugin("stats_udafs")

pytestmark = [
    pytest.mark.skipif(
        not hasattr(_ext, "LazyFrame"), reason="extension built without Arrow support"
    ),
    pytest.mark.skipif(not _PLUGIN, reason="no stats_udafs plugin installed next to the extension"),
]

_KEYS = [1, 1, 1, 2, 2, 3, 3, 3, 3, 1, 2, 3]
_VALS = [4.0, 1.0, 9.0, 2.0, None, 8.0, 3.0, None, 5.0, 7.0, 6.0, 1.5]


def _df():
    return DataFrame.from_arrow(pa.table({"k": _KEYS, "v": _VALS}))


def _groups():
    out = {}
    for k, v in zip(_KEYS, _VALS):
        out.setdefault(k, [])
        if v is not None:
            out[k].append(v)
    return out


def _p50(values):
    return sorted(values)[len(values) // 2]


def _geomean(values):
    positive = [v for v in values if v > 0]
    return math.exp(sum(math.log(v) for v in positive) / len(positive))


def _by_key(df):
    table = df.to_arrow().to_pydict()
    return {k: i for i, k in enumerate(table["k"])}, table


def test_udaf_is_a_read_only_mapping_of_registered_names():
    assert "p50" not in dft.udaf
    with pytest.raises(KeyError):
        dft.udaf["p50"]
    plugins = Plugins([_PLUGIN])
    assert {"p50", "geomean"} <= set(dft.udaf)
    assert "p50" in dft.udaf
    assert len(dft.udaf) >= 2
    handle = dft.udaf["p50"]
    assert handle.name == "p50"
    assert handle.min_args == handle.max_args == 1
    assert isinstance(handle.flags, int)
    assert dft.udaf.get("p50") is not None
    assert dft.udaf.get("no_such_udaf") is None
    del plugins
    assert "p50" not in dft.udaf


def test_a_name_that_is_not_a_string_is_not_registered():
    assert 1 not in dft.udaf
    assert dft.udaf.get(1) is None  # type: ignore[arg-type]
    with pytest.raises(KeyError):
        dft.udaf[1]  # type: ignore[index]


def test_group_by_agg_matches_the_reference_with_nulls_and_several_groups():
    plugins = Plugins([_PLUGIN])
    out = _df().group_by("k").agg(dft.udaf["p50"](col("v")), dft.udaf["geomean"](col("v")))
    index, table = _by_key(out)
    assert set(table) == {"k", "p50_v", "geomean_v"}
    for k, values in _groups().items():
        assert table["p50_v"][index[k]] == _p50(values)
        assert table["geomean_v"][index[k]] == pytest.approx(_geomean(values))
    del plugins


def test_a_result_stays_readable_after_its_plugin_is_unloaded():
    plugins = Plugins([_PLUGIN])
    out = _df().group_by("k").agg(dft.udaf["geomean"](col("v")))
    del plugins
    index, table = _by_key(out)
    for k, values in _groups().items():
        assert table["geomean_v"][index[k]] == pytest.approx(_geomean(values))
    del out


def test_alias_and_named_keyword_name_the_output():
    plugins = Plugins([_PLUGIN])
    p50 = dft.udaf["p50"]
    aliased = _df().group_by("k").agg(p50(col("v")).alias("median"))
    assert set(aliased.to_arrow().to_pydict()) == {"k", "median"}
    named = _df().group_by("k").agg(med=p50(col("v")))
    assert set(named.to_arrow().to_pydict()) == {"k", "med"}
    assert named.to_arrow().to_pydict()["med"] == pytest.approx(
        [_p50(_groups()[k]) for k in named.to_arrow().to_pydict()["k"]]
    )
    del plugins


def test_udaf_mixes_with_built_in_aggregates():
    plugins = Plugins([_PLUGIN])
    out = _df().group_by("k").agg(dft.udaf["p50"](col("v")), col("v").sum(), dft.count())
    index, table = _by_key(out)
    for k, values in _groups().items():
        assert table["p50_v"][index[k]] == _p50(values)
        assert table["sum_v"][index[k]] == pytest.approx(sum(values))
    del plugins


def test_an_aggregate_of_an_unloaded_plugin_fails_by_name_when_it_runs():
    plugins = Plugins([_PLUGIN])
    agg = dft.udaf["p50"](col("v"))
    del plugins
    assert "p50" not in dft.udaf
    with pytest.raises(ValueError, match="p50"):
        _df().group_by("k").agg(agg)


def test_unknown_name_raises_key_error_at_the_lookup():
    with pytest.raises(KeyError):
        dft.udaf["no_such_udaf"]


def test_wrong_argument_count_is_a_type_error_naming_the_udaf():
    plugins = Plugins([_PLUGIN])
    with pytest.raises(TypeError, match="p50"):
        dft.udaf["p50"]()
    with pytest.raises(TypeError, match="p50"):
        dft.udaf["p50"](col("v"), col("k"))
    del plugins


def test_an_argument_the_udaf_refuses_is_a_value_error():
    plugins = Plugins([_PLUGIN])
    frame = DataFrame.from_arrow(pa.table({"k": [1, 1], "s": ["a", "b"]}))
    with pytest.raises(ValueError, match="p50"):
        frame.group_by("k").agg(dft.udaf["p50"](col("s")))
    del plugins


def test_a_computed_argument_is_refused_not_miscomputed():
    plugins = Plugins([_PLUGIN])
    with pytest.raises(ValueError, match="bare column"):
        _df().group_by("k").agg(dft.udaf["p50"](col("v") + 1.0))
    del plugins


def _sorted_pairs(table, *names):
    return sorted(zip(*(table[n] for n in names)), key=lambda r: r[0])


def test_lazy_group_by_matches_the_eager_result():
    plugins = Plugins([_PLUGIN])
    aggs = (dft.udaf["p50"](col("v")), dft.udaf["geomean"](col("v")))
    eager = _df().group_by("k").agg(*aggs).to_arrow().to_pydict()
    lazy = _df().lazy().group_by("k").agg(*aggs).collect().to_arrow().to_pydict()
    assert set(lazy) == {"k", "p50_v", "geomean_v"}
    for name in ("k", "p50_v", "geomean_v"):
        assert sorted(lazy[name]) == pytest.approx(sorted(eager[name]))
    for k, p, g in zip(lazy["k"], lazy["p50_v"], lazy["geomean_v"]):
        assert p == _p50(_groups()[k])
        assert g == pytest.approx(_geomean(_groups()[k]))
    del plugins


def test_lazy_group_by_collect_async_matches_collect():
    plugins = Plugins([_PLUGIN])
    plan = _df().lazy().group_by("k").agg(dft.udaf["p50"](col("v")).alias("m"))
    got = plan.collect_async().get().to_arrow().to_pydict()
    want = plan.collect().to_arrow().to_pydict()
    assert _sorted_pairs(got, "k", "m") == _sorted_pairs(want, "k", "m")
    del plugins


def test_lazy_udaf_with_a_computed_argument_is_a_value_error():
    plugins = Plugins([_PLUGIN])
    with pytest.raises(ValueError, match="bare column"):
        _df().lazy().group_by("k").agg(dft.udaf["p50"](col("v") + 1.0))
    del plugins


def _timed():
    t = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
    v = [4.0, 1.0, 9.0, 2.0, None, 8.0, 3.0, 5.0, 7.0, 6.0]
    return DataFrame.from_arrow(pa.table({"t": t, "v": v}))


def test_group_by_dynamic_matches_the_eager_windows():
    plugins = Plugins([_PLUGIN])
    aggs = (dft.udaf["p50"](col("v")), dft.udaf["geomean"](col("v")))
    for frame in (_timed(), _timed().lazy()):
        out = frame.sort_by("t").group_by_dynamic("t", 5, None, aggs)
        if hasattr(out, "collect"):
            out = out.collect()
        table = out.to_arrow().to_pydict()
        rows = sorted(zip(table["t"], table["p50_v"], table["geomean_v"]))
        assert [r[0] for r in rows] == [0, 5]
        first = [4.0, 1.0, 9.0, 2.0]
        second = [8.0, 3.0, 5.0, 7.0, 6.0]
        assert rows[0][1] == _p50(first)
        assert rows[1][1] == _p50(second)
        assert rows[0][2] == pytest.approx(_geomean(first))
        assert rows[1][2] == pytest.approx(_geomean(second))
    del plugins


def test_lazy_group_by_mixes_a_udaf_with_a_built_in_aggregate():
    plugins = Plugins([_PLUGIN])
    out = (
        _df()
        .lazy()
        .group_by("k")
        .agg(dft.udaf["p50"](col("v")), col("v").sum(), dft.count())
        .collect()
        .to_arrow()
        .to_pydict()
    )
    for i, k in enumerate(out["k"]):
        assert out["p50_v"][i] == _p50(_groups()[k])
        assert out["sum_v"][i] == pytest.approx(sum(_groups()[k]))
    del plugins


def test_group_by_dynamic_takes_a_built_in_aggregate():
    for frame in (_timed(), _timed().lazy()):
        out = frame.sort_by("t").group_by_dynamic("t", 5, None, [col("v").sum()])
        if hasattr(out, "collect"):
            out = out.collect()
        table = out.to_arrow().to_pydict()
        assert sorted(zip(table["t"], table["sum_v"])) == [(0, 16.0), (5, 29.0)]


def test_a_malformed_udaf_spec_is_a_type_error_not_a_crash():
    lazy = _df().lazy()
    for bad in [("p50",), ("p50", "out"), ("p50", "out", "v"), ("p50", "out", ["v"], 1)]:
        with pytest.raises(TypeError):
            lazy.group_by(["k"], [bad])


def test_over_runs_a_udaf_per_group_and_broadcasts_it():
    plugins = Plugins([_PLUGIN])
    frame = _df().with_columns(m=dft.udaf["p50"](col("v")).over("k"))
    table = frame.to_arrow().to_pydict()
    for k, m in zip(table["k"], table["m"]):
        assert m == _p50(_groups()[k])
    lazy = _df().lazy().with_columns(m=dft.udaf["p50"](col("v")).over("k")).collect()
    got = lazy.to_arrow().to_pydict()
    for k, m in zip(got["k"], got["m"]):
        assert m == _p50(_groups()[k])
    del plugins


def test_trace_viewer_agg_refuses_a_udaf_naming_it():
    from dftracer.utils.trace_viewer import _agg_specs

    plugins = Plugins([_PLUGIN])
    with pytest.raises(TypeError, match="p50"):
        _agg_specs([dft.udaf["p50"](col("v"))])
    del plugins


def test_dask_agg_refuses_a_udaf_naming_it():
    dask_frame = pytest.importorskip("dftracer.utils.dask_frame")
    plugins = Plugins([_PLUGIN])
    with pytest.raises(TypeError, match="p50"):
        dask_frame._parse("m", dft.udaf["p50"](col("v")))
    del plugins
