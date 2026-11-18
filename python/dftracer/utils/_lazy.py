# SPDX-License-Identifier: MIT
"""Load the names of a package from its submodules on first use."""

from __future__ import annotations

from importlib import import_module
from typing import Any, Callable, Dict, List, Tuple


def lazy_names(
    package: str, namespace: Dict[str, Any], submodules: Dict[str, str]
) -> Tuple[Callable[[str], Any], Callable[[], List[str]]]:
    """The module ``__getattr__`` and ``__dir__`` of ``package``: a name in
    ``submodules`` is read from its submodule on first access and cached in
    ``namespace`` (the module's ``globals()``)."""

    def getattr_(name: str) -> Any:
        submodule = submodules.get(name)
        if submodule is None:
            raise AttributeError(f"module {package!r} has no attribute {name!r}")
        value = getattr(import_module(f".{submodule}", package), name)
        namespace[name] = value
        return value

    def dir_() -> List[str]:
        return sorted([*namespace, *submodules])

    return getattr_, dir_
