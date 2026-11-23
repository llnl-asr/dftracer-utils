# SPDX-License-Identifier: MIT
"""The pandas ``Series`` surface that is a composition of engine ops, mixed
into :class:`Series`: spellings (``between``, ``drop_duplicates``,
``factorize``), the frame-shaped results (``to_frame``, ``describe``,
``reset_index``), positional accessors and the conveniences."""

from __future__ import annotations

import math
import numbers
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Iterator,
    Literal,
    Mapping,
    Sequence,
    TypeVar,
    Union,
    cast,
)

from ._enums import DType
from ._label_align import LabelAlignMixin
from ._levels import (
    LevelArg,
    LevelsMixin,
    label_text,
    pick_levels,
    sort_plan,
    sorted_positions,
)
from ._reshape import unstack_series
from ._series_index import (
    default_name,
    drop_levels,
    labels_from,
    levels_of,
    names_of,
    positional,
    positions,
    public_name,
    rename_levels,
    renamed_names,
    reset,
    to_frame,
    with_levels,
)
from ._time_index import TimeIndexMixin

if TYPE_CHECKING:
    import numpy as np
    import numpy.typing as npt
    import pyarrow as pa
    from numpy import generic
    from pandas._libs.missing import NAType
    from typing_extensions import Concatenate, ParamSpec

    from ._dataframe import DataFrame
    from ._groupby import GroupBy
    from ._series import Series
    from .dftracer_utils_ext import Scalar

    # A value of ``replace`` as the caller writes it, and as the engine takes it.
    PlainScalar = Union[int, float, str, None, NAType]
    ReplaceScalar = Union[PlainScalar, generic]
    ReplaceValue = Union[ReplaceScalar, Sequence[ReplaceScalar]]
    ReplaceKey = Union[ReplaceValue, Mapping[ReplaceScalar, ReplaceScalar]]

_Scalar = Union[int, float]
_R = TypeVar("_R")
if TYPE_CHECKING:
    _P = ParamSpec("_P")

_CMP_NE = 5  # DFTU_CMP_NE, dataframe/c_api.h

# Dtypes the row-by-row compare kernel takes; the others (nested, decimal)
# compare by value in Python.
_NATIVE_COMPARE = frozenset(
    [
        DType.BOOL,
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
        DType.STRING,
        DType.LARGE_STRING,
        DType.BINARY,
        DType.LARGE_BINARY,
        DType.DATE32,
        DType.DATE64,
        DType.TIME32,
        DType.TIME64,
        DType.TIMESTAMP,
        DType.DURATION,
    ]
)


def _kernel_operand(s: Series) -> Series:
    return s.astype("int8") if s.dtype == DType.BOOL else s


_TEMPORAL = frozenset([DType.TIME32, DType.TIME64, DType.TIMESTAMP, DType.DURATION])
_FLOATS = frozenset([DType.FLOAT32, DType.FLOAT64])
_BYTES = frozenset([DType.STRING, DType.LARGE_STRING, DType.BINARY, DType.LARGE_BINARY])


def _is_number(d: DType) -> bool:
    return d.is_numeric or d == DType.BOOL


