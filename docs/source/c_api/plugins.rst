:description: The stable plugin C ABI: fill a dftu_plugin struct and reach host services through dftu_plugin_host and its dftu_svc_* extension groups.

Plugins
=======

.. seealso::

   :doc:`../plugins` for the plugin guide, :doc:`../jit` to author a plugin in
   Python, and :doc:`../cpp_api/plugins` for the C++ SDK that wraps this ABI.

The stable plugin C ABI. A plugin includes only ``abi.h`` (an umbrella over
one header per service group under ``abi/``) and fills a ``dftu_plugin``
struct; host services are reached through ``dftu_plugin_host::get_service``
by string id (for example ``dftu.svc.agg``), each answering a
``dftu_svc_*`` struct or NULL. Every table starts with ``struct_size``.

Every callback takes the host-owned ``dftu_ctx*`` first and returns a
``dftu_status``: negative is an error, which the callback reports with
``dftu_fail(ctx, code, message)``, and a long loop polls ``dftu_check(ctx)``
and returns what it gives. Every host service call takes the caller's ``ctx``
first and returns a ``dftu_status`` with results in out-parameters. The host
copies the message, and it names the plugin and the callback in the error the
caller sees.

A plugin library exports two symbols. ``dftracer_plugin_info`` returns a
``dftu_plugin_info`` with the ABI version the plugin was built against; define
it with ``DFTU_PLUGIN_INFO(name, version)``. The loader reads it before it runs
any plugin code and refuses a plugin that has none, whose ABI major differs from
the host's, or whose ABI minor is newer than the host's. ``dftracer_plugin`` is
the factory.

Every record a plugin hands the host starts with ``struct_size``. The host
accepts a size from the size of that record before its trailing optional
callbacks up to the host's own ``sizeof``, copies that many bytes into a zeroed
record of its own and reads only that copy: a callback the plugin's headers did
not have reads NULL, and the host skips it. A smaller or a larger size is
refused at the call that hands the record over, with a message that names the
record.

.. code-block:: c

   #include <dftracer/utils/plugins/abi.h>

Core: the descriptor, the host and the values
----------------------------------------------

``dftu_plugin`` is the fold a plugin fills (``make_slice`` / ``on_batch`` or
``transform`` / ``merge`` / ``on_finalize``, plus the optional ``reads``,
``plan_duql``, ``provides`` / ``consumes``, ``config_keys``);
``dftu_plugin_host`` is what the host lends back;
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
register the plugin's own (``<plugin>.<name>``), and register a scalar UDF
that duql calls by name or a user-defined aggregate.

.. doxygenfile:: dftracer/utils/plugins/abi/ops.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/dataframe/c_api_udf.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/dataframe/c_api_udaf.h
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

``dftu.svc.result``: emit a named result (bytes, a frame or a lazy plan) the
caller reads back after the scan.

.. doxygenfile:: dftracer/utils/plugins/abi/result.h
   :project: dftracer-utils

``dftu.svc.ports``: per-batch producer to consumer channels between plugins.

.. doxygenfile:: dftracer/utils/plugins/abi/ports.h
   :project: dftracer-utils

``dftu.svc.task``: async work. A job is a typed recipe (an input value in, an
output value out) that can be piped, fanned out and raced; a task is one run of
a job. ``spawn`` runs a function on a pool worker, and ``drive`` runs a step in
rounds for a fan-in whose count is known only at run time.

.. doxygenfile:: dftracer/utils/plugins/abi/task.h
   :project: dftracer-utils

``dftu.svc.io``, ``dftu.svc.fileio``, ``dftu.svc.trace``:
files, the file readers and writers (line and chunk readers over plain and gzip
files, the gzip line writer, the parallel writer, whole-file gzip and the file
helpers) and trace files.

.. doxygenfile:: dftracer/utils/plugins/abi/io.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/fileio.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/trace.h
   :project: dftracer-utils

``dftu.svc.fs``: the runtime file system layer. Whole-file and positioned
reads and writes, directory listing and search, existence, creation and removal,
and batched stat.

.. doxygenfile:: dftracer/utils/plugins/abi/fs.h
   :project: dftracer-utils

``dftu.svc.index``: register an index extension. The host hands its builder
every record line of a file during an index build and asks it, per filter
leaf, which chunks may hold a match.

.. doxygenfile:: dftracer/utils/plugins/abi/index.h
   :project: dftracer-utils

``dftu.svc.duql`` and ``dftu.svc.sketch``: duql filters against events, and
quantile sketches.

.. doxygenfile:: dftracer/utils/plugins/abi/duql.h
   :project: dftracer-utils

.. doxygenfile:: dftracer/utils/plugins/abi/sketch.h
   :project: dftracer-utils

The plan node and source vtables (``dftu_node_vt``, ``dftu_source_vt``,
``dftu_cursor_vt``) are documented in ``abi/nodes.h`` and ``abi/providers.h``
above. The dataframe ABI keeps only the calls that take a name:
``dftu_lazyframe_from_provider``, ``dftu_lazyframe_op``,
``dftu_provider_unregister`` and ``dftu_node_unregister``: see :doc:`dataframe`.

Numeric primitives
------------------

Header-only ``dftu_*`` helpers for plugin and raw JIT bodies: bit counting, log2
bucketing, power-of-two rounding, integer sqrt/gcd, alignment, and fast integer
hashing. The JIT layer lowers ``jit.ilog2`` and friends to these.

.. doxygenfile:: dftracer/utils/plugins/prims.h
   :project: dftracer-utils

``dftu.svc.json`` and ``dftu.svc.yaml``: JSON and YAML. Both ids serve the same
table type. A document parses into a ``dftu_value`` tree with line and column
errors, and a streaming writer builds a document in memory.

.. doxygenfile:: dftracer/utils/plugins/abi/format.h
   :project: dftracer-utils

``dftu.svc.memory``: the host's memory manager. A plugin has one pool, named
after it, for its blocks and its reservations. A reservation grows or fails
cleanly and may carry a reclaimer that the host calls when a grow needs room.

.. doxygenfile:: dftracer/utils/plugins/abi/memory.h
   :project: dftracer-utils
