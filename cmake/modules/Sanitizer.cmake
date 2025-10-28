# ##############################################################################
# Sanitizer Configuration Module
# ##############################################################################
#
# This module provides configuration for various sanitizers: - AddressSanitizer
# (ASan): Detects memory errors (buffer overflows, use-after-free, etc.) -
# LeakSanitizer (LSan): Detects memory leaks (enabled with ASan) -
# UndefinedBehaviorSanitizer (UBSan): Detects undefined behavior -
# ThreadSanitizer (TSan): Detects data races and deadlocks
#
# Usage: cmake -DDFTRACER_UTILS_ENABLE_ASAN=ON ... cmake
# -DDFTRACER_UTILS_ENABLE_UBSAN=ON ... cmake -DDFTRACER_UTILS_ENABLE_TSAN=ON ...
#
# Note: TSan is incompatible with ASan/LSan
#

function(configure_sanitizers)
  # Check if sanitizers are supported on this platform
  if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if(DFTRACER_UTILS_ENABLE_ASAN
       OR DFTRACER_UTILS_ENABLE_UBSAN
       OR DFTRACER_UTILS_ENABLE_TSAN)
      message(
        WARNING
          "Sanitizers are not fully supported with GCC on macOS.\n"
          "Sanitizer libraries (libasan, libubsan, libtsan) may not be available.\n"
          "Consider using:\n"
          "  - Linux for GCC with sanitizers\n"
          "  - Clang on macOS (which has built-in sanitizer support)\n"
          "Disabling sanitizers to allow build to proceed.")
      return()
    endif()
  endif()

  # Check for incompatible sanitizer combinations
  if(DFTRACER_UTILS_ENABLE_TSAN AND DFTRACER_UTILS_ENABLE_ASAN)
    message(
      FATAL_ERROR
        "ThreadSanitizer is incompatible with AddressSanitizer/LeakSanitizer.\n"
        "Current settings:\n"
        "  DFTRACER_UTILS_ENABLE_ASAN: ${DFTRACER_UTILS_ENABLE_ASAN}\n"
        "  DFTRACER_UTILS_ENABLE_TSAN: ${DFTRACER_UTILS_ENABLE_TSAN}\n"
        "Please set one of them to OFF, or delete CMakeCache.txt and reconfigure:\n"
        "  rm -rf build/build-dev/CMakeCache.txt\n"
        "  cmake -B build/build-dev -DDFTRACER_UTILS_TESTS=ON")
  endif()

  set(SANITIZER_FLAGS
      ""
      PARENT_SCOPE)
  set(SANITIZER_LINK_FLAGS
      ""
      PARENT_SCOPE)

  # AddressSanitizer + LeakSanitizer
  if(DFTRACER_UTILS_ENABLE_ASAN)
    message(STATUS "Enabling AddressSanitizer and LeakSanitizer")
    list(APPEND SANITIZER_FLAGS "-fsanitize=address" "-fno-omit-frame-pointer")
    list(APPEND SANITIZER_LINK_FLAGS "-fsanitize=address")

    # LeakSanitizer is not available on macOS with Clang It's integrated into
    # ASan automatically on macOS
    if(NOT APPLE)
      list(APPEND SANITIZER_FLAGS "-fsanitize=leak")
      list(APPEND SANITIZER_LINK_FLAGS "-fsanitize=leak")
    endif()

    # Set runtime options for better error reports
    if(NOT DEFINED ENV{ASAN_OPTIONS})
      message(
        STATUS
          "  ASan runtime options: detect_leaks=1:check_initialization_order=1")
      set(ENV{ASAN_OPTIONS} "detect_leaks=1:check_initialization_order=1")
    endif()
  endif()

  # UndefinedBehaviorSanitizer
  if(DFTRACER_UTILS_ENABLE_UBSAN)
    message(STATUS "Enabling UndefinedBehaviorSanitizer")
    list(APPEND SANITIZER_FLAGS "-fsanitize=undefined"
         "-fno-omit-frame-pointer")
    list(APPEND SANITIZER_LINK_FLAGS "-fsanitize=undefined")

    # Set runtime options
    if(NOT DEFINED ENV{UBSAN_OPTIONS})
      message(STATUS "  UBSan runtime options: print_stacktrace=1")
      set(ENV{UBSAN_OPTIONS} "print_stacktrace=1")
    endif()
  endif()

  # ThreadSanitizer
  if(DFTRACER_UTILS_ENABLE_TSAN)
    message(STATUS "Enabling ThreadSanitizer")
    list(APPEND SANITIZER_FLAGS "-fsanitize=thread" "-fno-omit-frame-pointer")
    list(APPEND SANITIZER_LINK_FLAGS "-fsanitize=thread")

    # Set runtime options
    if(NOT DEFINED ENV{TSAN_OPTIONS})
      message(STATUS "  TSan runtime options: second_deadlock_stack=1")
      set(ENV{TSAN_OPTIONS} "second_deadlock_stack=1")
    endif()
  endif()

  # Apply sanitizer flags if any were set
  if(SANITIZER_FLAGS)
    message(STATUS "Applying sanitizer flags to all targets")

    # Use add_compile_options and link_libraries to ensure sanitizer flags are
    # consistently applied to both compilation and linking
    add_compile_options(${SANITIZER_FLAGS})
    link_libraries(${SANITIZER_LINK_FLAGS})

    # Also set the linker flags for targets that might not use link_libraries
    string(JOIN " " SANITIZER_LINK_FLAGS_STR ${SANITIZER_LINK_FLAGS})
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS_STR}"
        PARENT_SCOPE)
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS_STR}"
        PARENT_SCOPE)
    set(CMAKE_MODULE_LINKER_FLAGS
        "${CMAKE_MODULE_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS_STR}"
        PARENT_SCOPE)

    # Ensure debug symbols for better stack traces
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
      message(
        STATUS "Adding debug symbols (-g) for better sanitizer stack traces")
      add_compile_options("-g")
    endif()

    # Disable optimizations for better error detection (optional but
    # recommended)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      message(
        WARNING
          "Sanitizers are enabled with Release build. "
          "Consider using Debug or RelWithDebInfo for better error detection.")
    endif()
  endif()
endfunction()

# Print sanitizer status
function(print_sanitizer_status)
  message(STATUS "")
  message(STATUS "Sanitizer Configuration:")
  message(
    STATUS "  AddressSanitizer (ASan):         ${DFTRACER_UTILS_ENABLE_ASAN}")
  message(
    STATUS
      "  UndefinedBehaviorSanitizer (UBSan): ${DFTRACER_UTILS_ENABLE_UBSAN}")
  message(
    STATUS "  ThreadSanitizer (TSan):          ${DFTRACER_UTILS_ENABLE_TSAN}")
  message(STATUS "")
endfunction()
