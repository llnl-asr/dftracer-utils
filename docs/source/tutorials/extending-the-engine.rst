:description: Lesson 3: write a JIT plugin in Python with plain state, a batch hook, a config, a finalize task and a channel, then a UDF for duql.

Extending the engine
====================

You write one plugin in Python, in six steps, and each step adds one idea.
Every step runs a file from ``examples/jit/`` and prints the output shown here.
Plan on 20 minutes of reading.
The first run of a plugin compiles it into a cache, which is ``DFTRACER_UTILS_JIT_CACHE`` or ``~/.cache/dftracer-utils/jit``.
With an empty cache, all six steps took about 5 seconds in total on our machine, and a second pass about 2.

This lesson assumes :doc:`first-analysis` and :doc:`analysis-in-depth`.
The C++ and C route is :doc:`write-a-c-plugin`.
The rules for every form below are in :doc:`../jit`.

Before you start
----------------

You need a C++ compiler on your ``PATH``, and pandas and pyarrow in your Python.
Run every command from the root of a checkout of the repository, because the paths start with ``examples/jit/``.
The engine writes INFO lines to stderr while it indexes the trace.
They are not part of the output shown below, and the compile step prints nothing.

Write the trace that all six steps read.
It has 300 events on two processes.

.. literalinclude:: ../../../examples/jit/make_trace.py
   :language: python

.. code-block:: bash

   python examples/jit/make_trace.py trace

.. code-block:: bash

   ls trace

.. code-block:: text

   trace.pfw.gz

1. Count with plain state
-------------------------

A plugin is a class that sees each event once.
Its state is an annotated field, and the update code tells the host how to merge the copies of the workers.
Here a ``dict`` counts the slow events of each process, behind a condition.

.. literalinclude:: ../../../examples/jit/step1_per_event.py
   :language: python

.. code-block:: bash

   python examples/jit/step1_per_event.py trace

.. literalinclude:: ../../../examples/jit/expected/step1_per_event.txt
   :language: text

A field comes back under its name as a frame with the key in ``k0``.
The last line shows which lowering the compiler picked.
The rules for ``mask`` and ``branch`` are in :ref:`jit-conditions`.

2. Fold a whole batch
---------------------

A per-event body runs once for each event.
A batch hook runs once for each batch of columns, and the engine fuses its expressions.
The closed vocabulary and the matching reducer and target pairs are in :ref:`jit-batch-hooks`.

.. literalinclude:: ../../../examples/jit/step2_batch.py
   :language: python

.. code-block:: bash

   python examples/jit/step2_batch.py trace

.. literalinclude:: ../../../examples/jit/expected/step2_batch.txt
   :language: text

The slow count is 114, the same share of events as in step 1 (76 plus 38).

3. Take the threshold from a config
-----------------------------------

A dataclass holds the settings that change between runs.
The body reads them as ``self.config.<field>``, and ``Plugins`` checks the values before it loads the plugin.

.. literalinclude:: ../../../examples/jit/step3_config.py
   :language: python

.. code-block:: bash

   python examples/jit/step3_config.py trace

.. literalinclude:: ../../../examples/jit/expected/step3_config.txt
   :language: text

The two ``Plugins`` objects hold their own config, and the last line shows a wrong type refused before any scan.

4. Read a file when the scan ends
---------------------------------

A ``@jit.finalize`` body runs once, after the workers merge.
It can open a file, parse each line as JSON and emit a named result.
The rules are in :ref:`jit-files`, :ref:`jit-values` and :ref:`jit-tasks`.

.. literalinclude:: ../../../examples/jit/step4_finalize.py
   :language: python

.. code-block:: bash

   python examples/jit/step4_finalize.py trace

.. literalinclude:: ../../../examples/jit/expected/step4_finalize.txt
   :language: text

The limit is 100 plus 250.
An emitted result is ``bytes`` and a field is a frame, as :ref:`jit-tasks` says.

5. Send values to a second plugin
---------------------------------

A channel carries values from one plugin to another.
The sender stores values during the scan, and the receiver drains them in its finalize task.
The ordering and credit rules are in :ref:`jit-channels`.

.. literalinclude:: ../../../examples/jit/step5_channel.py
   :language: python

.. code-block:: bash

   python examples/jit/step5_channel.py trace

.. literalinclude:: ../../../examples/jit/expected/step5_channel.txt
   :language: text

Four durations in each run of 50 events are above 55, so six runs give 24 values.
The channel has 512 credits, more than the 24 values that it carries.

6. Call a UDF from duql
-----------------------

``@dft.udf`` compiles a typed function to native code and registers its name for expressions and duql.
A scope removes the names when it ends.

.. literalinclude:: ../../../examples/jit/step6_udf.py
   :language: python

.. code-block:: bash

   python examples/jit/step6_udf.py trace

.. literalinclude:: ../../../examples/jit/expected/step6_udf.txt
   :language: text

Process 2 costs twice its duration, so its 68 rows above 50 are more than the 36 of process 1.

What you built
--------------

You wrote a counting plugin, a batch hook, a configured plugin, a finalize task, a channel pair and a UDF.
:doc:`../jit` is the reference for each of them, and :doc:`../guides/plugins/python-udfs` covers the per-batch UDF form.
:doc:`../guides/plugins/inter-plugin-comms` covers ports between plugins, and :doc:`../plugins` covers the ABI under all of it.
