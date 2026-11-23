# SPDX-License-Identifier: MIT
"""YAML reader and writer.

See ``dftracer.utils.formats`` for what the two formats share.
"""

from __future__ import annotations

from ..._public import set_home as _set_home
from ...dftracer_utils_ext import YamlArrayBuilder as ArrayBuilder
from ...dftracer_utils_ext import YamlKind as Kind
from ...dftracer_utils_ext import YamlMutableValue as MutableValue
from ...dftracer_utils_ext import YamlObjectBuilder as ObjectBuilder
from ...dftracer_utils_ext import YamlRecordBatch as RecordBatch
from ...dftracer_utils_ext import YamlRecordIterator as RecordIterator
from ...dftracer_utils_ext import YamlValue as Value
from ...dftracer_utils_ext import YamlWriter as Writer
from ...dftracer_utils_ext import yaml_load as load
from ...dftracer_utils_ext import yaml_loads as loads
from ...dftracer_utils_ext import yaml_parse as parse
from ...dftracer_utils_ext import yaml_read as read
from ...dftracer_utils_ext import yaml_read_records as read_records

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
    "dftracer.utils.formats.yaml", globals(), ("load", "loads", "parse", "read", "read_records")
)
