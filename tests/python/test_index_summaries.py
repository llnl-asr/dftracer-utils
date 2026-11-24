# SPDX-License-Identifier: MIT
"""The summaries API: Indexer(summaries=...), Index.summaries, explain and hash_value."""

import gzip
import json
import math
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402

EVENTS = 240
PER_MEMBER = 10
VECTORS = (
    Path(__file__).resolve().parents[1] / "trace" / "index" / "data" / "value_hash_vectors.txt"
)


def write_trace(directory: Path) -> str:
    """240 events in gzip members of 10: `size` rises with the event number and
    `m` repeats in runs of ten, so each chunk holds its own range and values."""
    out = bytearray()
    for first in range(0, EVENTS, PER_MEMBER):
        lines = []
        for i in range(first, first + PER_MEMBER):
            event = {
                "id": i,
                "name": "read" if i % 3 else "write",
                "cat": "POSIX",
                "pid": 1,
                "tid": 2,
                "ph": 1,
                "ts": 1000 + i,
                "dur": 5,
                "args": {"size": i * 10, "m": f"m{i // 10}"},
            }
            lines.append(json.dumps(event, separators=(",", ":")))
        out += gzip.compress(("\n".join(lines) + "\n").encode())
    path = directory / "summaries.pfw.gz"
    path.write_bytes(bytes(out))
    return str(path)


def kinds(frame: "dft.DataFrame", path: str) -> set:
    data = frame.to_dict()
    return {s for p, s in zip(data["path"], data["summary"]) if p == path}


@pytest.fixture()
def trace(tmp_path):
    return write_trace(tmp_path), str(tmp_path / "idx")


def test_a_path_keeps_only_the_summaries_it_is_given(trace):
    path, idx = trace
    indexer = dft.index.Indexer(
        path, index_dir=idx, summaries={"args.size": ["zonemap"], "m": ["bloom", "zonemap"]}
    )
    status = indexer.ensure()
    frame = dft.index.Index(status.index_dir).summaries(path)

    assert list(frame.to_dict()) == ["path", "chunk", "summary", "bytes", "hash_version"]
    assert kinds(frame, "size") == {"zonemap"}
    assert kinds(frame, "m") == {"bloom", "zonemap"}
    assert kinds(frame, "name") >= {"zonemap", "bloom", "counts", "postings"}

    data = frame.to_dict()
    rows = list(
        zip(data["path"], data["chunk"], data["summary"], data["bytes"], data["hash_version"])
    )
    assert all(b > 0 for _, _, _, b, _ in rows)
    assert (
        len({c for p, c, s, _, _ in rows if p == "size" and s == "zonemap"}) == EVENTS // PER_MEMBER
    )
    assert all(v is None for p, _, s, _, v in rows if p == "size")
    assert all(v == 1 for p, _, s, _, v in rows if p == "m" and s == "bloom")


def test_indexer_summaries_is_the_index_summaries(trace):
    path, idx = trace
    indexer = dft.index.Indexer(path, index_dir=idx, summaries={"size": ["bloom"]})
    status = indexer.ensure()
    assert (
        indexer.summaries(path).to_dict()
        == dft.index.Index(status.index_dir).summaries(path).to_dict()
    )


def test_a_missing_file_is_not_found(trace):
    path, idx = trace
    status = dft.index.Indexer(path, index_dir=idx).ensure()
    with pytest.raises(dft.errors.DFTUtilsNotFoundError):
        dft.index.Index(status.index_dir).summaries(path + ".other")


@pytest.mark.parametrize(
    ("summaries", "named"),
    [
        ({"size": ["sketchy"]}, "sketchy"),
        ({"size": ["postings"]}, "postings"),
        ({"": ["bloom"]}, "empty"),
        ({"size": []}, "size"),
        ({"size": ["bloom"], "args.size": ["bloom"]}, "size"),
        ({"size": ["bloom", "bloom"]}, "bloom"),
    ],
)
def test_a_bad_summary_choice_is_an_error_that_names_it(trace, summaries, named):
    path, idx = trace
    indexer = dft.index.Indexer(path, index_dir=idx, summaries=summaries)
    with pytest.raises(dft.errors.DFTUtilsValueError, match=named):
        indexer.status()


