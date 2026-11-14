# @NAME@

A dftracer-utils format: it teaches the engine to read its own file type.
`dftracer_plugin new format` wrote it; edit `plugin.cpp` to do the work.

    cmake -S . -B build -DCMAKE_PREFIX_PATH=<install prefix>
    cmake --build build
    dftracer_plugin check ./build/@NAME@.so --tests tests

The golden test in `tests/` holds the expected output of a query.
