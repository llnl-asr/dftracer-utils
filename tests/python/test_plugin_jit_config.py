# SPDX-License-Identifier: MIT
"""A runtime-configurable @jit.plugin: ``config: Cfg`` names a dataclass whose
int, float, bool and str fields (or Optional of one) are loaded from the
Plugins config at load time and read in the body as ``self.config.<field>``."""

import gzip
import json
import shutil
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")
_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))
_needs_cxx = pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler for the jit backend")


def _write(path, rows):
    """rows: (name, dur) pairs, all on pid 1."""
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (name, d) in enumerate(rows):
            f.write(
                f'{{"name":{json.dumps(name)},"cat":"POSIX","pid":1,"tid":1,'
                f'"ts":{1000 + i},"dur":{d},"ph":1,"args":{{}}}}\n'
            )


def _trace(tmp_path, rows):
    _write(str(tmp_path / "t.pfw.gz"), rows)
    return str(tmp_path)


_DURS = [("read", d) for d in (10, 20, 30, 40, 50, 60, 70)]


def _count(plugins, path, **kwargs):
    return int(
        pa.table(plugins.run(path, **kwargs).results["hits"])
        .column("value")
        .to_numpy(zero_copy_only=False)
        .sum()
    )


def _sum(plugins, path):
    return float(
        pa.table(plugins.run(path).results["hits"])
        .column("value")
        .to_numpy(zero_copy_only=False)
        .sum()
    )


@_needs_cxx
class TestFieldTypes:
    def test_int_field_drives_behavior(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int = 0

        @jit.plugin
        class IntCfg:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.threshold:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, _DURS)
        assert _count(Plugins([IntCfg], config={"IntCfg": {"threshold": 45}}), path) == 3
        assert _count(Plugins([IntCfg], config={"IntCfg": {"threshold": 5}}), path) == 7

    def test_float_field_scales_a_sum(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            scale: float = 1.0

        @jit.plugin
        class FloatCfg:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.sum())

            @jit.each_event
            def step(self, e):
                self.hits[(e.pid,)] += e.dur * self.config.scale

        path = _trace(tmp_path, _DURS)
        assert _sum(Plugins([FloatCfg], config={"FloatCfg": {"scale": 0.5}}), path) == 140.0
        # An int is accepted for a float field.
        assert _sum(Plugins([FloatCfg], config={"FloatCfg": {"scale": 2}}), path) == 560.0

    def test_bool_field_guards_the_body(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            enabled: bool = False

        @jit.plugin
        class BoolCfg:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if self.config.enabled:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, _DURS)
        assert _count(Plugins([BoolCfg], config={"BoolCfg": {"enabled": True}}), path) == 7
        assert _count(Plugins([BoolCfg], config={"BoolCfg": {"enabled": False}}), path) == 0

    def test_str_field_matches_an_event_string(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            label: str = "read"

        @jit.plugin
        class StrCfg:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.name == self.config.label:
                    self.hits[(e.pid,)] += 1

        rows = [("read", 1), ("write", 2), ("read", 3), ("open", 4)]
        path = _trace(tmp_path, rows)
        assert _count(Plugins([StrCfg], config={"StrCfg": {"label": "write"}}), path) == 1
        assert _count(Plugins([StrCfg], config={"StrCfg": {"label": "read"}}), path) == 2
        # The dataclass default applies when the plugin is given no config.
        assert _count(Plugins([StrCfg]), path) == 2

    def test_optional_field_is_set_or_not(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            cap: Optional[int] = None

        @jit.plugin
        class OptCfg:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if self.config.cap is not None:
                    if e.dur > self.config.cap:
                        self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, _DURS)
        # Unset: the outer guard is false, nothing counts.
        assert _count(Plugins([OptCfg]), path) == 0
        assert _count(Plugins([OptCfg], config={"OptCfg": {"cap": None}}), path) == 0
        assert _count(Plugins([OptCfg], config={"OptCfg": {"cap": 45}}), path) == 3

    def test_optional_with_a_default_value(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            cap: Optional[int] = 25

        @jit.plugin
        class OptDefault:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.cap:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, _DURS)
        assert _count(Plugins([OptDefault]), path) == 5
        assert _count(Plugins([OptDefault], config={"OptDefault": {"cap": 55}}), path) == 2


@_needs_cxx
class TestDefaultsAndOverrides:
    def _plugin(self):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int = 45
            scale: float = 1.0

        @jit.plugin
        class Both:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.sum())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.threshold:
                    self.hits[(e.pid,)] += e.dur * self.config.scale

        return Both

    def test_default_is_used_without_config(self, tmp_path):
        Both = self._plugin()
        assert _sum(Plugins([Both]), _trace(tmp_path, _DURS)) == 180.0  # 50 + 60 + 70

    def test_given_value_overrides_the_default(self, tmp_path):
        Both = self._plugin()
        plugins = Plugins([Both], config={"Both": {"threshold": 55, "scale": 2.0}})
        assert _sum(plugins, _trace(tmp_path, _DURS)) == 260.0  # (60 + 70) * 2

    def test_a_partial_config_keeps_the_other_default(self, tmp_path):
        Both = self._plugin()
        plugins = Plugins([Both], config={"Both": {"scale": 3}})
        assert _sum(plugins, _trace(tmp_path, _DURS)) == 540.0  # 180 * 3


