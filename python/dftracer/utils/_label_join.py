# SPDX-License-Identifier: MIT
"""The pandas joins on row labels of two eager frames: ``DataFrame.join`` with
no key, and ``DataFrame.merge`` with ``left_index`` or ``right_index``.

Joining two label sides uses the native label align (``_align_rows``). A join of the
labels of one side with the columns of the other goes through the keyed join,
then puts the label side's index and the key column as pandas does."""

from __future__ import annotations

from collections import Counter
from typing import TYPE_CHECKING, Optional, Sequence, Tuple, cast

from . import dftracer_utils_ext as _ext
from ._enums import DType
from ._series import Series
from ._series_index import default_name, public_name

if TYPE_CHECKING:
    from ._dataframe import DataFrame

Suffixes = Tuple[Optional[str], Optional[str]]

LABEL_HOWS = ("left", "right", "inner", "outer", "full")
_CODES = {"inner": 0, "left": 1, "right": 2, "outer": 3, "full": 3}
_NUMBERS = frozenset(
    {
        DType.INT8,
        DType.INT16,
        DType.INT32,
        DType.INT64,
        DType.UINT8,
        DType.UINT16,
        DType.UINT32,
        DType.UINT64,
        DType.FLOAT32,
        DType.FLOAT64,
    }
)
_LID = "__dftu_lid"
_RID = "__dftu_rid"


def _frame(native: _ext.DataFrame) -> DataFrame:
    return cast("DataFrame", native)


def check_how(how: str, who: str) -> None:
    if how not in LABEL_HOWS:
        raise ValueError(
            f"{who}: how={how!r} is not valid on row labels; use one of "
            f"{', '.join(repr(h) for h in LABEL_HOWS)}"
        )


def suffix_maps(
    left: Sequence[str], right: Sequence[str], suffixes: Suffixes
) -> tuple[dict[str, str], dict[str, str]]:
    """The renames of the columns both sides have. A side with no suffix keeps
    its names; when neither side has one and a name repeats, pandas' error."""
    if len(suffixes) != 2:
        raise ValueError("suffixes must be a pair of two strings or None")
    lsuf, rsuf = (s or "" for s in suffixes)
    seen = set(right)
    clash = [c for c in left if c in seen]
    if not clash:
        return {}, {}
    if not lsuf and not rsuf:
        raise ValueError(f"columns overlap but no suffix specified: {clash}")
    lmap = {c: c + lsuf for c in clash} if lsuf else {}
    rmap = {c: c + rsuf for c in clash} if rsuf else {}
    names = [lmap.get(c, c) for c in left] + [rmap.get(c, c) for c in right]
    repeated = sorted(n for n, count in Counter(names).items() if count > 1)
    if repeated:
        raise ValueError(
            f"Passing 'suffixes' which cause duplicate columns {repeated} is not allowed."
        )
    return lmap, rmap


def _level_names(frame: DataFrame) -> list[str | None]:
    """The pandas names of the index levels (``None`` for an unnamed one)."""
    return [public_name(n) for n in frame.index_names()] or [None]


def label_join(left: DataFrame, right: DataFrame, how: str, suffixes: Suffixes) -> DataFrame:
    """The pandas row-label join: the rows of both frames on the joined labels,
    this frame's columns then ``other``'s, the overlapping ones suffixed. The
    result keeps the joined index and its names (the right frame's for
    ``"right"``). When a side has several levels and the names differ, the
    levels are matched by name."""
    check_how(how, "join")
    lnames, rnames = _level_names(left), _level_names(right)
    if (len(lnames) > 1 or len(rnames) > 1) and lnames != rnames:
        return _named_levels_join(left, right, how, suffixes)
    if _same_labels(left, right):
        return _named_levels_join(left, right, how, suffixes, positional=True)
    lmap, rmap = suffix_maps(left.columns, right.columns, suffixes)
    a, b = _ext.DataFrame._align_rows(left, right, _CODES[how], None)
    out = _frame(_ext.DataFrame.rename(a, lmap) if lmap else a)
    other = _frame(b)
    if how == "right":
        have = list(out.index_names())
        want = list(right.index_names()) or [default_name(i) for i in range(len(have))]
        if have != want:
            clash = [n for n in want if n in out.columns]
            if clash:
                raise ValueError(f"join: the index level {clash[0]!r} is also a column name")
            renamed = _ext.DataFrame.rename(out, dict(zip(have, want)))
            out = _frame(_ext.DataFrame.with_index(renamed, want))
    taken = set(out.index_names())
    for name in other.columns:
        shown = rmap.get(name, name)
        if shown in taken:
            raise ValueError(f"join: the column {shown!r} is also an index level name")
        out = out.with_column(shown, other[name])
    return out