def test_a_summary_outside_extensions_is_an_error(trace):
    path, idx = trace
    indexer = dft.index.Indexer(
        path, index_dir=idx, summaries={"size": ["bloom"]}, extensions=["zonemap"]
    )
    with pytest.raises(dft.errors.DFTUtilsValueError, match="bloom"):
        indexer.status()


def test_summaries_are_shaped_as_a_mapping_of_lists(trace):
    path, idx = trace
    with pytest.raises(TypeError):
        dft.index.Indexer(path, index_dir=idx, summaries={"size": "bloom"}).status()  # type: ignore[dict-item]
    with pytest.raises(TypeError):
        dft.index.Indexer(path, index_dir=idx, summaries=["size"]).status()  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        dft.index.Indexer(path, index_dir=idx, summaries={1: ["bloom"]}).status()  # type: ignore[dict-item]


def test_a_changed_choice_rebuilds_and_the_same_one_does_not(trace):
    path, idx = trace

    def indexer(summaries):
        return dft.index.Indexer(path, index_dir=idx, summaries=summaries)

    a = {"size": ["zonemap"]}
    b = {"size": ["bloom"]}
    assert indexer(a).ensure().needs_work == []
    assert indexer(a).status().needs_work == []
    assert indexer(b).status().needs_work == [path]
    assert indexer(b).ensure().needs_work == []
    assert kinds(indexer(b).summaries(path), "size") == {"bloom"}
    assert indexer(None).status().needs_work == [path]
    assert indexer(b).status().needs_work == []


def test_explain_counts_the_chunks_each_summary_skips(trace):
    path, idx = trace
    indexer = dft.index.Indexer(
        path, index_dir=idx, summaries={"size": ["zonemap"], "m": ["bloom"]}
    )
    indexer.ensure()

    (rng,) = indexer.explain("size >= 2000")
    assert [(s["path"], s["summary"]) for s in rng["summaries"]] == [("size", "zonemap")]
    (skip,) = rng["summaries"]
    assert skip["chunks"] == rng["chunks"]
    assert 0 < skip["skipped"] == rng["chunks"] - len(rng["read"])

    (eq,) = indexer.explain('m == "m3"')
    assert [(s["path"], s["summary"]) for s in eq["summaries"]] == [("m", "bloom")]
    assert eq["summaries"][0]["skipped"] == eq["chunks"] - len(eq["read"]) > 0

    (both,) = indexer.explain('size >= 2000 and m == "m23"')
    assert {(s["path"], s["summary"]) for s in both["summaries"]} == {
        ("size", "zonemap"),
        ("m", "bloom"),
    }

    text = indexer.explain_text("size >= 2000")
    assert f"  summary size zonemap: skipped {skip['skipped']} of {skip['chunks']} chunks\n" in text


def _vectors():
    for line in VECTORS.read_text().splitlines():
        if line and not line.startswith("#"):
            kind, arg, digest = line.split(" ")
            yield kind, arg, int(digest, 16)


def _value(kind: str, arg: str):
    raw = b"" if arg == "-" else bytes.fromhex(arg) if kind in ("str", "bytes") else b""
    return {
        "null": lambda: None,
        "bool": lambda: arg == "1",
        "i64": lambda: int(arg),
        "u64": lambda: int(arg),
        "f64": lambda: float(arg),
        "str": lambda: raw.decode(),
        "bytes": lambda: raw,
    }[kind]()


@pytest.mark.parametrize(("kind", "arg", "digest"), list(_vectors()))
def test_hash_value_equals_the_cpp_value(kind, arg, digest):
    assert dft.index.hash_value(_value(kind, arg)) == digest


def test_hash_value_canonical_rules():
    h = dft.index.hash_value
    assert h(42.0) == h(42)
    assert h(1) == h(True) and h(0) == h(False)
    assert h(-0.0) == h(0.0) == h(0)
    assert h(math.nan) == h(float("nan")) == h(-math.nan)
    assert h(42.5) != h(42) and h(42.5) != h(43)
    assert h("42") != h(42)
    assert h("a") == h(b"a")
    assert h(2**63) == h(9223372036854775808.0)
    assert h(None) != h(0)
    assert 0 <= h("x") < 2**64


def test_hash_value_rejects_other_types():
    with pytest.raises(TypeError):
        dft.index.hash_value([1])  # type: ignore[call-overload]
    with pytest.raises(OverflowError):
        dft.index.hash_value(2**64)
    with pytest.raises(OverflowError):
        dft.index.hash_value(-(2**63) - 1)
