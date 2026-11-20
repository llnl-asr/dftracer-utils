# SPDX-License-Identifier: MIT
"""Key-value store: namespaces, batched writes and point-in-time reads, and the
order-preserving byte encoding of its keys and values."""

from __future__ import annotations

from .codec import (
    BytesLike as BytesLike,
)
from .codec import (
    Encodable as Encodable,
)
from .codec import (
    StoreAware as StoreAware,
)
from .codec import (
    decode as decode,
)
from .codec import (
    encode as encode,
)
from .codec import (
    encode_prefix as encode_prefix,
)
from .codec import (
    matches_prefix as matches_prefix,
)
from .codec import (
    register_type as register_type,
)
from .store import (
    Batch as Batch,
)
from .store import (
    CopyInfo as CopyInfo,
)
from .store import (
    Namespace as Namespace,
)
from .store import (
    PackReport as PackReport,
)
from .store import (
    RawBatch as RawBatch,
)
from .store import (
    RawNamespace as RawNamespace,
)
from .store import (
    Scope as Scope,
)
from .store import (
    ScopeState as ScopeState,
)
from .store import (
    Snapshot as Snapshot,
)
from .store import (
    SourceStat as SourceStat,
)
from .store import (
    Store as Store,
)

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
