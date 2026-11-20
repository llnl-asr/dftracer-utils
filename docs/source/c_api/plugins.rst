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

The shape of these headers is recorded, not only the version. ``cmake/plugin_abi_manifest.txt``
holds one line per struct size and alignment, field offset and size, enumerator
value, integer and string ``DFTU_*`` constant, typedef underlying type and
function pointer spelling (with the parameter names stripped, so a rename
changes nothing), and ctest ``plugin_abi_manifest`` rebuilds it from the headers
and fails on any difference, so a shape change records the new manifest in the
same commit (``cmake --build <build dir> --target plugin_abi_manifest_update``).
CI also diffs that file against the newest release tag of the form
``v``\ *major*\ ``.``\ *minor*\ ``.``\ *patch* that carries one: an added line
passes, and so does a struct that grew while its alignment and its old fields
stayed put, while a removed or changed line fails unless the ``abi`` major in
``VERSION`` moved, because a plugin built against the tag reads a field that is
gone or moved.

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
every record line of a file during an index build, or the typed columns of the
paths it names, and asks it, per filter leaf, which chunks may hold a match. It
also hashes values and columns with the host's own value hash.

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

``dftu.svc.store``: the store of the index root the scan reads, in the
plugin's own namespaces. Every call forwards one function of the store C ABI
(``dftracer/utils/store/c_api.h``) with the same arguments, handles and
meaning, except that an error goes to the callback's ``ctx``. The host owns
the store, so a plugin opens nothing: no open, close, pack, drop, flush,
publish, fail or refresh, and it never sees a directory.

The plugin passes a name of its own, ``<ns>``, and the host registers it as
``plugin.<plugin name>.<ns>``. A plugin therefore reaches only its own
namespaces, and two plugins that choose the same ``<ns>`` keep separate data.
A name that is empty, contains ``..``, starts with ``.``, starts with
``dftu.`` or ``server.``, or holds a byte outside ``[a-z0-9_.-]`` is refused
with ``DFTU_E_INVALID`` and a message that names it.

A commit is not durable by itself. A snapshot of the same process sees the
batch once its bytes reach a published extent, and a reader in another process
sees it after the host flushes its writer at the end of the fold, or earlier
once the batch's extent fills. A snapshot can be up to a second old, so read a
value back in a later fold, not in the call that wrote it. The plugin has no
flush of its own, because the host holds the one writer of the directory; the
host flushes that writer once, from the fold's finalize, whichever fold of the
plugin committed, and a failed flush fails the run. ``commit`` consumes its
batch whether it commits or is refused. Only a durable namespace is synced to disk, so a machine crash
can still lose a committed batch of any other namespace.

The store is the one writer per process of that directory, so a batch needs no
open or close of its own. A scan covers one index root: a unit of a second root
makes every call of this group fail with ``DFTU_E_INVALID`` and a message that
names both roots.

A namespace, batch or snapshot handle belongs to the store directory the host
lent, compared by that directory rather than by the fold's address. Every fold
of one index root lends the same directory, so a handle a slice step made works
in the same plugin's master finalize. A handle of another root, or a stale one
from a fold that ended, is refused with ``DFTU_E_INVALID``, because namespace
ids are sequential per writer and an id alone would reach another store's
namespace. A NULL handle of any of those, and of a cursor, is refused with
``DFTU_E_INVALID`` and a message that names it.

A fold callback's host answers this group from its first unit on, since the
unit's index carries the root. A handle's cursor host and the build host answer
NULL, because neither runs over one trace folder.

.. doxygenfile:: dftracer/utils/plugins/abi/store.h
   :project: dftracer-utils
