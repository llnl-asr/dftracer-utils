# SPDX-License-Identifier: MIT
"""The exceptions the package raises: ``DFTUtilsError`` and its ten subclasses.

The native module defines them; this module is the one place the rest of the
package, and ``dftracer.utils``, take them from.
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
    "DFTUtilsValueError",
]

_set_home(__name__, globals(), __all__)
