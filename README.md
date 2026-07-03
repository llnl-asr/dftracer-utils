<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/source/_static/logo-dark.png">
    <img alt="DFTracer Utils" src="docs/source/_static/logo-light.png" width="480">
  </picture>
</div>

# dftracer-utils

A collection of utilities for DFTracer

[![CI](https://github.com/LLNL/dftracer-utils/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/LLNL/dftracer-utils/actions/workflows/ci.yml)
[![Coverage Status](https://coveralls.io/repos/github/llnl/dftracer-utils/badge.svg?branch=develop)](https://coveralls.io/github/llnl/dftracer-utils?branch=develop)
[![Documentation Status](https://readthedocs.org/projects/dftracer-utils/badge/?version=latest)](https://dftracer.readthedocs.io/projects/utils/)

## Documentation

Full documentation is available at [Read the Docs](https://dftracer.readthedocs.io/projects/utils/).

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
