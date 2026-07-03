"""Tests for the logging control bindings."""

import pytest

import dftracer.utils as du


@pytest.fixture(autouse=True)
def _restore_level():
    """Keep global logger state from leaking between tests."""
    yield
    du.set_log_level("info")
    du.set_log_color("auto")


def test_set_get_log_level_roundtrip():
    for level in ["trace", "debug", "info", "warn", "error", "off"]:
        du.set_log_level(level)
        assert du.get_log_level() == level


def test_warning_alias_normalizes_to_warn():
    du.set_log_level("warning")
    assert du.get_log_level() == "warn"


def test_set_log_level_invalid_raises():
    with pytest.raises(ValueError):
        du.set_log_level("bogus")


def test_set_log_color_valid():
    for mode in ["auto", "always", "never"]:
        du.set_log_color(mode)


def test_set_log_color_invalid_raises():
    with pytest.raises(ValueError):
        du.set_log_color("rainbow")
