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

**Save to binary format:**

.. code-block:: cpp

   // Save to default path (based on input directory)
   tree.save_to_file();

   // Save to custom path
   tree.save_to_file("output.calltree");

**Save to JSON (Chrome Tracing / Perfetto):**

.. code-block:: cpp

   // Compatible with chrome://tracing and Perfetto UI
   tree.save_to_json("output.pfw");

**Save to text file:**

.. code-block:: cpp

   tree.print_depth_first_to_file("output.txt", 5);  // Max depth 5

**Load from previously saved file:**

.. code-block:: cpp

   CallTree loaded_tree;
   loaded_tree.load_from_file("output.calltree");

   auto stats = loaded_tree.get_statistics();
   printf("Loaded tree: %zu nodes, %zu levels\n",
          stats.total_nodes, stats.num_levels);

Output Formats
--------------

Binary Format
~~~~~~~~~~~~~

Efficient binary serialization preserving all call tree information including node hierarchy, timing, function names, categories, and arguments.

JSON Format (Chrome Tracing)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Follows the Chrome Tracing format specification, viewable in ``chrome://tracing`` or `Perfetto UI <https://ui.perfetto.dev/>`_. Shows timeline of function calls with nested relationships and duration.

Text Format
~~~~~~~~~~~

Human-readable text with indentation showing hierarchical structure, function names, categories, and timing at each level.

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
