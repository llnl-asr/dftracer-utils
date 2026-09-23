# Third-Party Notices

`dftracer-utils` bundles and links the third-party components listed below. Each
is fetched at configure time (via CPM, see `cmake/modules/Dependencies.cmake`)
and statically or dynamically linked into the libraries, CLI binaries, and
Python extension. Each component remains under its own license, reproduced in
full in that component's source tree (under `.cpmsource/<name>/.../LICENSE`
after a build) and available at the linked homepage.

This project itself is licensed under the terms in [LICENSE](LICENSE); the
notices here cover only the third-party dependencies.

## Runtime dependencies

| Component | License (SPDX) | Homepage |
|---|---|---|
| Highway | `Apache-2.0 OR BSD-3-Clause` | https://github.com/google/highway |
| RocksDB | `Apache-2.0 OR GPL-2.0-only` | https://github.com/facebook/rocksdb |
| simdjson | `Apache-2.0 OR MIT` | https://github.com/simdjson/simdjson |
| Apache Arrow nanoarrow | `Apache-2.0` | https://github.com/apache/arrow-nanoarrow |
| zlib-ng | `Zlib` | https://github.com/zlib-ng/zlib-ng |
| Zstandard (zstd) | `BSD-3-Clause OR GPL-2.0-only` | https://github.com/facebook/zstd |
| libdeflate | `MIT` | https://github.com/ebiggers/libdeflate |
| Boost.Math | `BSL-1.0` | https://github.com/boostorg/math |
| yaml-cpp | `MIT` | https://github.com/jbeder/yaml-cpp |
| argparse | `MIT` | https://github.com/p-ranav/argparse |
| moodycamel concurrentqueue | `BSD-2-Clause` | https://github.com/cameron314/concurrentqueue |
| moodycamel readerwriterqueue | `BSD-2-Clause` | https://github.com/cameron314/readerwriterqueue |
| unordered_dense | `MIT` | https://github.com/martinus/unordered_dense |
| tl::expected | `CC0-1.0` | https://github.com/TartanLlama/expected |
| ghc::filesystem | `MIT` | https://github.com/gulrak/filesystem |

## Test-only dependencies

Not linked into the distributed libraries or wheels; used only when building the
test suite.

| Component | License (SPDX) | Homepage |
|---|---|---|
| doctest | `MIT` | https://github.com/doctest/doctest |
| Unity | `MIT` | https://github.com/ThrowTheSwitch/Unity |

## Notes

- Dual-licensed components (Highway, RocksDB, simdjson, Zstandard) are used under
  the permissive option listed first (Apache-2.0 / BSD-3-Clause), never the GPL
  option.
- moodycamel's queues embed Jeff Preshing's semaphore implementation, which
  carries a `Zlib` license (see the note in each queue's `LICENSE.md`).
- Exact pinned versions are defined in `cmake/modules/Dependencies.cmake`; that
  file is the source of truth for what is fetched.
- The lists above are generated from the dependencies actually vendored into the
  build (`.cpmsource/`). Some entries in `Dependencies.cmake` are conditional and
  are only fetched for certain configurations.
