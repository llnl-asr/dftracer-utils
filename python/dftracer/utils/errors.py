# SPDX-License-Identifier: MIT
"""The exceptions the package raises: ``DFTUtilsError`` and its subclasses.

The native module defines all but ``DFTUtilsTranspileError``; this module is the one
place the rest of the package, and ``dftracer.utils``, take them from.
"""

from ._public import set_home as _set_home
from .dftracer_utils_ext import (
    DFTUtilsAggregationError,
    DFTUtilsCompressionError,
    DFTUtilsDuqlError,
    DFTUtilsError,
    DFTUtilsIndexerError,
    DFTUtilsIOError,
    DFTUtilsNotFoundError,
    DFTUtilsParseError,
    DFTUtilsPipelineError,
    DFTUtilsReaderError,
    DFTUtilsValueError,
)


class DFTUtilsTranspileError(DFTUtilsValueError):
    """An expression or function uses a form with no engine lowering; the
    message names it. ``DataFrame.eval`` and ``query`` raise it, and ``apply``
    catches it to run the function in Python."""


__all__ = [
    "DFTUtilsAggregationError",
    "DFTUtilsCompressionError",
    "DFTUtilsDuqlError",
    "DFTUtilsError",
    "DFTUtilsIndexerError",
    "DFTUtilsIOError",
    "DFTUtilsNotFoundError",
    "DFTUtilsParseError",
    "DFTUtilsPipelineError",
    "DFTUtilsReaderError",
    "DFTUtilsTranspileError",
    "DFTUtilsValueError",
]

_set_home(__name__, globals(), __all__)