@_needs_cxx
class TestValidation:
    def _plugin(self):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int
            label: str = "x"
            scale: float = 1.0
            enabled: bool = True
            cap: Optional[int] = None

        @jit.plugin
        class Checked:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.threshold:
                    self.hits[(e.pid,)] += 1

        return Checked

    def test_missing_required_field_names_the_field(self):
        Checked = self._plugin()
        with pytest.raises(jit.JitError, match="'threshold' is required"):
            Plugins([Checked])

    @pytest.mark.parametrize(
        "field_name, value, expected",
        [
            ("threshold", "5", "int"),
            ("threshold", 1.5, "int"),
            ("threshold", True, "int"),
            ("label", 3, "str"),
            ("scale", "fast", "float"),
            ("enabled", 1, "bool"),
            ("cap", "big", "Optional\\[int\\]"),
        ],
    )
    def test_wrong_type_names_the_field(self, field_name, value, expected):
        Checked = self._plugin()
        given = {"threshold": 1, field_name: value}
        with pytest.raises(jit.JitError, match=f"'{field_name}' expects {expected}"):
            Plugins([Checked], config={"Checked": given})

    def test_unknown_key_names_the_key(self):
        Checked = self._plugin()
        with pytest.raises(jit.JitError, match="no config field 'thresh0ld'"):
            Plugins([Checked], config={"Checked": {"threshold": 1, "thresh0ld": 2}})

    def test_error_points_at_the_plugins_call(self):
        Checked = self._plugin()
        with pytest.raises(jit.JitError) as ei:
            Plugins([Checked])  # MARK:load
        err = ei.value
        assert err.line is not None and err.source is not None
        assert err.source.rstrip().endswith("# MARK:load")
        assert str(err).startswith(f"{err.file}:{err.line}:{err.col}: ")

    def test_valid_config_loads(self):
        Checked = self._plugin()
        Plugins([Checked], config={"Checked": {"threshold": 1, "label": "y", "cap": 3}})


