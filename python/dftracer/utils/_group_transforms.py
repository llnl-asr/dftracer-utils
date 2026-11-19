# SPDX-License-Identifier: MIT
"""The per-group row transforms that GroupBy and LazyGroupBy share."""

from __future__ import annotations

from typing import Generic, Sequence, TypeVar

R = TypeVar("R")


class GroupTransforms(Generic[R]):
    """Each method is one ``_transform`` kind; ``R`` is the result type
    (``DataFrame`` for :class:`GroupBy`, ``LazyFrame`` for ``LazyGroupBy``)."""

    def _transform(
        self, kind: str, n: int = 0, method: str = "average", ascending: bool = True
    ) -> R:
        raise NotImplementedError

    def _pandas_face(self) -> bool:
        """Whether this is the pandas ``groupby`` (a negative ``head`` or
        ``tail`` ``n`` is allowed) and not the Polars ``group_by``."""
        return False

    def _check_pick(self, n: int) -> None:
        if n < 0 and not self._pandas_face():
            raise ValueError(f"a negative n ({n}) is not taken by the Polars group_by head or tail")

    def take(self, indices: Sequence[int]) -> R:
        raise NotImplementedError

    def _take_rows(self, indices: Sequence[int]) -> R:
        """The rows at ``indices`` of each group, in input order."""
        raise NotImplementedError

    def reduce(self, agg: str) -> R:
        raise NotImplementedError

    def cumsum(self) -> R:
        return self._transform("cumsum")

    def cummax(self) -> R:
        return self._transform("cummax")

    def cummin(self) -> R:
        return self._transform("cummin")

    def cumprod(self) -> R:
        """Each numeric column's running product within its group (Float64)."""
        return self._transform("cumprod")

    def cumcount(self) -> R:
        """The 0-based position of each row within its group, as ``cumcount``."""
        return self._transform("cumcount")

    def shift(self, periods: int = 1) -> R:
        """Each column shifted by ``periods`` rows within its group (a negative
        ``periods`` looks ahead); the vacated rows are null."""
        return self._transform("shift", periods)

    def diff(self) -> R:
        return self._transform("diff")

    def pct_change(self) -> R:
        return self._transform("pct_change")

    def rank(self, method: str = "average", ascending: bool = True) -> R:
        """Each numeric column's rank within its group by value: ``method`` is
        ``average`` (the pandas default), ``min``, ``max``, ``dense`` or ``first``; a
        null value has a null rank."""
        return self._transform("rank", 0, method, ascending)

    def ngroup(self) -> R:
        """The 0-based number of each row's group, groups numbered in sorted
        key order (pandas ``ngroup``)."""
        return self._transform("ngroup")

    def head(self, n: int = 5) -> R:
        """The first ``n`` rows of each group, in input order (pandas
        ``head``). A negative ``n`` (pandas ``groupby`` only) keeps all but the
        last ``-n`` rows of each group."""
        self._check_pick(n)
        return self._transform("head", n)

    def tail(self, n: int = 5) -> R:
        """The last ``n`` rows of each group, in input order. A negative ``n``
        (pandas ``groupby`` only) keeps all but the first ``-n`` rows."""
        self._check_pick(n)
        return self._transform("tail", n)

    def nth(self, n: int | Sequence[int]) -> R:
        """The ``n``-th row of each group (0-based; negative counts from the
        end), or of each position in a list, in input order."""
        if isinstance(n, int):
            return self._transform("nth", n)
        return self._take_rows(n)

    def prod(self) -> R:
        return self.reduce("prod")
