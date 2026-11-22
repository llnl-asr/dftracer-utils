# SPDX-License-Identifier: MIT
"""A slice of a String column shares its parent's offsets and data, so its
offsets start above 0. Every op gives the same values as on a column built from
the same values with no slice."""

import pickle

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as d  # noqa: E402

VALS = ["zz", "apple", None, "Banana", "cherry", "apple", "", "date", "Eclair", "fig"] * 3
PAD = ["PAD", "PADDING-LONG-XXXXX"]


def _frame(strings):
    return d.DataFrame.from_dict({"s": strings, "k": list(range(len(VALS)))})


@pytest.fixture(scope="module")
def frames():
    padded = d.DataFrame.from_dict(
        {"s": PAD + VALS + ["TAIL"], "k": [0, 0] + list(range(len(VALS))) + [0]}
    )
    return padded.slice(len(PAD), len(VALS)), _frame(VALS)


OPS = {
    "to_list": lambda f: f["s"].to_list(),
    "len": lambda f: f["s"].str.len_chars().to_list(),
    "upper": lambda f: f["s"].str.to_uppercase().to_list(),
    "lower": lambda f: f["s"].str.to_lowercase().to_list(),
    "title": lambda f: f["s"].str.to_titlecase().to_list(),
    "capitalize": lambda f: f["s"].str.capitalize().to_list(),
    "contains": lambda f: f["s"].str.contains("an").to_list(),
    "starts_with": lambda f: f["s"].str.starts_with("a").to_list(),
    "replace": lambda f: f["s"].str.replace("a", "@").to_list(),
    "str_slice": lambda f: f["s"].str.slice(1, 3).to_list(),
    "sort": lambda f: f.sort("s")["s"].to_list(),
    "unique": lambda f: sorted(x for x in f["s"].unique().to_list() if x is not None),
    "n_unique": lambda f: f["s"].n_unique(),
    "value_counts": lambda f: sorted(f["s"].value_counts().to_pandas().to_dict().items()),
    "group_by": lambda f: sorted(
        f.group_by("s").agg(d.col("k").sum()).to_pandas().fillna("NA").values.tolist()
    ),
    "join": lambda f: f.join(f.select("s").unique(), on="s").height,
    "eq": lambda f: (f["s"] == "apple").to_list(),
    "lt": lambda f: (f["s"] < "c").to_list(),
    "filter": lambda f: f.filter(d.col("s").str.contains("e"))["s"].to_list(),
    "head": lambda f: f.head(4)["s"].to_list(),
    "slice_df": lambda f: f.slice(3, 5)["s"].to_list(),
    "concat": lambda f: d.concat([f, f]).collect()["s"].to_list(),
    "dictionary_encode": lambda f: f["s"].dictionary_encode().to_list(),
    "to_numpy": lambda f: f["s"].to_numpy().tolist(),
    "to_pandas": lambda f: f.to_pandas()["s"].tolist(),
    "to_arrow": lambda f: f.to_arrow().column("s").to_pylist(),
    "fill_null": lambda f: f["s"].fill_null("X").to_list(),
    "is_duplicated": lambda f: f["s"].is_duplicated().to_list(),
    "min_max": lambda f: (f["s"].min(), f["s"].max()),
    "pickle": lambda f: pickle.loads(pickle.dumps(f))["s"].to_list(),
    "pickle_series": lambda f: pickle.loads(pickle.dumps(f["s"])).to_list(),
    "lazy": lambda f: (
        f.lazy()
        .filter(d.col("k") > 3)
        .select(d.col("s").str.to_uppercase().alias("s"))
        .collect()["s"]
        .to_list()
    ),
}


@pytest.mark.parametrize("name", sorted(OPS))
def test_sliced_string_column_matches_unsliced(frames, name):
    sliced, plain = frames
    assert OPS[name](sliced) == OPS[name](plain)


def test_slice_keeps_values(frames):
    sliced, _ = frames
    assert sliced["s"].to_list() == VALS
