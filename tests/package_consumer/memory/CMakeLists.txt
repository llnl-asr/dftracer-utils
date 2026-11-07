# SPDX-License-Identifier: MIT
find_package(dftracer_utils_memory REQUIRED)

foreach(variant shared static)
  if(TARGET dftracer_utils_memory::${variant})
    add_executable(memory_consumer_${variant} memory_consumer.cpp)
    target_link_libraries(memory_consumer_${variant}
                          PRIVATE dftracer_utils_memory::${variant})
  endif()
endforeach()
