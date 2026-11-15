# SPDX-License-Identifier: MIT
# @NAME@: a dftracer-utils plugin. It builds against the installed package
# alone, so nothing here names a header or a library of a source checkout.
cmake_minimum_required(VERSION 3.20)
project(@NAME@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(dftracer_utils REQUIRED)
find_package(dftracer_utils_dataframe REQUIRED)

add_library(@NAME@ MODULE plugin.cpp)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(@NAME@ PRIVATE dftracer_utils_dataframe::shared)
else()
  target_link_libraries(@NAME@ PRIVATE dftracer_utils_dataframe::static)
endif()
set_target_properties(@NAME@ PROPERTIES PREFIX "" OUTPUT_NAME @NAME@)
