:description: Reference for the header-only C++ plugin SDK that wraps the plugin C ABI: Host services and typed helpers over each extension group.

Plugins
=======

.. seealso::

   :doc:`../plugins` for the plugin guide (Fold lifecycle, memory model,
   building, and running), :doc:`../jit` to author a plugin in Python, and
   :doc:`../c_api/plugins` for the stable C ABI a plugin fills.

The header-only C++ SDK in ``dftracer::utils::plugins`` wraps the plugin C ABI
for writing a plugin in C++. Host services are reached through ``Host`` and the
typed helpers over each extension group: ``Io``, ``Agg`` (with the ``AggCol``
builder and the ``agg::`` per-op factories), ``Writer``, ``Event``, ``Batch``,
``Config``, ``Sketch``, ``OutPort`` / ``InPort``, ``Task``, and ``AsyncOp``. A
plugin is registered with ``make_plugin``.

.. code-block:: cpp

   #include <dftracer/utils/plugins/plugin.h>

   using namespace dftracer::utils::plugins;

Type relationships
------------------

Composition among the plugin SDK types:

.. mermaid:: /_generated/plugins.mmd

.. include:: /cpp_api/_generated/plugins.rst.inc
.. include:: /cpp_api/_generated/plugins.reflect.rst.inc
.. include:: /cpp_api/_generated/plugins.scalar.rst.inc
