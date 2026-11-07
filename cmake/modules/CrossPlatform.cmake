# SPDX-License-Identifier: MIT
# Enable cross-platform large file support
add_definitions(-D_FILE_OFFSET_BITS=64)
if(NOT WIN32)
  add_definitions(-D_LARGEFILE64_SOURCE)
endif()
