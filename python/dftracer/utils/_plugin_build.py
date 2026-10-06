"""Shared compile backend for DFTracer plugins: resolve the ABI include dir,
select a compiler, and build a source file to a loadable shared object.

Both the ``@jit.plugin`` backend and the ``dftracer_plugin`` console-script use
this. Include-dir resolution order: (1) ``DFTRACER_PLUGIN_INCLUDE``, (2) headers
bundled in the installed package (``<pkg>/include``), (3) the source-tree parent
walk (dev checkout).
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List


class PluginBuildError(Exception):
    """Include-dir resolution failed or the compiler rejected the source."""


def include_dir() -> str:
    """Resolve the directory that contains ``dftracer/utils/plugins/abi.h``."""
    env = os.environ.get("DFTRACER_PLUGIN_INCLUDE")
    if env:
        return env
    rel = Path("dftracer") / "utils" / "plugins" / "abi.h"
    # An installed package ships them under dftracer/include, beside
    # dftracer/lib and dftracer/bin; a source checkout has include/.
    for base in Path(__file__).resolve().parents:
        if (base / "include" / rel).is_file():
            return str(base / "include")
    raise PluginBuildError("cannot locate the plugin include dir; set DFTRACER_PLUGIN_INCLUDE")


def compiler() -> str:
    return os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or "c++"


def _plugin_abi_headers(base: Path) -> List[Path]:
    # plugins/abi.h plus every part header it includes (plugins/abi/*.h),
    # sorted so the cache key hashes them in a fixed order.
    plugins_dir = base / "plugins"
    headers = [plugins_dir / "abi.h"]
    headers += sorted((plugins_dir / "abi").glob("*.h"))
    return headers


def _host_abi_version() -> int:
    # The loaded extension is the host a JIT plugin is loaded into, so its
    # version is the one the plugin must carry.
    from .dftracer_utils_ext import PLUGIN_ABI_VERSION

    return int(PLUGIN_ABI_VERSION)


def _ensure_abi_version_header(include: str) -> str | None:
    """Write dftracer/utils/plugins/abi_version.h into the JIT cache when
    ``include`` has none (a source checkout, where CMake writes the header into
    its build tree), stamped with the loaded host's plugin ABI version. Returns
    the extra include directory to add, or None when ``include`` already has
    one (an installed package or a CMake build tree)."""
    rel = Path("dftracer") / "utils" / "plugins" / "abi_version.h"
    if (Path(include) / rel).is_file():
        return None
    version = _host_abi_version()
    overlay = cache_dir() / "abi_version_include"
    header_path = overlay / rel
    header_path.parent.mkdir(parents=True, exist_ok=True)
    text = (
        "#ifndef DFTRACER_UTILS_PLUGINS_ABI_VERSION_H\n"
        "#define DFTRACER_UTILS_PLUGINS_ABI_VERSION_H\n\n"
        f"#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_MAJOR {version >> 16}\n"
        f"#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_MINOR {(version >> 8) & 0xFF}\n"
        f"#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_PATCH {version & 0xFF}\n"
        f"#define DFTRACER_UTILS_PLUGIN_ABI_VERSION 0x{version:X}u\n\n"
        "#endif  // DFTRACER_UTILS_PLUGINS_ABI_VERSION_H\n"
    )
    if not header_path.is_file() or header_path.read_text() != text:
        header_path.write_text(text)
    return str(overlay)


def cflags() -> List[str]:
    """Compile flags a plugin needs: C++20, position-independent, shared."""
    inc = include_dir()
    flags = ["-std=c++20", "-fPIC", "-shared", f"-I{inc}"]
    overlay = _ensure_abi_version_header(inc)
    if overlay:
        flags.append(f"-I{overlay}")
    if sys.platform == "darwin":
        flags += ["-undefined", "dynamic_lookup"]
    return flags


def cache_dir() -> Path:
    env = os.environ.get("DFTRACER_JIT_CACHE")
    base = Path(env) if env else Path.home() / ".cache" / "dftracer-utils" / "jit"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _abi_fingerprint(include: str) -> str:
    # A compiled plugin's struct layout depends on the ABI headers, so a change
    # to them must invalidate the cache - the emitted source text alone would
    # not, silently reusing a .so built against an incompatible layout.
    h = hashlib.sha256()
    base = Path(include) / "dftracer" / "utils"
    for path in _plugin_abi_headers(base):
        try:
            h.update(path.read_bytes())
        except OSError:
            h.update(b"\0")
    try:
        h.update((base / "dataframe/abi.h").read_bytes())
    except OSError:
        h.update(b"\0")
    return h.hexdigest()[:16]


def source_digest(source: str, include: str, cxx: str) -> str:
    parts = [
        source,
        include,
        cxx,
        sys.platform,
        _abi_fingerprint(include),
        str(_host_abi_version()),
    ]
    return hashlib.sha256("\0".join(parts).encode("utf-8")).hexdigest()[:16]


def build_shared(src: str, out: str | None = None, name: str | None = None) -> str:
    """Compile the source file ``src`` to a loadable ``.so`` and return its path.

    With ``out`` unset, the output is a content-hashed path in the cache dir
    (``name`` sets its basename, defaulting to ``src``'s stem) so an unchanged
    source never recompiles. Raises :class:`PluginBuildError` on compile failure.
    """
    src_path = Path(src)
    cxx = compiler()
    inc = include_dir()
    if out is None:
        source = src_path.read_text(encoding="utf-8")
        digest = source_digest(source, inc, cxx)
        out_path = cache_dir() / f"{name or src_path.stem}.{digest}.so"
        if out_path.is_file():
            return str(out_path)
    else:
        out_path = Path(out)
    cmd = [cxx, *cflags(), "-o", str(out_path), str(src_path)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise PluginBuildError(proc.stderr)
    return str(out_path)
