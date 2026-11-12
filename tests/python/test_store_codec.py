#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for the store key and value codec."""

import collections
import enum
import math
import random
import struct
import subprocess
import sys

import pytest

from dftracer.utils import store_codec
from dftracer.utils.store import Store
from dftracer.utils.store_codec import (
    MAX_DEPTH,
    decode,
    encode,
    encode_prefix,
    matches_prefix,
    register_type,
)


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


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class Unregistered:
    pass


def test_round_trip_every_type():
    values = [
        None,
        True,
        False,
        0,
        1,
        -1,
        255,
        256,
        -256,
        2**63,
        -(2**63),
        2**64,
        -(2**64),
        2**100 + 7,
        -(2**100) - 7,
        2**2000,
        -(2**2000),
        0.0,
        1.5,
        -1.5,
        float("inf"),
        float("-inf"),
        "",
        "plain",
        "nul\x00inside",
        "caf\u00e9",
        b"",
        b"\x00",
        b"a\x00b\x00\xff",
        (),
        (1, "a", None, b"\x00"),
        ((), (None,), (None, None)),
        [],
        [1, [2, None], (3, None)],
        {},
        {"a": 1, 2: [None, b"x"], None: None, (1, 2): {"k": "v"}},
    ]
    for value in values:
        assert decode(encode(value)) == value
        assert type(decode(encode(value))) is type(value)


def test_round_trip_nan_and_negative_zero():
    assert math.isnan(decode(encode(float("nan"))))
    zero = decode(encode(-0.0))
    assert zero == 0.0
    assert math.copysign(1.0, zero) == -1.0
    assert encode(-0.0) < encode(0.0)


def test_bytes_like_decode_to_bytes():
    assert decode(encode(bytearray(b"a\x00"))) == b"a\x00"
    assert decode(encode(memoryview(b"a\x00"))) == b"a\x00"


def test_int_known_bytes():
    assert encode(0) == b"\x14"
    assert encode(-1) == b"\x13\xfe"
    assert encode(1) == b"\x15\x01"
    assert encode(True) == b"\x27"


def test_int_too_large():
    with pytest.raises(ValueError):
        encode(2 ** (8 * 256))
    with pytest.raises(ValueError):
        encode(-(2 ** (8 * 256)))


def _check_order(values):
    ordered = sorted(values)
    encoded = [encode(v) for v in ordered]
    for a, b, ea, eb in zip(ordered, ordered[1:], encoded, encoded[1:]):
        if a < b:
            assert ea < eb, (a, b)
        else:
            assert ea == eb


def _random_str(rng):
    return "".join(rng.choice("ab\x00\x01\u00e9z") for _ in range(rng.randint(0, 5)))


def _random_bytes(rng):
    return bytes(rng.choice(b"\x00\x01\xfe\xff") for _ in range(rng.randint(0, 5)))


def test_order_invariant_ints():
    rng = random.Random(7)
    values = [
        rng.randint(-(2 ** rng.randint(1, 200)), 2 ** rng.randint(1, 200)) for _ in range(500)
    ]
    values += [0, 1, -1, 255, 256, -255, -256, 2**64 - 1, 2**64, -(2**64), -(2**64) - 1]
    _check_order(values)


def test_order_invariant_floats():
    rng = random.Random(8)
    values = [struct.unpack(">d", struct.pack(">Q", rng.getrandbits(64)))[0] for _ in range(500)]
    values = [v for v in values if not math.isnan(v)]
    values += [float("-inf"), float("inf"), -1.0, -0.5, 0.5, 1.0, 5e-324, -5e-324]
    _check_order([v for v in values if v != 0.0])


def test_order_invariant_str_and_bytes():
    rng = random.Random(9)
    _check_order([_random_str(rng) for _ in range(500)])
    _check_order([_random_bytes(rng) for _ in range(500)])


def test_order_invariant_tuples():
    rng = random.Random(10)
    values = [
        (rng.randint(-1000, 1000), _random_str(rng), rng.randint(-(2**70), 2**70))
        for _ in range(500)
    ]
    _check_order(values)
    _check_order([(_random_str(rng),) * rng.randint(0, 3) for _ in range(300)])


def test_prefix():
    assert encode_prefix("ab") == encode("ab")[:-1]
    assert encode("abc").startswith(encode_prefix("ab"))
    assert encode("a\x00").startswith(encode_prefix("a"))
    assert encode(b"ab\x00").startswith(encode_prefix(b"ab"))
    assert encode(b"a\xff").startswith(encode_prefix(b"a"))
    assert encode(("a", 1, 2)).startswith(encode_prefix(("a", 1)))
    assert not encode(("b", 1)).startswith(encode_prefix(("a",)))
    with pytest.raises(TypeError):
        encode_prefix(5)
    with pytest.raises(TypeError):
        encode_prefix(None)


