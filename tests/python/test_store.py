#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for the Store Python bindings."""

import os
import struct
import subprocess
import sys

import pytest

import dftracer.utils as dftu_utils
from dftracer.utils import store_codec
from dftracer.utils.store import (
    Namespace,
    RawBatch,
    RawNamespace,
    ScopeState,
    Store,
    decode,
    encode,
)

FILE_A = "/traces/a.pfw"
FILE_B = "/traces/b.pfw"


@pytest.fixture(autouse=True)
def clean_registry():
    by_class = dict(store_codec._by_class)
    by_id = dict(store_codec._by_id)
    unaware = set(store_codec._unaware)
    yield
    store_codec._by_class.clear()
    store_codec._by_class.update(by_class)
    store_codec._by_id.clear()
    store_codec._by_id.update(by_id)
    store_codec._unaware.clear()
    store_codec._unaware.update(unaware)


@pytest.fixture
def store(tmp_path):
    return Store.open(str(tmp_path / "store"))


def commit(store, ns, scope, rows, **kwargs):
    with store.batch(ns, scope, **kwargs) as batch:
        for key, value in rows:
            batch.put(key, value)


def snapshot_of(store):
    store.flush()
    return store.snapshot()


class TestRoundTrip:
    def test_put_then_get(self, store):
        ns = store.namespace("dftu.test.rw", format_version=3)
        commit(store, ns, FILE_A, [(b"k1", b"v1"), (b"k2", b"value two")])
        view = snapshot_of(store)
        assert view.get(ns, FILE_A, b"k1") == b"v1"
        assert view.get(ns, FILE_A, b"k2") == b"value two"
        assert view.get(ns, FILE_A, b"missing") is None
        assert view.get(ns, FILE_B, b"k1") is None
        assert view.get(ns, None, b"k1") is None

    def test_bytes_like_keys_and_values(self, store):
        ns = store.namespace("dftu.test.bytes_like")
        with store.batch(ns, None) as batch:
            batch.put(bytearray(b"a"), memoryview(b"one"))
        view = snapshot_of(store)
        assert view.get(ns, None, memoryview(b"a")) == b"one"
        with pytest.raises(TypeError):
            view.get(ns, None, object())

    def test_multi_get_keeps_order(self, store):
        ns = store.namespace("dftu.test.multi")
        commit(store, ns, FILE_A, [(b"a", b"1"), (b"b", b"2")])
        view = snapshot_of(store)
        assert view.multi_get(ns, FILE_A, [b"b", b"zz", b"a"]) == [b"2", None, b"1"]
        assert view.multi_get(ns, FILE_A, []) == []

    def test_scope_forms(self, store):
        ns = store.namespace("dftu.test.scopes")
        commit(store, ns, [FILE_B, FILE_A, FILE_A], [(b"k", b"joint")])
        commit(store, ns, None, [(b"k", b"root")])
        commit(store, ns, FILE_A, [(b"k", b"single")])
        view = snapshot_of(store)
        assert view.get(ns, (FILE_A, FILE_B), b"k") == b"joint"
        assert view.get(ns, None, b"k") == b"root"
        assert view.get(ns, FILE_A, b"k") == b"single"
        assert view.get(ns, os.fspath(FILE_A), b"k") == b"single"
        with pytest.raises(ValueError):
            view.get(ns, [], b"k")
        with pytest.raises(TypeError):
            view.get(ns, [1], b"k")

    def test_namespace_properties(self, store):
        ns = store.namespace("dftu.test.props", format_version=7, durable=True)
        assert ns.name == "dftu.test.props"
        assert ns.format_version == 7
        assert ns.durable is True
        assert ns.fold == 0

    def test_unknown_fold_is_rejected(self, store):
        with pytest.raises(ValueError):
            store.namespace("dftu.test.badfold", fold="median")

    def test_fold_namespace_sums(self, store):
        ns = store.namespace("dftu.test.fold", fold="sum_i64")
        with store.batch(ns, None) as batch:
            batch.fold(b"n", struct.pack("<q", 1))
            batch.fold(b"n", struct.pack("<q", 2))
        view = snapshot_of(store)
        assert struct.unpack("<q", view.get(ns, None, b"n"))[0] == 3

    def test_put_into_fold_namespace_fails_at_commit(self, store):
        ns = store.namespace("dftu.test.foldput", fold="sum_i64")
        batch = store.batch(ns, None)
        batch.put(b"n", b"x")
        with pytest.raises(dftu_utils.DFTUtilsValueError):
            batch.commit()


