:description: Make a plugin project, build it and prove it with golden tests.

Write and test a plugin
=======================

``dftracer_plugin`` makes a plugin project and checks a built one. Use it to
start from a project that builds, and to prove a plugin on every machine that
runs it.

Make a project
--------------

.. code-block:: bash

   dftracer_plugin new udf my_udfs --dir ~/src

The kinds are ``fold``, ``udf``, ``aggregate``, ``format``, ``sink`` and
``node``. The name must be a C identifier. The command refuses a folder that
already exists. It writes ``CMakeLists.txt``, ``plugin.cpp``, ``README.md`` and
``tests/basic.test``. The project finds an installed dftracer-utils and needs
no file from this source tree.

.. code-block:: bash

   cmake -S my_udfs -B my_udfs/build -DCMAKE_PREFIX_PATH=<install prefix>
   cmake --build my_udfs/build

Check a built plugin
--------------------

.. code-block:: bash

   dftracer_plugin check my_udfs/build/my_udfs.so

``my_udfs/build/my_udfs.so`` is the module the project builds: ``CMakeLists.txt``
sets ``PREFIX ""`` and keeps the default module suffix.

The check runs in a child process, so a plugin that crashes or leaks cannot
stop the tool. The child loads the plugin, prints its name, version, ABI
version and every kind list, runs the golden tests, and unloads the plugin
with the memory debug mode and the exit check on. The parent prints each
golden test result, the child's leaks, or the signal that killed it. The exit
code is 0 only when everything passed.

Golden tests
------------

A ``*.test`` file is plain text, in the style of sqllogictest:

.. code-block:: text

   # a comment
   input data/sample.pfw.gz
   query
   select name, dur
   ----
   name	dur
   open	300

``input`` names a trace file, a trace folder or a file of a format the plugin
claims, relative to the ``.test`` file. It applies to every ``query`` block
after it, so the first block needs one and a block before any ``input`` line
fails with ``no input``. The query text is duql, run the way
``dftracer_view --duql`` runs it. The expected block holds one tab-separated
row per line, with the column names first, and a blank line ends it. An empty
expected block expects no rows. A row keeps its spaces and its empty cells as
written: only the line ending is dropped.

A plugin that carries a fold also runs that fold over every trace input, so the
golden test of a fold plugin proves that the fold ran.

How a value prints
------------------

A column header and every cell go in the file as they read, with a tab between
cells. A double follows the same rule as the engine's JSON output: a whole
value prints as an integer with no dot (``2``, ``91``), and any other value in
the shortest form that reads back as the same double (``0.1234567``, ``91.5``,
``55.74255561017869``). There is no fixed number of decimals, so no value is
rounded away. Write the value you expect as the engine prints it, and let
``--record`` fill it in the first time. A string cell keeps its own text, with a
tab, a newline, a carriage return and a backslash escaped as ``\t``, ``\n``,
``\r`` and ``\\``, so a row stays on one line. A null prints as ``null``.

``query rowsort`` sorts the rows before the compare. ``query error <text>``
passes when the query fails with an error whose message holds ``text``.

`check` runs every ``*.test`` file under ``--tests DIR``, and none without it:

.. code-block:: bash

   dftracer_plugin check my_udfs/build/my_udfs.so --tests my_udfs/tests

Rewrite the expected output
---------------------------

.. code-block:: bash

   dftracer_plugin check my_udfs/build/my_udfs.so --tests my_udfs/tests --record

``--record`` writes what each query gave back into the ``.test`` file and
keeps the comments, the ``input`` lines, the queries and anything after the
last block. A block whose query failed keeps its expected output and still
fails the run, so a recorded run exits 0 only when every block ran. Read the
diff of the file before you commit it: a recorded run that exits 0 always
passes, so it proves nothing until you look at what changed.

Names
-----

A plugin may register a format or a sink under its own bare name, such as
``toy`` from plugin ``toy``, or under ``<plugin>.<name>``. Any other name is
refused at load. A name that belongs to another plugin or to ``dftu.`` is
refused too.
