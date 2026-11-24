# SPDX-License-Identifier: MIT
"""Key-value store: namespaces, batched writes and point-in-time reads, and the
order-preserving byte encoding of its keys and values."""

from __future__ import annotations

from .._public import set_home as _set_home
from ._codec import (
    MAX_DEPTH as MAX_DEPTH,
)
from ._codec import (
    BytesLike as BytesLike,
)
from ._codec import (
    Encodable as Encodable,
)
from ._codec import (
    StoreAware as StoreAware,
)
from ._codec import (
    decode as decode,
)
from ._codec import (
    encode as encode,
)
from ._codec import (
    encode_prefix as encode_prefix,
)
from ._codec import (
    matches_prefix as matches_prefix,
)
from ._codec import (
    register_type as register_type,
)
from ._store import (
    Batch as Batch,
)
from ._store import (
    CopyInfo as CopyInfo,
)
from ._store import (
    Namespace as Namespace,
)
from ._store import (
    PackReport as PackReport,
)
from ._store import (
    RawBatch as RawBatch,
)
from ._store import (
    RawNamespace as RawNamespace,
)
from ._store import (
    Scope as Scope,
)
from ._store import (
    ScopeState as ScopeState,
)
from ._store import (
    Snapshot as Snapshot,
)
from ._store import (
    SourceStat as SourceStat,
)
from ._store import (
    Store as Store,
)

__all__ = [
    "Batch",
    "MAX_DEPTH",
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

_set_home(__name__, globals(), __all__)
