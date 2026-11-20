# SPDX-License-Identifier: MIT
"""Finding the trace files the indexer would read."""

from __future__ import annotations

import glob
import os
from typing import Iterable, Union

from .dftracer_utils_ext import INPUT_FILE_PATTERNS, scan_files

__all__ = ["discover_trace_files"]

_PathArg = Union[str, "os.PathLike[str]"]


def _inputs_of(directory: str, recursive: bool) -> list[str]:
    return [p for p, _size in scan_files(directory, list(INPUT_FILE_PATTERNS), recursive, None)]


def _keep_inputs(paths: Iterable[str]) -> list[str]:
    """The files of ``paths`` whose names the native scan takes as input."""
    by_dir: dict[str, set[str]] = {}
    kept = []
    for path in paths:
        if not os.path.isfile(path):
            continue
        directory = os.path.dirname(path) or "."
        names = by_dir.get(directory)
        if names is None:
            names = {os.path.basename(p) for p in _inputs_of(directory, False)}
            by_dir[directory] = names
        if os.path.basename(path) in names:
            kept.append(path)
    return sorted(kept)


def discover_trace_files(path: _PathArg, recursive: bool = False) -> list[str]:
    """The trace files under ``path`` that the indexer reads, sorted.

    A directory is scanned for files whose name matches the indexer input
    patterns (``INPUT_FILE_PATTERNS``), skipping the index and output folders
    the indexer skips; ``recursive`` also enters its subdirectories. A file
    gives itself, and a glob gives the files it matches (``**`` with
    ``recursive``), each kept only when its name matches those patterns. A
    glob takes every folder it matches, the skipped ones too. A glob that
    matches nothing gives an empty list.

    Raises:
        FileNotFoundError: ``path`` has no glob characters and does not exist.
    """
    text = os.fspath(path)
    if os.path.isdir(text):
        return sorted(_inputs_of(text, recursive))
    if os.path.isfile(text):
        return _keep_inputs([text])
    if glob.has_magic(text):
        return _keep_inputs(glob.glob(text, recursive=recursive))
    raise FileNotFoundError(f"no trace file or directory at {text}")
