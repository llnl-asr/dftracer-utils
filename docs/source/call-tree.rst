Call Tree Utility
=================

The call tree utility builds hierarchical call trees from DFTracer trace files. It analyzes trace files to reconstruct calling relationships between functions, creating a tree structure that represents the execution flow.

.. code-block:: cpp

   #include <dftracer/utils/call_tree/call_tree.h>

Overview
--------

The Call Tree utility is designed to perform the following tasks:

- Parse plain text or gzipped DFTracer trace files (``.pfw``, ``.pfw.gz``) and extract function call information
- Build hierarchical call trees showing parent-child relationships between function calls
- Support distributed processing using MPI for handling large-scale trace datasets
- Serialize call trees in multiple formats (binary, JSON/Chrome Tracing)
- Provide statistical analysis of call patterns and execution timings

.. mermaid::

   graph LR
       Input["Trace Files<br/>(.pfw, .pfw.gz)"] --> Parse["Parse Events"]
       Parse --> Build["Build Call Tree"]
       Build --> Tree["CallTree"]
       Tree --> Stats["Statistics<br/>(CallTreeStats)"]
       Tree --> DFS["Depth-First<br/>Traversal"]
       Tree --> Serial["Serialize"]
       Serial --> Bin["Binary (.calltree)"]
       Serial --> JSON["JSON (Chrome Tracing)"]
       Serial --> Txt["Text"]

Types
-----

.. code-block:: cpp

   // Node information in the call tree
   struct CallTreeNodeInfo {
       std::uint64_t id;
       std::string name;
       std::string category;
       std::uint64_t start_time_us;
       std::uint64_t duration_us;
       int level;
       std::uint64_t parent_id;
       size_t num_children;
       std::vector<std::uint64_t> children_ids;
       std::unordered_map<std::string, std::string> args;
   };

   // Aggregate statistics
   struct CallTreeStats {
       size_t total_nodes;
       size_t num_levels;
       size_t num_leaf_nodes;
       size_t num_processes;
       int max_depth;
       std::vector<double> avg_time_per_level_us;
       std::vector<size_t> nodes_per_level;
   };

CallTree
--------

**Build and inspect a call tree:**

.. code-block:: cpp

   #include <dftracer/utils/call_tree/call_tree.h>

   using namespace dftracer::utils::call_tree;

   CallTree tree;

   // Load trace files from a directory
   tree.load_from_directory("/path/to/traces", "*.pfw.gz");

   // Generate the call tree structure
   tree.generate();

   // Print statistics
   tree.print_statistics();

   // Print tree in depth-first order (limit to 3 levels)
   tree.print_depth_first(3);

**Traverse nodes programmatically:**

.. code-block:: cpp

   // Get all nodes in depth-first order
   auto nodes = tree.get_nodes_depth_first();

   for (const auto& node : nodes) {
       printf("[level=%d] %s (%s) duration=%.3fms\n",
              node.level,
              node.name.c_str(),
              node.category.c_str(),
              static_cast<double>(node.duration_us) / 1000.0);
   }

**Query by process and thread:**

.. code-block:: cpp

   // Get all process IDs in the tree
   auto pids = tree.get_process_ids();

   for (auto pid : pids) {
       // Get thread IDs for this process
       auto tids = tree.get_thread_ids(pid);

       for (auto tid : tids) {
           // Get root nodes for this process/thread
           auto roots = tree.get_root_nodes(pid, tid);
           printf("PID %u, TID %u: %zu root nodes\n",
                  pid, tid, roots.size());
       }
   }

**Look up a specific node:**

.. code-block:: cpp

   auto node = tree.get_node_by_id(42);
   printf("Node %lu: %s, %zu children\n",
          node.id, node.name.c_str(), node.num_children);

   // Access node arguments (pid, tid, fhash, etc.)
   for (const auto& [key, value] : node.args) {
       printf("  %s = %s\n", key.c_str(), value.c_str());
   }

Serialization
-------------

Serialization moved to coroutine-based ``save_binary`` / ``save_arrow``
free functions in ``dftracer/utils/call_tree/mpi/serializable.h``. The
legacy ``CallTree::save_to_file`` / ``save_to_json`` / ``load_from_file``
methods have been removed; the API now exposes
``CallTree::internal_tree()`` for direct access to the underlying
``internal::CallTree`` consumed by the save/load coroutines.

