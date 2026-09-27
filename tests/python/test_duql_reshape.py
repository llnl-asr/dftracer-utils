"""duql window, expand, pivot, unpivot and quantifiers on a TraceViewer,
each against a pandas or plain Python reference over the same records."""

import gzip
import json
import math

import pandas as pd
import pyarrow as pa
import pytest

import dftracer.utils as dftu

N = 90
TAGS = (["a", "b"], [], ["c"], None)
H = ([], [1, 5], None, [1, "x"], [9, "x"], [0, 2], [None, 4])


def _record(i):
    args = {"metric": ("cpu", "mem", "io")[i % 3]}
    if i % 7:
        args["x"] = i % 5
    if TAGS[i % 4] is not None:
        args["tags"] = TAGS[i % 4]
    if H[i % 7] is not None:
        args["h"] = H[i % 7]
    return {
        "ph": "X",
        "name": "abcd"[i % 4],
        "cat": "POSIX",
        "pid": 1,
        "tid": i % 2,
        "ts": 1000 + 10 * i,
        "dur": (i * 37) % 11,
        "args": args,
    }


RECORDS = [_record(i) for i in range(N)]


@pytest.fixture(scope="module")
def tv(tmp_path_factory):
    path = str(tmp_path_factory.mktemp("reshape") / "reshape.pfw.gz")
    with gzip.open(path, "wt") as f:
        for r in RECORDS:
            f.write(json.dumps(r) + "\n")
    with dftu.Indexer(files=[path]) as indexer:
        indexer.ensure_indexed()
    return dftu.TraceViewer(path)


@pytest.fixture(scope="module")
def frame():
    rows = []
    for r in RECORDS:
        a = r["args"]
        rows.append(
            {
                "name": r["name"],
                "tid": r["tid"],
                "ts": r["ts"],
                "dur": r["dur"],
                "x": a.get("x"),
                "metric": a["metric"],
            }
        )
    return pd.DataFrame(rows)


def _table(tv, q):
    return pa.table(tv.duql(q).collect()).to_pydict()


def _same(got, want):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        if w is None or (isinstance(w, float) and math.isnan(w)):
            assert g is None
        else:
            assert g == pytest.approx(w)


def test_window_functions_match_pandas(tv, frame):
    q = (
        "window name sort -dur { r = row_number(), k = rank(),"
        " d = dense_rank(), l = lag(dur), n = lead(dur, 2),"
        " rs = running_sum(x), rc = running_count(), c = count(),"
        " cx = count(x), s = sum(dur), mn = min(x), mx = max(x),"
        " m = mean(dur), f = first(x), la = last(x) }"
        " | select ts, r, k, d, l, n, rs, rc, c, cx, s, mn, mx, m, f, la"
    )
    got = _table(tv, q)
    df = frame.sort_values(["name", "dur"], ascending=[True, False], kind="stable")
    g = df.groupby("name", sort=False)
    ref = pd.DataFrame(index=df.index)
    ref["r"] = g.cumcount() + 1
    ref["k"] = g["dur"].rank(method="min", ascending=False)
    ref["d"] = g["dur"].rank(method="dense", ascending=False)
    ref["l"] = g["dur"].shift(1)
    ref["n"] = g["dur"].shift(-2)
    ref["rs"] = g["x"].transform(lambda s: s.fillna(0).cumsum().where(s.notna().cumsum() > 0))
    ref["rc"] = g.cumcount() + 1
    ref["c"] = g["dur"].transform("size")
    ref["cx"] = g["x"].transform("count")
    ref["s"] = g["dur"].transform("sum")
    ref["mn"] = g["x"].transform("min")
    ref["mx"] = g["x"].transform("max")
    ref["m"] = g["dur"].transform("mean")
    ref["f"] = g["x"].transform("first")
    ref["la"] = g["x"].transform("last")
    ref = ref.sort_index()
    assert got["ts"] == list(frame["ts"])
    for c in ref.columns:
        _same(got[c], [None if pd.isna(v) else v for v in ref[c]])


def test_gap_to_previous_call(tv, frame):
    got = _table(tv, "window tid sort ts { g = ts - lag(ts) } | select g")
    want = frame.groupby("tid")["ts"].diff()
    _same(got["g"], [None if pd.isna(v) else v for v in want])


