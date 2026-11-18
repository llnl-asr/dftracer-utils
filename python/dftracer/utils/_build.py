# SPDX-License-Identifier: MIT
"""The one compile pipeline for DFTracer plugins and ops: resolve the ABI include
dir, select a compiler, and build a source text or file to a loadable shared object.

``@jit.plugin``, ``@jit.op``, ``@dft.udf`` and the ``dftracer_plugin`` console
script all build through it. Include-dir resolution order:
(1) ``DFTRACER_UTILS_PLUGIN_INCLUDE``, (2) headers bundled in the installed package
(``<pkg>/include``), (3) the source-tree parent walk (dev checkout).

The compiler, the flags and the digest of everything but the source are resolved
once per environment (:func:`toolchain`), so a cached build costs one hash of its
source and one ``stat``.
"""

from __future__ import annotations

import functools
import hashlib
import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple, Union

from .dftracer_utils_ext import PLUGIN_ABI_VERSION


class BuildError(Exception):
    """Include-dir resolution failed or the compiler rejected the source."""


class Toolchain(NamedTuple):
    """What every build in one environment shares."""

    include: str
    cxx: str
    flags: Tuple[str, ...]
    stamp: str
    """A digest of the compiler, the include dir, the platform and the ABI headers;
    a source digest starts from it, so a change to any of them is a new cache key."""


def include_dir() -> str:
    """Resolve the directory that contains ``dftracer/utils/plugins/abi.h``."""
    env = os.environ.get("DFTRACER_UTILS_PLUGIN_INCLUDE")
    if env:
        return env
    rel = Path("dftracer") / "utils" / "plugins" / "abi.h"
    # An installed package ships them under dftracer/include, beside
    # dftracer/lib and dftracer/bin; a source checkout has include/.
    for base in Path(__file__).resolve().parents:
        if (base / "include" / rel).is_file():
            return str(base / "include")
    raise BuildError("cannot locate the plugin include dir; set DFTRACER_UTILS_PLUGIN_INCLUDE")


def compiler() -> str:
    return os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or "c++"


@functools.lru_cache(maxsize=None)
def _is_gcc(cxx: str) -> bool:
    # GCC binds the statics of inline functions and templates as GNU unique
    # symbols, which keep a dlopen-ed plugin mapped after dlclose.
    try:
        proc = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    except OSError:
        return False
    text = proc.stdout.lower()
    return "clang" not in text and ("free software foundation" in text or "gcc" in text)


@functools.lru_cache(maxsize=None)
def _cxx_version(cxx: str) -> str:
    # A compiler upgraded in place keeps its path; its version line must still
    # change the cache key.
    try:
        proc = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    except OSError:
        return ""
    return proc.stdout


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
    return int(PLUGIN_ABI_VERSION)


def _ensure_abi_version_header(include: str) -> Optional[str]:
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


def cache_dir() -> Path:
    env = os.environ.get("DFTRACER_UTILS_JIT_CACHE")
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
        h.update((base / "dataframe/c_api.h").read_bytes())
    except OSError:
        h.update(b"\0")
    return h.hexdigest()[:16]


def _make_toolchain() -> Toolchain:
    include = include_dir()
    cxx = compiler()
    gcc = _is_gcc(cxx)
    flags = ["-std=c++20", "-fPIC", "-shared", f"-I{include}"]
    if gcc:
        flags.append("-fno-gnu-unique")
    overlay = _ensure_abi_version_header(include)
    if overlay:
        flags.append(f"-I{overlay}")
    if sys.platform == "darwin":
        flags += ["-undefined", "dynamic_lookup"]
    parts = [
        include,
        cxx,
        _cxx_version(cxx),
        "no-gnu-unique" if gcc else "",
        sys.platform,
        _abi_fingerprint(include),
        str(_host_abi_version()),
    ]
    stamp = hashlib.sha256("\0".join(parts).encode("utf-8")).hexdigest()
    return Toolchain(include, cxx, tuple(flags), stamp)