def _named_levels_join(
    left: DataFrame, right: DataFrame, how: str, suffixes: Suffixes, positional: bool = False
) -> DataFrame:
    """Join on the index levels both frames name alike, whatever their order
    and number, as pandas does for a MultiIndex; with ``positional`` on every
    level in order (equal labels). The result levels are this frame's then
    the other's that are not shared (the right frame's first for ``"right"``);
    ``"outer"`` orders the rows by the shared labels."""
    lint, rint = list(left.index_names()), list(right.index_names())
    lpub, rpub = _level_names(left), _level_names(right)
    ltag = {n: f"__dftu_l{j}" for j, n in enumerate(lint)}
    rtag = {n: f"__dftu_r{j}" for j, n in enumerate(rint)}
    lside = [(n, ltag[i]) for n, i in zip(lpub, lint)] if lint else [(None, _LID)]
    rside = [(n, rtag[i]) for n, i in zip(rpub, rint)] if rint else [(None, _RID)]
    if positional:
        pairs = list(zip(range(len(lside)), range(len(rside))))
    else:
        if None in lpub and None in rpub:
            raise ValueError(
                "join: unnamed index levels on both sides are ambiguous; name the levels"
            )
        pairs = [(lpub.index(n), rpub.index(n)) for n in lpub if n is not None and n in rpub]
        if not pairs:
            raise ValueError("cannot join with no overlapping index names")
    lmap, rmap = suffix_maps(left.columns, right.columns, suffixes)

    def flat(frame: DataFrame, mapping: dict[str, str]) -> _ext.DataFrame:
        columns = frame._names()
        plain = _ext.DataFrame.with_index(_ext.DataFrame.select(frame, *columns), [])
        return _ext.DataFrame.rename(plain, mapping)

    lf = _ext.DataFrame.with_row_index(flat(left, {**lmap, **ltag}), _LID)
    rf = _ext.DataFrame.with_row_index(flat(right, {**rmap, **rtag}), _RID)
    ltmp = [lside[i][1] for i, _ in pairs]
    rtmp = [rside[j][1] for _, j in pairs]
    retype: dict[int, DType] = {}
    for m in range(len(pairs)):
        lt, rt = lf[ltmp[m]].type, rf[rtmp[m]].type
        if lt != rt and lt in _NUMBERS and rt in _NUMBERS:
            if how == "left" or (how == "inner" and len(lside) > 1):
                retype[m] = DType(lt)
            elif how == "right":
                retype[m] = DType(rt)
            ltmp[m], rtmp[m] = f"__dftu_lk{m}", f"__dftu_rk{m}"
            lf = _ext.DataFrame.with_column(
                lf,
                ltmp[m],
                _ext.DataFrame.__getitem__(lf, lside[pairs[m][0]][1]).astype(DType.FLOAT64),
            )
            rf = _ext.DataFrame.with_column(
                rf,
                rtmp[m],
                _ext.DataFrame.__getitem__(rf, rside[pairs[m][1]][1]).astype(DType.FLOAT64),
            )
    lmulti, rmulti = len(lside) > 1, len(rside) > 1
    keys: dict[int, Series] = {}
    levels: list[tuple[str | None, str, int | None]]
    if lmulti != rmulti and not positional:
        # One level against several: pandas walks the several-level frame in
        # its own order, keeps its rows when its side or "outer" asks for
        # them, and drops the unmatched rows of the one-level frame.
        keep = how in ("outer", "full") or how == ("left" if lmulti else "right")
        joined = _keyed_join(lf, rf, "left" if keep else "inner", ltmp, rtmp, swap=rmulti)
        levels = [(n, t, None) for n, t in (lside if lmulti else rside)]
    else:
        joined = _keyed_join(lf, rf, how, ltmp, rtmp)
        lhas = joined[_LID].notna()
        keys = {k: joined[ltmp[k]].where(lhas, joined[rtmp[k]]) for k in range(len(pairs))}
        for slot, kind in retype.items():
            keys[slot] = keys[slot].astype(kind)
        lshared = {i: k for k, (i, _) in enumerate(pairs)}
        rshared = {j: k for k, (_, j) in enumerate(pairs)}
        lentries = [(n, t, lshared.get(i)) for i, (n, t) in enumerate(lside)]
        rentries = [(n, t, rshared.get(j)) for j, (n, t) in enumerate(rside)]
        first, second = (rentries, lentries) if how == "right" else (lentries, rentries)
        levels = first + [e for e in second if e[2] is None]
    names_out = [lmap.get(c, c) for c in left.columns] + [rmap.get(c, c) for c in right.columns]
    final = [n if n is not None else default_name(p) for p, (n, _, _) in enumerate(levels)]
    clash = [n for n in final if n in names_out]
    if clash:
        raise ValueError(f"join: the index level {clash[0]!r} is also a column name")
    out = joined
    for name, (_, tag, k) in zip(final, levels):
        out = out.with_column(name, joined[tag] if k is None else keys[k])
    if how in ("outer", "full") and keys:
        for k in keys:
            out = out.with_column(f"__dftu_sk{k}", keys[k])
        out = out.sort_by_multi([f"__dftu_sk{k}" for k in keys], nulls_last=True)
    picked = _ext.DataFrame.select(out, *names_out, *final)
    return _frame(_ext.DataFrame.with_index(picked, final))


