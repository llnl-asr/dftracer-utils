:description: The DFTUtilsError exception hierarchy that dftracer.utils raises.

Errors
======

Every failure of the native engine raises ``DFTUtilsError`` or one of its
subclasses, so ``except DFTUtilsError`` catches them all. ``DFTUtilsError``
derives from ``RuntimeError``. A task handle re-raises the original typed error
(see :doc:`runtime`).

.. autoexception:: dftracer.utils.errors.DFTUtilsError

.. autoexception:: dftracer.utils.errors.DFTUtilsValueError

.. autoexception:: dftracer.utils.errors.DFTUtilsTranspileError

.. autoexception:: dftracer.utils.errors.DFTUtilsNotFoundError

.. autoexception:: dftracer.utils.errors.DFTUtilsIOError

.. autoexception:: dftracer.utils.errors.DFTUtilsParseError

.. autoexception:: dftracer.utils.errors.DFTUtilsCompressionError

.. autoexception:: dftracer.utils.errors.DFTUtilsDuqlError

.. autoexception:: dftracer.utils.errors.DFTUtilsReaderError

.. autoexception:: dftracer.utils.errors.DFTUtilsIndexerError

.. autoexception:: dftracer.utils.errors.DFTUtilsPipelineError

.. autoexception:: dftracer.utils.errors.DFTUtilsAggregationError
