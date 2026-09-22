:description: Use the stable C ABI: parse queries, build and operate on columns, and evaluate a query as a mask, following the library's ownership rules.

Use the library from C
=======================

dftracer-utils ships a stable C ABI alongside the C++ API: opaque handles,
``extern "C"`` functions, and plain C types. It is not a thin wrapper bolted on
for interop - it is the same engine the C++ and Python surfaces call into, so a
C program gets the same SIMD kernels and query engine with no C++ in its own
translation unit.

Two headers cover the pieces most programs need:

- ``dftracer/utils/query/abi.h`` - parse and build query predicates.
- ``dftracer/utils/dataframe/abi.h`` - build, inspect, and operate on columns
  (``dftu_series``) and frames (``dftu_dataframe``), including evaluating a
  query as a mask.

A third header, ``dftracer/utils/plugins/abi.h``, is the C ABI for writing a
plugin that rides the engine's fused scan (a ``dftu_plugin_host`` vtable, interned
strings, typed extension tables); see :doc:`../../plugins` if you are extending
the engine rather than consuming its output.

Every function returns an owned handle or ``NULL`` on failure; free what you
own. There is no partial ownership: a "consumes" argument is freed for you
even on error, and a "borrows" argument is never freed by the callee.

Parse a query
-------------

``dftu_query_parse`` compiles the same DSL string the Python and C++ builders
produce (see :doc:`query-dsl`) into an opaque ``dftu_query*``. You can also
build one from typed pieces without a string, which avoids escaping and
sidesteps a parse error at runtime:

.. code-block:: c

   #include <dftracer/utils/query/abi.h>

   /* From DSL text: */
   dftu_query* q1 = dftu_query_parse("cat == \"POSIX\" and dur > 1000");

   /* Or from typed builders (structurally equivalent): */
   dftu_query* a = dftu_query_cmp_str("cat", DFTU_QCMP_EQ, "POSIX");
   dftu_query* b = dftu_query_cmp_i64("dur", DFTU_QCMP_GT, 1000);
   dftu_query* q2 = dftu_query_and(a, b);   /* consumes a and b */

   dftu_query_free(q1);
   dftu_query_free(q2);

``dftu_query_and``/``dftu_query_or``/``dftu_query_not`` **consume** their
argument handles: do not free or reuse ``a``/``b`` after passing them in, and
a ``NULL`` argument frees the other side and yields ``NULL``. The comparison
ops are ``DFTU_QCMP_EQ``/``NE``/``GT``/``LT``/``GE``/``LE``; pattern matching
goes through ``dftu_query_match`` with ``DFTU_QMATCH_LIKE``/``ILIKE``/``REGEX``/
``IREGEX``/``ICONTAINS``. ``dftu_query_in_i64``/``dftu_query_in_str`` (and their
``not_in`` counterparts) build a membership test over an array of values.
``dftu_query_to_string`` serializes a built query back to the canonical DSL
string (free the result with ``dftu_query_string_free``).

Build columns
-------------

A ``dftu_series`` is one typed, columnar array. Build one by copying data in,
or borrow an existing buffer to avoid the copy:

.. code-block:: c

   #include <dftracer/utils/dataframe/abi.h>

   /* Element types are the named dftu_dtype enum (DFTU_TYPE_BOOL ...
      DFTU_TYPE_BINARY), whose values mirror dataframe::TypeId's C++ enum
      ordinals. No hand-defined macros are needed. */

   int64_t durs[4] = {100, 250, 50, 900};
   dftu_series* dur = dftu_series_new_flat(DFTU_TYPE_INT64, durs, 4,
                                          /* validity = */ NULL);

   int32_t offsets[3] = {0, 4, 9};
   dftu_series* cat = dftu_series_new_string(DFTU_TYPE_STRING, offsets,
                                            "readwrite", 2, NULL);

``dftu_series_new_flat_borrowed`` shares a caller-owned buffer zero-copy
instead of copying, releasing it through a callback when the column's last
reference drops. The element type is the named ``dftu_dtype`` enum, whose values
match the ``dataframe::TypeId`` C++ enum ordinals; the encoding code returned by
``dftu_series_encoding`` mirrors ``dataframe::Encoding``.

Evaluate a mask
---------------

``dftu_dataframe_mask`` evaluates a compiled query as a bit-packed boolean mask
over a batch of named columns, the same columnar path the engine uses during a
scan:

.. code-block:: c

   const dftu_series* columns[2] = {cat, dur};
   const char* col_names[2] = {"cat", "dur"};

   dftu_series* mask = dftu_dataframe_mask(q2, columns, col_names, 2);
   if (mask == NULL) {
       /* No columnar lowering for this predicate (e.g. a pattern the
          columnar evaluator does not cover, or a referenced field missing
          from the batch) - fall back to the scan-time evaluator. */
   } else {
       dftu_series* hits = dftu_series_filter(dur, mask);
       /* ... read hits ... */
       dftu_series_free(hits);
       dftu_series_free(mask);
   }

For a scalar operand in a comparison or arithmetic call (``dftu_series_compare``,
``dftu_series_add_scalar``, ...), tag it with ``dftu_scalar_tag`` so the kernel
converts it to the column's element type. From C, the ``DFTU_SCALAR_I64``/
``DFTU_SCALAR_U64``/``DFTU_SCALAR_F64`` macros build the tagged value in one
expression:

.. code-block:: c

   dftu_series* over_100 =
       dftu_series_compare(dur, DFTU_CMP_GT, DFTU_SCALAR_I64(100));

Build a frame
-------------

``dftu_dataframe`` is the named-columns counterpart to ``dftu_series``.
``dftu_dataframe_new`` takes ownership of the column handles you pass it (do
not free them afterward); every frame op below returns a new owned frame:

.. code-block:: c

   dftu_series* cols[2] = {cat, dur};
   const char* col_names2[2] = {"cat", "dur"};
   dftu_dataframe* df = dftu_dataframe_new(col_names2, cols, 2);

   dftu_dataframe* filtered = dftu_dataframe_filter(df, mask);
   dftu_series* dur_col = dftu_dataframe_column(filtered, "dur");

   dftu_series_free(dur_col);
   dftu_dataframe_free(filtered);
   dftu_dataframe_free(df);

Run any op by name
------------------

Every ``Series`` / ``DataFrame`` / ``LazyFrame`` method is a registered op
with a name and a packed signature, so a C caller is not limited to the
``dftu_series_*`` entry points: ``dftu_op_find(name)`` looks one up
(built-in or a plugin's) and ``dftu_op_run`` / ``dftu_op_run_frame`` /
``dftu_op_run_aggregate`` / ``dftu_op_run_lazy`` run it on handles, the
other operands in a ``dftu_op_arg`` bag. ``dftu_op_count`` / ``dftu_op_at``
list the registry, ``dftu_op_signature`` spells a signature
(``"(series, scalar) -> series"``).

.. code-block:: c

   const dftu_op_desc* op = dftu_op_find("dftu.series.cumsum");
   const dftu_series* in[1] = {dur};
   dftu_series* running = dftu_op_run(op, in, 1, NULL);

Errors and results
------------------

A call that can fail in more than one way returns a tagged result
(``DFTU_RESULT_DECL``): ``DFTU_RESULT_OK(r)`` says which half of the union is
live, ``DFTU_RESULT_VALUE(r)`` is the owned handle, ``DFTU_RESULT_ERROR(r)``
a ``dftu_error`` with a ``(domain, code)`` identity, a portable
``condition`` (``DFTU_COND_NOT_FOUND``, ``INVALID_ARGUMENT``, ...) and a
borrowed ``message``. No exception ever crosses the C boundary. The simpler
``dftu_series_*`` calls return NULL on failure.

Plans from C
------------

``dftu_dataframe_lazy(df)`` starts a plan over a frame,
``dftu_lazyframe_from_provider(name)`` one over a source registered with
``dftu_provider_register``; every builder step is a ``dftu_lazyframe_*``
call, ``dftu_lazyframe_op(lf, name, args)`` appends a node registered with
``dftu_node_register``, ``dftu_lazyframe_explain`` prints the plan and
``dftu_lazyframe_collect`` runs it. :doc:`../data/lazyframe` has the
contract a source and a node fill.

Clean up
--------

Free every owned handle exactly once, and only handles you still own (a
"consumes" call already freed its inputs):

.. code-block:: c

   dftu_query_free(q2);

Link a C program
-----------------

The C ABI ships in the same libraries as the C++ API, so linking is identical
whether your translation unit is C or C++: link ``dftracer::utils`` and
include the C headers.

.. code-block:: cmake

   find_package(dftracer_utils REQUIRED)

   add_executable(my_c_tool main.c)
   target_link_libraries(my_c_tool PRIVATE dftracer::utils)

See :doc:`../../getting-started/installation` for the full build, install, and
``pkg-config`` details (they are not C-specific).

See also
--------

- :doc:`query-dsl` for the predicate language ``dftu_query_parse`` compiles.
- :doc:`../data/dataframe` and :doc:`../data/series` for the C++/Python
  surface the same engine exposes.
- :doc:`../../plugins` for the plugin C ABI (``dftracer/utils/plugins/abi.h``),
  which extends the scan itself rather than consuming its output.
