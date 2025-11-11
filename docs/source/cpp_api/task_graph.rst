Task Graph API
==============

DAG-based task graph builder for constructing parallel computation graphs. All classes are in the ``dftracer::utils::task_graph`` namespace.

For usage examples, see :doc:`/pipeline`.

TaskGraph
---------

Builder for constructing task DAGs with fan-in, fan-out, map, and reduce patterns.

.. doxygenclass:: dftracer::utils::task_graph::TaskGraph
   :project: dftracer-utils
   :members:
   :undoc-members:

TaskGroup
---------

Handle to a collection of tasks that produce a specific type.

.. doxygenclass:: dftracer::utils::task_graph::TaskGroup
   :project: dftracer-utils
   :members:
   :undoc-members:

Type Tags
---------

Strong types for specifying operation parameters.

.. doxygenstruct:: dftracer::utils::task_graph::split_every
   :project: dftracer-utils
   :members:

.. doxygenstruct:: dftracer::utils::task_graph::num_outputs
   :project: dftracer-utils
   :members:

.. doxygenstruct:: dftracer::utils::task_graph::num_partitions
   :project: dftracer-utils
   :members:

Factory Functions
-----------------

Standalone functions for creating task patterns.

.. doxygenfunction:: dftracer::utils::task_graph::make_fan_out
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::task_graph::make_fan_in
   :project: dftracer-utils

.. doxygenfunction:: dftracer::utils::task_graph::make_tree_reduce
   :project: dftracer-utils
