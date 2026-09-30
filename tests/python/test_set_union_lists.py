"""``set_union`` over list columns, and a text form that loses no value
(openspec change set-union-over-list-cells)."""

import gzip
import json

import pytest

from dftracer.utils import DataFrame, col

SEP = "\x1e"


def _set(cell):
    return set(cell.split(SEP)) if cell != "" else set()


def _by_group(frame, column):
    pdf = frame.to_pandas()
    return dict(zip(pdf["g"], pdf[column]))


def _frame():
    return DataFrame.from_dict(
        {
            "g": ["a", "a", "b"],
            "v": ["x", "y", "x"],
            "L": [[1, 2], [2, 3], [9]],
            "LS": [["p", "q"], ["q"], ["r"]],
        }
    )


def test_proposal_repro_string_and_exploded_forms_still_work():
    e = _frame()
    got = _by_group(e.group_by("g").agg(u=col("v").set_union()), "u")
    assert _set(got["a"]) == {"x", "y"} and _set(got["b"]) == {"x"}
    exploded = _by_group(e.explode("L").group_by("g").agg(u=col("L").set_union()), "u")
    assert _set(exploded["a"]) == {"1", "2", "3"} and _set(exploded["b"]) == {"9"}


def test_list_of_integers_is_the_union_of_its_elements():
    e = _frame()
    got = _by_group(e.group_by("g").agg(u=col("L").set_union()), "u")
    assert _set(got["a"]) == {"1", "2", "3"}
    assert _set(got["b"]) == {"9"}
    exploded = _by_group(e.explode("L").group_by("g").agg(u=col("L").set_union()), "u")
    assert got == exploded


def test_list_of_strings_is_the_union_of_its_elements():
    got = _by_group(_frame().group_by("g").agg(u=col("LS").set_union()), "u")
    assert _set(got["a"]) == {"p", "q"}
    assert _set(got["b"]) == {"r"}


def test_nulls_and_empty_lists_contribute_nothing():
    e = DataFrame.from_dict({"g": ["a", "a", "a"], "L": [[1, None], [], None]})
    got = _by_group(e.group_by("g").agg(u=col("L").set_union()), "u")
    assert _set(got["a"]) == {"1"}


def test_unsupported_column_types_are_refused():
    import datetime

    ts = DataFrame.from_dict({"g": ["a"], "t": [datetime.datetime(2020, 1, 1)]})
    with pytest.raises(Exception, match="timestamp"):
        ts.group_by("g").agg(u=col("t").set_union())
    nested = DataFrame.from_dict({"g": ["a"], "LL": [[[1], [2]]]})
    with pytest.raises(Exception, match="list"):
        nested.group_by("g").agg(u=col("LL").set_union())


def test_float_keeps_every_digit():
    e = DataFrame.from_dict({"g": ["a", "a"], "v": [0.1234567891, 0.5]})
    got = _by_group(e.group_by("g").agg(u=col("v").set_union()), "u")
    assert _set(got["a"]) == {"0.1234567891", "0.5"}


def test_empty_string_is_a_value():
    e = DataFrame.from_dict({"g": ["a", "a"], "v": ["x", ""]})
    got = _by_group(e.group_by("g").agg(u=col("v").set_union()), "u")
    assert got["a"].split(SEP) == ["", "x"]


def test_string_holding_the_separator_is_refused():
    e = DataFrame.from_dict({"g": ["a", "a"], "v": ["x" + SEP + "y", "z"]})
    with pytest.raises(Exception, match="separator"):
        e.group_by("g").agg(u=col("v").set_union())


def test_view_and_dataframe_engines_agree(tmp_path):
    pytest.importorskip("pyarrow")
    import dftracer.utils as dft
    from dftracer.utils import TraceViewer

    values = [0.1234567891, 0.5, 0.5]
    names = ["x", "y", "x"]
    path = tmp_path / "t.pfw.gz"
    with gzip.open(path, "wt") as f:
        for i, (v, n) in enumerate(zip(values, names)):
            f.write(
                json.dumps(
                    {
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 1,
                        "ts": 100 + i,
                        "dur": 5,
                        "ph": "X",
                        "args": {"v": v, "n": n},
                    }
                )
                + "\n"
            )
    with dft.Indexer(files=[str(path)]) as ix:
        ix.ensure_indexed()
    view = TraceViewer(str(path)).agg("set_union:args.v", "set_union:args.n").collect().to_pandas()
    frame = DataFrame.from_dict({"g": ["a"] * 3, "v": values, "n": names})
    df = frame.group_by("g").agg(v=col("v").set_union(), n=col("n").set_union()).to_pandas()
    assert _set(view["set_args.v"].iloc[0]) == _set(df["v"].iloc[0])
    assert _set(view["set_args.n"].iloc[0]) == _set(df["n"].iloc[0])
