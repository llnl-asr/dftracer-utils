# SPDX-License-Identifier: MIT
# Drops Highway's `cmake_policy(SET CMP0111 OLD)`, which CMake reports as
# deprecated. It only works around imported GTest targets, and we build
# Highway without tests. Run in the Highway source folder as a patch step.
file(READ CMakeLists.txt text)
string(REPLACE "cmake_policy(SET CMP0111 OLD)" "" text "${text}")
file(WRITE CMakeLists.txt "${text}")
