:description: Reference for the jit module: decorators and types to author a DFTracer plugin in Python and compile it to a native plugin.

JIT Module
==========

.. seealso::

   :doc:`../jit` for the JIT guide (the ``@jit.plugin`` / ``jit.map`` /
   ``@jit.each_event`` model and the supported subset).

Author a DFTracer plugin in Python: a class decorated with ``@jit.plugin`` is
compiled to a native plugin against the stable plugin ABI. This module exposes
the decorator, the typed-map declaration, the field types, and the aggregate
value constructors.

.. automodule:: dftracer.utils.jit
   :members:
   :undoc-members:
   :show-inheritance:
