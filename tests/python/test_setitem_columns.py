"""Assigning several columns from a frame or a list of Series
(openspec change dataframe-setitem-columns)."""

import pytest

from dftracer.utils import DataFrame


def _frame():
    return DataFrame.from_dict({"a": [1.0, 2.0, 3.0], "b": [10.0, 20.0, 30.0], "c": [7, 8, 9]})


def _other():
    return DataFrame.from_dict({"a": [100.0, 200.0, 300.0], "b": [1000.0, 2000.0, 3000.0]})


def _cols(frame):
    return frame.to_pandas().to_dict("list")


def test_proposal_repro_replace_existing_columns():
    d, o = _frame(), _other()
    d[["a", "b"]] = o[["a", "b"]]
    assert _cols(d) == {
        "a": [100.0, 200.0, 300.0],
        "b": [1000.0, 2000.0, 3000.0],
        "c": [7, 8, 9],
    }


def test_new_columns_are_added_after_the_existing_ones_in_order():
    d, o = _frame(), _other()
    d[["n1", "n2"]] = o[["a", "b"]]
    assert list(d.columns) == ["a", "b", "c", "n1", "n2"]
    assert _cols(d)["n1"] == [100.0, 200.0, 300.0]
    assert _cols(d)["n2"] == [1000.0, 2000.0, 3000.0]


def test_pairing_is_by_position_not_by_name():
    d, o = _frame(), _other()
    d[["a", "b"]] = o[["b", "a"]]
    got = _cols(d)
    assert got["a"] == [1000.0, 2000.0, 3000.0]
    assert got["b"] == [100.0, 200.0, 300.0]


def test_list_of_series_assigns_one_series_per_target():
    d, o = _frame(), _other()
    d[["p", "q"]] = [o["a"], o["b"]]
    got = _cols(d)
    assert got["p"] == [100.0, 200.0, 300.0]
    assert got["q"] == [1000.0, 2000.0, 3000.0]
    d[["r", "s"]] = (o["b"], o["a"])
    assert _cols(d)["r"] == [1000.0, 2000.0, 3000.0]


def test_a_column_count_that_differs_changes_nothing():
    d, o = _frame(), _other()
    before = _cols(d)
    with pytest.raises(ValueError, match=r"2 target columns but the value has 1 columns"):
        d[["a", "b"]] = o[["a"]]
    with pytest.raises(ValueError, match=r"2 target columns but the value has 3 columns"):
        d[["a", "b"]] = [o["a"], o["b"], o["a"]]
    assert _cols(d) == before


def test_a_row_count_that_differs_names_the_column_and_changes_nothing():
    d = _frame()
    before = _cols(d)
    short = DataFrame.from_dict({"x": [1.0, 2.0], "y": [3.0, 4.0]})
    with pytest.raises(ValueError, match=r"column 'a' gets 2 rows; the frame has 3"):
        d[["a", "b"]] = short
    assert _cols(d) == before


def test_a_failure_after_the_first_column_leaves_the_frame_as_it_was():
    d = _frame()
    before = _cols(d)
    # The first column writes, the second (a list column into a float one) cannot.
    mixed = DataFrame.from_dict({"x": [1.0, 2.0], "y": [[1], [2]]})
    with pytest.raises(TypeError, match="cannot cast list to float64"):
        d.loc[d["c"] != 8, ["a", "b"]] = mixed
    assert _cols(d) == before


def test_a_frame_with_one_column_fills_one_name():
    d, o = _frame(), _other()
    d["z"] = o[["b"]]
    assert _cols(d)["z"] == [1000.0, 2000.0, 3000.0]


def test_existing_forms_keep_their_meaning():
    d, o = _frame(), _other()
    d["z"] = o["b"]
    assert _cols(d)["z"] == [1000.0, 2000.0, 3000.0]
    d[["a", "b"]] = 0
    got = _cols(d)
    assert got["a"] == [0.0, 0.0, 0.0] and got["b"] == [0.0, 0.0, 0.0]
    d[["p", "q"]] = o["a"]
    assert _cols(d)["p"] == _cols(d)["q"] == [100.0, 200.0, 300.0]
    # a list of scalars is still one column of values
    d["w"] = [1, 2, 3]
    assert _cols(d)["w"] == [1, 2, 3]
    d[d["c"] > 7] = 0
    assert _cols(d)["c"] == [7, 0, 0]


def test_masked_frame_assignment_writes_only_the_selected_rows():
    d = _frame()
    selected = DataFrame.from_dict({"a": [100.0, 300.0], "b": [1000.0, 3000.0]})
    mask = d["c"] != 8
    d.loc[mask, ["a", "b"]] = selected
    got = _cols(d)
    assert got["a"] == [100.0, 2.0, 300.0]
    assert got["b"] == [1000.0, 20.0, 3000.0]
    assert got["c"] == [7, 8, 9]


def test_masked_row_count_mismatch_names_the_selection():
    d = _frame()
    mask = d["c"] != 8
    too_many = DataFrame.from_dict({"x": [1.0, 2.0, 3.0, 4.0], "y": [1.0, 2.0, 3.0, 4.0]})
    with pytest.raises(ValueError, match=r"column 'a' gets 4 rows; the selection has 2"):
        d.loc[mask, ["a", "b"]] = too_many


def test_iloc_setter_takes_a_frame():
    d, o = _frame(), _other()
    d.iloc[:, [0, 1]] = o[["a", "b"]]
    assert _cols(d)["a"] == [100.0, 200.0, 300.0]


def test_pandas_parity_on_random_data():
    import numpy as np
    import pandas as pd

    rng = np.random.default_rng(3)
    base = pd.DataFrame(
        {"a": rng.normal(size=50), "b": rng.integers(0, 9, 50), "c": rng.normal(size=50)}
    )
    other = pd.DataFrame({"x": rng.normal(size=50), "y": rng.integers(0, 9, 50)})
    want = base.copy()
    want[["a", "b"]] = other[["x", "y"]].to_numpy()
    d = DataFrame.from_pandas(base)
    d[["a", "b"]] = DataFrame.from_pandas(other)
    got = d.to_pandas()
    assert np.allclose(got["a"], want["a"]) and (got["b"].to_numpy() == want["b"].to_numpy()).all()
    assert np.allclose(got["c"], want["c"])


def test_a_value_the_column_type_refuses_raises_without_pyarrow():
    d = _frame()
    for value in ("x", 1.5, True):
        with pytest.raises(TypeError, match="cannot assign"):
            d["c"] = value
    d["a"] = 4
    assert _cols(d)["a"] == [4.0, 4.0, 4.0]
    d.loc[d["c"] > 7, "c"] = 0
    assert _cols(d)["c"] == [7, 0, 0]
