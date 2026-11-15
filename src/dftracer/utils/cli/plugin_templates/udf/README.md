# @NAME@

A dftracer-utils udf: it adds a scalar function to duql.
`dftracer_plugin new udf` wrote it; edit `plugin.cpp` to do the work.

    cmake -S . -B build -DCMAKE_PREFIX_PATH=<install prefix>
    cmake --build build
    dftracer_plugin check ./build/@NAME@.so --tests tests

The golden test in `tests/` holds the expected output of a query.
