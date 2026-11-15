# SPDX-License-Identifier: MIT
"""Collecting a frame through the async handle keeps the column buffers."""

import asyncio

import numpy as np
import pytest

import dftracer.utils as dft


def _addr(frame: "dft.DataFrame", name: str) -> int:
    arr = frame[name].to_numpy()
    return int(arr.__array_interface__["data"][0])


@pytest.fixture
def frame() -> "dft.DataFrame":
    return dft.DataFrame(
        {"x": np.arange(1_000_000, dtype=np.int64), "y": np.arange(1_000_000, dtype=np.float64)}
    )


def test_to_numpy_is_a_view(frame: "dft.DataFrame") -> None:
    assert _addr(frame, "x") == _addr(frame, "x")


def test_collect_keeps_the_buffer(frame: "dft.DataFrame") -> None:
    assert _addr(frame.lazy().collect(), "x") == _addr(frame, "x")


def test_collect_async_get_keeps_the_buffer(frame: "dft.DataFrame") -> None:
    out = frame.lazy().collect_async().get()
    assert _addr(out, "x") == _addr(frame, "x")
    assert _addr(out, "y") == _addr(frame, "y")


def test_collect_async_get_twice_gives_the_same_object(frame: "dft.DataFrame") -> None:
    handle = frame.lazy().collect_async()
    assert handle.get() is handle.get()


def test_await_collect_async_keeps_the_buffer(frame: "dft.DataFrame") -> None:
    async def run() -> "dft.DataFrame":
        return await frame.lazy().collect_async()

    out = asyncio.run(run())
    assert _addr(out, "x") == _addr(frame, "x")


def test_select_through_async_keeps_the_buffer(frame: "dft.DataFrame") -> None:
    out = frame.lazy().select("y").collect_async().get()
    assert _addr(out, "y") == _addr(frame, "y")
