:description: Reference for the C++ duql filter language: the predicate IR, the field builder, the string codec, and the evaluator that runs predicates over columns.

duql
====

.. seealso::

   :doc:`../concepts/indexing-and-pushdown` for how predicates push down into
   the index, and :doc:`../c_api/duql` for the flat C ABI.

The duql filter language in ``dftracer::utils::duql``: the predicate IR, the
field builder, the string codec, and the evaluator that runs predicates over
columns.

Pipeline builder
----------------

``duql::Pipe`` and ``duql::Col`` build a whole query that parses to the same
tree as the equal text; ``Pipe::text()`` prints the canonical text and
``View::duql(pipe)`` runs it with the parameters bound by ``Pipe::bind``.
A builder call that gets a value no duql literal can hold, or a call that
cannot make a valid query, does not throw.
The ``Col``, ``Pipe`` or ``Source`` it returns keeps the first such error,
``error()`` reads it, and every value built from it keeps it.
``Pipe::text()`` returns a ``dstd::Expected<dstd::String, DuqlError>`` that
holds the first error, or the parser's error when the text does not parse.
``View::duql(pipe)`` throws ``INVALID_ARGUMENT`` for a ``Pipe`` that holds an
error. The header links nothing, so a plugin can build text with ``raw()``.
``raw()`` does not check the error, so test ``error()`` before it.

.. code-block:: cpp

   using namespace dftracer::utils::duql;
   const Pipe q = source("trace.pfw.gz")
                      .where(c("cat") == "POSIX" && c("dur") > duration(1, "ms"))
                      .group({"name"}, {{"n", fn("count")}})
                      .sort({-c("n")})
                      .take(10);
   View out = view.duql(q);

``c("xs")[c("i")]`` (``Col::operator[]``) gives ``xs[i]``, the element at
that row's index: 0-based, negative from the end, null when out of range.

``Pipe::bucket(width, fill, as, mode, low, high, every, at)`` adds the
``bucket`` stage; ``every`` makes hopping windows and ``at`` aligns the
starts.

``Pipe::define(name, params, const Pipe& body)`` declares a pipeline macro
and ``Pipe::use(name, args)`` calls it as a stage.

``duql::Source`` builds the duql source of a record schema:
``rowset(name, pipe)`` adds a row set, ``define(name, params, body)`` a
macro and ``flag(name, value)`` a flag such as ``args_fallback``.
``text()`` returns the canonical member text a schema stores (row sets
first, then macros and flags, each in the order added) as a
``dstd::Expected<dstd::String, DuqlError>``. It holds the first input error,
or the parser's error when a member does not parse. A row set whose pipe
starts with a source (``from ...``) is an input error.

.. code-block:: cpp

   const auto source =
       duql::Source()
           .rowset("data", duql::Pipe().where(duql::c("ph") != "M"))
           .flag("args_fallback", true)
           .text();
   // *source holds the member text:
   // data = where ph != "M";
   // def args_fallback = true

``Pipe::parse(const Col& col, const Col& pattern)`` adds the ``parse``
stage; ``regex_replace`` goes through the generic call builder:
``fn("regex_replace")`` with the column, the pattern and the replacement as
arguments.

Type relationships
------------------

The predicate IR node types and how they nest:

.. mermaid:: /_generated/duql.mmd

.. include:: /cpp_api/_generated/duql.rst.inc