class TestCompileErrors:
    def _line(self, mark):
        lines = open(__file__, encoding="utf-8").read().splitlines()
        return next(i for i, t in enumerate(lines, 1) if t.rstrip().endswith(f"# MARK:{mark}"))

    def test_unsupported_field_type_is_rejected_at_the_field(self):
        @dataclass(frozen=True)
        class Cfg:
            ok: int = 0
            names: List[str] = field(default_factory=list)  # MARK:listfield

        with pytest.raises(jit.JitError, match="config field 'names'") as ei:

            @jit.plugin
            class ListCfg:
                config: Cfg
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += 1

        err = ei.value
        assert err.file is not None and err.file.endswith("test_plugin_jit_config.py")
        assert err.line == self._line("listfield")
        assert err.source is not None and "names: List[str]" in err.source
        assert err.col == err.source.index("names") + 1
        assert str(err) == f"{err.file}:{err.line}:{err.col}: {err.reason}\n{err.source}"

    def test_config_must_be_a_dataclass(self):
        class NotADataclass:
            threshold = 1

        with pytest.raises(jit.JitError, match="must be annotated with a dataclass") as ei:

            @jit.plugin
            class Plain:
                config: NotADataclass  # MARK:notdc
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += 1

        assert ei.value.line == self._line("notdc")
        assert ei.value.source is not None and ei.value.col == ei.value.source.index("config") + 1

    def test_unknown_config_field_in_the_body(self):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int = 0

        with pytest.raises(jit.JitError, match="unknown config field self.config.nope") as ei:

            @jit.plugin
            class Typo:
                config: Cfg
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    if e.dur > self.config.nope:  # MARK:typo
                        self.hits[(e.pid,)] += 1

        assert ei.value.line == self._line("typo")
        assert ei.value.source is not None
        assert ei.value.col == ei.value.source.index("self.config.nope") + 1

    def test_string_config_in_arithmetic_is_rejected(self):
        @dataclass(frozen=True)
        class Cfg:
            label: str = "x"

        with pytest.raises(jit.JitError, match="string config field") as ei:

            @jit.plugin
            class StrMath:
                config: Cfg
                hits = jit.map(key=(jit.i64,), value=jit.sum())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += e.dur + self.config.label  # MARK:strmath

        assert ei.value.line == self._line("strmath")

    def test_default_must_match_the_field_type(self):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int = "five"  # type: ignore[assignment]  # MARK:baddefault

        with pytest.raises(jit.JitError, match="has default 'five', not a int") as ei:

            @jit.plugin
            class BadDefault:
                config: Cfg
                hits = jit.map(key=(jit.i64,), value=jit.count())

                @jit.each_event
                def step(self, e):
                    self.hits[(e.pid,)] += 1

        assert ei.value.line == self._line("baddefault")

    def test_jit_config_marker_is_gone(self):
        assert not hasattr(jit, "config")
        assert "config" not in jit.__all__


@_needs_cxx
def test_config_typo_in_load_config_fails_before_the_run():
    @dataclass(frozen=True)
    class Cfg:
        threshold: int = 0

    @jit.plugin
    class Threshed:
        config: Cfg
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.dur > self.config.threshold:
                self.hits[(e.pid,)] += 1

    with pytest.raises(jit.JitError, match="thresh0ld"):
        Plugins([Threshed], config={"Threshed": {"thresh0ld": 45}})

    Plugins([Threshed], config={"Threshed": {"threshold": 45}})


@_needs_cxx
def test_jit_plugin_without_config_is_not_validated():
    @jit.plugin
    class NoConfig:
        hits = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.hits[(e.pid,)] += 1

    # It declares no config, so nothing is checked.
    Plugins([NoConfig], config={"NoConfig": {"anything": 1}})


@_needs_cxx
class TestInstances:
    """The config belongs to one loaded instance, not to the shared library."""

    def _int_plugin(self):
        @dataclass(frozen=True)
        class Cfg:
            threshold: int = 0

        @jit.plugin
        class PerInstance:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.threshold:
                    self.hits[(e.pid,)] += 1

        return PerInstance

    def test_two_live_instances_keep_their_own_int_config(self, tmp_path):
        P = self._int_plugin()
        path = _trace(tmp_path, _DURS)
        a = Plugins([P], config={"PerInstance": {"threshold": 45}})
        b = Plugins([P], config={"PerInstance": {"threshold": 5}})
        assert _count(a, path) == 3
        assert _count(b, path) == 7
        assert _count(a, path) == 3

    def test_two_live_instances_keep_their_own_str_config(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            label: str = "read"

        @jit.plugin
        class PerStr:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.name == self.config.label:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, [("read", 1), ("write", 2), ("read", 3)])
        a = Plugins([PerStr], config={"PerStr": {"label": "read"}})
        b = Plugins([PerStr], config={"PerStr": {"label": "write"}})
        assert (_count(a, path), _count(b, path), _count(a, path)) == (2, 1, 2)

    def test_instances_run_at_once_on_threads(self, tmp_path):
        P = self._int_plugin()
        path = _trace(tmp_path, _DURS)
        a = Plugins([P], config={"PerInstance": {"threshold": 45}})
        b = Plugins([P], config={"PerInstance": {"threshold": 5}})
        assert _count(a, path) == 3  # builds the index once, before the threads
        with ThreadPoolExecutor(max_workers=4) as pool:
            futures = [
                pool.submit(_count, a if i % 2 == 0 else b, path, auto_index=False)
                for i in range(8)
            ]
            got = [f.result() for f in futures]
        assert got == [3, 7] * 4


