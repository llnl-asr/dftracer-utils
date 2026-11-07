:description: The third-party open-source components dftracer-utils vendors via CPM, with their SPDX licenses and homepages.

Open source
===========

dftracer-utils is built on excellent open-source work. The components in
``include/dftracer/utils/dstd/vendor/`` ship as source in this repository; every
other one is fetched at configure time (via CPM; see
``cmake/modules/Dependencies.cmake``, the source of truth for pinned versions)
and linked into the libraries, CLI binaries, and the Python extension. Each
remains under its own license, reproduced in full in its source tree
(``.cpmsource/<name>/.../LICENSE`` after a build, and in the vendored header
itself). dftracer-utils itself is under the terms in ``LICENSE``; this page
covers only the third-party components.

Dual-licensed components (Highway, simdjson, Zstandard) are used under
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
     - https://github.com/meta/zstd
   * - libdeflate
     - ``MIT``
     - https://github.com/ebiggers/libdeflate
   * - Boost.Math
     - ``BSL-1.0``
     - https://github.com/boostorg/math
   * - yaml-cpp
     - ``MIT``
     - https://github.com/jbeder/yaml-cpp
   * - moodycamel concurrentqueue
     - ``BSD-2-Clause``
     - https://github.com/cameron314/concurrentqueue
   * - unordered_dense
     - ``MIT``
     - https://github.com/martinus/unordered_dense
   * - tl::expected
     - ``CC0-1.0``
     - https://github.com/TartanLlama/expected
   * - PCRE2
     - ``BSD-3-Clause WITH PCRE2-exception``
     - https://github.com/PCRE2Project/pcre2
   * - Vectorscan (optional)
     - ``BSD-3-Clause``
     - https://github.com/VectorCamp/vectorscan

PCRE2 and Vectorscan are the regex engines of duql, and Vectorscan also
prefilters raw lines. Vectorscan is built unless you configure with
``-DDFTRACER_UTILS_ENABLE_VECTORSCAN=OFF``, and it needs ``ragel`` at build
time.

Three of these rows are vendored single headers: moodycamel concurrentqueue,
unordered_dense and tl::expected. Their full text, unchanged apart from one
provenance line, is in ``include/dftracer/utils/dstd/vendor/`` as
``concurrentqueue.h``, ``unordered_dense.h`` and ``expected.hpp``, and it
installs with the rest of ``include/``, so a plugin built from a source checkout
and a plugin built against an install prefix see the same headers. The vendored
concurrentqueue carries the thread-exit unsubscribe fix upstream has not taken;
its first line names the upstream commit.

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

   moodycamel concurrentqueue embeds Jeff Preshing's semaphore implementation
   (``Zlib`` license). The full third-party notices, including this detail,
   live in ``THIRD-PARTY-NOTICES.md`` at the repository root.