_TOOLCHAIN_ENV = (
    "CXX",
    "PATH",
    "HOME",
    "DFTRACER_UTILS_PLUGIN_INCLUDE",
    "DFTRACER_UTILS_JIT_CACHE",
)
_TOOLCHAINS: Dict[Tuple[Optional[str], ...], Toolchain] = {}


def toolchain() -> Toolchain:
    """The compiler, flags and cache stamp for the current environment, computed on
    the first call and again only when the compiler, ``PATH``, the include dir or
    the cache dir changes."""
    key = tuple(os.environ.get(name) for name in _TOOLCHAIN_ENV)
    tc = _TOOLCHAINS.get(key)
    if tc is None:
        tc = _TOOLCHAINS[key] = _make_toolchain()
    return tc


def cflags() -> List[str]:
    """Compile flags a plugin needs: C++20, position-independent, shared."""
    return list(toolchain().flags)


def source_digest(source: str, extra_flags: Sequence[str] = ()) -> str:
    """The cache key of ``source`` built with ``extra_flags`` in this environment."""
    h = hashlib.sha256(toolchain().stamp.encode("utf-8"))
    for flag in extra_flags:
        h.update(b"\0" + flag.encode("utf-8"))
    h.update(b"\0\0" + source.encode("utf-8"))
    return h.hexdigest()[:16]


def _unique() -> str:
    return f"{os.getpid()}.{threading.get_ident()}"


def _compile(src: Path, out: Path, extra_flags: Sequence[str]) -> None:
    # Built beside the final name and renamed, so a process or thread that builds
    # the same source at the same time never loads a half-written library.
    tc = toolchain()
    tmp = out.with_name(f"{out.stem}.{_unique()}{out.suffix}")
    cmd = [tc.cxx, *tc.flags, *extra_flags, "-o", str(tmp), str(src)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        tmp.unlink(missing_ok=True)
        raise BuildError(proc.stderr)
    os.replace(tmp, out)


def compile_source(
    source: str,
    name: str,
    *,
    out: Union[str, "os.PathLike[str]", None] = None,
    cache: Union[str, "os.PathLike[str]", None] = None,
    extra_flags: Sequence[str] = (),
    group: str = "",
) -> str:
    """Compile the C++ ``source`` to a loadable ``.so`` and return its path.

    With ``out`` unset the library is ``<cache>/<group>/<digest>/<name>.so``, where
    the digest covers the source, the flags and the toolchain, so an unchanged
    source never recompiles; ``cache`` replaces the default cache dir; ``name`` is the library's file stem, which the host
    uses to name a plugin and its store namespaces. With ``out`` set it always
    compiles to that path. The source is kept beside the library as ``.cpp``.
    Raises :class:`BuildError` on a compile failure."""
    extra = list(extra_flags)
    if out is not None:
        out_path = Path(out)
    else:
        base = Path(cache) if cache is not None else cache_dir()
        out_path = base / group / source_digest(source, extra) / f"{name}.so"
        if out_path.is_file():
            return str(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    src_path = out_path.with_suffix(".cpp")
    tmp_src = out_path.with_name(f"{name}.{_unique()}.cpp")
    tmp_src.write_text(source, encoding="utf-8")
    try:
        _compile(tmp_src, out_path, extra)
    finally:
        os.replace(tmp_src, src_path)
    return str(out_path)


def compile_file(
    src: Union[str, "os.PathLike[str]"], out: Union[str, "os.PathLike[str]", None] = None
) -> str:
    """Compile the source file ``src`` to a loadable ``.so`` and return its path.

    With ``out`` unset, the output is a content-hashed path in the cache dir named
    after ``src``'s stem, so an unchanged source never recompiles."""
    src_path = Path(src)
    if out is not None:
        out_path = Path(out)
    else:
        digest = source_digest(src_path.read_text(encoding="utf-8"))
        out_path = cache_dir() / digest / f"{src_path.stem}.so"
        if out_path.is_file():
            return str(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
    _compile(src_path, out_path, ())
    return str(out_path)
