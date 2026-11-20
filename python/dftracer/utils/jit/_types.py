# SPDX-License-Identifier: MIT
"""Phantom column types, the interned-string marker and the C type table."""

from __future__ import annotations

import builtins
from typing import Generic, NamedTuple, TypeVar

from .._enums import DType
from .._jit_errors import JitError

T = TypeVar("T")
T_co = TypeVar("T_co", covariant=True)
K = TypeVar("K")
N = TypeVar("N", int, float)
K1 = TypeVar("K1")
K2 = TypeVar("K2")
K3 = TypeVar("K3")
K4 = TypeVar("K4")
C = TypeVar("C")
SK = TypeVar("SK")
SV = TypeVar("SV")
MISSING = object()


class Str(str):
    """An interned-string id: usable as a STR key / SET_STR|LIST_STR element and
    comparable to a ``str`` literal. Authoring-only, never instantiated; it is a
    ``str`` to the type checker so a plain ``Set[str]`` or ``Dict[Tuple[int, str], int]``
    state field takes ``e.name``."""

    __slots__ = ()


class TypeTag(Generic[T_co]):
    """Phantom type marker: the parameter tracks the Python type a jit column
    maps to (u64 -> int, str_ -> str) and is never consumed, so it is
    covariant - TypeTag[int] is a TypeTag[object]."""

    __slots__ = ("dft",)

    def __init__(self, dft: str) -> None:
        self.dft = dft


i64: TypeTag[int] = TypeTag("DFTU_T_I64")
str_: TypeTag[Str] = TypeTag("DFTU_T_STR")
bytes: TypeTag[builtins.bytes] = TypeTag("DFTU_T_BYTES")

i8: TypeTag[int] = TypeTag("DFTU_T_I8")
i16: TypeTag[int] = TypeTag("DFTU_T_I16")
i32: TypeTag[int] = TypeTag("DFTU_T_I32")
u8: TypeTag[int] = TypeTag("DFTU_T_U8")
u16: TypeTag[int] = TypeTag("DFTU_T_U16")
u32: TypeTag[int] = TypeTag("DFTU_T_U32")
u64: TypeTag[int] = TypeTag("DFTU_T_U64")
f32: TypeTag[float] = TypeTag("DFTU_T_F32")
f64: TypeTag[float] = TypeTag("DFTU_T_F64")


class _Sentinel:
    __slots__ = ()

    # Custom equality keeps ``e.fhash != jit.NONE`` valid for a ``str``-typed Str
    # under mypy --strict (strict equality skips an operand that defines __eq__).
    def __eq__(self, other: object) -> bool:
        return self is other

    def __hash__(self) -> int:
        return id(self)


NONE = _Sentinel()


class CType(NamedTuple):
    """One scalar type of a jit body in every form the emitter needs."""

    dft: str
    """The ``DFTU_T_*`` op value type name."""
    ctype: str
    """The C scalar type."""
    size: int
    """The size in bytes of the C scalar."""
    pack: str
    """The :mod:`struct` format of the C scalar."""
    column: str
    """The ``DFTU_TYPE_*`` dtype of a batch column that holds the value."""


C_TYPES: dict[DType, CType] = {
    DType.INT64: CType("DFTU_T_I64", "int64_t", 8, "q", "DFTU_TYPE_INT64"),
    DType.INT32: CType("DFTU_T_I32", "int32_t", 4, "i", "DFTU_TYPE_INT32"),
    DType.INT16: CType("DFTU_T_I16", "int16_t", 2, "h", "DFTU_TYPE_INT16"),
    DType.INT8: CType("DFTU_T_I8", "int8_t", 1, "b", "DFTU_TYPE_INT8"),
    DType.UINT64: CType("DFTU_T_U64", "uint64_t", 8, "Q", "DFTU_TYPE_UINT64"),
    DType.UINT32: CType("DFTU_T_U32", "uint32_t", 4, "I", "DFTU_TYPE_UINT32"),
    DType.UINT16: CType("DFTU_T_U16", "uint16_t", 2, "H", "DFTU_TYPE_UINT16"),
    DType.UINT8: CType("DFTU_T_U8", "uint8_t", 1, "B", "DFTU_TYPE_UINT8"),
    DType.FLOAT64: CType("DFTU_T_F64", "double", 8, "d", "DFTU_TYPE_FLOAT64"),
    DType.FLOAT32: CType("DFTU_T_F32", "float", 4, "f", "DFTU_TYPE_FLOAT32"),
    # An interned-string id: equality against a literal only, never arithmetic or
    # ordering. DFTU_T_BYTES has no entry: it crosses as an opaque variable-length
    # record, not a fixed-size comparable value.
    DType.STRING: CType("DFTU_T_STR", "dftu_str", 4, "I", "DFTU_TYPE_STRING"),
}

DFT_DTYPES: dict[str, DType] = {c.dft: d for d, c in C_TYPES.items()}

# The value family each accumulator element is stored as.
KIND_DTYPES: dict[str, DType] = {
    "i64": DType.INT64,
    "f64": DType.FLOAT64,
    "str": DType.STRING,
}


def c_type(dft: str) -> CType:
    """The :class:`CType` of a ``DFTU_T_*`` name."""
    return C_TYPES[DFT_DTYPES[dft]]


def kind_type(kind: str) -> CType:
    """The :class:`CType` of a value family (``"i64"``, ``"f64"`` or ``"str"``)."""
    return C_TYPES[KIND_DTYPES[kind]]


def is_float_dft(dft: str) -> bool:
    """Whether a ``DFTU_T_*`` name is a floating-point type."""
    dtype = DFT_DTYPES.get(dft)
    return dtype is not None and dtype.is_float


def key_type(dft: str) -> CType:
    """The buffer type a map key of ``dft`` is stored as: a string id, a double
    for a float and an int64 for every integer."""
    dtype = DFT_DTYPES.get(dft)
    if dtype is DType.STRING:
        return C_TYPES[DType.STRING]
    if dtype is not None and dtype.is_float:
        return C_TYPES[DType.FLOAT64]
    return C_TYPES[DType.INT64]


I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1
U64_MAX = (1 << 64) - 1


def int_literal(v: int) -> str:
    """C text for an integer literal, rejecting one that would not fit in the
    64-bit key/value slot (Python ints are unbounded; the compiled body is not,
    so an out-of-range literal would silently wrap)."""
    if v < I64_MIN or v > U64_MAX:
        raise JitError(
            f"integer literal {v} does not fit in 64 bits; jit keys and values "
            "are 64-bit and it would silently wrap"
        )
    return f"{v}ULL" if v > I64_MAX else str(v)
