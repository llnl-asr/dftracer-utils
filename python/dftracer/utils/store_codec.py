# SPDX-License-Identifier: MIT
"""Order-preserving byte encoding of Python values for store keys and values."""

from __future__ import annotations

import struct
import threading
from typing import (
    Any,
    Callable,
    Dict,
    Iterable,
    List,
    Mapping,
    Optional,
    Protocol,
    Sequence,
    Set,
    Tuple,
    Type,
    TypeVar,
    Union,
    cast,
    overload,
)

# Layout after the FoundationDB tuple layer, https://github.com/apple/foundationdb/blob/main/design/tuple.md, Apache-2.0.

BytesLike = Union[bytes, bytearray, memoryview]
Encodable = Union[
    None,
    bool,
    int,
    float,
    str,
    bytes,
    bytearray,
    memoryview,
    Sequence["Encodable"],
    # Any: the key of a Mapping is invariant, so no type accepts every key type.
    Mapping[Any, "Encodable"],
]


class StoreAware(Protocol):
    """A class that stores itself: an id, ``to_store`` and ``from_store``."""

    @property
    def store_type_id(self) -> int: ...

    def to_store(self) -> Encodable: ...

    @classmethod
    def from_store(cls, value: Encodable) -> "StoreAware": ...


T = TypeVar("T")
S = TypeVar("S", bound=StoreAware)

_NONE = 0x00
_BYTES = 0x01
_STR = 0x02
_TUPLE = 0x05
_LIST = 0x06
_DICT = 0x07
_NEG_LONG = 0x0B
_INT_ZERO = 0x14
_POS_LONG = 0x1D
_FLOAT = 0x21
_FALSE = 0x26
_TRUE = 0x27
_REGISTERED = 0x40

_MAX_LONG = 255
MAX_DEPTH = 64

_DOUBLE = struct.Struct(">d")
_BITS = struct.Struct(">Q")

_ToValue = Callable[[object], Encodable]
_FromValue = Callable[[Encodable], object]

_by_class: Dict[type, Tuple[int, _ToValue]] = {}
_by_id: Dict[int, Tuple[type, _FromValue]] = {}
_unaware: Set[type] = set()
_lock = threading.Lock()
_BUILTIN_TYPES = frozenset(
    (type(None), bool, int, float, str, bytes, bytearray, memoryview, tuple, list, dict)
)
_PROTOCOL = ("store_type_id", "to_store", "from_store")


def _register_unlocked(
    cls: type,
    type_id: int,
    to_value: _ToValue,
    from_value: _FromValue,
) -> None:
    if not isinstance(type_id, int) or isinstance(type_id, bool) or type_id < 0:
        raise ValueError("type_id must be a non-negative int, got %r" % (type_id,))
    known = _by_class.get(cls)
    if known is not None:
        same = known[0] == type_id and known[1] == to_value
        entry = _by_id[known[0]]
        if same and entry[1] == from_value:
            return
        raise ValueError("%s is already registered" % cls.__name__)
    if type_id in _by_id:
        raise ValueError(
            "type_id %d is already registered for %s" % (type_id, _by_id[type_id][0].__name__)
        )
    _by_id[type_id] = (cls, from_value)
    _by_class[cls] = (type_id, to_value)
    _unaware.discard(cls)


def _register(
    cls: type,
    type_id: int,
    to_value: _ToValue,
    from_value: _FromValue,
) -> None:
    with _lock:
        _register_unlocked(cls, type_id, to_value, from_value)


def _detect(cls: type) -> Optional[Tuple[int, _ToValue]]:
    with _lock:
        known = _by_class.get(cls)
        if known is not None:
            return known
        found = _protocol(cls)
        if found is None:
            _unaware.add(cls)
            return None
        _register_unlocked(cls, *found)
        return _by_class[cls]


