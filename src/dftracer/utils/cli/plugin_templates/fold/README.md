# @NAME@

A dftracer-utils fold: it walks the trace and keeps its own accumulators.
`dftracer_plugin new fold` wrote it; edit `plugin.cpp` to do the work.

    cmake -S . -B build -DCMAKE_PREFIX_PATH=<install prefix>
    cmake --build build
    dftracer_plugin check ./build/@NAME@.so --tests tests

The golden test in `tests/` holds the expected output of a query.