**Save to binary format:**

The custom binary format uses a shared string dictionary (name, category,
arg keys / string values share storage) and preserves typed args
(``int`` / ``uint`` / ``double`` / ``bool`` instead of flattening to
strings).

.. code-block:: cpp

   #include <dftracer/utils/call_tree/call_tree.h>
   #include <dftracer/utils/call_tree/mpi/serializable.h>
   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/tasks/task.h>

   using namespace dftracer::utils;
   using namespace dftracer::utils::call_tree;

   CallTree tree;
   tree.load_from_directory("/path/to/traces", "*.pfw.gz");
   tree.generate();

   auto task = make_task(
       [&tree](CoroScope& scope) -> coro::CoroTask<void> {
           co_await save_binary(&scope, tree.internal_tree(),
                                "output.calltree");
           co_return;
       },
       "save_binary");

   Pipeline pipeline(PipelineConfig().with_name("calltree-save"));
   pipeline.set_source({task});
   pipeline.execute();

**Save to Arrow IPC (.arrow):**

Columnar Arrow IPC with buffer-level zstd compression and
dictionary-encoded ``name`` / ``category`` columns. Readable directly by
``pyarrow``, ``polars``, ``nanoarrow``, and DuckDB. Requires the build to
be configured with ``DFTRACER_UTILS_ENABLE_ARROW_IPC=ON``.

.. code-block:: cpp

   auto task = make_task(
       [&tree](CoroScope& scope) -> coro::CoroTask<void> {
           co_await save_arrow(&scope, tree.internal_tree(),
                               "output.arrow");
           co_return;
       },
       "save_arrow");

**Load a previously saved tree:**

Both loaders are coroutines that return a fresh ``internal::CallTree``:

.. code-block:: cpp

   auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
       auto loaded = co_await load_binary(&scope, "output.calltree");
       // or: auto loaded = co_await load_arrow(&scope, "output.arrow");
       printf("Loaded tree: %zu nodes\n", loaded->num_nodes());
       co_return;
   }, "load");

**Save to text file (still available on the high-level API):**

.. code-block:: cpp

   tree.print_depth_first_to_file("output.txt", 5);  // Max depth 5

Output Formats
--------------

Binary Format (``.calltree``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Compact custom format with a global string dictionary and typed args; best
for round-tripping trees between dftracer-utils runs (for example, between
a coordinator and downstream MPI ranks). Backed by the
``CALLTREE_BINARY_VERSION = 2`` header.

Arrow IPC (``.arrow``)
~~~~~~~~~~~~~~~~~~~~~~

Columnar Arrow IPC file with zstd buffer compression and
dictionary-encoded ``name`` / ``category`` columns. Best for analysis
pipelines that already speak Arrow (pyarrow, polars, DuckDB, nanoarrow).

JSON Format (Chrome Tracing)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``dftracer_call_tree`` CLI emits Chrome Tracing JSON (gzipped with
``--gzip``) suitable for ``chrome://tracing`` and
`Perfetto UI <https://ui.perfetto.dev/>`_. Programmatic JSON export is no
longer exposed on the ``CallTree`` C++ API.

Text Format
~~~~~~~~~~~

Human-readable text with indentation showing hierarchical structure,
function names, categories, and timing at each level.

Performance Considerations
--------------------------

**Index Files**
  The utility creates index files for compressed traces to enable efficient random access. These are cached and reused across runs.

**Checkpoint Size**
  Larger checkpoint sizes reduce index file size but may increase memory usage during processing. Default is 32 MB.

**Thread Count**
  Each MPI rank can use multiple threads for parallel processing within the rank. Balance thread count with available cores.

**PID Distribution**
  PIDs are distributed across MPI ranks. More ranks enable processing more PIDs in parallel, but increase communication overhead during the gather phase.

See Also
--------

- :doc:`/cpp_api/index` - Full C++ API documentation
- :doc:`/cli` - Command-line tools (``dftracer_call_tree``)
- :doc:`/utilities/replay` - Replay utility with call tree integration
