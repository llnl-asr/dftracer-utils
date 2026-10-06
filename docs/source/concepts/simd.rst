:description: How columnar kernels are written once against Highway and dispatched to the widest SIMD instruction set the running CPU supports.

How the columnar engine vectorizes
===================================

What this explains: why kernels in the columnar engine are written once and
run fast on whatever CPU they land on, and how the batch-at-a-time scan model
feeds them data shaped for that. For the column types themselves see
:doc:`dataframe-model`; for the one-pass scan see :doc:`fused-scan`.

Portable SIMD: one kernel, many instruction sets
-----------------------------------------------------

A filter or a comparison over a million-row column is only fast if it moves
several values per CPU instruction instead of one. The obvious way to get
that is to hand-write the loop per instruction set - SSE4, AVX2, AVX-512 on
x86, NEON or SVE on ARM - but that multiplies every kernel by the number of
targets it supports and makes each one a maintenance burden.

The engine's kernels (``src/dftracer/utils/dataframe/kernels/``) are written
once against `Highway <https://github.com/google/highway>`_ (vendored at
1.4.0, see ``cmake/modules/Dependencies.cmake``), a portable-SIMD library that
compiles a single templated body against several instruction-set targets and
dispatches to the widest one the running CPU actually supports, chosen once at
load time rather than decided at compile time. A kernel file includes
``hwy/foreach_target.h`` to get itself recompiled per target inside an
``HWY_NAMESPACE``, then exports each variant with ``HWY_EXPORT`` and reaches it
through ``HWY_DYNAMIC_DISPATCH``:

.. code-block:: cpp

   // dataframe/kernels/arithmetic.cpp
   #undef HWY_TARGET_INCLUDE
   #define HWY_TARGET_INCLUDE "dftracer/utils/dataframe/kernels/arithmetic.cpp"
   #include <hwy/foreach_target.h>
   #include <hwy/highway.h>
   // ... AddKernel etc. defined once inside HWY_NAMESPACE ...
   HWY_EXPORT(AddKernel);
   // at the call site:
   HWY_DYNAMIC_DISPATCH(AddKernel)(t, pa, pb, po, n);

The body itself is written against ``hn::ScalableTag<T>`` and the ``Lanes`` /
``Load`` / ``Gt`` / ``Set`` style operations Highway provides, so it has no
architecture-specific code at all; a machine without any of the extended
instruction sets still runs the same source through Highway's scalar target.
This is what lets one kernel run correctly (with predictable, testable
behavior) on a laptop, an AVX-512 compute node, and an ARM system without a
per-architecture code path anywhere in the tree.

Batches, not rows: why the scan feeds kernels this way
-------------------------------------------------------------

Vectorizing the kernel only pays off if the data reaching it is already
contiguous and typed - a ``for`` loop over parsed JSON objects, dispatching
per field by name, defeats SIMD before a kernel ever runs. The trace scan is
built so that never happens: :doc:`fused-scan` parses each event once into a
plain batch and hands every fold that batch as a unit, not one event at a
time through a virtual call. A fold's ``step`` receives a
``std::span<const FoldEvent>`` (or the raw batch a plugin's ``on_batch``
sees) and works over the whole span, so the per-event cost is amortized across
the batch instead of paid once per virtual dispatch.

That same batch-grained shape is what a ``Series`` continues once the scan's
output lands in the :doc:`dataframe-model`. Column :doc:`encodings
<dataframe-model>` exist for the same reason kernels are written against
Highway: a FLAT column is a contiguous buffer a kernel can ``Load`` directly;
CONSTANT and SELECTION avoid materializing values a kernel would otherwise
have to touch one by one; DICTIONARY lets a string predicate compare small
integer codes instead of re-comparing bytes per row. Each encoding is a
different way of keeping the data a kernel sees dense and typed, which is the
precondition portable SIMD needs to be worth using at all.

.. mermaid::

   graph LR
       Scan["Fused scan<br/>(parse once)"] --> Batch["POD batch<br/>(no per-row dispatch)"]
       Batch --> Series["Series<br/>(FLAT/CONSTANT/DICTIONARY/SELECTION/VIEW/CHUNKED)"]
       Series --> Kernel["Highway kernel<br/>(one body, runtime-dispatched)"]
       Kernel --> ISA["SSE4 / AVX2 / AVX-512 / NEON<br/>(picked at load time)"]

Why this is one concept, not two
-------------------------------------

Portable SIMD and batch-at-a-time processing solve the same problem from two
ends: Highway means a kernel does not need a different body per CPU, and the
fold-fusion scan means a kernel is never called with less than a batch's
worth of contiguous, already-typed data to work on. Either alone would leave
the other's cost on the table - a fast kernel fed one row at a time is bound
by dispatch overhead, and a batched scan feeding scalar kernels wastes the
lanes the CPU has. Together they are why a query over a multi-gigabyte trace
does the numeric and string work at native vector width without the codebase
carrying per-architecture kernel code.

See also
--------

- :doc:`dataframe-model` for ``Series``/``DataFrame`` and the six column
  encodings.
- :doc:`fused-scan` for the one-pass, batch-grained scan that feeds the
  engine.
- :doc:`../columnar-engine` and :doc:`../guides/data/dataframe` for the
  operations available on top of this.
