# SPDX-License-Identifier: MIT
"""Where the engine finds time zone files: the directories of
``zoneinfo.TZPATH`` and the data of the ``tzdata`` package, given once."""

from __future__ import annotations

import importlib
import importlib.util
import os

from . import dftracer_utils_ext as _ext


def zone_directories() -> list[str]:
    paths: list[str] = []
    try:
        paths.extend(importlib.import_module("zoneinfo").TZPATH)
    except ImportError:  # Python 3.8 has no zoneinfo
        pass
    spec = importlib.util.find_spec("tzdata")
    for base in (spec.submodule_search_locations or ()) if spec else ():
        folder = os.path.join(base, "zoneinfo")
        if os.path.isdir(folder):
            paths.append(folder)
    return [p for p in paths if p and os.pathsep not in p]


def install_search_path() -> None:
    _ext._tz_set_search_path(os.pathsep.join(zone_directories()))