def _scaled_differs(a: Series, b: Series) -> Series[bool]:
    """``differs`` for a duration or time in two units: the finer side ``f``
    times coarser equals the finer value exactly when it divides evenly and
    the quotient matches, so no value is scaled up and none can overflow."""
    if a.time_unit > b.time_unit:
        return _scaled_differs(b, a)
    factor = 1000 ** (b.time_unit - a.time_unit)
    fine, coarse = b.astype("int64"), a.astype("int64")
    same = ((fine % factor) == 0) & ((fine // factor) == coarse)
    ne = (~same).fill_null(False)
    nulls = _kernel_operand(a.is_null()).compare_series(_kernel_operand(b.is_null()), _CMP_NE)
    return cast("Series[bool]", ne | nulls)


def differs(a: Series, b: Series) -> Series[bool]:
    """Bool, never null: true where the two Series (one length) hold different
    values; a null equals a null, a null differs from a value and a NaN equals
    a NaN only at the same position. A number against text differs on every
    present row. Times of one type but another unit or zone compare as
    instants (a timestamp), differ on every present row (a timestamp, aware
    against naive) or exactly in integers (a duration or time of two units); a
    pair of dtypes the row compare kernel does not take (nested, decimal,
    mixed time kinds) also compares by value in Python."""
    da, db = a.dtype, b.dtype
    if (_is_number(da) and db in _BYTES) or (da in _BYTES and _is_number(db)):
        return ~(a.is_null() & b.is_null())
    by_value = not ((da == db and da in _NATIVE_COMPARE) or (_is_number(da) and _is_number(db)))
    if da == db and da in _TEMPORAL and (a.time_unit, a.timezone) != (b.time_unit, b.timezone):
        if da != DType.TIMESTAMP:
            return _scaled_differs(a, b)
        elif bool(a.timezone) != bool(b.timezone):
            return ~(a.is_null() & b.is_null())
        else:
            a, b = a.dt.epoch("ns"), b.dt.epoch("ns")
    if by_value:
        from ._series import Series

        return cast(
            "Series[bool]",
            Series.from_list(
                [
                    not (x == y or (x is None and y is None))
                    for x, y in zip(a.to_list(), b.to_list())
                ],
                DType.BOOL,
            ),
        )
    ne = _kernel_operand(a).compare_series(_kernel_operand(b), _CMP_NE).fill_null(False)
    if da in _FLOATS and db in _FLOATS:
        ne = ne & ~(a.is_nan().fill_null(False) & b.is_nan().fill_null(False))
    if a.null_count == 0 and b.null_count == 0:
        return ne
    nulls = _kernel_operand(a.is_null()).compare_series(_kernel_operand(b.is_null()), _CMP_NE)
    return ne | nulls


def valid_edge(mask: Series, first: bool) -> int | None:
    """The position of the first (or last) true row of a Bool mask, ``None``
    when none is true."""
    if not mask.any():
        return None
    flags = mask.astype("int8")
    if first:
        return flags.arg_max()
    return len(flags) - 1 - flags.reverse().arg_max()


def present_edge(s: Series, first: bool) -> int | None:
    """The position of the first (or last) present value of ``s``, ``None``
    when it has none; a column with no nulls answers without a scan."""
    nulls = s.null_count
    if nulls == len(s):
        return None
    if nulls == 0:
        return 0 if first else len(s) - 1
    return valid_edge(s.is_not_null(), first)


def edge_label(labels: object, pos: int | None) -> object | None:
    """The row label at ``pos`` of an index as ``.index`` gives it: a Series
    of labels, a frame of levels (a tuple, as pandas), or positions when there
    is none."""
    from ._dataframe import DataFrame
    from ._pandas_frame import column_series
    from ._series import Series

    if pos is None:
        return None
    if isinstance(labels, Series):
        return labels.iloc[pos]
    if isinstance(labels, DataFrame):
        return tuple(column_series(labels, n).iloc[pos] for n in labels.columns)
    return pos


def present_or(s: Series, other: Series, kept: DType | None = None) -> Series:
    """``s`` where it has a value, else ``other``, for two Series on the same
    labels, as pandas ``combine_first``: two number types meet as Float64, and
    an all-null side of another kind takes the other's type. The result goes
    back to ``kept`` (the type both had before align) when it has no nulls."""
    if s.dtype != other.dtype:
        if s.dtype.is_numeric and other.dtype.is_numeric:
            s = s.cast("Float64")
            other = other.cast("Float64")
        elif other.null_count == len(other):
            other = other.cast(s.dtype)
        elif s.null_count == len(s):
            s = s.cast(other.dtype)
    out = s.where(s.is_not_null(), other)
    if kept is not None and out.dtype != kept and out.null_count == 0:
        out = out.cast(kept)
    return out


def realigned(aligned: Series, before: Series) -> Series:
    """A column one side alone has, on the aligned rows: back in its own type
    when align added no null, as pandas."""
    if aligned.dtype != before.dtype and aligned.null_count == before.null_count:
        return aligned.cast(before.dtype)
    return aligned


def kept_by_update(out: Series, kept: DType) -> Series:
    """``out`` in ``kept``, the type of the frame or Series that ``update``
    writes into, as pandas, which refuses a value that type cannot hold."""
    if out.dtype == kept:
        return out
    back = out.cast(kept)
    if (back.cast(out.dtype) != out).fill_null(False).any():
        raise TypeError(f"Invalid value for dtype '{kept.name.lower()}'")
    return back


def has_duplicate_labels(obj: object) -> bool:
    """Whether the row labels of a Series or a frame repeat (positions never
    do)."""
    from ._dataframe import DataFrame
    from ._series import Series

    labels = getattr(obj, "index", None)
    if isinstance(labels, (Series, DataFrame)):
        return bool(labels.duplicated().any())
    return False


def kept_type(a: Series | None, b: Series | None) -> DType | None:
    """The type to give a combined column back: the one both sides had."""
    if a is None or b is None or a.dtype != b.dtype:
        return None
    return a.dtype


class Omitted:
    """The type of ``OMITTED``: ``replace``'s ``value`` when the caller gave none."""


OMITTED = Omitted()


def _plain(v: ReplaceScalar) -> PlainScalar:
    """A NumPy scalar as the Python number the engine accepts."""
    if type(v).__module__ == "numpy" and getattr(v, "ndim", 1) == 0:
        return cast("PlainScalar", getattr(v, "item")())
    raw: object = v
    return cast("PlainScalar", raw)


def _kind(v: PlainScalar) -> str:
    """What a replace value is: ``null``, ``nan``, ``str`` or ``num``."""
    if v is None or type(v).__name__ == "NAType":  # pd.NA, without importing pandas
        return "null"
    if isinstance(v, str):
        return "str"
    if type(v).__name__ in ("bool", "bool_"):
        raise TypeError("replace: bool values are not supported")
    if isinstance(v, numbers.Real):
        return "nan" if v != v else "num"
    raise TypeError(f"replace: cannot replace a {type(v).__name__} value")


def replace_pairs(
    to_replace: ReplaceKey, value: ReplaceValue | Omitted
) -> list[tuple[ReplaceScalar, ReplaceScalar]]:
    """The (old, new) pairs of any replace form."""
    if isinstance(to_replace, dict):
        if value is not OMITTED:
            raise TypeError("replace: value must be omitted when to_replace is a dict")
        return list(to_replace.items())
    if isinstance(value, Omitted):
        raise TypeError("replace() missing required argument: 'value'")
    if isinstance(to_replace, (list, tuple)):
        keys = cast("Sequence[ReplaceScalar]", to_replace)
        if isinstance(value, (list, tuple)):
            if len(value) != len(keys):
                raise ValueError(
                    f"replace: to_replace has {len(keys)} values but value has {len(value)}"
                )
            return list(zip(keys, cast("Sequence[ReplaceScalar]", value)))
        return [(old, cast("ReplaceScalar", value)) for old in keys]
    if isinstance(value, (list, tuple)):
        raise TypeError("replace: value is a list but to_replace is a scalar")
    return [(cast("ReplaceScalar", to_replace), cast("ReplaceScalar", value))]


def replace_in(
    s: Series,
    pairs: list[tuple[ReplaceScalar, ReplaceScalar]],
    column: str | None,
    strict: bool,
) -> Series:
    """``s`` with every pair applied together. ``column`` names a frame column in
    errors; ``strict`` False leaves a column of an unsupported type alone."""
    from ._enums import DType
    from ._series import Series

    dtype = s.dtype
    if dtype == DType.STRING:
        is_str = True
    elif dtype.is_numeric:
        is_str = False
    elif strict:
        raise TypeError(f"replace: a {dtype.name.lower()} column is not supported")
    else:
        return s
    floats = dtype in (DType.FLOAT32, DType.FLOAT64)
    steps: list[tuple[Series, Series | int | float | None]] = []
    for old, new in pairs:
        old, new = _plain(old), _plain(new)
        old_kind, new_kind = _kind(old), _kind(new)
        hit: Series[bool] | None
        if old_kind == "null":
            hit = s.is_null()
        elif old_kind == "nan":
            hit = (s != s) if floats else None
        elif is_str == (old_kind == "str"):
            hit = s.str_eq(cast(str, old)) if is_str else s.eq(cast(_Scalar, old))
        else:
            hit = None  # an old value the column type cannot hold matches nothing
        if hit is None:
            continue
        if new_kind == "null":
            other: Series | int | float | None = None
        elif (new_kind == "str") != is_str:
            where = f" (column {column!r})" if column is not None else ""
            what = "a string" if new_kind == "str" else "a number"
            raise TypeError(f"replace: cannot put {what} into a {dtype.name.lower()} column{where}")
        else:
            other = Series.from_list([new] * len(s)) if is_str else cast(_Scalar, new)
        steps.append((hit, other))
    out: Series = s
    for step_hit, step_other in steps:
        out = out.mask(step_hit, step_other)
    return out


class SeriesPandasMixin(LabelAlignMixin, LevelsMixin, TimeIndexMixin):
    """``self`` is a :class:`Series`."""

    __slots__ = ()

    def _s(self) -> Series:
        from ._series import Series

        assert isinstance(self, Series)
        return self

    def _level_names(self) -> list[str]:
        return names_of(self._s())

    def _keep_levels(self, order: list[int]) -> Series:
        s = self._s()
        names = names_of(s)
        levels = levels_of(s)
        return with_levels(s, [names[i] for i in order], [levels[i] for i in order])

    def _axis_is_rows(self, axis: object) -> bool:
        if axis not in (0, "index", "rows"):
            raise ValueError(f"No axis named {axis} for object type Series")
        return True

    # -- spellings -----------------------------------------------------------------
    def between(self, left: _Scalar, right: _Scalar, inclusive: str = "both") -> Series:
        """``left <= x <= right`` as a Bool mask; ``inclusive`` is ``both``
        (default), ``neither``, ``left`` or ``right``."""
        s = self._s()
        if inclusive == "both":
            return s.is_between(left, right)
        lo = s.ge(left) if inclusive == "left" else s.gt(left)
        hi = s.le(right) if inclusive == "right" else s.lt(right)
        if inclusive not in ("left", "right", "neither"):
            raise ValueError("between: inclusive must be both, neither, left or right")
        return lo.logical(0, hi)

    def drop_duplicates(self, keep: Literal["first", "last"] | bool = "first") -> Series:
        """The pandas ``drop_duplicates``: the values that are not a repeat,
        in order, one null kept. ``keep`` is ``"first"`` (default), ``"last"``
        or ``False`` (drop every value that repeats). See :meth:`unique`."""
        s = positional(self._s())
        return s.filter(~s.duplicated(keep))

    @property
    def is_monotonic_increasing(self) -> bool:
        return self._s().is_sorted()

    @property
    def is_monotonic_decreasing(self) -> bool:
        return self._s().is_sorted(descending=True)

    @property
    def hasnans(self) -> bool:
        s = self._s()
        return s.null_count > 0 or (s.dtype.is_float and bool(s.is_nan().any()))

    @property
    def empty(self) -> bool:
        return len(self._s()) == 0

    @property
    def ndim(self) -> int:
        return 1

    @property
    def values(self) -> npt.NDArray[np.generic]:
        return self._s().to_numpy()

    @property
    def array(self) -> pa.Array:
        return self._s().to_arrow()

    def items(self) -> Iterator[tuple[int, object]]:
        return enumerate(self._s().to_list())

    def keys(self) -> list[int]:
        return list(range(len(self._s())))

    def get(self, key: object, default: object = None) -> object:
        """The value or rows ``loc`` finds for the label ``key``, ``default``
        when no row has it (a Series without labels has the positions as
        labels)."""
        from ._loc import series_by_label

        try:
            return series_by_label(self._s(), key)
        except (KeyError, IndexError, TypeError):
            return default

    def copy(self, deep: bool = True) -> Series:
        """A new handle on the same immutable buffers (:meth:`share`)."""
        return self._s().share()

    def convert_dtypes(
        self,
        infer_objects: bool = True,
        convert_string: bool = True,
        convert_integer: bool = True,
        convert_boolean: bool = True,
        convert_floating: bool = True,
    ) -> Series:
        """The pandas rules that map to engine types. A column is typed and
        nullable already, so only ``convert_integer`` changes it: a Float64
        column whose valid values are all whole numbers (nulls allowed)
        becomes Int64. A float ``NaN`` is a value here and not a null, so a
        column holding one stays Float64, where pandas reads it as missing and
        converts. The other keywords change nothing."""
        s = self._s()
        if not convert_integer or s.dtype not in (DType.FLOAT32, DType.FLOAT64):
            return self.copy()
        if not (s.floor() == s).fill_null(True).all():
            return self.copy()
        largest = s.abs().max()
        if largest is not None and cast(float, largest) >= 2.0**63:
            return self.copy()
        return s.astype("int64")

    def infer_objects(self) -> Series:
        """:meth:`copy`: an engine column is already typed."""
        return self.copy()

    def squeeze(self) -> Scalar | Series:
        """The one element of a one-element Series, else the Series itself."""
        s = self._s()
        return cast("Scalar | Series", s.item(0) if len(s) == 1 else s)

    @property
    def T(self) -> Series:
        """The Series itself (it has one dimension)."""
        return self._s()

    def transpose(self) -> Series:
        """The Series itself (it has one dimension)."""
        return self._s()

    @property
    def dtypes(self) -> DType:
        """:attr:`dtype` (the pandas Series spelling)."""
        return self._s().dtype

    def pipe(
        self, func: Callable[Concatenate[Series, _P], _R], *args: _P.args, **kwargs: _P.kwargs
    ) -> _R:
        return func(self._s(), *args, **kwargs)

    def factorize(self) -> tuple[Series, Series]:
        """``(codes, uniques)``: ``uniques`` is :meth:`unique` (the distinct
        values, sorted, nulls dropped) and ``codes`` each value's position in
        it, -1 for a null (pandas numbers uniques by first occurrence)."""
        s = self._s()
        uniques = s.drop_nulls().unique()
        codes = cast("Series", uniques.search_sorted(s.fill_null(0) if s.null_count else s))
        if s.null_count:
            codes = codes.mask(s.is_null(), -1)
        return codes, uniques

    def repeat(self, repeats: int) -> Series:
        """Each element repeated ``repeats`` times, in place, with its label."""
        s = positional(self._s())
        n = int(repeats)
        if n < 0:
            raise ValueError("repeat: repeats must be non-negative")
        if n == 0:
            return s.head(0)

        frame = to_frame(s, "v").with_column("_i", positions(len(s)))
        out = frame.concat(*[frame] * (n - 1)).sort_values("_i")["v"]
        return out.rename(s.name)

    def replace(self, to_replace: ReplaceKey, value: ReplaceValue | Omitted = OMITTED) -> Series:
        """Replace values, pandas' forms: ``replace(old, new)``,
        ``replace([old, ...], new)``, ``replace([old, ...], [new, ...])`` (paired
        by position) and ``replace({old: new, ...})``. Every match is found on the
        original values and applied together, so ``{1: 2, 2: 1}`` swaps. A new value
        of ``None`` or ``pd.NA`` makes the value null; a NaN makes it NaN. An old
        ``None`` or ``pd.NA`` matches the nulls, an old NaN the NaN values. The
        column keeps its type (a float into an integer column widens it, as
        :meth:`where` does); a string needs a string column and a number a numeric
        one, else ``TypeError``. An old value its column type cannot hold matches
        nothing. Bool, list and temporal columns raise ``TypeError``."""
        return replace_in(self._s(), replace_pairs(to_replace, value), None, True)

    def to_frame(self, name: str | None = None) -> DataFrame:
        """A one-column frame; the column is ``name``, else this Series' name,
        else ``"0"``. The row labels become the frame's row index. A name that
        a label level also has raises ``ValueError`` (pandas allows it)."""
        return to_frame(self._s(), name)

    def to_dict(self) -> dict[int, object]:
        return dict(enumerate(self._s().to_list()))

    def reset_index(
        self,
        level: LevelArg = None,
        *,
        drop: bool = False,
        name: str | Omitted = OMITTED,
    ) -> Series | DataFrame:
        """The pandas ``reset_index``: a frame of the row labels as columns
        (``index`` for one unnamed level, ``level_<i>`` for several; named levels
        keep their name) and the values under ``name`` (default: the Series' name,
        else ``"0"``). ``level`` picks which levels move out, the others stay the
        index; with ``drop`` the Series itself comes back under the remaining
        levels. A positional Series has the one level ``index`` of positions. A
        column name that repeats raises ``ValueError`` (a frame has no duplicate
        names, so ``allow_duplicates`` is not offered)."""
        return reset(self._s(), level, drop, name)

    def unstack(
        self, level: LevelArg = -1, fill_value: object = None, sort: bool = True
    ) -> DataFrame:
        """The pandas ``unstack``: the row-label ``level`` (a name, a position,
        a negative position or a list) becomes the columns, one for each distinct
        label, named with its text (the text of the tuple for a list). The other
        levels are the result's index, sorted; a cell the Series has no value
        for is null, or ``fill_value``. ``sort=False`` keeps the order of first
        appearance (pandas 2 returns wrong values for it). ``ValueError`` for a
        Series with fewer than two levels and for a label pair that occurs
        twice. The name of the column level is lost."""
        return cast("DataFrame", unstack_series(self._s(), level, fill_value, sort))

    def reindex(
        self,
        index: object = None,
        *,
        axis: int | str | None = None,
        method: str | None = None,
        copy: bool | None = None,
        level: object = None,
        fill_value: object = None,
        limit: int | None = None,
        tolerance: object = None,
    ) -> Series:
        """The pandas ``reindex``: this Series on the row labels ``index`` (a
        list, a Series, a pandas ``Index``; tuples for a ``MultiIndex``), by the
        native label lookup. A label with no row gets a null (an integer column
        becomes Float64) or ``fill_value``, which keeps the type when it fits.
        ``method`` (``'ffill'``/``'pad'``, ``'bfill'``/``'backfill'``,
        ``'nearest'``) fills it from the row of the nearest label on monotonic
        labels, at most ``limit`` labels per row and no farther than
        ``tolerance`` (a number, or a timedelta for timestamp labels).
        ``ValueError`` for repeated labels (unless ``index`` equals them), for a
        method on labels that are not monotonic, and for ``limit`` or
        ``tolerance`` with no method. A positional Series has the labels
        ``0 .. n-1``. ``level`` is not supported."""
        from ._reindex import reindex_series

        return reindex_series(
            self._s(),
            index,
            axis=axis,
            method=method,
            level=level,
            fill_value=fill_value,
            limit=limit,
            tolerance=tolerance,
        )

    def set_axis(self, labels: object, *, axis: int | str = 0, copy: bool | None = None) -> Series:
        """The pandas ``set_axis``: this Series under new row labels, a sequence
        as long as it (of tuples for a ``MultiIndex``), a Series, a frame (one
        level per column) or a pandas ``Index``. ``ValueError`` for another
        length, ``ValueError`` for an axis other than 0."""
        if axis not in (0, "index", "rows"):
            raise ValueError(f"No axis named {axis} for object type Series")
        s = self._s()
        names, levels = labels_from(labels, len(s))
        return with_levels(s, names, levels)

    def rename_axis(
        self,
        mapper: object = OMITTED,
        *,
        index: object = OMITTED,
        axis: int | str = 0,
        copy: bool | None = None,
        inplace: bool = False,
    ) -> Series:
        """The pandas ``rename_axis``: the name of the row labels (``None`` to
        clear it). A list names each level of a ``MultiIndex``; with ``index=`` a
        dict or a function maps the current names. A positional Series gains the
        labels ``0 .. n-1`` under the name."""
        if inplace:
            raise ValueError("rename_axis: a Series is immutable; use the result")
        if axis not in (0, "index", "rows"):
            raise ValueError(f"No axis named {axis} for object type Series")
        given = index if not isinstance(index, Omitted) else mapper
        if isinstance(given, Omitted):
            return self._s()
        if given is mapper and (callable(given) or isinstance(given, dict)):
            raise ValueError("Use `.rename` to alter labels with a mapper.")
        s = self._s()
        return rename_levels(s, renamed_names(names_of(s) or [default_name(0)], given))

    @property
    def index(self) -> Series | DataFrame:
        """The row labels: a Series of the labels for one level (named by the
        level), a frame of them for several, and the positions ``0 .. n-1`` (the
        pandas ``RangeIndex``) for a Series with none. The labels of a Series
        that came from ``df[col]`` are the frame's row index; they follow the
        rows through ``sort_values``, ``filter``, ``head`` and the like, and
        two labelled Series align on them in arithmetic."""
        from ._dataframe import DataFrame

        s = self._s()
        names = names_of(s)
        if not names:
            return positions(len(s))
        levels = levels_of(s)
        if len(names) == 1:
            return levels[0].rename(public_name(names[0]))
        return DataFrame.from_dict(dict(zip(names, levels)))

    @property
    def axes(self) -> list[Series | DataFrame]:
        """``[index]``, the pandas ``axes`` of a Series."""
        return [self.index]

    def truncate(self, before: object = None, after: object = None) -> Series:
        """The pandas ``truncate``: the rows whose label is between ``before`` and
        ``after``, both included, of a sorted index (``ValueError`` when the
        labels are not sorted, or ``after`` is before ``before``; a Series with
        several levels is not supported). A positional Series takes them as
        positions ``[before, after]``."""
        s = self._s()
        names = names_of(s)
        if not names:
            s = positional(s)
            lo = 0 if before is None else int(cast(int, before))
            hi = len(s) - 1 if after is None else int(cast(int, after))
            return s.slice(lo, max(hi - lo + 1, 0))
        if len(names) != 1:
            raise NotImplementedError("truncate: a Series with several index levels")
        label = levels_of(s)[0]
        ascending = label.is_sorted()
        if not ascending and not label.is_sorted(descending=True):
            raise ValueError("truncate requires a sorted index")
        lo, hi = cast("tuple[Any, Any]", (before, after) if ascending else (after, before))
        if before is not None and after is not None and lo > hi:
            raise ValueError(f"Truncate: {after} must be after {before}")
        keep = None
        if lo is not None:
            keep = label >= lo
        if hi is not None:
            upper = label <= hi
            keep = upper if keep is None else keep & upper
        return s if keep is None else s.filter(keep)

    def _rows_where(self, mask: Series) -> Series:
        return self._s().filter(mask)

    def first_valid_index(self) -> object | None:
        """The label of the first present value (``None`` when there is
        none)."""
        s = self._s()
        return edge_label(s.index, present_edge(s, first=True))

    def last_valid_index(self) -> object | None:
        s = self._s()
        return edge_label(s.index, present_edge(s, first=False))

    def asof(self, where: int) -> object:
        """The last present value at or before position ``where`` (pandas
        ``Series.asof`` over the positional index): ``ffill`` then the cell."""
        s = self._s()
        if where < 0 or where >= len(s):
            raise IndexError(f"asof: position {where} out of range")
        return s.ffill()[where]

    def autocorr(self, lag: int = 1) -> float:
        """Pearson correlation with itself shifted by ``lag`` (pandas
        ``autocorr``); the shifted-in nulls drop from the pair."""
        s = self._s()
        shifted = s.shift(lag)
        keep = shifted.is_not_null()
        return s.filter(keep).corr(shifted.filter(keep))

    def combine_first(self, other: Series) -> Series:
        """The pandas ``combine_first``: both on the union of their labels, with
        this Series' value where it has one and ``other``'s elsewhere."""
        mine = self._s()
        if has_duplicate_labels(mine) or has_duplicate_labels(other):
            raise ValueError("cannot reindex on an axis with duplicate labels")
        s, o = mine.align(other, join="outer")
        return present_or(s, cast("Series", o), kept_type(mine, other))

    def update(self, other: Series) -> Series:
        """``other`` on this Series' labels, and its present values over this
        Series' (pandas ``update`` mutates, this returns the new Series)."""
        mine = self._s()
        if has_duplicate_labels(other):
            raise ValueError("cannot reindex on an axis with duplicate labels")
        s, o = mine.align(other, join="left")
        out = kept_by_update(present_or(cast("Series", o), s), mine.dtype)
        return out.rename(mine.name)

    def drop(self, positions: int | Sequence[int]) -> Series:
        """Every element except those at ``positions`` (the positional index),
        pandas ``Series.drop``."""
        s = self._s()
        gone = {positions} if isinstance(positions, int) else set(positions)
        for p in gone:
            if p < 0 or p >= len(s):
                raise KeyError(p)
        from ._dataframe import DataFrame

        index = DataFrame.from_dict({"v": s}).with_row_index("_i")["_i"]
        return s.filter(~index.is_in(sorted(gone)))

    def equals(self, other: object) -> bool:
        """Same type, length and values, null for null (pandas ``equals``)."""
        from ._series import Series

        s = self._s()
        if not isinstance(other, Series) or s.dtype != other.dtype or len(s) != len(other):
            return False
        if s.dtype not in _NATIVE_COMPARE:
            return s.to_list() == other.to_list()
        return not differs(s, other).any()

    def pop(self, item: object = None) -> object:
        raise TypeError("pop: a Series is immutable; use drop(position) for the rest")

    def sem(self, ddof: int = 1) -> float:
        """Standard error of the mean: ``std(ddof) / sqrt(count)``."""
        s = self._s()
        n = s.count()
        return float("nan") if n == 0 else cast(float, s.std(ddof)) / math.sqrt(n)

    def xs(
        self, key: object, axis: int = 0, level: object = None, drop_level: bool = True
    ) -> object:
        """The rows with the label ``key`` (:attr:`loc`); with ``level`` the rows
        whose index level of that name or position is ``key``, that level left
        out unless ``drop_level`` is false."""
        from ._loc import series_by_label

        if axis not in (0, "index"):
            raise ValueError("xs: a Series has one axis")
        return series_by_label(self._s(), key, level, drop_level)

    def sort_index(
        self,
        ascending: bool | Sequence[bool] = True,
        na_position: Literal["first", "last"] = "last",
        *,
        axis: int | str = 0,
        level: LevelArg = None,
        sort_remaining: bool = True,
        ignore_index: bool = False,
        kind: str = "quicksort",
        key: Callable[[Series], Series] | None = None,
        inplace: bool = False,
    ) -> Series:
        """The rows ordered by their row labels (a null label last, or first with
        ``na_position="first"``). With ``level`` (a name, a position or a list)
        the rows are ordered by those levels first and, with ``sort_remaining``,
        by the others after them; ``ascending`` is one flag or one for each
        level in ``level``. ``key`` is called on each level and returns the
        values to order by; ``ignore_index`` drops the labels. The sort is
        stable, so ``kind`` is accepted and not used. A Series without labels
        has the positional index, which is already sorted: this Series, or
        reversed for ``ascending=False``."""
        if inplace:
            raise ValueError("sort_index: a Series is immutable; use the result")
        self._axis_is_rows(axis)
        s = self._s()
        names = names_of(s)
        if not names and key is None:
            _, flags, _ = sort_plan([default_name(0)], level, ascending, sort_remaining)
            if flags[0]:
                return s
        s = positional(s)
        names = names_of(s)
        picked, flags, keyed = sort_plan(names, level, ascending, sort_remaining)
        levels = levels_of(s)
        order = sorted_positions(
            [names[i] for i in picked], [levels[i] for i in picked], flags, na_position, key, keyed
        )
        out = s.gather(order)
        return drop_levels(out) if ignore_index else out

    def add_prefix(self, prefix: str, axis: int | str | None = None) -> Series:
        """The pandas ``add_prefix``: every row label as text with ``prefix`` in
        front (each level of a ``MultiIndex``); a Series with no labels gets the
        positions. A Series has only the row axis."""
        return self._text_labels(prefix, "", axis)

    def add_suffix(self, suffix: str, axis: int | str | None = None) -> Series:
        """As :meth:`add_prefix`, with ``suffix`` after each label."""
        return self._text_labels("", suffix, axis)

    def _text_labels(self, prefix: str, suffix: str, axis: int | str | None) -> Series:
        if axis is not None:
            self._axis_is_rows(axis)
        s = positional(self._s())
        levels = [label_text(level, prefix, suffix) for level in levels_of(s)]
        return with_levels(s, names_of(s), levels)

    # -- the pandas Index surface of what ``index`` returns --------------------------
    @property
    def names(self) -> list[str | None]:
        """The level names when this Series is the ``index`` of another object:
        the one name of a single level."""
        return [self._s().name]

    @property
    def nlevels(self) -> int:
        """1: a Series is the single level of the ``index`` it came from."""
        return 1

    def get_level_values(self, level: int | str | None) -> Series:
        """This Series, for the one level of an ``index``: ``0``, ``-1`` or its
        name. ``IndexError`` or ``KeyError`` for another level, as pandas."""
        s = self._s()
        pick_levels([s.name if s.name is not None else default_name(0)], level)
        return s

    def set_names(self, names: object, *, level: LevelArg = None, inplace: bool = False) -> Series:
        """This Series under a new name (a string, ``None`` or a list of one): the
        ``Index.set_names`` of the ``index`` of another object. ``level`` is
        ``0`` or its current name."""
        if inplace:
            raise ValueError("set_names: a Series is immutable; use the result")
        s = self._s()
        if level is not None:
            pick_levels([s.name if s.name is not None else default_name(0)], level)
        if isinstance(names, (list, tuple)):
            if len(names) != 1:
                raise ValueError(f"Length of new names must be 1, got {len(names)}")
            names = names[0]
        return s.rename(None if names is None else str(names))

    def memory_usage(self, index: bool = False, deep: bool = False) -> int:
        """The bytes this column's Arrow buffers hold (``nbytes``)."""
        return int(self._s().nbytes)

    # -- reductions and stats with a frame shape -------------------------------------
    def corr(self, other: Series) -> float:
        """Pearson correlation with ``other`` (positional; the group-by
        ``corr`` aggregate over one group). NaN below two complete pairs and
        when a column has no spread, as in pandas."""
        return self._pair(other, "corr")

    def cov(self, other: Series) -> float:
        return self._pair(other, "covar_samp")

    def _pair(self, other: Series, agg: str) -> float:
        from ._dataframe import DataFrame
        from ._expr import Agg, Col
        from ._groupby import GroupBy

        s = self._s()
        # pandas gives NaN below two complete pairs; the engine's readout 0.
        pairs = s.is_not_null().logical(0, other.is_not_null())
        if int(pairs.astype("int64").sum()) < 2:
            return float("nan")
        frame = DataFrame.from_dict({"x": s, "y": other})
        if agg == "corr":
            # pandas gives NaN when a column has no spread over the complete
            # pairs; the aggregate's readout is 0.
            if s.filter(pairs).var() == 0 or other.filter(pairs).var() == 0:
                return float("nan")
        out = GroupBy(frame, []).agg(Agg(agg, Col("y"), "r", by=Col("x")))
        value = out["r"][0]
        assert isinstance(value, float)
        return value

    def describe(self) -> DataFrame:
        """count / mean / std / min / 25% / 50% / 75% / max as a two-column
        frame (``statistic``, ``value``); the quantiles are the engine's
        sketch."""
        from ._dataframe import DataFrame

        s = self._s()
        rows = [
            ("count", s.count()),
            ("mean", s.mean()),
            ("std", s.stddev()),
            ("min", s.min()),
            ("25%", s.quantile(0.25)),
            ("50%", s.quantile(0.5)),
            ("75%", s.quantile(0.75)),
            ("max", s.max()),
        ]
        return DataFrame(
            {"statistic": [r[0] for r in rows], "value": [float(cast(float, r[1])) for r in rows]}
        )

    def agg(self, func: str | Sequence[str]) -> Scalar | DataFrame:
        """A reduction by name (``"sum"``), or a list of names as a
        two-column frame."""
        s = self._s()
        if isinstance(func, str):
            return cast("Scalar", getattr(s, func)())
        from ._dataframe import DataFrame

        names = list(func)
        return DataFrame({"statistic": names, "value": [getattr(s, n)() for n in names]})

    def transform(self, func: str | Callable[[Series], Series]) -> Series:
        s = self._s()
        return getattr(s, func)() if isinstance(func, str) else func(s)

    def radd(self, other: _Scalar) -> Series:
        return self._s() + other

    def rsub(self, other: _Scalar) -> Series:
        return other - self._s()

    def rmul(self, other: _Scalar) -> Series:
        return self._s() * other

    def rdiv(self, other: _Scalar) -> Series:
        """``other / self``: a Series operand divides directly, a scalar is
        broadcast to a Series first."""
        s = self._s()
        if isinstance(other, type(s)):
            return other / s
        return s.full_like(other) / s

    def __rtruediv__(self, other: _Scalar) -> Series:
        return self.rdiv(other)

    def case_when(self, caselist: Sequence[tuple[Series, Series | _Scalar]]) -> Series:
        """Replace where each ``(condition, replacement)`` holds, first match
        wins (pandas ``case_when``)."""
        out = self._s()
        for cond, value in reversed(list(caselist)):
            out = out.mask(cond, value)
        return out

    def groupby(self, by: Series) -> GroupBy:
        """Group this Series' values by ``by`` (positional): a frame
        ``GroupBy`` over columns ``key`` and ``value``."""
        from ._dataframe import DataFrame

        frame = DataFrame.from_dict({"key": by, "value": self._s()})
        return frame.group_by("key")

    def explode(self) -> Series:
        """A List Series flattened to one row per element."""
        from ._dataframe import DataFrame

        return DataFrame.from_dict({"v": self._s()}).explode("v")["v"]

    # -- accessors: iloc and iat are positional; loc and at look labels up ----------
    @property
    def iloc(self) -> _SeriesILoc:
        return _SeriesILoc(self._s())

    @property
    def loc(self) -> _SeriesLabelLoc:
        return _SeriesLabelLoc(self._s())

    at = loc

    def to_csv(self, path: str | None = None, **kwargs: object) -> str | None:
        to_csv = cast("Callable[..., str | None]", self._s().to_pandas().to_csv)
        return to_csv(path, index=False, **kwargs)

    def to_json(self, path: str | None = None, **kwargs: object) -> str | None:
        to_json = cast("Callable[..., str | None]", self._s().to_pandas().to_json)
        return to_json(path, **kwargs)

    def to_string(self, **kwargs: object) -> str:
        to_string = cast("Callable[..., str]", self._s().to_pandas().to_string)
        return to_string(index=False, **kwargs)


class _SeriesILoc:
    """``s.iloc[i]`` / ``s.iloc[a:b]`` / ``s.iloc[[i, j]]`` / ``s.iloc[mask]``:
    rows by position."""

    __slots__ = ("_s",)

    def __init__(self, s: Series) -> None:
        self._s = s

    def __getitem__(self, key: object) -> object:
        from ._loc import rows_by_position
        from ._series import Series

        s = self._s
        if isinstance(key, bool):
            raise TypeError("iloc: a bool is not a position")
        if isinstance(key, int):
            if not -len(s) <= key < len(s):
                raise IndexError(f"iloc: position {key} is out of bounds for {len(s)} elements")
            return s.item(key + len(s) if key < 0 else key)
        rows, _ = rows_by_position(len(s), key)
        if rows.kind == "all":
            return s
        if rows.kind == "slice":
            return s.slice(rows.offset, rows.length)
        if rows.kind == "take":
            return s.take(rows.positions)
        mask = rows.mask
        assert isinstance(mask, Series)
        return s.filter(mask)

    def __setitem__(self, key: object, value: object) -> None:
        """A Series is immutable; assign through a frame column
        (``df.loc[rows, "c"] = value``)."""
        raise TypeError("a Series is immutable; assign through df.loc[rows, column] = value")


class _SeriesLabelLoc(_SeriesILoc):
    """``s.loc[...]`` and ``s.at[...]``: a label, a list of labels, a label
    slice (both ends included) or a Bool mask, through the lookup the Series
    keeps for its labels; a Series without labels has the positions as labels."""

    __slots__ = ()

    def __getitem__(self, key: object) -> object:
        from ._loc import series_by_label

        return series_by_label(self._s, key)

    def __setitem__(self, key: object, value: object) -> None:
        from ._loc import set_series_label

        set_series_label(self._s, key, value)


__all__ = ["SeriesPandasMixin", "_SeriesILoc", "_SeriesLabelLoc"]
