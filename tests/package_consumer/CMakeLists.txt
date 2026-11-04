# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(dftracer_utils_package_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project must find everything through the installed packages alone, so
# it names no dependency of ours such as Threads.
#
# Imported targets stay in the directory that found them and its children, so
# the memory package goes first, on its own: the configs loaded below also find
# Threads and would hide a gap in it.
add_subdirectory(memory)

find_package(dftracer_utils_duql REQUIRED)
find_package(dftracer_utils REQUIRED)

# One program per package and variant that the installed tree has.
foreach(variant shared static)
  if(TARGET dftracer_utils_duql::${variant})
    add_executable(filter_consumer_${variant} filter_consumer.cpp)
    target_link_libraries(filter_consumer_${variant}
                          PRIVATE dftracer_utils_duql::${variant})
  endif()
  if(TARGET dftracer_utils::${variant})
    add_executable(bundle_consumer_${variant} filter_consumer.cpp)
    target_link_libraries(bundle_consumer_${variant}
                          PRIVATE dftracer_utils::${variant})
  endif()
endforeach()
