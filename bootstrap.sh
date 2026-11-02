#!/bin/sh
# SPDX-License-Identifier: MIT
set -e
mkdir -p build
cd build
cmake ..
make