class TestErase:
    def test_erase_hides_a_key(self, store):
        ns = store.namespace("dftu.test.erase")
        commit(store, ns, FILE_A, [(b"a", b"1"), (b"b", b"2"), (b"c", b"3")])
        store.flush()
        with store.batch(ns, FILE_A) as batch:
            batch.erase(b"a")
        view = snapshot_of(store)
        assert view.get(ns, FILE_A, b"a") is None
        assert view.get(ns, FILE_A, b"b") == b"2"

    def test_erase_range_is_half_open(self, store):
        ns = store.namespace("dftu.test.erase_range")
        rows = [(bytes([c]), bytes([c])) for c in b"abcde"]
        commit(store, ns, FILE_A, rows)
        store.flush()
        with store.batch(ns, FILE_A) as batch:
            batch.erase_range(b"b", b"d")
        view = snapshot_of(store)
        keys = [key for key, _ in view.scan(ns, FILE_A)]
        assert keys == [b"a", b"d", b"e"]

    def test_erase_range_to_the_last_key(self, store):
        ns = store.namespace("dftu.test.erase_tail")
        rows = [(bytes([c]), bytes([c])) for c in b"abc"]
        commit(store, ns, FILE_A, rows)
        store.flush()
        with store.batch(ns, FILE_A) as batch:
            batch.erase_range(b"b")
        view = snapshot_of(store)
        assert [key for key, _ in view.scan(ns, FILE_A)] == [b"a"]


class TestBatch:
    def test_context_manager_commits_on_clean_exit(self, store):
        ns = store.namespace("dftu.test.commit")
        with store.batch(ns, FILE_A) as batch:
            batch.put(b"k", b"v")
        assert snapshot_of(store).get(ns, FILE_A, b"k") == b"v"

    def test_context_manager_discards_on_exception(self, store):
        ns = store.namespace("dftu.test.discard")
        with pytest.raises(KeyError):
            with store.batch(ns, FILE_A) as batch:
                batch.put(b"k", b"v")
                raise KeyError("stop")
        assert snapshot_of(store).get(ns, FILE_A, b"k") is None

    def test_commit_inside_the_block_is_not_repeated(self, store):
        ns = store.namespace("dftu.test.inner_commit")
        with store.batch(ns, FILE_A) as batch:
            batch.put(b"k", b"v")
            batch.commit()
        assert snapshot_of(store).get(ns, FILE_A, b"k") == b"v"

    def test_discard_inside_the_block_writes_nothing(self, store):
        ns = store.namespace("dftu.test.inner_discard")
        with store.batch(ns, FILE_A) as batch:
            batch.put(b"k", b"v")
            batch.discard()
        assert snapshot_of(store).get(ns, FILE_A, b"k") is None

    def test_exception_after_commit_keeps_the_data_and_the_exception(self, store):
        ns = store.namespace("dftu.test.inner_commit_raise")
        with pytest.raises(KeyError):
            with store.batch(ns, FILE_A) as batch:
                batch.put(b"k", b"v")
                batch.commit()
                raise KeyError("stop")
        assert snapshot_of(store).get(ns, FILE_A, b"k") == b"v"

    @pytest.mark.parametrize("bad", [-1, 2**64])
    def test_params_hash_out_of_range_raises(self, store, bad):
        ns = store.namespace("dftu.test.hash_range")
        with pytest.raises(OverflowError):
            store.batch(ns, FILE_A, params_hash=bad)

    @pytest.mark.parametrize("bad", [-1, 2**32])
    def test_format_version_out_of_range_raises(self, store, bad):
        with pytest.raises(OverflowError):
            store.namespace("dftu.test.version_range", format_version=bad)

    def test_source_fields_out_of_range_raise(self, store):
        ns = store.namespace("dftu.test.source_range")
        with pytest.raises(OverflowError):
            store.batch(ns, FILE_A, sources=[(-1, 0, 0)])

    def test_params_hash_keeps_the_full_range(self, store):
        ns = store.namespace("dftu.test.hash_full")
        commit(store, ns, FILE_A, [(b"k", b"v")], params_hash=2**64 - 1)
        info = snapshot_of(store).copy_info(ns, FILE_A)
        assert info.params_hash == 2**64 - 1

    def test_used_batch_is_closed(self, store):
        ns = store.namespace("dftu.test.closed")
        batch = store.batch(ns, FILE_A)
        batch.commit()
        with pytest.raises(dftu_utils.DFTUtilsError):
            batch.put(b"k", b"v")
        with pytest.raises(dftu_utils.DFTUtilsError):
            batch.commit()
        batch.discard()

    def test_drop_hides_the_scope(self, store):
        ns = store.namespace("dftu.test.drop")
        commit(store, ns, FILE_A, [(b"k", b"v")])
        store.drop(ns, FILE_A)
        assert snapshot_of(store).get(ns, FILE_A, b"k") is None


class TestScan:
    def test_prefix_scan_is_in_key_order(self, store):
        ns = store.namespace("dftu.test.scan")
        rows = [
            (b"b2", b"B2"),
            (b"a1", b"A1"),
            (b"b1", b"B1"),
            (b"c1", b"C1"),
        ]
        commit(store, ns, FILE_A, rows)
        view = snapshot_of(store)
        assert list(view.scan(ns, FILE_A, b"b")) == [(b"b1", b"B1"), (b"b2", b"B2")]
        assert [key for key, _ in view.scan(ns, FILE_A)] == [
            b"a1",
            b"b1",
            b"b2",
            b"c1",
        ]
        assert list(view.scan(ns, FILE_A, b"zz")) == []

    def test_cursor_stays_at_its_end(self, store):
        ns = store.namespace("dftu.test.scan_end")
        commit(store, ns, FILE_A, [(b"k", b"v")])
        cursor = snapshot_of(store).scan(ns, FILE_A)
        assert next(cursor) == (b"k", b"v")
        with pytest.raises(StopIteration):
            next(cursor)
        with pytest.raises(StopIteration):
            next(cursor)

    def test_scan_sees_the_snapshot_not_later_writes(self, store):
        ns = store.namespace("dftu.test.scan_snapshot")
        commit(store, ns, FILE_A, [(b"a", b"1")])
        view = snapshot_of(store)
        commit(store, ns, FILE_A, [(b"b", b"2")])
        store.flush()
        assert [key for key, _ in view.scan(ns, FILE_A)] == [b"a"]


class TestCopyInfo:
    def test_absent_before_any_write(self, store):
        ns = store.namespace("dftu.test.copy_absent")
        info = snapshot_of(store).copy_info(ns, FILE_A)
        assert info.state == ScopeState.ABSENT
        assert info.sources == []

    def test_ready_with_params_and_sources(self, store):
        ns = store.namespace("dftu.test.copy_ready")
        commit(
            store,
            ns,
            FILE_A,
            [(b"k", b"v")],
            params_hash=77,
            sources=[(11, 22, 33)],
        )
        info = snapshot_of(store).copy_info(ns, FILE_A)
        assert info.state == ScopeState.READY
        assert info.params_hash == 77
        assert info.generation > 0
        assert info.sources == [(11, 22, 33)]

    def test_absent_after_drop(self, store):
        ns = store.namespace("dftu.test.copy_drop")
        commit(store, ns, FILE_A, [(b"k", b"v")])
        store.drop(ns, FILE_A)
        info = snapshot_of(store).copy_info(ns, FILE_A)
        assert info.state == ScopeState.ABSENT


class TestPack:
    def test_pack_keeps_the_data(self, store):
        ns = store.namespace("dftu.test.pack")
        commit(store, ns, FILE_A, [(b"p1", b"one")])
        store.close()
        commit(store, ns, FILE_B, [(b"p2", b"two")])
        store.close()
        report = store.pack(small_file_bytes=1 << 30, keep_seconds=0)
        assert report.files_packed >= 2
        assert report.copies_written >= 2
        view = snapshot_of(store)
        assert view.get(ns, FILE_A, b"p1") == b"one"
        assert view.get(ns, FILE_B, b"p2") == b"two"

    def test_pack_with_defaults(self, store):
        ns = store.namespace("dftu.test.pack_defaults")
        commit(store, ns, FILE_A, [(b"k", b"v")])
        store.pack()
        assert snapshot_of(store).get(ns, FILE_A, b"k") == b"v"