def test_tuple_prefix_filter():
    cases = [
        (("a",), ("a\x00",), False),
        (("a", b"x"), ("a", b"x\x00"), False),
        (("a", ("b",)), ("a", ("b", None)), False),
        (("a",), ("a",), True),
        (("a",), ("a", 1), True),
        (("a",), ("a", None), True),
        (("a", ("b",)), ("a", ("b",), 2), True),
        (("a", None), ("a", None, 1), True),
        ((), ("x",), True),
        (("a",), ("b", 1), False),
    ]
    for prefix, key, expected in cases:
        raw_prefix = encode_prefix(prefix)
        assert matches_prefix(raw_prefix, encode(key)) is expected, (prefix, key)


def test_register_type_outside_class():
    register_type(Point, 9001, lambda p: (p.x, p.y), lambda v: Point(*v))
    data = encode(("pts", Point(1, None)))
    out = decode(data)
    assert out[0] == "pts"
    assert isinstance(out[1], Point)
    assert (out[1].x, out[1].y) == (1, None)
    assert encode(Point(1, 2)) < encode(Point(1, 3))
    assert encode(Point(1, 2)) < encode(Point(2, 0))


def test_register_type_duplicates():
    def to_value(p):
        return (p.x, p.y)

    def from_value(v):
        return Point(*v)

    assert register_type(Point, 9001, to_value, from_value) is Point
    register_type(Point, 9001, to_value, from_value)
    with pytest.raises(ValueError):
        register_type(Point, 9002, to_value, from_value)
    with pytest.raises(ValueError):
        register_type(Point, 9001, lambda p: p.x, from_value)
    with pytest.raises(ValueError):
        register_type(Unregistered, 9001, to_value, from_value)
    with pytest.raises(TypeError):
        register_type(Unregistered, 9003)


def test_register_type_bad_id():
    for bad in (-1, "x", 1.5, True):
        with pytest.raises(ValueError):
            register_type(Unregistered, bad, lambda p: 0, lambda v: None)


class Auto:
    store_type_id = 9101

    def __init__(self, n):
        self.n = n

    def to_store(self):
        return self.n

    @classmethod
    def from_store(cls, value):
        return cls(value)


class AutoStatic:
    store_type_id = 9102

    def __init__(self, n, tag):
        self.n = n
        self.tag = tag

    def to_store(self):
        return (self.n, self.tag)

    @staticmethod
    def from_store(value):
        return AutoStatic(*value)


def test_store_aware_class_encodes_without_registering():
    out = decode(encode(((Auto(3), None), [AutoStatic(-4, "t")], {"k": Auto(1)})))
    assert isinstance(out[0][0], Auto) and out[0][0].n == 3 and out[0][1] is None
    assert isinstance(out[1][0], AutoStatic) and (out[1][0].n, out[1][0].tag) == (-4, "t")
    assert out[2]["k"].n == 1


def test_store_aware_order_follows_to_store():
    values = list(range(-50, 50, 7))
    encoded = [encode(Auto(v)) for v in values]
    assert encoded == sorted(encoded)


def test_decorator_form():
    @register_type
    class Deco:
        store_type_id = 9103

        def to_store(self):
            return 1

        @classmethod
        def from_store(cls, value):
            return cls()

    assert isinstance(decode(encode(Deco())), Deco)


def test_register_protocol_class_directly():
    assert register_type(Auto) is Auto
    register_type(Auto)
    assert decode(encode(Auto(2))).n == 2


def test_decode_in_process_that_registered_but_never_encoded(tmp_path):
    data = encode((Auto(5), "x"))
    script = (
        "import sys\n"
        "from dftracer.utils.store_codec import decode, register_type\n"
        "@register_type\n"
        "class Auto:\n"
        "    store_type_id = 9101\n"
        "    def __init__(self, n):\n"
        "        self.n = n\n"
        "    def to_store(self):\n"
        "        return self.n\n"
        "    @classmethod\n"
        "    def from_store(cls, value):\n"
        "        return cls(value)\n"
        "out = decode(bytes.fromhex(sys.argv[1]))\n"
        "assert out[0].n == 5 and out[1] == 'x'\n"
    )
    subprocess.run([sys.executable, "-c", script, data.hex()], check=True)
    bare = (
        "from dftracer.utils.store_codec import decode\ntry:\n    decode(bytes.fromhex(%r))\nexcept ValueError as e:\n    assert 'register_type' in str(e)\n    raise SystemExit(0)\nraise SystemExit(1)\n"
        % data.hex()
    )
    subprocess.run([sys.executable, "-c", bare], check=True)


def test_subclass_needs_its_own_id():
    class Child(Auto):
        pass

    with pytest.raises(TypeError, match="own store_type_id"):
        encode(Child(1))

    class Own(Auto):
        store_type_id = 9104

    assert isinstance(decode(encode(Own(1))), Own)