@_needs_cxx
class TestOptionalNone:
    def test_explicit_none_overrides_a_non_none_default(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            cap: Optional[int] = 5

        @jit.plugin
        class OptNone:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if self.config.cap is None:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, _DURS)
        assert _count(Plugins([OptNone]), path) == 0  # default 5 is set
        assert _count(Plugins([OptNone], config={"OptNone": {"cap": 7}}), path) == 0
        assert _count(Plugins([OptNone], config={"OptNone": {"cap": None}}), path) == 7


@_needs_cxx
class TestStringEscapes:
    NAME = 'a\nb\t"q"\\\u00e9'

    def test_default_and_literal_with_control_bytes_compile_and_match(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            label: str = 'a\nb\t"q"\\\u00e9'

        @jit.plugin
        class Escapes:
            config: Cfg
            by_default = jit.map(key=(jit.i64,), value=jit.count())
            by_literal = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.name == self.config.label:
                    self.by_default[(e.pid,)] += 1
                if e.name == 'tab\there "quoted"\n':
                    self.by_literal[(e.pid,)] += 1

        rows = [(self.NAME, 1), (self.NAME, 2), ('tab\there "quoted"\n', 3), ("other", 4)]
        path = _trace(tmp_path, rows)
        run = Plugins([Escapes]).run(path)
        d = pa.table(run.results["by_default"]).column("value").to_numpy(zero_copy_only=False)
        lit = pa.table(run.results["by_literal"]).column("value").to_numpy(zero_copy_only=False)
        assert (int(d.sum()), int(lit.sum())) == (2, 1)

    def test_nul_in_a_str_config_is_kept_by_length(self, tmp_path):
        @dataclass(frozen=True)
        class Cfg:
            label: str = "x"

        @jit.plugin
        class Nul:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.name == self.config.label:
                    self.hits[(e.pid,)] += 1

        path = _trace(tmp_path, [("a", 1), ("a\x00b", 2)])
        plugins = Plugins([Nul], config={"Nul": {"label": "a\x00b"}})
        assert _count(plugins, path) == 1


@_needs_cxx
class TestPrecompiledLibrary:
    """A library loaded by path skips Python's config check, so the plugin
    checks the type of an optional key itself."""

    def _path(self):
        @dataclass(frozen=True)
        class Cfg:
            cap: Optional[int] = 5
            label: Optional[str] = None

        @jit.plugin
        class ByPath:
            config: Cfg
            hits = jit.map(key=(jit.i64,), value=jit.count())

            @jit.each_event
            def step(self, e):
                if e.dur > self.config.cap:
                    self.hits[(e.pid,)] += 1

        return jit.compile_class(ByPath)

    def test_wrong_typed_optional_value_fails_the_load_naming_the_key(self):
        so = self._path()
        key = Path(so).stem
        for given, name in (({"cap": "x"}, "cap"), ({"label": 3}, "label")):
            with pytest.raises(Exception, match=f"config key '{name}' has the wrong type"):
                Plugins([so], config={key: given})

    def test_right_typed_and_null_values_load(self, tmp_path):
        so = self._path()
        key = Path(so).stem
        Plugins([so], config={key: {"cap": 7, "label": "x"}})
        Plugins([so], config={key: {"cap": None, "label": None}})
