# SPDX-License-Identifier: MIT
"""JSON reader and writer.

See ``dftracer.utils.formats`` for what the two formats share.
"""

from __future__ import annotations

from ..._public import set_home as _set_home
from ...dftracer_utils_ext import JsonArrayBuilder as ArrayBuilder
from ...dftracer_utils_ext import JsonKind as Kind
from ...dftracer_utils_ext import JsonMutableValue as MutableValue
from ...dftracer_utils_ext import JsonObjectBuilder as ObjectBuilder
from ...dftracer_utils_ext import JsonRecordBatch as RecordBatch
from ...dftracer_utils_ext import JsonRecordIterator as RecordIterator
from ...dftracer_utils_ext import JsonValue as Value
from ...dftracer_utils_ext import JsonWriter as Writer
from ...dftracer_utils_ext import json_load as load
from ...dftracer_utils_ext import json_loads as loads
from ...dftracer_utils_ext import json_parse as parse
from ...dftracer_utils_ext import json_read as read
from ...dftracer_utils_ext import json_read_records as read_records

__all__ = [
    "ArrayBuilder",
    "Kind",
    "MutableValue",
    "ObjectBuilder",
    "RecordBatch",
    "RecordIterator",
    "Value",
    "Writer",
    "load",
    "loads",
    "parse",
    "read",
    "read_records",
]

_set_home(
    "dftracer.utils.formats.json", globals(), ("load", "loads", "parse", "read", "read_records")
)
