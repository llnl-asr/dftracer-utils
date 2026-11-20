:description: The DFTUtilsError exception hierarchy that dftracer.utils raises.

Errors
======

Every failure of the native engine raises ``DFTUtilsError`` or one of its
subclasses, so ``except DFTUtilsError`` catches them all. ``DFTUtilsError``
derives from ``RuntimeError``. A task handle re-raises the original typed error
(see :doc:`runtime`).

.. autoexception:: dftracer.utils.DFTUtilsError

.. autoexception:: dftracer.utils.DFTUtilsValueError

.. autoexception:: dftracer.utils.DFTUtilsNotFoundError

.. autoexception:: dftracer.utils.DFTUtilsIOError

.. autoexception:: dftracer.utils.DFTUtilsParseError

.. autoexception:: dftracer.utils.DFTUtilsCompressionError

.. autoexception:: dftracer.utils.DFTUtilsDuqlError

.. autoexception:: dftracer.utils.DFTUtilsReaderError

.. autoexception:: dftracer.utils.DFTUtilsIndexerError

.. autoexception:: dftracer.utils.DFTUtilsPipelineError

.. autoexception:: dftracer.utils.DFTUtilsAggregationError
