# SPDX-License-Identifier: MIT
"""Human-readable byte/duration parsing (dftracer.utils._units)."""

import pytest

from dftracer.utils._units import (
    coerce_bytes,
    coerce_duration,
    parse_bytes,
    parse_duration_seconds,
)


@pytest.mark.parametrize(
    "text,expected",
    [
        ("0", 0),
        ("512", 512),
        ("1KB", 1024),
        ("1KiB", 1024),
        ("64KB", 64 * 1024),
        ("2MB", 2 * 1024 * 1024),
        ("1GB", 1024**3),
        ("1.5GiB", int(1.5 * 1024**3)),
        ("8b", 1),
        ("8B", 8),
        ("8kb", 1024),
        ("8Kib", 1024),
        ("  4MiB  ", 4 * 1024 * 1024),
    ],
)
def test_parse_bytes(text, expected):
    assert parse_bytes(text) == expected


@pytest.mark.parametrize("text", ["", "abc", "12xb", "5zz", "-4KB"])
def test_parse_bytes_invalid(text):
    assert parse_bytes(text) is None


@pytest.mark.parametrize(
    "text,expected",
    [
        ("0", 0.0),
        ("30", 30.0),
        ("45s", 45.0),
        ("5m", 300.0),
        ("2h", 7200.0),
        ("1d", 86400.0),
        ("500ms", 0.5),
        ("1.5h", 5400.0),
        ("90SEC", 90.0),
    ],
)
def test_parse_duration_seconds(text, expected):
    assert parse_duration_seconds(text) == pytest.approx(expected)


@pytest.mark.parametrize("text", ["", "abc", "10q", "-5s"])
def test_parse_duration_invalid(text):
    assert parse_duration_seconds(text) is None


def test_coerce_bytes_number_and_string():
    assert coerce_bytes(1048576) == 1048576
    assert coerce_bytes("1MB") == 1048576
    with pytest.raises(ValueError):
        coerce_bytes("bad")


def test_coerce_duration_native_unit():
    assert coerce_duration(5000, 1e3) == 5000.0
    assert coerce_duration("5s", 1e3) == 5000.0
    assert coerce_duration("1ms", 1e6) == 1000.0
    with pytest.raises(ValueError):
        coerce_duration("nope", 1e3)
