"""Record schemas: the record formats the index and ``TraceViewer`` understand.

The built-in schemas are ``dftracer`` and ``generic``. A user schema declares
typed fields, as a class::

    class Nginx(RecordSchema, id="nginx"):
        status: int
        request_time: float = field(role="duration", unit="s")
        host: Optional[str] = field(path="meta.host")
        upstream: str = field(always_index=True)
        tags: Optional[Json]

or as a YAML or JSON spec (``id``, ``extends``, ``fields``,
``index.path_budget``, ``dictionaries``) registered here, loaded from
``$DFTRACER_SCHEMA_PATH``, or placed in ``<index_dir>/schemas/`` next to an
index. Registered schemas last for the process.
"""

from __future__ import annotations

import json
import os
import sys
import types
import typing
from typing import Any, ClassVar, Dict, List, NewType, Optional, Type, Union

from . import dftracer_utils_ext as _ext
from .dftracer_utils_ext import DFTUtilsValueError

if sys.version_info >= (3, 11):
    from typing import dataclass_transform
else:
    from typing_extensions import dataclass_transform

__all__ = [
    "DFTracer",
    "Generic",
    "Json",
    "RecordSchema",
    "detect",
    "explain",
    "field",
    "list",
    "load",
    "register",
    "schema_id",
]

Json = NewType("Json", str)
"""A field holding any JSON value, an object or array included, as canonical
JSON text (no whitespace, keys sorted); filters compare it as that text."""

_TYPES: Dict[Any, str] = {bool: "bool", int: "int", float: "float", str: "string", Json: "json"}
_ROLES = ("time", "duration", "entity")
_UNITS = ("ns", "us", "ms", "s")


class _Field:
    __slots__ = ("path", "role", "unit", "always_index")

    def __init__(
        self, path: Optional[str], role: Optional[str], unit: Optional[str], always_index: bool
    ) -> None:
        self.path = path
        self.role = role
        self.unit = unit
        self.always_index = always_index


def field(
    *,
    path: Optional[str] = None,
    role: Optional[str] = None,
    unit: Optional[str] = None,
    always_index: bool = False,
) -> Any:
    """Options of a schema class field: the JSON ``path`` (the attribute name
    when None), a ``role`` ("time", "duration" or "entity"), the ``unit`` of a
    time or duration ("ns", "us", "ms" or "s"; microseconds when None), and
    ``always_index`` to index the field even past the path budget."""
    return _Field(path, role, unit, always_index)


def _own_annotations(cls: type) -> Dict[str, Any]:
    if sys.version_info >= (3, 10):
        import inspect

        return dict(inspect.get_annotations(cls))
    return dict(cls.__dict__.get("__annotations__", {}))


def _field_type(cls: type, name: str, hint: Any) -> "tuple[str, bool]":
    origin = typing.get_origin(hint)
    union_types: tuple = (Union,)
    if sys.version_info >= (3, 10):
        union_types = (Union, types.UnionType)
    if origin in union_types:
        args = [a for a in typing.get_args(hint) if a is not type(None)]
        if len(args) == 1 and len(typing.get_args(hint)) == 2 and args[0] in _TYPES:
            return _TYPES[args[0]], True
    elif hint in _TYPES:
        return _TYPES[hint], False
    raise DFTUtilsValueError(
        f"{cls.__qualname__}.{name}: {hint!r} is not a field type; use bool, int, "
        "float, str, Json or Optional of one"
    )


def _spec(cls: Type[RecordSchema], schema_id_: str) -> Dict[str, Any]:
    hints = typing.get_type_hints(cls)
    fields: Dict[str, Any] = {}
    paths: Dict[str, str] = {}
    for name in _own_annotations(cls):
        hint = hints[name]
        if typing.get_origin(hint) is ClassVar or hint is ClassVar:
            continue
        type_name, optional = _field_type(cls, name, hint)
        entry: Dict[str, Any] = {"type": type_name}
        if optional:
            entry["optional"] = True
        default = cls.__dict__.get(name)
        paths[name] = name
        if isinstance(default, _Field):
            if default.path is not None:
                paths[name] = default.path
                entry["path"] = default.path
            if default.role is not None:
                if default.role not in _ROLES:
                    raise DFTUtilsValueError(
                        f"{cls.__qualname__}.{name}: role {default.role!r} is not one of {_ROLES}"
                    )
                entry["role"] = default.role
            if default.unit is not None:
                if default.unit not in _UNITS:
                    raise DFTUtilsValueError(
                        f"{cls.__qualname__}.{name}: unit {default.unit!r} is not one of {_UNITS}"
                    )
                entry["unit"] = default.unit
            if default.always_index:
                entry["always_index"] = True
            delattr(cls, name)
        elif default is not None:
            raise DFTUtilsValueError(
                f"{cls.__qualname__}.{name}: a field's default must be field(...), got {default!r}"
            )
        fields[name] = entry
    parent = next(
        (b for b in cls.__mro__[1:] if isinstance(b, type) and issubclass(b, RecordSchema)),
        RecordSchema,
    )
    cls._paths = {**parent._paths, **paths}
    spec: Dict[str, Any] = {"id": schema_id_, "extends": parent.id or "generic"}
    if fields:
        spec["fields"] = fields
    return spec


