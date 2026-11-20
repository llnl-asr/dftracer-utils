# SPDX-License-Identifier: MIT
import dataclasses

import pytest

from dftracer.utils import MemoryBudgetAdvice, memory_budget_advice

GIB = 1024**3


def test_fits_when_peak_is_within_available():
    advice = memory_budget_advice(1024, available_bytes=64 * GIB)
    assert isinstance(advice, MemoryBudgetAdvice)
    assert advice.fits
    assert advice.required_bytes == 1024
    assert advice.peak_bytes >= 1024
    assert advice.available_bytes == 64 * GIB
    assert advice.suggested_nodes == 1
    assert advice.warning == ""


def test_too_big_suggests_enough_nodes_and_warns():
    advice = memory_budget_advice(40 * GIB, available_bytes=8 * GIB)
    assert not advice.fits
    assert advice.peak_bytes > advice.available_bytes
    assert advice.suggested_nodes * advice.available_bytes >= advice.peak_bytes
    assert (advice.suggested_nodes - 1) * advice.available_bytes < advice.peak_bytes
    assert advice.warning


def test_zero_available_detects_this_host():
    assert memory_budget_advice(1).available_bytes > 0


def test_result_is_frozen():
    advice = memory_budget_advice(1, available_bytes=GIB)
    with pytest.raises(dataclasses.FrozenInstanceError):
        advice.fits = False  # type: ignore[misc]


def test_negative_bytes_are_refused():
    with pytest.raises(ValueError, match="must not be negative"):
        memory_budget_advice(-1)
    with pytest.raises(ValueError, match="must not be negative"):
        memory_budget_advice(1, available_bytes=-1)
