# SPDX-License-Identifier: MIT
"""Give the public classes and functions of a module its public name."""

from __future__ import annotations

import types
from typing import Any, Iterable


def set_home(home: str, namespace: dict[str, Any], names: Iterable[str]) -> None:
    """Set ``__module__`` of each function in ``names`` that this package defines
    to ``home``, so a repr, a pickle path and the documentation name the public
    module and not the file that holds the code.

    A class keeps the module that defines it: ``typing.get_type_hints`` reads the
    annotations of a class in the namespace of its ``__module__``, and the
    annotations are written for the defining module. A function reads its own
    ``__globals__``, so its ``__module__`` is free to change."""
    for name in names:
        obj = namespace.get(name)
        kinds = (types.FunctionType, types.BuiltinFunctionType)
        if isinstance(obj, kinds) and (obj.__module__ or "").startswith("dftracer.utils"):
            obj.__module__ = home


def set_home_of(home: str, obj: object) -> None:
    set_home(home, {"obj": obj}, ("obj",))
