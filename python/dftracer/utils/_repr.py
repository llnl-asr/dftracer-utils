"""Plain-text and HTML rendering of frames and series, shaped like pandas' default
``repr`` and built from only the rows that are shown."""

import html
import math
from typing import Any, List, Sequence, Tuple

MAX_ROWS = 60
EDGE_ROWS = 5
_ELLIPSIS = "..."


def _float_cells(values: Sequence[object]) -> List[str]:
    finite = [v for v in values if isinstance(v, float) and math.isfinite(v)]
    plain = all(v == 0 or 1e-6 <= abs(v) < 1e6 for v in finite)
    decimals = 1
    if plain:
        for v in finite:
            text = f"{v:.6f}".rstrip("0")
            decimals = max(decimals, len(text) - text.index(".") - 1)
    out = []
    for v in values:
        if v is None or (isinstance(v, float) and math.isnan(v)):
            out.append("NaN")
        elif isinstance(v, float) and not math.isfinite(v):
            out.append("inf" if v > 0 else "-inf")
        elif plain:
            out.append(f"{v:.{decimals}f}")
        else:
            out.append(f"{v:.6e}")
    return out


def _cells(values: Sequence[object], is_float: bool) -> List[str]:
    if is_float:
        return _float_cells(values)
    return ["None" if v is None else str(v) for v in values]


def _is_float(dtype: Any) -> bool:
    return str(getattr(dtype, "name", "")).startswith("FLOAT")


def _shown(n: int) -> "Tuple[bool, List[int]]":
    if n <= MAX_ROWS:
        return False, list(range(n))
    return True, list(range(EDGE_ROWS)) + list(range(n - EDGE_ROWS, n))


def _column(series: Any, cut: bool) -> List[str]:
    if cut:
        values = series.head(EDGE_ROWS).to_list() + series.tail(EDGE_ROWS).to_list()
    else:
        values = series.to_list()
    cells = _cells(values, _is_float(series.dtype))
    if cut:
        cells.insert(EDGE_ROWS, _ELLIPSIS)
    return cells


def _row_labels(n: int, cut: bool) -> List[str]:
    _, rows = _shown(n)
    labels = [str(i) for i in rows]
    if cut:
        labels.insert(EDGE_ROWS, _ELLIPSIS)
    return labels


def _table(
    index_names: List[str],
    index_cols: List[List[str]],
    names: List[str],
    cols: List[List[str]],
) -> str:
    index_widths = [max(len(h), *(len(c) for c in col)) for h, col in zip(index_names, index_cols)]
    widths = [max(len(h), *(len(c) for c in col)) for h, col in zip(names, cols)]
    header = "  ".join(
        [h.ljust(w) for h, w in zip(index_names, index_widths)]
        + [h.rjust(w) for h, w in zip(names, widths)]
    )
    lines = [header]
    for i in range(len(cols[0])):
        lines.append(
            "  ".join(
                [col[i].ljust(w) for col, w in zip(index_cols, index_widths)]
                + [col[i].rjust(w) for col, w in zip(cols, widths)]
            )
        )
    return "\n".join(lines)


def _frame_parts(
    frame: Any,
) -> "Tuple[bool, List[str], List[List[str]], List[str], List[List[str]]]":
    columns = list(frame.columns)
    cut, _ = _shown(len(frame))
    index = getattr(frame, "_index", None)
    if index:
        index_names = list(index)
        index_cols = [_column(frame[c], cut) for c in index_names]
    else:
        index_names = [""]
        index_cols = [_row_labels(len(frame), cut)]
    names = [c for c in columns if c not in index_names]
    if not names:
        names = columns
        index_names = [""]
        index_cols = [_row_labels(len(frame), cut)]
    cols = [_column(frame[c], cut) for c in names]
    return cut, index_names, index_cols, names, cols


def frame_repr(frame: Any) -> str:
    columns = list(frame.columns)
    n = len(frame)
    if n == 0 or not columns:
        return f"Empty DataFrame\nColumns: [{', '.join(columns)}]\nIndex: []"
    cut, index_names, index_cols, names, cols = _frame_parts(frame)
    text = _table(index_names, index_cols, names, cols)
    if cut:
        text += f"\n\n[{n} rows x {len(names)} columns]"
    return text


def frame_html(frame: Any) -> str:
    columns = list(frame.columns)
    n = len(frame)
    if n == 0 or not columns:
        return f"<pre>{html.escape(frame_repr(frame))}</pre>"
    cut, index_names, index_cols, names, cols = _frame_parts(frame)
    th = "".join(f"<th>{html.escape(h)}</th>" for h in index_names + names)
    out = [
        '<table border="1" class="dataframe">',
        '<thead><tr style="text-align: right;">',
        th,
        "</tr></thead>",
        "<tbody>",
    ]
    for i in range(len(cols[0])):
        cells = "".join(f"<th>{html.escape(c[i])}</th>" for c in index_cols)
        cells += "".join(f"<td>{html.escape(c[i])}</td>" for c in cols)
        out.append(f"<tr>{cells}</tr>")
    out.append("</tbody></table>")
    if cut:
        out.append(f"<p>{n} rows x {len(names)} columns</p>")
    return "\n".join(out)


def series_repr(series: Any) -> str:
    n = len(series)
    dtype = str(series.dtype.name).lower()
    if n == 0:
        return f"Series([], dtype: {dtype})"
    cut, _ = _shown(n)
    labels = _row_labels(n, cut)
    cells = _column(series, cut)
    lw = max(len(s) for s in labels)
    vw = max(len(s) for s in cells)
    lines = [f"{a.ljust(lw)}    {b.rjust(vw)}" for a, b in zip(labels, cells)]
    footer = f"Length: {n}, dtype: {dtype}" if cut else f"dtype: {dtype}"
    return "\n".join(lines + [footer])
