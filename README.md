<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/source/_static/logo-banner-dark.svg">
    <img alt="DFTracer Utils" src="docs/source/_static/logo-banner-light.svg" width="520">
  </picture>
</div>

# dftracer-utils

Tools for reading, indexing, querying and analyzing [DFTracer](https://github.com/hariharan-devarajan/dftracer)
traces: a Python query API over a SIMD columnar DataFrame engine, a plugin
ABI for analyses that ride the scan, and the CLI utilities around them.

[![CI](https://github.com/LLNL/dftracer-utils/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/LLNL/dftracer-utils/actions/workflows/ci.yml)
[![Coverage Status](https://coveralls.io/repos/github/llnl/dftracer-utils/badge.svg?branch=develop)](https://coveralls.io/github/llnl/dftracer-utils?branch=develop)
[![Documentation Status](https://readthedocs.org/projects/dftracer-utils/badge/?version=latest)](https://dftracer.readthedocs.io/projects/utils/)

## Quickstart

```bash
pip install dftracer-utils
```

```python
from dftracer.utils import TraceViewer

view = TraceViewer("traces/")            # a directory of .pfw / .pfw.gz files
df = (
    view.filter('cat == "POSIX"')
        .group_by("name")
        .agg("count", "sum:dur", "max:dur")
        .collect()                        # runs the plan, returns a DataFrame
)
print(df.sort_values("sum_dur", ascending=False).head(3).to_pandas())
```

```
    name  count  sum_dur  max_dur
0  pread     16      280       22
1   read     13      244       25
2  fread      8      156       24
```

The frame is the engine's own `DataFrame`: the pandas and polars spellings
work on it (`import dftracer.utils.pandas as pd`), and `.to_pandas()` /
`.to_arrow()` hand the buffers over without a copy.

## Documentation

Full documentation is available at [Read the Docs](https://dftracer.readthedocs.io/projects/utils/):

- [Getting started](https://dftracer.readthedocs.io/projects/utils/en/latest/getting-started/index.html): install, first query, first pipeline, first plugin.
- [Guides](https://dftracer.readthedocs.io/projects/utils/en/latest/guides/index.html): task recipes, the pandas and polars surfaces, plans, joins, the memory budget, performance.
- [API reference](https://dftracer.readthedocs.io/projects/utils/en/latest/api/index.html) and the [C ABI](https://dftracer.readthedocs.io/projects/utils/en/latest/c_api/index.html).
- [Plugins](https://dftracer.readthedocs.io/projects/utils/en/latest/plugins.html): analyses that ride the fused scan.

To build documentation locally:

```bash
pip install .
cd docs
pip install -r requirements.txt
make html
```

See [docs/README.md](docs/README.md) for detailed documentation building instructions.

## Building

### Prerequisites

- CMake 3.20 or higher
- C++20 compatible compiler (GCC 11+, Clang 14+)
- zlib development library
- pkg-config

### Build

```bash
mkdir build && cd build
cmake ..
make
```

## Installation

```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=<LOCATION>
make
make install
```

## Developers Guide

Please see [Developers Guide](DEVELOPERS_GUIDE.md) for more information how to test, run coverage, etc.

## Citation

If you use this software, please cite:

> Ray A. O. Sinurat, William Nixon, Haryadi S. Gunawi, Nikoli Dryden, and Hariharan Devarajan. 2026. HORATIO: Bridging Management and Analysis of Traces at Scale. In The International Conference on Scalable Scientific Data Management 2026 (SSDBM 2026), August 11-13, 2026, San Diego, CA, USA. ACM, New York, NY, USA. doi:[10.1145/3828820.3828825](https://doi.org/10.1145/3828820.3828825)

```bibtex
@inproceedings{sinurat2026horatio,
  author    = {Sinurat, Ray A. O. and Nixon, William and Gunawi, Haryadi S. and Dryden, Nikoli and Devarajan, Hariharan},
  title     = {HORATIO: Bridging Management and Analysis of Traces at Scale},
  year      = {2026},
  isbn      = {979-8-4007-2708-5},
  publisher = {Association for Computing Machinery},
  address   = {New York, NY, USA},
  doi       = {10.1145/3828820.3828825},
  booktitle = {The International Conference on Scalable Scientific Data Management 2026 (SSDBM 2026)},
  location  = {San Diego, CA, USA},
  series    = {SSDBM 2026},
}
```

## License

dftracer-utils is distributed under the terms of the MIT license.
All new contributions must be made under this license.

See [LICENSE](LICENSE) and [NOTICE](NOTICE) for details.

SPDX-License-Identifier: MIT

LLNL-CODE-2024514 — Applied Storage Research (ASR)
