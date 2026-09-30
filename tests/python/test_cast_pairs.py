import pytest

from dftracer.utils import DataFrame


def frame():
    return DataFrame.from_dict({"i": [1, 2], "f": [1.5, 0.0], "b": [True, False], "s": ["7", "x"]})


def test_numbers_and_bools_cast_to_string():
    c = frame()
    assert c["i"].astype("string").to_list() == ["1", "2"]
    assert c["f"].astype("string").to_list() == ["1.5", "0.0"]
    assert c["b"].astype("string").to_list() == ["True", "False"]


def test_whole_floats_and_exponents_show_a_point_or_exponent():
    d = DataFrame.from_dict({"f": [2.0, 1e21, 0.1234567891]})
    assert d["f"].astype("string").to_list() == ["2.0", "1e+21", "0.1234567891"]


def test_numbers_cast_to_bool():
    d = DataFrame.from_dict({"i": [1, 0, -3], "f": [1.5, 0.0, float("nan")]})
    assert d["i"].astype("bool").to_list() == [True, False, True]
    assert d["f"].astype("bool").to_list() == [True, False, None]


def test_null_stays_null_through_casts():
    d = DataFrame.from_dict({"x": [1, None, 3]})
    assert d["x"].astype("string").to_list() == ["1", None, "3"]
    assert d["x"].astype("bool").to_list() == [True, None, True]


@pytest.mark.parametrize(
    "src, dst, names",
    [("s", "bool", ("string", "bool")), ("b", "float64", None)],
)
def test_unsupported_pair_names_both_types(src, dst, names):
    c = frame()
    if names is None:
        c[src].astype(dst)  # bool to float64 is supported
        return
    with pytest.raises(TypeError) as e:
        c[src].astype(dst)
    assert all(n in str(e.value) for n in names)


def test_same_type_cast_is_the_column():
    c = frame()
    assert c["s"].astype("string").to_list() == ["7", "x"]
    assert c["b"].astype("bool").to_list() == [True, False]


def test_string_to_number_still_parses():
    assert frame()["s"].astype("int64").to_list() == [7, None]
