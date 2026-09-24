:description: Reference for dftracer.utils.schemas: record schema classes, field options, registration, loading, detection and explain.

Schemas Module
==============

``dftracer.utils.schemas`` declares and registers record schemas, the record
formats the index and :class:`~dftracer.utils.TraceViewer` read. The spec
keys and field settings are listed in :doc:`../reference/record-schema`.

Schema classes
--------------

.. autoclass:: dftracer.utils.schemas.RecordSchema

.. autofunction:: dftracer.utils.schemas.field

.. autodata:: dftracer.utils.schemas.Json
   :annotation:

.. autoclass:: dftracer.utils.schemas.DFTracer

.. autoclass:: dftracer.utils.schemas.Generic

Registry
--------

.. autofunction:: dftracer.utils.schemas.register

.. autofunction:: dftracer.utils.schemas.load

.. autofunction:: dftracer.utils.schemas.list

.. autofunction:: dftracer.utils.schemas.detect

.. autofunction:: dftracer.utils.schemas.explain

.. autofunction:: dftracer.utils.schemas.schema_id