@pytest.mark.skipif(not hasattr(os, "fork"), reason="needs os.fork")
class TestFork:
    def test_child_refuses_inherited_handle_and_opens_its_own(self, tmp_path):
        directory = str(tmp_path / "store")
        store = Store.open(directory)
        ns = store.namespace("dftu.test.fork")
        commit(store, ns, FILE_A, [(b"k", b"parent")])
        store.flush()

        pid = os.fork()
        if pid == 0:
            code = 1
            try:
                try:
                    store.namespace("dftu.test.fork")
                    code = 10
                except dftu_utils.DFTUtilsError as err:
                    if "fork" not in str(err):
                        code = 11
                    else:
                        own = Store.open(directory)
                        own_ns = own.namespace("dftu.test.fork")
                        commit(own, own_ns, FILE_B, [(b"k", b"child")])
                        own.close()
                        view = own.snapshot()
                        ok = view.get(own_ns, FILE_B, b"k") == b"child"
                        ok = ok and view.get(own_ns, FILE_A, b"k") == b"parent"
                        code = 0 if ok else 12
            except BaseException:
                code = 13
            finally:
                os._exit(code)
        _, status = os.waitpid(pid, 0)
        assert os.WIFEXITED(status)
        assert os.WEXITSTATUS(status) == 0
        assert snapshot_of(store).get(ns, FILE_A, b"k") == b"parent"


def test_shared_namespace_keeps_keys_of_every_writer(tmp_path):
    path = str(tmp_path / "shared")
    script = (
        "import sys\n"
        "from dftracer.utils.store import Store\n"
        "store = Store.open(sys.argv[1])\n"
        "ns = store.namespace('artifacts', shared=True)\n"
        "batch = store.batch(ns, None)\n"
        "batch.put(sys.argv[2].encode(), b'value')\n"
        "batch.commit()\n"
        "store.close()\n"
    )
    for key in ("first", "second", "third"):
        subprocess.run([sys.executable, "-c", script, path, key], check=True)
    store = Store.open(path)
    ns = store.namespace("artifacts", shared=True)
    assert ns.shared
    view = store.snapshot()
    for key in (b"first", b"second", b"third"):
        assert view.get(ns, None, key) == b"value"


