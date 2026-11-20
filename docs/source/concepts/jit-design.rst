:description: Why the JIT layer is a small checked subset, why state merges are inferred, why strings are views and why Python UDFs share one lane.

Why JIT plugins look the way they do
====================================

:doc:`../jit` is the reference for the rules.
This page gives the reasons.

A small subset, checked at decoration
-------------------------------------

A plugin body runs once for each event, on every worker, inside the fused scan.
A body in full Python would need the interpreter in that loop.
The JIT layer reads the body as a syntax tree instead and lowers it to C.
It accepts only forms that it can lower with the same meaning as Python.
Anything else is a ``JitError`` with the line and column, so no body is ever lowered to different behavior.

State without a merge function
------------------------------

Each worker folds its own events into its own copy of the state, and the host merges the copies at the end.
For that the merge must be associative and must not depend on the order of the workers.
A sum, a minimum, a maximum, a union and a time-ordered list have such merges, and each has one update form in the code.
The compiler infers the merge from that form.
An update that has no such merge, and a read of state inside the body, are errors, because a worker sees only its own slice.

Batch hooks take a closed vocabulary
------------------------------------

A batch hook is not run as Python over columns.
It is lowered to engine expressions that run fused over the batch.
Only reductions whose batch value merges exactly into the target are allowed, such as the sum of a count and the maximum of maxima.
A column is valid for one call, so a hook cannot keep one.

Strings are views with lifetimes
--------------------------------

A line of a file, a chunk and a JSON string are bytes that the host lends.
Copying each into an interned string would cost an allocation for every line of a large file.
A view is valid in the loop, the ``with`` block or the document that made it.
The compiler checks each use against that lifetime, and it interns a view only where an id is needed, as a key or an element.

Tasks start at finalize
-----------------------

A scan callback is synchronous, and its row buffers die when it returns.
A task started there would have no place to wait.
Tasks, files read once and channel receives therefore live in the ``@jit.finalize`` body, which runs after the merge.

One Python lane for per-batch UDFs
----------------------------------

A compiled UDF never takes the GIL.
A per-batch UDF is Python, so it holds the GIL while it runs.
If every worker took the GIL in turn, the workers would wait on each other and on other Python threads.
One lane thread runs every per-batch call, and a worker queues its batch and waits without the GIL.
The queue and the hand-off already allow more than one lane for a Python build without the GIL.