def _protocol(cls: type) -> Optional[Tuple[int, _ToValue, _FromValue]]:
    # Method detection after cereal, https://uscilab.github.io/cereal/serialization_functions.html, BSD-style 3-clause.
    present = [name for name in _PROTOCOL if hasattr(cls, name)]
    if not present:
        return None
    missing = [name for name in _PROTOCOL if name not in present]
    if missing:
        raise TypeError(
            "%s defines %s but not %s; a store-aware class needs all of %s"
            % (cls.__name__, ", ".join(present), ", ".join(missing), ", ".join(_PROTOCOL))
        )
    if "store_type_id" not in cls.__dict__:
        raise TypeError(
            "%s inherits store_type_id; give the subclass its own store_type_id" % cls.__name__
        )
    return getattr(cls, "store_type_id"), getattr(cls, "to_store"), getattr(cls, "from_store")


@overload
def register_type(cls: Type[S]) -> Type[S]: ...


@overload
def register_type(
    cls: Type[T],
    type_id: int,
    to_value: Callable[[T], Encodable],
    from_value: Callable[[Encodable], T],
) -> Type[T]: ...


def register_type(
    cls: Type[T],
    type_id: Optional[int] = None,
    to_value: Optional[Callable[[T], Encodable]] = None,
    from_value: Optional[Callable[[Encodable], T]] = None,
) -> Type[T]:
    """Make ``encode`` and ``decode`` handle ``cls``, and return ``cls``.

    With one argument, ``cls`` defines ``store_type_id``, ``to_store`` and
    ``from_store``, and this also works as a class decorator. With four, the
    functions turn an object into an encodable value and back, for a class
    you cannot edit. The id is stored with the data, so it must never change.
    A reader must register each class before it decodes data that holds it.
    """
    if type_id is None and to_value is None and from_value is None:
        found = _protocol(cls)
        if found is None:
            raise TypeError("%s defines none of %s" % (cls.__name__, ", ".join(_PROTOCOL)))
        _register(cls, *found)
        return cls
    if type_id is None or to_value is None or from_value is None:
        raise TypeError("register_type takes either a class or a class, id and two functions")
    _register(cls, type_id, cast("_ToValue", to_value), cast("_FromValue", from_value))
    return cls


def _escape(data: bytes) -> bytes:
    return data.replace(b"\x00", b"\x00\xff")


def _encode_int(value: int, out: bytearray) -> None:
    if value == 0:
        out.append(_INT_ZERO)
        return
    magnitude = -value if value < 0 else value
    size = (magnitude.bit_length() + 7) // 8
    if size > _MAX_LONG:
        raise ValueError("int needs %d bytes, the limit is %d" % (size, _MAX_LONG))
    if value > 0:
        if size <= 8:
            out.append(_INT_ZERO + size)
        else:
            out.append(_POS_LONG)
            out.append(size)
        out += magnitude.to_bytes(size, "big")
    else:
        if size <= 8:
            out.append(_INT_ZERO - size)
        else:
            out.append(_NEG_LONG)
            out.append(size ^ 0xFF)
        out += (((1 << (8 * size)) - 1) - magnitude).to_bytes(size, "big")


def _encode_float(value: float, out: bytearray) -> None:
    (bits,) = _BITS.unpack(_DOUBLE.pack(value))
    if bits >> 63:
        bits ^= 0xFFFFFFFFFFFFFFFF
    else:
        bits ^= 1 << 63
    out.append(_FLOAT)
    out += _BITS.pack(bits)


def _check_depth(depth: int) -> None:
    if depth >= MAX_DEPTH:
        raise ValueError("nesting is deeper than %d levels" % MAX_DEPTH)


def _encode_items(tag: int, items: Iterable[object], out: bytearray, depth: int) -> None:
    _check_depth(depth)
    out.append(tag)
    for item in items:
        _encode_into(item, out, True, depth + 1)
    out.append(0x00)


