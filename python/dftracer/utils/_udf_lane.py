# SPDX-License-Identifier: MIT
"""The Python side of a per-batch ``@dft.udf(batch=True)`` call.

The native Python lane thread calls :func:`call` once per batch, holding the
GIL. It turns the borrowed argument buffers into read-only numpy arrays, runs
the user's function and checks what it returned. The checks raise the errors a
caller sees, and the native side prefixes the UDF name.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Callable

if TYPE_CHECKING:
    import numpy.typing as npt


class _Column:
    """The owner behind one argument array.

    The array's ``base`` is this object, which holds a share of the engine's
    column through ``keep``. A view that the function keeps therefore stays
    valid and read-only after the call, never a dangling pointer. (A
    ``memoryview`` could not guard it: ``numpy.frombuffer`` takes no export of
    it, so releasing the memoryview leaves the array reading freed memory.)
    """

    __slots__ = ("keep", "__array_interface__")

    def __init__(self, keep: object, ptr: int, typestr: str, count: int) -> None:
        self.keep = keep
        iface: dict[str, Any] = {
            "version": 3,
            "shape": (count,),
            "typestr": typestr,
            "data": (ptr, True),
        }
        self.__array_interface__ = iface


def call(
    fn: Callable[..., object], rows: int, returns: str, specs: list[tuple[str, int, object]]
) -> object:
    """Run ``fn`` over one batch of ``rows`` rows and return the result as a
    contiguous buffer: ``returns`` elements, or bit-packed bits (least
    significant bit first) for ``"bool"``.

    ``specs`` holds one ``(dtype name, address, keep)`` per argument. A numeric
    column is a zero-copy read-only view of the engine's buffer; it keeps the
    buffer alive for as long as the array lives. A bool column arrives
    bit-packed and is unpacked to a new ``bool`` array.
    """
    import numpy as np

    args: list[npt.NDArray[Any]] = []
    for dtype, ptr, keep in specs:
        if rows == 0:
            args.append(np.empty(0, dtype=np.bool_ if dtype == "bool" else dtype))
        elif dtype == "bool":
            bits = np.asarray(_Column(keep, ptr, "|u1", (rows + 7) // 8))
            args.append(np.unpackbits(bits, count=rows, bitorder="little").astype(np.bool_))
        else:
            args.append(np.asarray(_Column(keep, ptr, np.dtype(dtype).str, rows)))
    result = np.asarray(fn(*args))
    if result.ndim != 1:
        raise ValueError(f"must return a 1-d array, got {result.ndim} dimensions")
    if result.shape[0] != rows:
        raise ValueError(f"returned {result.shape[0]} rows for a batch of {rows} rows")
    if returns == "bool":
        if result.dtype != np.bool_:
            raise TypeError(f"returned dtype {result.dtype}, but the declared result is bool")
        return np.packbits(result, bitorder="little")
    if not np.can_cast(result.dtype, returns, casting="safe"):
        raise TypeError(
            f"returned dtype {result.dtype}, which cannot be cast safely to the declared {returns}"
        )
    return np.ascontiguousarray(result, dtype=returns)
