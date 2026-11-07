# SPDX-License-Identifier: MIT
"""Key-value store: namespaces, batched writes and point-in-time reads."""

from __future__ import annotations

import enum
import os
from dataclasses import dataclass, field
from typing import (
    TYPE_CHECKING,
    Callable,
    ClassVar,
    Dict,
    Generic,
    Iterator,
    List,
    Literal,
    NamedTuple,
    Optional,
    Sequence,
    Tuple,
    Type,
    TypeVar,
    Union,
    cast,
    overload,
)

from .dftracer_utils_ext import Store as _NativeStore
from .dftracer_utils_ext import StoreBatch as _NativeBatch
from .dftracer_utils_ext import StoreNamespace as _NativeNamespace
from .dftracer_utils_ext import StoreSnapshot as _NativeSnapshot
from .store_codec import (
    BytesLike,
    Encodable,
    StoreAware,
    decode,
    encode,
    encode_prefix,
    matches_prefix,
    register_type,
)

if TYPE_CHECKING:
    from types import TracebackType

K = TypeVar("K")
V = TypeVar("V")
T = TypeVar("T")
_B = TypeVar("_B", bound="_BatchBase")
Path = Union[str, "os.PathLike[str]"]
Scope = Union[None, Path, Sequence[Path]]

_FOLDS: Dict[str, int] = {
    "sum_i64": 1,
    "sum_f64": 2,
    "min_i64": 3,
    "max_i64": 4,
    "min_f64": 5,
    "max_f64": 6,
    "count": 7,
    "last": 8,
    "concat": 9,
}


class ScopeState(enum.IntEnum):
    """The state of the current copy of a scope."""

    ABSENT = 0
    READY = 1
    FAILED = 2
    BAD = 3


class SourceStat(NamedTuple):
    """The signature of one trace file of a scope."""

    mtime: int
    size: int
    hash: int


@dataclass
class CopyInfo:
    """The current copy of one scope."""

    state: ScopeState
    generation: int
    params_hash: int
    sources: List[SourceStat] = field(default_factory=list)


@dataclass
class PackReport:
    """What :meth:`Store.pack` did."""

    files_packed: int
    copies_written: int
    copies_skipped: int
    files_deleted: int
    bytes_deleted: int


class _NamespaceBase:
    """The properties that every namespace forwards from the native one."""

    def __init__(self, native: _NativeNamespace) -> None:
        self.native = native

    @property
    def name(self) -> str:
        return self.native.name

    @property
    def format_version(self) -> int:
        return self.native.format_version

    @property
    def fold(self) -> int:
        return self.native.fold

    @property
    def durable(self) -> bool:
        return self.native.durable

    @property
    def shared(self) -> bool:
        return self.native.shared


class RawNamespace(_NamespaceBase):
    """A namespace of bytes-like keys and values that reads return as ``bytes``.

    Use it for data that C++ or the C ABI reads or writes.
    """

    raw: ClassVar[bool] = True


class Namespace(_NamespaceBase, Generic[K, V]):
    """A namespace of keys of type ``K`` and values of type ``V``.

    Keys and values go through :func:`encode`, and reads return decoded objects.
    With ``key_type`` and ``value_type``, each key and value that goes in or
    comes out must be an instance of them, or the call raises ``TypeError``.
    """

    raw: ClassVar[bool] = False

    def __init__(
        self,
        native: _NativeNamespace,
        key_type: Optional[Type[K]] = None,
        value_type: Optional[Type[V]] = None,
    ) -> None:
        super().__init__(native)
        self._key_type = key_type
        self._value_type = value_type

    def _encode_key(self, key: K) -> bytes:
        _check(key, self._key_type, "key")
        return encode(key)

    def _encode_value(self, value: V) -> bytes:
        _check(value, self._value_type, "value")
        return encode(value)

    def _decode_key(self, data: bytes) -> K:
        return _decode_as(data, self._key_type, "key")

    def _decode_value(self, data: bytes) -> V:
        return _decode_as(data, self._value_type, "value")


_RAW = (bytes, bytearray, memoryview)


def _raw_arg(obj: BytesLike) -> BytesLike:
    if not isinstance(obj, _RAW):
        raise TypeError(
            "a raw namespace takes bytes, bytearray or memoryview, not %s" % type(obj).__name__
        )
    return obj