def _expand(keep_empty):
    out = []
    for r in RECORDS:
        tags = r["args"].get("tags")
        if tags is None:
            out.append((r["ts"], None, None))
        elif not tags:
            if keep_empty:
                out.append((r["ts"], None, None))
        else:
            out.extend((r["ts"], t, i) for i, t in enumerate(tags))
    return out


@pytest.mark.parametrize("keep_empty", [False, True])
def test_expand_matches_python(tv, keep_empty):
    q = "expand tags with_index i" + (" keep_empty" if keep_empty else "")
    got = _table(tv, q + " | select ts, tags, i")
    assert list(zip(got["ts"], got["tags"], got["i"])) == _expand(keep_empty)


@pytest.mark.parametrize("values", [None, ["mem", "zzz"]])
def test_pivot_matches_python(tv, frame, values):
    fixed = "" if values is None else " in " + json.dumps(values)
    got = _table(
        tv,
        "select name, metric, dur | pivot metric" + fixed + " { s = sum(dur), n = count() }",
    )
    keys = sorted(frame["name"].unique())
    cols = values if values is not None else sorted(frame["metric"].unique())
    assert list(got) == ["name"] + [f"{a}.{v}" for a in ("s", "n") for v in cols]
    assert got["name"] == keys
    for v in cols:
        cell = frame[frame["metric"] == v].groupby("name")["dur"]
        sums, counts = cell.sum(), cell.size()
        assert got[f"s.{v}"] == [int(sums[k]) if k in sums else None for k in keys]
        assert got[f"n.{v}"] == [int(counts[k]) if k in counts else None for k in keys]


def test_open_pivot_is_the_last_stage(tv):
    with pytest.raises(dftu.DFTUtilsValueError, match="in"):
        tv.duql("pivot metric { n = count() } | take 1")


def test_unpivot_matches_python(tv):
    got = _table(tv, "select ts, x, dur | unpivot x, dur as k, v | select ts, k, v")
    want = []
    for r in RECORDS:
        want.append((r["ts"], "x", r["args"].get("x")))
        want.append((r["ts"], "dur", r["dur"]))
    assert list(zip(got["ts"], got["k"], got["v"])) == want


def _truth(value):
    return value if isinstance(value, bool) else None


def _cmp(e, op, v):
    if e is None or isinstance(e, str) != isinstance(v, str):
        return None
    return {"<": e < v, ">": e > v, "==": e == v}[op]


def _quant(all_, items, op, v):
    if not isinstance(items, list):
        return None
    ts = [_cmp(e, op, v) for e in items]
    if all_:
        if any(t is False for t in ts):
            return False
        return True if all(t is True for t in ts) else None
    if any(t is True for t in ts):
        return True
    return False if all(t is False for t in ts) else None


@pytest.mark.parametrize(
    "form", [("any", ">", 3), ("all", ">", 0), ("any", "==", "x"), ("all", "<", 5)]
)
@pytest.mark.parametrize("negate", [False, True])
def test_quantifier_truth_tables(tv, form, negate):
    kind, op, v = form
    cond = f"{kind}(h, . {op} {json.dumps(v)})"
    if negate:
        cond = "not " + cond
    want = []
    for r in RECORDS:
        t = _quant(kind == "all", r["args"].get("h"), op, v)
        if negate and t is not None:
            t = not t
        if t is True:
            want.append(r["ts"])
    got = _table(tv, f"where {cond} | select ts")
    assert sorted(got["ts"]) == want


def test_quantifier_after_the_scan_matches_the_scan(tv):
    for cond in ('any(tags, . == "b")', 'all(tags, . != "a")', "any(tags, ^.dur > 5)"):
        scan = _table(tv, f"where {cond} | select ts")["ts"]
        after = _table(tv, f"derive z = 1 | where {cond} | select ts")["ts"]
        assert sorted(scan) == sorted(after)
        assert scan


def test_mixed_arrays_fail_after_the_scan(tv):
    with pytest.raises(dftu.DFTUtilsValueError, match="mix types"):
        tv.duql("derive z = 1 | where any(h, . > 3)").collect()
