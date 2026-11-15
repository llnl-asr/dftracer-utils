# SPDX-License-Identifier: MIT
import pytest

pl = pytest.importorskip("polars")

from dftracer.utils import DataFrame, Series  # noqa: E402


@pytest.mark.parametrize(
    "s",
    [
        pl.Series("c", ["b", "a", None, "b"], dtype=pl.Categorical),
        pl.Series("c", ["x", None, "y"], dtype=pl.Enum(["y", "x", "z"])),
    ],
)
def test_categorical_round_trips_as_categorical(s):
    native = Series.from_polars(s)
    assert native.encoding == 2
    assert native.to_list() == s.to_list()
    out = native.to_polars()
    assert out.dtype == pl.Categorical
    assert out.to_list() == s.to_list()


def test_dictionary_column_gives_categorical_in_a_frame():
    df = DataFrame.from_dict({"k": [1, 2, 3]})
    df = df.with_column("s", Series(["p", None, "p"]).dictionary_encode())
    out = df.to_polars()
    assert out["s"].dtype == pl.Categorical
    assert out["s"].to_list() == ["p", None, "p"]