def _check(obj: object, expected: Optional[Type[T]], what: str) -> None:
    if expected is not None and not isinstance(obj, expected):
        raise TypeError("the %s must be %s, not %s" % (what, expected.__name__, type(obj).__name__))


def _decode_as(data: bytes, expected: Optional[Type[T]], what: str) -> T:
    try:
        value = decode(data)
    except ValueError as exc:
        raise ValueError(
            "cannot decode the stored bytes (%s); if the namespace holds raw bytes, "
            "open it with raw=True" % exc
        ) from None
    _check(value, expected, "stored " + what)
    return cast("T", value)


class _BatchBase:
    """The commit and discard of one batch.

    As a context manager it commits on a clean exit and discards the writes
    when the block raises.
    """

    def __init__(self, native: _NativeBatch) -> None:
        self._native = native
        self._done = False

    def commit(self) -> None:
        """Write the batch. It cannot be used afterwards."""
        self._done = True
        self._native.commit()

    def discard(self) -> None:
        """Drop the batch without writing it."""
        self._done = True
        self._native.discard()

    def __enter__(self: _B) -> _B:
        return self

    def __exit__(
        self,
        exc_type: Optional[type],
        exc_val: Optional[BaseException],
        exc_tb: Optional["TracebackType"],
    ) -> None:
        if self._done:
            return
        if exc_type is None:
            self.commit()
        else:
            self.discard()


class Batch(_BatchBase, Generic[K, V]):
    """The writes of one commit, atomic: visible whole or not at all."""

    def __init__(
        self,
        native: _NativeBatch,
        encode_key: Callable[[K], BytesLike],
        encode_value: Callable[[V], BytesLike],
    ) -> None:
        super().__init__(native)
        self._encode_key = encode_key
        self._encode_value = encode_value

    def put(self, key: K, value: V) -> None:
        """Add a key and value. The bytes are copied."""
        self._native.put(self._encode_key(key), self._encode_value(value))

    def erase(self, key: K) -> None:
        """Hide ``key`` in the copy of this batch's scope."""
        self._native.erase(self._encode_key(key))

    def erase_range(self, begin: K, end: Optional[K] = None) -> None:
        """Hide every key in ``[begin, end)``; ``None`` means to the last key."""
        self._native.erase_range(
            self._encode_key(begin), None if end is None else self._encode_key(end)
        )


class RawBatch(Batch[BytesLike, BytesLike]):
    """A batch for a raw namespace. Only this batch has :meth:`fold`."""

    def __init__(self, native: _NativeBatch) -> None:
        super().__init__(native, _raw_arg, _raw_arg)

    def fold(self, key: BytesLike, value: BytesLike) -> None:
        """Add a fold input to ``key`` (namespaces with a fold only)."""
        self._native.fold(_raw_arg(key), _raw_arg(value))