def _same_labels(left: DataFrame, right: DataFrame) -> bool:
    """Both frames have equal index labels, in the same order."""
    if len(left) != len(right):
        return False
    lv, rv = left.index_levels(), right.index_levels()
    return len(lv) == len(rv) and all(_same_level(a, b) for a, b in zip(lv, rv))


def _same_level(a: Series, b: Series) -> bool:
    """Equal values, comparing numbers of different types as Float64."""
    if a.equals(b):
        return True
    if a.type in _NUMBERS and b.type in _NUMBERS and a.type != b.type:
        return a.astype(DType.FLOAT64).equals(b.astype(DType.FLOAT64))
    return False


def _keyed_join(
    lf: _ext.DataFrame,
    rf: _ext.DataFrame,
    how: str,
    ltmp: list[str],
    rtmp: list[str],
    swap: bool = False,
) -> DataFrame:
    """The keyed join that gives pandas' row order: this frame's rows in
    order, each with its matches in the other's order. ``"right"`` or ``swap``
    walks the right frame instead (its rows in order, ``how`` read from it)."""
    swap = swap or how == "right"
    first, second = (rf, lf) if swap else (lf, rf)
    return _frame(
        _ext.DataFrame.join(
            _frame(first),
            _frame(second),
            on=None,
            how="left" if how == "right" else how,
            left_on=rtmp if swap else ltmp,
            right_on=ltmp if swap else rtmp,
            suffix="_right",
            nulls_equal=True,
        )
    )


def _plain(frame: DataFrame, mapping: dict[str, str]) -> _ext.DataFrame:
    """The data columns of ``frame``, renamed, with no index."""
    cols = frame.columns
    out = _ext.DataFrame.with_index(_ext.DataFrame.select(frame, *cols), [])
    return _ext.DataFrame.rename(out, mapping) if mapping else out


