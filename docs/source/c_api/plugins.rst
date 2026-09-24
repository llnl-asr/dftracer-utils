:description: The stable plugin C ABI: fill a dftu_plugin struct and reach host services through dftu_plugin_host and its dftu_svc_* extension groups.

Plugins
=======

.. seealso::

   :doc:`../plugins` for the plugin guide, :doc:`../jit` to author a plugin in
   Python, and :doc:`../cpp_api/plugins` for the C++ SDK that wraps this ABI.

The stable plugin C ABI. A plugin includes only ``abi.h`` (an umbrella over
one header per service group under ``abi/``) and fills a ``dftu_plugin``
struct; host services are reached through ``dftu_plugin_host::get_service``
by string id, each answering a versioned ``dftu_svc_*`` struct or NULL.
``abi_version`` is a hash of these headers, computed at build time: a plugin
built against different headers is refused at load.

.. code-block:: c

   #include <dftracer/utils/plugins/abi.h>

Core: the descriptor, the host and the values
----------------------------------------------

``dftu_plugin`` is the fold a plugin fills (``make_slice`` / ``on_batch`` or
``transform`` / ``merge`` / ``on_finalize``, plus the optional ``reads``,
``plan_query``, ``provides`` / ``consumes``, ``config_keys``, ``bytes`` /
``reclaim``); ``dftu_plugin_host`` is what the host lends back;
``dftu_value`` is the config tree.

.. doxygenfile:: dftracer/utils/plugins/abi/core.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/plugin.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/value.h
   :project: dftracer-utils

Service groups
--------------

One header per ``DFTU_SVC_*`` id. Every slot is optional to the plugin; a
missing group comes back NULL from ``get_service``.

``dftu.svc.ops``: run any registered engine op on handles the plugin holds,
and register the plugin's own (``<plugin>.<name>``).

.. doxygenfile:: dftracer/utils/plugins/abi/ops.h
   :project: dftracer-utils

``dftu.svc.agg``: host-owned mergeable accumulators, and ``register_state``
for a plugin's own mergeable state type (spilled by the host through its
``serialize`` pair).

.. doxygenfile:: dftracer/utils/plugins/abi/agg.h
   :project: dftracer-utils

``dftu.svc.providers``: register the plugin as a named source a
``LazyFrame`` can scan.

.. doxygenfile:: dftracer/utils/plugins/abi/providers.h
   :project: dftracer-utils

``dftu.svc.nodes``: register a named plan node that ``dftu_lazyframe_op``
stacks on a plan; the host unregisters it before the plugin unloads.

.. doxygenfile:: dftracer/utils/plugins/abi/nodes.h
   :project: dftracer-utils

``dftu.svc.result``: emit a named result (bytes, an Arrow array, a frame or a
lazy plan) the caller reads back after the scan.

.. doxygenfile:: dftracer/utils/plugins/abi/result.h
   :project: dftracer-utils

``dftu.svc.ports``: per-batch producer to consumer channels between plugins.

.. doxygenfile:: dftracer/utils/plugins/abi/ports.h
   :project: dftracer-utils

``dftu.svc.coro`` and ``dftu.svc.compose``: tasks the host drives, and
value-typed async ops composed as handles.

.. doxygenfile:: dftracer/utils/plugins/abi/coro.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/compose.h
   :project: dftracer-utils

``dftu.svc.io``, ``dftu.svc.writer``, ``dftu.svc.trace``, ``dftu.svc.arrow``:
files, output writers, trace files and Arrow IPC.

.. doxygenfile:: dftracer/utils/plugins/abi/io.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/writer.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/trace.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/arrow.h
   :project: dftracer-utils

``dftu.svc.query`` and ``dftu.svc.sketch``: the query DSL against events, and
quantile sketches.

.. doxygenfile:: dftracer/utils/plugins/abi/query.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/sketch.h
   :project: dftracer-utils

The plan node and source registries (``dftu_node_register``,
``dftu_provider_register``, ``dftu_cursor_vt``, ``dftu_source_vt``) live in
the dataframe ABI, since a non-plugin C caller uses them too: see
:doc:`dataframe`.

Arrow ABI
---------

The Arrow extension group: build Arrow record batches from a plugin over the
Arrow C Data Interface.

.. doxygenfile:: dftracer/utils/plugins/arrow_abi.h
   :project: dftracer-utils

Numeric primitives
------------------

Header-only ``dftu_*`` helpers for plugin and raw JIT bodies: bit counting, log2
bucketing, power-of-two rounding, integer sqrt/gcd, alignment, and fast integer
hashing. The JIT layer lowers ``jit.ilog2`` and friends to these.

.. doxygenfile:: dftracer/utils/plugins/prims.h
   :project: dftracer-utils