def test_partial_protocol():
    class OnlyId:
        store_type_id = 9105

    class NoFrom:
        store_type_id = 9106

        def to_store(self):
            return 1

    with pytest.raises(TypeError, match="to_store"):
        encode(OnlyId())
    with pytest.raises(TypeError, match="from_store"):
        encode(NoFrom())
    with pytest.raises(TypeError, match="from_store"):
        register_type(NoFrom)


def test_duplicate_protocol_id():
    class Other:
        store_type_id = 9101

        def to_store(self):
            return 1

        @classmethod
        def from_store(cls, value):
            return cls()

    encode(Auto(1))
    with pytest.raises(ValueError):
        encode(Other())


def test_decode_unregistered_id():
    data = b"\x40" + encode(4242) + encode("x")
    with pytest.raises(ValueError, match="4242"):
        decode(data)


def test_unknown_type_names_type():
    with pytest.raises(TypeError, match="Unregistered.*register_type"):
        encode(Unregistered())
    with pytest.raises(TypeError):
        encode((1, {1, 2}))


def test_decode_errors():
    good = encode(("a", 1))
    with pytest.raises(ValueError):
        decode(b"\xfe")
    with pytest.raises(ValueError):
        decode(b"")
    with pytest.raises(ValueError):
        decode(good + b"\x00")
    for cut in range(len(good)):
        with pytest.raises(ValueError):
            decode(good[:cut])
    with pytest.raises(ValueError):
        decode(b"\x02abc")
    with pytest.raises(ValueError):
        decode(b"\x21\x00")
    with pytest.raises(ValueError):
        decode(b"\x16\x01")
    with pytest.raises(ValueError):
        decode(b"\x07\x14\x00")


def test_default_namespace_decodes_on_read(tmp_path):
    store = Store.open(str(tmp_path / "store"))
    ns = store.namespace("codec.default")
    with store.batch(ns, None) as batch:
        batch.put(5, 7)
    store.flush()
    view = store.snapshot()
    assert encode(5) != bytes(5)
    assert view.get(ns, None, 5) == 7
    assert decode(view._native.get(ns.native, None, encode(5))) == 7


class Color(enum.IntEnum):
    RED = 1


class Name(str):
    pass


Pair = collections.namedtuple("Pair", "a b")


class Registered:
    def __init__(self, n):
        self.n = n


class RegisteredChild(Registered):
    pass


def test_subclasses_raise():
    for value in (Color.RED, Name("x"), Pair(1, 2), (1, Pair(1, 2))):
        with pytest.raises(TypeError, match="register_type"):
            encode(value)
    register_type(Registered, 9301, lambda r: r.n, Registered)
    assert decode(encode(Registered(3))).n == 3
    with pytest.raises(TypeError, match="RegisteredChild"):
        encode(RegisteredChild(1))
    register_type(RegisteredChild, 9302, lambda r: r.n, RegisteredChild)
    assert isinstance(decode(encode(RegisteredChild(1))), RegisteredChild)


def test_registered_subclasses_of_builtins():
    register_type(Pair, 9303, tuple, lambda v: Pair(*v))
    out = decode(encode(("k", Pair(1, None))))
    assert out == ("k", Pair(1, None))
    assert type(out[1]) is Pair
    register_type(Color, 9304, int, Color)
    assert decode(encode(Color.RED)) is Color.RED


def _nest(depth, make):
    value = 1
    for _ in range(depth):
        value = make(value)
    return value


def test_depth_limit():
    for make in (lambda v: (v,), lambda v: [v], lambda v: {"k": v}):
        assert decode(encode(_nest(MAX_DEPTH, make))) == _nest(MAX_DEPTH, make)
        with pytest.raises(ValueError):
            encode(_nest(MAX_DEPTH + 1, make))
        with pytest.raises(ValueError):
            encode(_nest(100000, make))
    data = b"\x05" * (MAX_DEPTH + 1) + b"\x14" + b"\x00" * (MAX_DEPTH + 1)
    with pytest.raises(ValueError):
        decode(data)
    with pytest.raises(ValueError):
        decode(b"\x05" * 100000)


def test_registered_self_reference_hits_depth_limit():
    class Loop:
        pass

    register_type(Loop, 9305, lambda o: o, lambda v: v)
    with pytest.raises(ValueError):
        encode(Loop())


def test_decode_rejects_non_canonical_ints():
    bad = [
        b"\x1d\x08" + b"\x01" * 8,
        b"\x1d\x01\x05",
        b"\x0b" + bytes([8 ^ 0xFF]) + b"\x01" * 8,
        b"\x0b" + bytes([0 ^ 0xFF]),
        b"\x1d\x09\x00" + b"\x01" * 8,
        b"\x0b" + bytes([9 ^ 0xFF]) + b"\xff" + b"\x00" * 8,
        b"\x16\x00\x05",
        b"\x12\xff\xfe",
    ]
    for data in bad:
        with pytest.raises(ValueError):
            decode(data)
    for value in (255, 256, -255, -256, 2**64 - 1, 2**64, -(2**64), -(2**64) - 1):
        assert decode(encode(value)) == value
