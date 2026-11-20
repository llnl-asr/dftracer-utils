:description: Add your own analysis that rides the fused scan, from C, C++ or Python.

Extend the engine
=================

Add your own analysis that rides the fused scan, from C, C++ or Python.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Write a plugin
      :link: ../plugins
      :link-type: doc

      Compile a plugin with the C++ SDK or the raw C ABI.

   .. grid-item-card:: JIT plugins in Python
      :link: ../jit
      :link-type: doc

      Write a plugin as a Python class that compiles to native code.

   .. grid-item-card:: Author a job
      :link: plugins/compose-ops
      :link-type: doc

      Write a small async transform and pipe it, fan it out or race it.

   .. grid-item-card:: Share data between plugins
      :link: plugins/inter-plugin-comms
      :link-type: doc

      Publish and consume batch-scoped ports in one scan.

   .. grid-item-card:: Write and test a plugin
      :link: plugins/write-and-test-a-plugin
      :link-type: doc

      Scaffold a project, then prove it with golden tests.

   .. grid-item-card:: Write a UDF in Python
      :link: plugins/python-udfs
      :link-type: doc

      Register a Python function as a scalar UDF, compiled or per batch.

   .. grid-item-card:: Write a sink plugin
      :link: plugins/write-a-sink
      :link-type: doc

      Take the rows of a query and write them where you want them.

   .. grid-item-card:: Use the library from C
      :link: core/c-abi
      :link-type: doc

      Parse queries and operate on columns through the C ABI.

.. toctree::
   :hidden:

   ../plugins
   ../jit
   plugins/compose-ops
   plugins/inter-plugin-comms
   plugins/write-a-sink
   plugins/write-and-test-a-plugin
   plugins/python-udfs
   core/c-abi