class Snapshot:
    """A point-in-time view of every data file of a store."""

    def __init__(self, native: _NativeSnapshot) -> None:
        self._native = native

    @overload
    def get(self, ns: RawNamespace, scope: Scope, key: BytesLike) -> Optional[bytes]: ...

    @overload
    def get(self, ns: Namespace[K, V], scope: Scope, key: K) -> Optional[V]: ...

    def get(
        self, ns: Union[RawNamespace, Namespace[K, V]], scope: Scope, key: Union[BytesLike, K]
    ) -> Union[None, bytes, V]:
        """The value of ``key``, or ``None`` when it is missing.

        A raw namespace returns bytes. Any other returns the decoded object.
        """
        if isinstance(ns, RawNamespace):
            return self._native.get(ns.native, scope, _raw_arg(cast(BytesLike, key)))
        data = self._native.get(ns.native, scope, ns._encode_key(cast(K, key)))
        return None if data is None else ns._decode_value(data)

    @overload
    def multi_get(
        self, ns: RawNamespace, scope: Scope, keys: Sequence[BytesLike]
    ) -> List[Optional[bytes]]: ...

    @overload
    def multi_get(
        self, ns: Namespace[K, V], scope: Scope, keys: Sequence[K]
    ) -> List[Optional[V]]: ...

    def multi_get(
        self,
        ns: Union[RawNamespace, Namespace[K, V]],
        scope: Scope,
        keys: Union[Sequence[BytesLike], Sequence[K]],
    ) -> Union[List[Optional[bytes]], List[Optional[V]]]:
        """One value or ``None`` per key, in the order of ``keys``."""
        if isinstance(ns, RawNamespace):
            raw_keys = [_raw_arg(cast(BytesLike, key)) for key in keys]
            return self._native.multi_get(ns.native, scope, raw_keys)
        found = self._native.multi_get(
            ns.native, scope, [ns._encode_key(cast(K, key)) for key in keys]
        )
        return [None if data is None else ns._decode_value(data) for data in found]

    @overload
    def scan(
        self, ns: RawNamespace, scope: Scope, prefix: Optional[BytesLike] = None
    ) -> Iterator[Tuple[bytes, bytes]]: ...

    @overload
    def scan(
        self,
        ns: Namespace[K, V],
        scope: Scope,
        prefix: Union[None, str, bytes, Tuple[Encodable, ...]] = None,
    ) -> Iterator[Tuple[K, V]]: ...

    def scan(
        self,
        ns: Union[RawNamespace, Namespace[K, V]],
        scope: Scope,
        prefix: Union[None, str, BytesLike, Tuple[Encodable, ...]] = None,
    ) -> Union[Iterator[Tuple[bytes, bytes]], Iterator[Tuple[K, V]]]:
        """Iterate ``(key, value)`` in key order over the keys with ``prefix``.

        ``None`` means every key. In a raw namespace the prefix is bytes-like.
        Otherwise it goes through :func:`encode_prefix`.
        """
        if isinstance(ns, RawNamespace):
            raw_prefix = b"" if prefix is None else _raw_arg(cast(BytesLike, prefix))
            return self._native.scan(ns.native, scope, raw_prefix)
        encoded = b"" if prefix is None else encode_prefix(prefix)
        rows: Iterator[Tuple[bytes, bytes]] = self._native.scan(ns.native, scope, encoded)
        if isinstance(prefix, tuple):
            rows = (row for row in rows if matches_prefix(encoded, row[0]))
        return ((ns._decode_key(key), ns._decode_value(value)) for key, value in rows)

    def copy_info(self, ns: Union[RawNamespace, Namespace[K, V]], scope: Scope) -> CopyInfo:
        """The current copy of ``scope``."""
        state, generation, params_hash, sources = self._native.copy_info(ns.native, scope)
        return CopyInfo(
            ScopeState(state),
            generation,
            params_hash,
            [SourceStat(*source) for source in sources],
        )