def index_merge(
    left: DataFrame,
    right: DataFrame,
    how: str,
    left_on: list[str] | None,
    right_on: list[str] | None,
    suffixes: Suffixes,
) -> DataFrame:
    """``merge`` of the index of one frame with columns of the other: exactly
    one of ``left_on`` and ``right_on`` is ``None``, and that frame's index is
    the key. As pandas, the result index is the other frame's index (null on
    the rows it has no match for) and each key column holds the key of
    whichever side has the row."""
    check_how(how, "merge")
    lcols, rcols = left.columns, right.columns
    lmap, rmap = suffix_maps(lcols, rcols, suffixes)
    if left_on is None:
        lkeys = left.index_levels()
        rkeys = [right[n] for n in cast("list[str]", right_on)]
        names = cast("list[str]", right_on)
    else:
        lkeys = [left[n] for n in left_on]
        rkeys = right.index_levels()
        names = left_on
    if len(lkeys) != len(rkeys):
        side, other = ("right", "left") if left_on is None else ("left", "right")
        raise ValueError(
            f'len({side}_on) must equal the number of levels in the index of "{other}"'
        )
    ltmp = [f"__dftu_lk{i}" for i in range(len(lkeys))]
    rtmp = [f"__dftu_rk{i}" for i in range(len(rkeys))]
    lf = _ext.DataFrame.with_row_index(_plain(left, lmap), _LID)
    rf = _ext.DataFrame.with_row_index(_plain(right, rmap), _RID)
    for name, key in zip(ltmp, lkeys):
        lf = _ext.DataFrame.with_column(lf, name, key)
    for name, key in zip(rtmp, rkeys):
        rf = _ext.DataFrame.with_column(rf, name, key)
    joined = _keyed_join(lf, rf, how, ltmp, rtmp)

    lid, rid = joined[_LID], joined[_RID]
    lhas = lid.notna()
    names_out = [lmap.get(c, c) for c in lcols] + [rmap.get(c, c) for c in rcols]
    replaced: dict[str, Series] = {}
    inserted: list[tuple[int, str, Series]] = []
    for i, name in enumerate(names):
        lkey, rkey = joined[ltmp[i]], joined[rtmp[i]]
        if name in names_out:
            if name in lcols:
                if lid.null_count:
                    replaced[name] = joined[name].where(lhas, rkey)
            elif rid.null_count:
                replaced[name] = lkey.where(lhas, joined[name])
        else:
            inserted.append((i, name, lkey.where(lhas, rkey)))

    labels, from_id = (right, rid) if left_on is None else (left, lid)
    level_names = labels.index_names() or [default_name(0)]
    key_levels = lkeys if left_on is None else rkeys
    wraps = len(key_levels) > 1 and len(labels) > 0
    take = from_id.fill_null(len(labels) - 1 if wraps else -1)
    clash = [n for n in level_names if n in names_out or any(n == k for _, k, _ in inserted)]
    if clash:
        raise ValueError(f"merge: the index level {clash[0]!r} is also a column name")
    out = joined
    if how in ("outer", "full"):
        sort_keys = [f"__dftu_sk{i}" for i in range(len(names))]
        for i, sort_name in enumerate(sort_keys):
            out = out.with_column(sort_name, joined[ltmp[i]].where(lhas, joined[rtmp[i]]))
    for name, series in {**replaced, **{k: s for _, k, s in inserted}}.items():
        out = out.with_column(name, series)
    for name, level in zip(level_names, labels.index_levels()):
        out = out.with_column(name, level.gather(take))
    if how in ("outer", "full"):
        out = out.sort_by_multi(sort_keys, nulls_last=True)
    order = list(names_out)
    for i, name, _ in inserted:
        order.insert(i, name)
    picked = _ext.DataFrame.select(out, *order, *level_names)
    return _frame(_ext.DataFrame.with_index(picked, level_names))