def _encode_into(obj: object, out: bytearray, nested: bool, depth: int) -> None:
    if obj is None:
        out += b"\x00\xff" if nested else b"\x00"
        return
    cls = type(obj)
    registered = _by_class.get(cls)
    if registered is None:
        # Exact-type dispatch: a subclass must be registered itself.
        if cls is bool:
            out.append(_TRUE if obj else _FALSE)
            return
        if cls is int:
            _encode_int(cast(int, obj), out)
            return
        if cls is float:
            _encode_float(cast(float, obj), out)
            return
        if cls is str:
            out.append(_STR)
            out += _escape(cast(str, obj).encode("utf-8"))
            out.append(0x00)
            return
        if cls is bytes or cls is bytearray or cls is memoryview:
            out.append(_BYTES)
            out += _escape(bytes(cast(BytesLike, obj)))
            out.append(0x00)
            return
        if cls is tuple:
            _encode_items(_TUPLE, cast(Tuple[object, ...], obj), out, depth)
            return
        if cls is list:
            _encode_items(_LIST, cast(List[object], obj), out, depth)
            return
        if cls is dict:
            _check_depth(depth)
            entries: List[Tuple[bytes, bytes]] = []
            for key, value in cast(Dict[object, object], obj).items():
                key_bytes = bytearray()
                _encode_into(key, key_bytes, True, depth + 1)
                value_bytes = bytearray()
                _encode_into(value, value_bytes, True, depth + 1)
                entries.append((bytes(key_bytes), bytes(value_bytes)))
            entries.sort(key=lambda entry: entry[0])
            out.append(_DICT)
            for packed_key, packed_value in entries:
                out += packed_key
                out += packed_value
            out.append(0x00)
            return
        if cls not in _unaware:
            registered = _detect(cls)
    if registered is None:
        raise TypeError(
            "cannot encode %s; register that exact class with "
            "dftracer.utils.store.register_type" % cls.__name__
        )
    _check_depth(depth)
    type_id, to_value = registered
    out.append(_REGISTERED)
    _encode_int(type_id, out)
    _encode_into(to_value(obj), out, nested, depth + 1)


def encode(obj: object) -> bytes:
    """Encode ``obj`` so that values of one type sort like their encodings."""
    # object, not Encodable: a registered or store-aware class cannot join a union.
    out = bytearray()
    _encode_into(obj, out, False, 0)
    return bytes(out)


def encode_prefix(obj: Union[str, BytesLike, Tuple[Encodable, ...]]) -> bytes:
    """The encoding of a ``str``, ``bytes`` or ``tuple`` without its terminator.

    It is a prefix of the encoding of each longer value that starts the same.
    For a ``tuple`` it also matches some keys that it should not: the
    escape of a NUL byte or of a ``None`` looks like more data after the
    prefix. A raw user of a tuple prefix filters the keys with
    :func:`matches_prefix`.
    """
    if isinstance(obj, (str, bytes, bytearray, memoryview, tuple)):
        return encode(obj)[:-1]
    raise TypeError("encode_prefix takes a str, bytes or tuple, not %s" % type(obj).__name__)


def matches_prefix(prefix: bytes, key: bytes) -> bool:
    """Whether the encoded ``key`` starts with the encoded tuple ``prefix``.

    ``prefix`` comes from :func:`encode_prefix` of a ``tuple``. No tag byte is
    0xFF, so a 0xFF right after the prefix is the escape of a longer element.
    """
    if not key.startswith(prefix):
        return False
    return len(key) == len(prefix) or key[len(prefix)] != 0xFF


def _truncated() -> ValueError:
    return ValueError("truncated data")


def _decode_text(data: bytes, pos: int) -> Tuple[bytes, int]:
    out = bytearray()
    size = len(data)
    while True:
        end = data.find(b"\x00", pos)
        if end < 0:
            raise _truncated()
        out += data[pos:end]
        if end + 1 < size and data[end + 1] == 0xFF:
            out.append(0)
            pos = end + 2
        else:
            return bytes(out), end + 1


def _decode_items(data: bytes, pos: int, depth: int) -> Tuple[List[object], int]:
    items: List[object] = []
    size = len(data)
    while True:
        if pos >= size:
            raise _truncated()
        if data[pos] == 0x00:
            if pos + 1 < size and data[pos + 1] == 0xFF:
                items.append(None)
                pos += 2
                continue
            return items, pos + 1
        item, pos = _decode_at(data, pos, True, depth + 1)
        items.append(item)


