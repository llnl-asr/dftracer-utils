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
| simdjson | `Apache-2.0 OR MIT` | https://github.com/simdjson/simdjson |
| Apache Arrow nanoarrow | `Apache-2.0` | https://github.com/apache/arrow-nanoarrow |
| zlib-ng | `Zlib` | https://github.com/zlib-ng/zlib-ng |
| Zstandard (zstd) | `BSD-3-Clause OR GPL-2.0-only` | https://github.com/meta/zstd |
| libdeflate | `MIT` | https://github.com/ebiggers/libdeflate |
| Boost.Math | `BSL-1.0` | https://github.com/boostorg/math |
| yaml-cpp | `MIT` | https://github.com/jbeder/yaml-cpp |
| argparse | `MIT` | https://github.com/p-ranav/argparse |
| moodycamel concurrentqueue | `BSD-2-Clause` | https://github.com/cameron314/concurrentqueue |
| moodycamel readerwriterqueue | `BSD-2-Clause` | https://github.com/cameron314/readerwriterqueue |
| unordered_dense | `MIT` | https://github.com/martinus/unordered_dense |
| tl::expected | `CC0-1.0` | https://github.com/TartanLlama/expected |
| PCRE2 | `BSD-3-Clause WITH PCRE2-exception` | https://github.com/PCRE2Project/pcre2 |
| Vectorscan | `BSD-3-Clause` | https://github.com/VectorCamp/vectorscan |
| utf8proc | MIT, and the Unicode, Inc. data files license (1991-2007) for `utf8proc_data.c` | https://github.com/JuliaStrings/utf8proc |

## Vendored build tooling

- CPM.cmake (`MIT`, https://github.com/cpm-cmake/CPM.cmake) is vendored as
  `cmake/vendor/CPM_0.42.1.cmake`, which keeps its full license text.

## Web viewer (embedded in the server binary)

The server embeds the built web viewer. Versions come from
`web/package-lock.json`.

| Component | Version | License (SPDX) | Homepage |
|---|---|---|---|
| solid-js | 1.9.14 | `MIT` | https://www.npmjs.com/package/solid-js |
| seroval | 1.5.4 | `MIT` | https://www.npmjs.com/package/seroval |
| seroval-plugins | 1.5.4 | `MIT` | https://www.npmjs.com/package/seroval-plugins |
| csstype | 3.2.3 | `MIT` | https://www.npmjs.com/package/csstype |
| cytoscape | 3.30.2 | `MIT` | https://github.com/cytoscape/cytoscape.js |
| cytoscape-dagre | 2.5.0 | `MIT` | https://www.npmjs.com/package/cytoscape-dagre |
| dagre | 0.8.5 | `MIT` | https://github.com/dagrejs/dagre |
| graphlib | 2.1.8 | `MIT` | https://github.com/dagrejs/graphlib |
| lodash | 4.18.1 | `MIT` | https://github.com/lodash/lodash |

## Adapted source code

Code adapted rather than linked. The DDSketch collapsing dense store in
`include/dftracer/utils/dataframe/sketch.h` and
`src/dftracer/utils/dataframe/sketch.cpp` is adapted from Datadog's
`sketches-py` and `sketches-java` under `Apache-2.0`. The code was modified:
it was ported to C++ and changed.

Their NOTICE texts:

```
Datadog sketches-py
Copyright 2020 Datadog, Inc.

This product includes software developed at Datadog (https://www.datadoghq.com/).
```

```
Datadog sketches-java
Copyright 2021 Datadog, Inc.

This product includes software developed at Datadog (https://www.datadoghq.com/).
```

## Test-only dependencies

Not linked into the distributed libraries or wheels; used only when building the
test suite.

| Component | License (SPDX) | Homepage |
|---|---|---|
| doctest | `MIT` | https://github.com/doctest/doctest |
| Unity | `MIT` | https://github.com/ThrowTheSwitch/Unity |

## Notes

- Dual-licensed components (Highway, simdjson, Zstandard) are used under
  the permissive option listed first (Apache-2.0 / BSD-3-Clause), never the GPL
  option.
- moodycamel's queues embed Jeff Preshing's semaphore implementation, which
  carries a `Zlib` license (see the note in each queue's `LICENSE.md`).
- Exact pinned versions are defined in `cmake/modules/Dependencies.cmake`; that
  file is the source of truth for what is fetched.
- The lists above are generated from the dependencies actually vendored into the
  build (`.cpmsource/`). Some entries in `Dependencies.cmake` are conditional and
  are only fetched for certain configurations.
