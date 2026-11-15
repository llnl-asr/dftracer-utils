:description: Write a plugin that takes the rows of a query and writes them somewhere.

Write a sink plugin
===================

A sink is a plugin that takes the rows of a query and writes them where you
want them: a CSV file, a database, an object store. This page writes one and
calls it from every surface.

Goal
----

A sink named ``csv.write`` that writes a header line and then every row, into
the file its option ``path`` names.

Write it
--------

A sink is one class with a ``bind`` and a writer class with ``write``,
``finalize`` and ``abort``. ``bind`` names the columns it takes and reads the
options; ``write`` gets one borrowed ``FrameView`` per call, so keep no pointer
to it.

.. code-block:: cpp

   #include <dftracer/utils/plugins/plugin.h>

   class Writer : public pl::SinkWriter {
    public:
       explicit Writer(FILE* out) : out_(out) {}
       dstd::Result<void> write(pl::FrameView batch) override {
           // One CSV line per row. bind took only "id", so it is column 0.
           // flat() shares a FLAT column's buffer with no copy, and turns a
           // selection or dictionary column into a FLAT one.
           dstd::Result<pl::Column> id = batch.column(0).flat();
           if (!id) return pl::invalid_argument("csv: no id column");
           const dstd::Span<const std::int64_t> ids =
               pl::ColumnView(id->get()).values<std::int64_t>();
           for (const std::int64_t value : ids)
               std::fprintf(out_, "%lld\n", static_cast<long long>(value));
           return {};
       }
       dstd::Result<void> finalize() override { std::fclose(out_); return {}; }
       void abort() override { std::fclose(out_); }
    private:
       FILE* out_;
   };

   class Csv : public pl::Sink {
    public:
       dstd::Result<dstd::UniquePtr<pl::SinkWriter>> bind(
           const pl::BindView& view) override {
           // view.names and view.types are the caller's columns in order;
           // view.options is the option tree it passed.
           return {dstd::make_unique<Writer>(
               open_path(view.options.get("path")))};
       }
   };

   return plugin(ctx, h, config).sink<Csv>("csv", out).build(out);

The name a caller uses is ``<plugin>.<name>``, so this one is ``csv.write``.
``tests/plugins/toy_sink_plugin.cpp`` and ``examples/plugins/csv_sink.cpp``
are complete versions.

Call it
-------

.. code-block:: cpp

   auto summary = plan.sink("csv.write", {{"path", "out.csv"}}).get();

.. code-block:: python

   import dftracer.utils as dft
   from dftracer.utils.plugins import Plugins

   Plugins(["./csv.dylib"])                     # keep it alive for the call
   summary = df.lazy().filter(dft.col("dur") > 1000).sink("csv.write", path="out.csv")
   print(summary.rows, summary.batches)

.. code-block:: text

   dftracer_view ./traces --plugin ./csv.dylib --sink csv.write \
                 --sink-option path=out.csv -q 'where dur > 1000'

The query result goes to the sink instead of to stdout, and the run prints one
line to stderr: ``sink csv.write: <rows> rows in <batches> batches``.

Check it
--------

Read the file back and compare it with the rows the same query prints. A name
no plugin registered fails with ``no sink named '<name>'`` and lists the sinks
it knows, so a typo is visible at once, and a sink that fails mid-way is aborted
once, not twice.

A duql pipeline can end in a sink too, and then it returns one row of the
totals: ``from data | where dur > 1000 | sink "csv.write" path "out.csv"``. See
:doc:`../../reference/duql`.