class Store:
    """A key-value store in one directory.

    A scope is ``None`` for the root, a path for one file, or a sequence of
    paths for a set of files. A handle that a forked child inherited raises
    :class:`DFTUtilsError` from every call; the child calls :meth:`open` for
    its own.
    """

    def __init__(self, native: _NativeStore) -> None:
        self._native = native

    @classmethod
    def open(cls, path: Path) -> "Store":
        """Open the store in ``path``, creating the directory when needed."""
        return cls(_NativeStore(path))

    @overload
    def namespace(
        self,
        name: str,
        format_version: int = ...,
        fold: Optional[str] = ...,
        durable: bool = ...,
        shared: bool = ...,
        *,
        raw: Literal[True],
    ) -> RawNamespace: ...

    @overload
    def namespace(
        self,
        name: str,
        format_version: int = ...,
        *,
        fold: str,
        durable: bool = ...,
        shared: bool = ...,
        raw: bool = ...,
    ) -> RawNamespace: ...

    @overload
    def namespace(
        self,
        name: str,
        format_version: int = ...,
        fold: None = ...,
        durable: bool = ...,
        shared: bool = ...,
        raw: Literal[False] = ...,
        *,
        key_type: Type[K],
        value_type: Type[V],
    ) -> Namespace[K, V]: ...

    @overload
    def namespace(
        self,
        name: str,
        format_version: int = ...,
        fold: None = ...,
        durable: bool = ...,
        shared: bool = ...,
        raw: Literal[False] = ...,
    ) -> Namespace[K, V]: ...

    def namespace(
        self,
        name: str,
        format_version: int = 0,
        fold: Optional[str] = None,
        durable: bool = False,
        shared: bool = False,
        raw: bool = False,
        *,
        key_type: Optional[Type[K]] = None,
        value_type: Optional[Type[V]] = None,
    ) -> Union[RawNamespace, Namespace[K, V]]:
        """Register a namespace with this process's writer.

        ``fold`` is ``None`` or one of ``sum_i64``, ``sum_f64``, ``min_i64``,
        ``max_i64``, ``min_f64``, ``max_f64``, ``count``, ``last`` and
        ``concat``.

        With ``shared``, each scope holds the keys of every writer and the
        newest copy wins for a key that several wrote. Otherwise the first
        batch of a new writer replaces the scope. A shared namespace has no
        fold and refuses erases.

        By default every key and value goes through :func:`encode`, and reads
        return decoded objects. The result is a :class:`Namespace` of the
        types ``K`` and ``V`` that you annotate the variable with, as in
        ``ns: Namespace[str, int] = store.namespace("n")``. That form makes
        no run-time check. To check at run time, pass both ``key_type`` and
        ``value_type``: each key and value that goes in or comes out must be
        an instance of them, or the call raises ``TypeError``. Passing only
        one is a ``TypeError``.

        With ``raw``, keys and values are bytes-like and reads return bytes,
        for data that C++ or the C ABI reads or writes. A namespace with a
        ``fold`` is always raw, because fold inputs are fixed binary values.
        ``raw`` and the types are not stored, so each process passes the same
        values.
        """
        if (key_type is None) != (value_type is None):
            raise TypeError("pass both key_type and value_type, or neither")
        if fold is None:
            kind = 0
        else:
            try:
                kind = _FOLDS[fold]
            except KeyError:
                raise ValueError("unknown fold %r" % (fold,)) from None
        native = self._native.namespace(name, format_version, kind, durable, shared)
        if raw or fold is not None:
            if key_type is not None:
                raise TypeError("key_type and value_type do not apply to a raw namespace")
            return RawNamespace(native)
        return Namespace(native, key_type, value_type)

    @overload
    def batch(
        self,
        ns: RawNamespace,
        scope: Scope,
        params_hash: int = ...,
        sources: Optional[Sequence[Tuple[int, int, int]]] = ...,
    ) -> RawBatch: ...

    @overload
    def batch(
        self,
        ns: Namespace[K, V],
        scope: Scope,
        params_hash: int = ...,
        sources: Optional[Sequence[Tuple[int, int, int]]] = ...,
    ) -> Batch[K, V]: ...

    def batch(
        self,
        ns: Union[RawNamespace, Namespace[K, V]],
        scope: Scope,
        params_hash: int = 0,
        sources: Optional[Sequence[Tuple[int, int, int]]] = None,
    ) -> Union[RawBatch, Batch[K, V]]:
        """Start a batch of writes for ``scope``.

        ``sources`` holds one ``(mtime, size, hash)`` per path of the scope
        in path order. A batch of a raw namespace is a :class:`RawBatch`,
        which alone has ``fold``.
        """
        native = self._native.batch(ns.native, scope, params_hash, sources)
        if isinstance(ns, RawNamespace):
            return RawBatch(native)
        return Batch(native, ns._encode_key, ns._encode_value)

    def flush(self) -> None:
        """Make the committed batches visible to readers."""
        self._native.flush()

    def refresh(self) -> None:
        """Look for new data of other processes now; snapshot() looks once per second."""
        self._native.refresh()

    def close(self) -> None:
        """Flush and close this process's writer; a later write opens a new one."""
        self._native.close()

    def drop(self, ns: Union[RawNamespace, Namespace[K, V]], scope: Scope) -> None:
        """Hide the whole scope."""
        self._native.drop(ns.native, scope)

    def pack(
        self,
        small_file_bytes: Optional[int] = None,
        dead_ratio: Optional[float] = None,
        keep_seconds: Optional[int] = None,
    ) -> PackReport:
        """Rewrite the chosen data files; run it with no other writer active."""
        return PackReport(*self._native.pack(small_file_bytes, dead_ratio, keep_seconds))

    def snapshot(self) -> Snapshot:
        """A point-in-time view of the store."""
        return Snapshot(self._native.snapshot())


__all__ = [
    "Batch",
    "BytesLike",
    "CopyInfo",
    "Encodable",
    "Namespace",
    "PackReport",
    "RawBatch",
    "RawNamespace",
    "Scope",
    "ScopeState",
    "Snapshot",
    "SourceStat",
    "Store",
    "StoreAware",
    "decode",
    "encode",
    "encode_prefix",
    "matches_prefix",
    "register_type",
]