def _decode_at(data: bytes, pos: int, nested: bool, depth: int) -> Tuple[object, int]:
    size = len(data)
    if pos >= size:
        raise _truncated()
    tag = data[pos]
    pos += 1
    if tag == _NONE:
        if nested:
            if pos >= size or data[pos] != 0xFF:
                raise ValueError("unexpected terminator")
            return None, pos + 1
        return None, pos
    if tag == _BYTES:
        return _decode_text(data, pos)
    if tag == _STR:
        raw, pos = _decode_text(data, pos)
        try:
            return raw.decode("utf-8"), pos
        except UnicodeDecodeError as exc:
            raise ValueError("invalid UTF-8 in str: %s" % exc) from None
    if tag == _TUPLE or tag == _LIST or tag == _DICT or tag == _REGISTERED:
        _check_depth(depth)
    if tag == _TUPLE:
        items, pos = _decode_items(data, pos, depth)
        return tuple(items), pos
    if tag == _LIST:
        return _decode_items(data, pos, depth)
    if tag == _DICT:
        items, pos = _decode_items(data, pos, depth)
        if len(items) % 2:
            raise ValueError("dict has a key without a value")
        try:
            return dict(zip(items[0::2], items[1::2])), pos
        except TypeError as exc:
            raise ValueError("unhashable dict key: %s" % exc) from None
    if tag == _INT_ZERO:
        return 0, pos
    if _INT_ZERO < tag <= _INT_ZERO + 8 or _INT_ZERO - 8 <= tag < _INT_ZERO:
        count = abs(tag - _INT_ZERO)
        negative = tag < _INT_ZERO
    elif tag == _POS_LONG or tag == _NEG_LONG:
        if pos >= size:
            raise _truncated()
        count = data[pos]
        if tag == _NEG_LONG:
            count ^= 0xFF
        pos += 1
        negative = tag == _NEG_LONG
    elif tag == _FLOAT:
        if pos + 8 > size:
            raise _truncated()
        (bits,) = _BITS.unpack_from(data, pos)
        if bits >> 63:
            bits ^= 1 << 63
        else:
            bits ^= 0xFFFFFFFFFFFFFFFF
        return _DOUBLE.unpack(_BITS.pack(bits))[0], pos + 8
    elif tag == _FALSE:
        return False, pos
    elif tag == _TRUE:
        return True, pos
    elif tag == _REGISTERED:
        type_id, pos = _decode_at(data, pos, nested, depth + 1)
        if type(type_id) is not int:
            raise ValueError("registered type id is not an int")
        entry = _by_id.get(type_id)
        if entry is None:
            raise ValueError(
                "type id %d is not registered; import the class and apply register_type" % type_id
            )
        value, pos = _decode_at(data, pos, nested, depth + 1)
        return entry[1](cast(Encodable, value)), pos
    else:
        raise ValueError("unknown tag 0x%02x" % tag)
    if pos + count > size:
        raise _truncated()
    number = int.from_bytes(data[pos : pos + count], "big")
    if negative:
        number = number - ((1 << (8 * count)) - 1)
    if count <= 8 and tag in (_POS_LONG, _NEG_LONG):
        raise ValueError("int is not in its shortest form")
    if (abs(number).bit_length() + 7) // 8 != count:
        raise ValueError("int is not in its shortest form")
    return number, pos + count


def decode(data: BytesLike) -> object:
    """Decode bytes that :func:`encode` made.

    The result is ``object`` because a registered class can be in it, so the
    caller narrows it.
    """
    raw = bytes(data)
    value, pos = _decode_at(raw, 0, False, 0)
    if pos != len(raw):
        raise ValueError("%d trailing bytes" % (len(raw) - pos))
    return value


__all__ = ["MAX_DEPTH", "decode", "encode", "encode_prefix", "matches_prefix", "register_type"]
