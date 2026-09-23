:description: The third-party open-source components dftracer-utils vendors via CPM, with their SPDX licenses and homepages.

Open source
===========

dftracer-utils is built on excellent open-source work. Every component below is
fetched at configure time (via CPM; see ``cmake/modules/Dependencies.cmake``,
the source of truth for pinned versions) and linked into the libraries, CLI
binaries, and the Python extension. Each remains under its own license,
reproduced in full in its source tree (``.cpmsource/<name>/.../LICENSE`` after a
build). dftracer-utils itself is under the terms in ``LICENSE``; this page
covers only the third-party components.

Dual-licensed components (Highway, RocksDB, simdjson, Zstandard) are used under
the permissive option (Apache-2.0 / BSD-3-Clause), never the GPL option.

Runtime dependencies
--------------------

.. list-table::
   :header-rows: 1
   :widths: 32 34 34

   * - Component
     - License (SPDX)
     - Homepage
   * - Highway (SIMD)
     - ``Apache-2.0 OR BSD-3-Clause``
     - https://github.com/google/highway
   * - RocksDB
     - ``Apache-2.0 OR GPL-2.0-only``
     - https://github.com/facebook/rocksdb
   * - simdjson
     - ``Apache-2.0 OR MIT``
     - https://github.com/simdjson/simdjson
   * - Apache Arrow nanoarrow
     - ``Apache-2.0``
     - https://github.com/apache/arrow-nanoarrow
   * - zlib-ng
     - ``Zlib``
     - https://github.com/zlib-ng/zlib-ng
   * - Zstandard (zstd)
     - ``BSD-3-Clause OR GPL-2.0-only``
     - https://github.com/facebook/zstd
   * - libdeflate
     - ``MIT``
     - https://github.com/ebiggers/libdeflate
   * - Boost.Math
     - ``BSL-1.0``
     - https://github.com/boostorg/math
   * - yaml-cpp
     - ``MIT``
     - https://github.com/jbeder/yaml-cpp
   * - argparse
     - ``MIT``
     - https://github.com/p-ranav/argparse
   * - moodycamel concurrentqueue
     - ``BSD-2-Clause``
     - https://github.com/cameron314/concurrentqueue
   * - moodycamel readerwriterqueue
     - ``BSD-2-Clause``
     - https://github.com/cameron314/readerwriterqueue
   * - unordered_dense
     - ``MIT``
     - https://github.com/martinus/unordered_dense
   * - tl::expected
     - ``CC0-1.0``
     - https://github.com/TartanLlama/expected
   * - ghc::filesystem
     - ``MIT``
     - https://github.com/gulrak/filesystem

Test-only dependencies
----------------------

Not linked into the distributed libraries or wheels; used only when building the
test suite.

.. list-table::
   :header-rows: 1
   :widths: 32 34 34

   * - Component
     - License (SPDX)
     - Homepage
   * - doctest
     - ``MIT``
     - https://github.com/doctest/doctest
   * - Unity
     - ``MIT``
     - https://github.com/ThrowTheSwitch/Unity

.. note::

   moodycamel's queues embed Jeff Preshing's semaphore implementation
   (``Zlib`` license). The full third-party notices, including this detail,
   live in ``THIRD-PARTY-NOTICES.md`` at the repository root.