class TestModes:
    def test_default_mode_decodes(self, store):
        ns = store.namespace("dftu.test.default")
        assert isinstance(ns, Namespace)
        assert not ns.raw
        assert ns.name == "dftu.test.default"
        assert ns.fold == 0
        assert not ns.shared
        assert not ns.durable
        assert ns.format_version == 0
        rows = [
            (-5, "neg"),
            (3, "three"),
            (2.5, "float"),
            ("s", {"a": [1, None]}),
            ((1, "x"), b"b"),
        ]
        commit(store, ns, FILE_A, rows)
        view = snapshot_of(store)
        for key, value in rows:
            assert view.get(ns, FILE_A, key) == value
        assert view.get(ns, FILE_A, 99) is None
        assert view.multi_get(ns, FILE_A, [3, 99, "s"]) == ["three", None, {"a": [1, None]}]
        assert decode(view._native.get(ns.native, FILE_A, encode(3))) == "three"

    def test_scan_in_key_order(self, store):
        ns = store.namespace("dftu.test.order")
        keys = [10, -3, 0, 7, -100, 2**70, -(2**70)]
        commit(store, ns, None, [(k, str(k)) for k in keys])
        view = snapshot_of(store)
        assert [k for k, _ in view.scan(ns, None)] == sorted(keys)
        assert [k for k, _ in view.scan(ns, None, None)] == sorted(keys)

    def test_tuple_prefix_scan(self, store):
        ns = store.namespace("dftu.test.prefix")
        keys = [("a", 1), ("a", 2), ("ab", 1), ("b", 1), ("a", 3, "x")]
        commit(store, ns, None, [(k, i) for i, k in enumerate(keys)])
        view = snapshot_of(store)
        assert [k for k, _ in view.scan(ns, None, ("a",))] == [("a", 1), ("a", 2), ("a", 3, "x")]
        assert [k for k, _ in view.scan(ns, None, ("a", 3))] == [("a", 3, "x")]

    def test_tuple_prefix_scan_skips_escaped_longer_elements(self, store):
        ns = store.namespace("dftu.test.prefixfilter")
        keys = [
            ("a",),
            ("a\x00",),
            ("a", b"x"),
            ("a", b"x\x00"),
            ("a", ("b",)),
            ("a", ("b", None)),
            ("a", None),
            ("a", None, 1),
        ]
        commit(store, ns, None, [(k, 0) for k in keys])
        view = snapshot_of(store)
        got = {k for k, _ in view.scan(ns, None, ("a",))}
        assert got == {
            ("a",),
            ("a", b"x"),
            ("a", b"x\x00"),
            ("a", ("b",)),
            ("a", ("b", None)),
            ("a", None),
            ("a", None, 1),
        }
        assert {k for k, _ in view.scan(ns, None, ("a", b"x"))} == {("a", b"x")}
        assert {k for k, _ in view.scan(ns, None, ("a", ("b",)))} == {("a", ("b",))}

    def test_erase(self, store):
        ns = store.namespace("dftu.test.erase")
        commit(store, ns, FILE_A, [(1, "a"), (2, "b"), (3, "c")])
        with store.batch(ns, FILE_A) as batch:
            batch.erase(2)
            batch.erase_range(3)
        view = snapshot_of(store)
        assert [k for k, _ in view.scan(ns, FILE_A)] == [1]
        assert view.copy_info(ns, FILE_A).state == ScopeState.READY

    def test_raw_passes_bytes_through(self, store):
        ns = store.namespace("dftu.test.raw", raw=True)
        assert ns.raw
        commit(store, ns, None, [(b"k1", b"\x00v"), (bytearray(b"k2"), memoryview(b"w"))])
        view = snapshot_of(store)
        assert view.get(ns, None, b"k1") == b"\x00v"
        assert view.get(ns, None, memoryview(b"k2")) == b"w"
        assert view.multi_get(ns, None, [b"k1", b"zz"]) == [b"\x00v", None]
        assert list(view.scan(ns, None, b"k")) == [(b"k1", b"\x00v"), (b"k2", b"w")]
        assert list(view.scan(ns, None)) == list(view.scan(ns, None, None))

    def test_raw_refuses_other_types(self, store):
        ns = store.namespace("dftu.test.rawtype", raw=True)
        with store.batch(ns, None) as batch:
            with pytest.raises(TypeError):
                batch.put("k", b"v")
            with pytest.raises(TypeError):
                batch.put(b"k", 5)
            with pytest.raises(TypeError):
                batch.erase(5)
        view = snapshot_of(store)
        with pytest.raises(TypeError):
            view.get(ns, None, "k")
        with pytest.raises(TypeError):
            view.multi_get(ns, None, ["k"])
        with pytest.raises(TypeError):
            list(view.scan(ns, None, "k"))

    def test_raw_data_read_in_default_mode_names_raw(self, store):
        raw = store.namespace("dftu.test.mixed", raw=True)
        commit(store, raw, None, [(encode("k"), b"\xfe not encoded")])
        view = snapshot_of(store)
        default = store.namespace("dftu.test.mixed")
        with pytest.raises(ValueError, match="raw=True"):
            view.get(default, None, "k")
        with pytest.raises(ValueError, match="raw=True"):
            list(view.scan(default, None))

    def test_key_type_and_value_type_check_at_run_time(self, store):
        ns = store.namespace("dftu.test.checked", key_type=str, value_type=int)
        assert isinstance(ns, Namespace)
        with store.batch(ns, None) as batch:
            batch.put("a", 1)
            with pytest.raises(TypeError):
                batch.put(1, 1)
            with pytest.raises(TypeError):
                batch.put("b", "x")
            with pytest.raises(TypeError):
                batch.erase(5)
        view = snapshot_of(store)
        assert view.get(ns, None, "a") == 1
        with pytest.raises(TypeError):
            view.get(ns, None, 5)
        with pytest.raises(TypeError):
            view.multi_get(ns, None, ["a", 5])

    def test_stored_wrong_type_is_refused_on_read(self, store):
        loose = store.namespace("dftu.test.claims")
        commit(store, loose, None, [("a", "text"), (7, 1)])
        view = snapshot_of(store)
        strict = store.namespace("dftu.test.claims", key_type=str, value_type=int)
        with pytest.raises(TypeError):
            view.get(strict, None, "a")
        with pytest.raises(TypeError):
            list(view.scan(strict, None))

    def test_only_one_of_key_type_and_value_type_is_refused(self, store):
        with pytest.raises(TypeError):
            store.namespace("dftu.test.one", key_type=str)
        with pytest.raises(TypeError):
            store.namespace("dftu.test.two", value_type=int)

    def test_batch_classes(self, store):
        raw = store.namespace("dftu.test.cls.raw", raw=True)
        typed = store.namespace("dftu.test.cls.typed")
        assert isinstance(raw, RawNamespace)
        assert isinstance(store.batch(raw, None), RawBatch)
        assert not hasattr(store.batch(typed, None), "fold")

    def test_fold_namespace_is_raw(self, store):
        ns = store.namespace("dftu.test.foldraw", fold="sum_i64")
        assert ns.raw
        with store.batch(ns, None) as batch:
            with pytest.raises(TypeError):
                batch.fold("n", 1)

    def test_refresh_shows_another_process(self, tmp_path):
        path = str(tmp_path / "typed-refresh")
        store = Store.open(path)
        ns = store.namespace("artifacts", shared=True)
        assert store.snapshot().get(ns, None, "k") is None
        script = (
            "import sys\n"
            "from dftracer.utils.store import Store\n"
            "store = Store.open(sys.argv[1])\n"
            "ns = store.namespace('artifacts', shared=True)\n"
            "batch = store.batch(ns, None)\n"
            "batch.put('k', 'v')\n"
            "batch.commit()\n"
            "store.close()\n"
        )
        subprocess.run([sys.executable, "-c", script, path], check=True)
        store.refresh()
        assert store.snapshot().get(ns, None, "k") == "v"

    def test_shared_across_writers(self, tmp_path):
        path = str(tmp_path / "typed-shared")
        script = (
            "import sys\n"
            "from dftracer.utils.store import Store\n"
            "store = Store.open(sys.argv[1])\n"
            "ns = store.namespace('artifacts', shared=True)\n"
            "batch = store.batch(ns, None)\n"
            "batch.put((sys.argv[2], 1), {'writer': sys.argv[2]})\n"
            "batch.commit()\n"
            "store.close()\n"
        )
        for key in ("first", "second", "third"):
            subprocess.run([sys.executable, "-c", script, path, key], check=True)
        store = Store.open(path)
        ns = store.namespace("artifacts", shared=True)
        assert ns.shared
        view = store.snapshot()
        for key in ("first", "second", "third"):
            assert view.get(ns, None, (key, 1)) == {"writer": key}
        assert [k for k, _ in view.scan(ns, None)] == [("first", 1), ("second", 1), ("third", 1)]


class Move:
    store_type_id = 9201

    def __init__(self, a, b):
        self.a = a
        self.b = b

    def to_store(self):
        return (self.a, self.b)

    @classmethod
    def from_store(cls, value):
        return cls(*value)


class Stat:
    store_type_id = 9202

    def __init__(self, n):
        self.n = n

    def to_store(self):
        return self.n

    @staticmethod
    def from_store(value):
        return Stat(value)


class TestClassKeys:
    def test_store_aware_class_as_key_and_value(self, store):
        ns = store.namespace("dftu.test.class")
        commit(
            store,
            ns,
            None,
            [(Move(2, 1), Stat(5)), (Move(1, 5), (Stat(1), Move(0, 0))), ((Move(1, 1), "t"), None)],
        )
        view = snapshot_of(store)
        assert view.get(ns, None, Move(2, 1)).n == 5
        pair = view.get(ns, None, Move(1, 5))
        assert isinstance(pair[0], Stat) and isinstance(pair[1], Move)
        assert view.get(ns, None, (Move(1, 1), "t")) is None
        keys = [k for k, _ in view.scan(ns, None)]
        moves = [(k.a, k.b) for k in keys if isinstance(k, Move)]
        assert moves == [(1, 5), (2, 1)]