@dataclass_transform(field_specifiers=(field,))
class RecordSchema:
    """Base of schema classes. ``class Nginx(RecordSchema, id="nginx")``
    registers ``nginx`` when the class is defined; each annotated attribute is
    a field (``bool``, ``int``, ``float``, ``str``, :data:`Json`, ``Optional``
    of one for an optional field), with options from :func:`field`. Subclassing another
    schema class extends it. A bad field or a registration conflict raises
    ``DFTUtilsValueError`` at class definition. An instance holds one
    attribute per field, from keywords, ``None`` when absent."""

    id: ClassVar[str] = ""
    _paths: ClassVar[Dict[str, str]] = {}

    def __init__(self, **values: Any) -> None:
        paths = type(self)._paths
        for name in values:
            if name not in paths:
                raise TypeError(f"{type(self).__qualname__} has no field {name!r}")
        for name in paths:
            setattr(self, name, values.get(name))

    def __repr__(self) -> str:
        items = ", ".join(f"{n}={getattr(self, n)!r}" for n in type(self)._paths)
        return f"{type(self).__qualname__}({items})"

    def __eq__(self, other: object) -> bool:
        return type(other) is type(self) and all(
            getattr(self, n) == getattr(other, n) for n in type(self)._paths
        )

    __hash__ = None  # type: ignore[assignment]

    def __init_subclass__(cls, id: Optional[str] = None, _builtin: bool = False, **kwargs: Any):
        super().__init_subclass__(**kwargs)
        if not id:
            raise DFTUtilsValueError(
                f"{cls.__qualname__} needs an id: class {cls.__name__}(RecordSchema, id=...)"
            )
        cls.id = id
        if not _builtin:
            source = f"class {cls.__module__}.{cls.__qualname__}"
            _ext._schema_register(json.dumps(_spec(cls, id)), source)


class DFTracer(RecordSchema, id="dftracer", _builtin=True):
    """The built-in dftracer schema, to extend by subclassing."""


class Generic(RecordSchema, id="generic", _builtin=True):
    """The built-in generic schema (no fields), to extend by subclassing."""


def schema_id(schema: Union[str, Type[RecordSchema]]) -> str:
    """The id of a schema given as an id or a schema class."""
    if isinstance(schema, str):
        return schema
    if isinstance(schema, type) and issubclass(schema, RecordSchema):
        return schema.id
    raise TypeError(f"a schema is an id or a RecordSchema class, got {schema!r}")


def register(spec: Union[Dict[str, Any], str], source: str = "<python>") -> str:
    """Register a schema from a dict or a YAML/JSON text; return its id.

    Raises ``DFTUtilsValueError`` for a malformed spec, an unknown key or
    ``extends``, a built-in id, or an id already registered with another
    definition.
    """
    text = json.dumps(spec) if isinstance(spec, dict) else spec
    return _ext._schema_register(text, source)


def load(path: Union[str, "os.PathLike[str]"]) -> List[Dict[str, Any]]:
    """Register every spec at ``path`` (a file, or a directory's ``.yaml``,
    ``.yml`` and ``.json`` files); return the registered schemas."""
    return json.loads(_ext._schema_load(os.fspath(path)))


def list() -> List[Dict[str, Any]]:  # noqa: A001
    """The registered schemas, built-ins first: ``id``, ``decoder``,
    ``fields``, ``require``, ``path_budget`` and the dictionary names."""
    return json.loads(_ext._schema_list())


def detect(path: Union[str, "os.PathLike[str]"]) -> str:
    """The id of the schema detected for the trace at ``path``."""
    return _ext._schema_detect(os.fspath(path))


def explain(path: Union[str, "os.PathLike[str]"]) -> Dict[str, Any]:
    """Why the trace at ``path`` gets its schema: ``chosen`` (the id),
    ``objects`` (sampled JSON objects) and ``scores``, each with ``id``,
    ``required`` (required path count) and ``share`` (of the objects holding
    every required path)."""
    return json.loads(_ext._schema_explain(os.fspath(path)))
