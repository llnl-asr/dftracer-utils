# ==============================================================================
# CPM Configuration
# ==============================================================================

set(CPM_USE_LOCAL_PACKAGES ON)
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpmsource")

# ==============================================================================
# System Dependencies
# ==============================================================================

find_package(Threads REQUIRED)

# Find MPI if enabled
option(DFTRACER_UTILS_ENABLE_MPI "Enable MPI support" OFF)
if(DFTRACER_UTILS_ENABLE_MPI)
  find_package(MPI REQUIRED)
  message(STATUS "MPI support enabled")
  message(STATUS "  MPI_CXX_COMPILER: ${MPI_CXX_COMPILER}")
  message(STATUS "  MPI_CXX_LIBRARIES: ${MPI_CXX_LIBRARIES}")
endif()

set(DEPENDENCY_LIBRARY_DIRS "")

if(CMAKE_VERSION VERSION_LESS 3.18)
  set(DEV_MODULE Development)
else()
  set(DEV_MODULE Development.Module)
endif()

find_package(
  Python 3.8
  COMPONENTS Interpreter ${DEV_MODULE}
  OPTIONAL_COMPONENTS Development.SABIModule)

# ==============================================================================
# Logging Dependencies
# ==============================================================================

function(need_cpplogger)
  # First try to find cpp-logger from the system or other projects
  find_package(cpp-logger 0.0.6 QUIET)

  if(cpp-logger_FOUND)
    message(STATUS "Found system cpp-logger")

    # The system package should provide cpp-logger::cpp-logger target If for
    # some reason it doesn't, try to create it from cpp-logger target
    if(NOT TARGET cpp-logger::cpp-logger AND TARGET cpp-logger)
      message(
        STATUS "Creating cpp-logger::cpp-logger alias for system cpp-logger")
      add_library(cpp-logger::cpp-logger ALIAS cpp-logger)
    endif()

    # Set variables in parent scope so they persist outside the function
    set(cpp-logger_FOUND
        ${cpp-logger_FOUND}
        PARENT_SCOPE)
    set(cpplogger_CPM
        FALSE
        PARENT_SCOPE)
  else()
    # Build with CPM
    if(NOT cpplogger_ADDED)
      # Download cpp-logger source without using their CMakeLists.txt This
      # avoids their broken installation paths and gives us full control
      cpmaddpackage(
        NAME
        cpplogger
        GITHUB_REPOSITORY
        hariharan-devarajan/cpp-logger
        VERSION
        0.0.6
        DOWNLOAD_ONLY
        YES)
    endif()

    if(cpplogger_ADDED)
      message(STATUS "Built cpp-logger with CPM (manual target creation)")

      set(CPPLOGGER_TARGETS)

      # Build static library if requested
      if(DFTRACER_UTILS_BUILD_STATIC)
        add_library(
          cpp-logger_static STATIC
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/clogger.cpp
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/logger.cpp)
        target_include_directories(
          cpp-logger_static
          PUBLIC $<BUILD_INTERFACE:${cpplogger_SOURCE_DIR}/include>
                 $<INSTALL_INTERFACE:include>)
        set_target_properties(
          cpp-logger_static
          PROPERTIES OUTPUT_NAME cpp-logger ARCHIVE_OUTPUT_DIRECTORY
                                            ${CMAKE_BINARY_DIR}/lib)
        # Suppress GNU extension warning from cpp-logger's use of ##__VA_ARGS__
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
          target_compile_options(cpp-logger_static PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
        endif()
        add_library(cpp-logger::cpp-logger_static ALIAS cpp-logger_static)
        list(APPEND CPPLOGGER_TARGETS cpp-logger_static)
        message(STATUS "Added cpp-logger static library")
      endif()

      # Build shared library if requested
      if(DFTRACER_UTILS_BUILD_SHARED)
        add_library(
          cpp-logger_shared SHARED
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/clogger.cpp
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/logger.cpp)
        target_include_directories(
          cpp-logger_shared
          PUBLIC $<BUILD_INTERFACE:${cpplogger_SOURCE_DIR}/include>
                 $<INSTALL_INTERFACE:include>)
        set_target_properties(
          cpp-logger_shared
          PROPERTIES OUTPUT_NAME cpp-logger
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        # Suppress GNU extension warning from cpp-logger's use of ##__VA_ARGS__
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
          target_compile_options(cpp-logger_shared PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
        endif()
        add_library(cpp-logger::cpp-logger ALIAS cpp-logger_shared)
        list(APPEND CPPLOGGER_TARGETS cpp-logger_shared)
        message(STATUS "Added cpp-logger shared library")
      elseif(DFTRACER_UTILS_BUILD_STATIC)
        # If only static is built, make it the default alias
        add_library(cpp-logger::cpp-logger ALIAS cpp-logger_static)
      endif()

      # Install headers
      install(
        DIRECTORY ${cpplogger_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp")

      # Install the libraries
      if(CPPLOGGER_TARGETS)
        install(
          TARGETS ${CPPLOGGER_TARGETS}
          EXPORT cpp-logger-targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

        # Install export Note: Using cpp-logger-targets.cmake to match what
        # their config file expects (the original cpp-logger has an
        # inconsistency between export and config)
        install(
          EXPORT cpp-logger-targets
          FILE cpp-logger-targets.cmake
          NAMESPACE cpp-logger::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cpp-logger)
      endif()

      set(cpplogger_CPM
          TRUE
          PARENT_SCOPE)
    endif()

    if(cpplogger_ADDED AND SKBUILD)
      set(DEPENDENCY_LIBRARY_DIRS
          "${DEPENDENCY_LIBRARY_DIRS}" ${Python_SITELIB}/${CMAKE_INSTALL_LIBDIR}
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(link_cpp_logger TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_cpp_logger: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR
        "link_cpp_logger: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_cpp_logger: Target '${TARGET_NAME}' does not exist")
  endif()

  # Link appropriate cpp-logger variant Use PUBLIC linkage since cpp-logger
  # headers are included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static cpp-logger if available
    if(TARGET cpp-logger_static)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger_static)
      message(STATUS "Linked ${TARGET_NAME} to cpp-logger_static")
    elseif(TARGET cpp-logger_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      message(STATUS "Linked ${TARGET_NAME} to cpp-logger (shared)")
    elseif(TARGET cpp-logger::cpp-logger)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      message(STATUS "Linked ${TARGET_NAME} to system cpp-logger")
    else()
      message(
        FATAL_ERROR
          "link_cpp_logger: No cpp-logger found! Call need_cpplogger() first.")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared cpp-logger if available
    if(TARGET cpp-logger_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      message(STATUS "Linked ${TARGET_NAME} to cpp-logger (shared)")
    elseif(TARGET cpp-logger_static)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger_static)
      message(STATUS "Linked ${TARGET_NAME} to cpp-logger_static")
    elseif(TARGET cpp-logger::cpp-logger)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      message(STATUS "Linked ${TARGET_NAME} to system cpp-logger")
    else()
      message(
        FATAL_ERROR
          "link_cpp_logger: No cpp-logger found! Call need_cpplogger() first.")
    endif()
  endif()
endfunction()

# ==============================================================================
# Utility Dependencies
# ==============================================================================

function(need_argparse)
  if(NOT argparse_ADDED)
    cpmaddpackage(
      NAME
      argparse
      GITHUB_REPOSITORY
      p-ranav/argparse
      VERSION
      3.2
      OPTIONS
      "ARGPARSE_BUILD_TESTS OFF"
      "ARGPARSE_BUILD_SAMPLES OFF"
      FORCE
      YES)
  endif()
endfunction()

function(need_ghc_filesystem)
  if(NOT ghc_filesystem_ADDED)
    cpmaddpackage(
      NAME
      ghc_filesystem
      GITHUB_REPOSITORY
      gulrak/filesystem
      VERSION
      1.5.14
      OPTIONS
      "GHC_FILESYSTEM_WITH_INSTALL ON"
      FORCE
      YES)
  endif()
endfunction()

function(need_nonstd_span)
  if(NOT nonstd_span_ADDED)
    cpmaddpackage(
      NAME
      nonstd_span
      GITHUB_REPOSITORY
      nonstd-lite/span-lite
      VERSION
      0.11.0)
  endif()
endfunction()

# ==============================================================================
# JSON and Serialization Dependencies
# ==============================================================================

function(need_yyjson)
  if(NOT yyjson_ADDED)
    cpmaddpackage(
      NAME
      yyjson
      GITHUB_REPOSITORY
      ibireme/yyjson
      VERSION
      0.12.0
      GIT_TAG
      0.12.0
      FORCE
      YES
      DOWNLOAD_ONLY
      YES)
  endif()

  set(YYJSON_SOVERSION 0)
  set(YYJSON_TARGETS)

  if(DFTRACER_UTILS_BUILD_STATIC)
    add_library(yyjson_static STATIC ${yyjson_SOURCE_DIR}/src/yyjson.h
                                     ${yyjson_SOURCE_DIR}/src/yyjson.c)
    target_include_directories(
      yyjson_static PUBLIC $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>)
    set_target_properties(
      yyjson_static
      PROPERTIES VERSION ${PROJECT_VERSION}
                 SOVERSION ${YYJSON_SOVERSION}
                 ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    add_library(yyjson::yyjson_static ALIAS yyjson_static)
    list(APPEND YYJSON_TARGETS yyjson_static)
    message(STATUS "Added yyjson static library")
  endif()

  if(DFTRACER_UTILS_BUILD_SHARED)
    add_library(yyjson_shared SHARED ${yyjson_SOURCE_DIR}/src/yyjson.h
                                     ${yyjson_SOURCE_DIR}/src/yyjson.c)
    target_include_directories(
      yyjson_shared PUBLIC $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>)
    set_target_properties(
      yyjson_shared
      PROPERTIES VERSION ${PROJECT_VERSION}
                 SOVERSION ${YYJSON_SOVERSION}
                 OUTPUT_NAME yyjson
                 LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                 ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    add_library(yyjson::yyjson ALIAS yyjson_shared)
    list(APPEND YYJSON_TARGETS yyjson_shared)
    message(STATUS "Added yyjson shared library")
  elseif(DFTRACER_UTILS_BUILD_STATIC)
    # If only static is built, make it the default alias
    add_library(yyjson::yyjson ALIAS yyjson_static)
  endif()

  install(FILES ${yyjson_SOURCE_DIR}/src/yyjson.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(YYJSON_TARGETS)
    install(
      TARGETS ${YYJSON_TARGETS}
      EXPORT yyjsonTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Install the export set so other projects can find yyjson
    install(
      EXPORT yyjsonTargets
      FILE yyjsonTargets.cmake
      NAMESPACE yyjson::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yyjson)
  endif()
endfunction()

# ==============================================================================
# Database Dependencies
# ==============================================================================

function(need_sqlite3)
  find_package(SQLite3 3.35 QUIET)

  if(SQLite3_FOUND)
    message(STATUS "Found system SQLite3: ${SQLite3_LIBRARIES}")

    # Create alias for system SQLite3 if it doesn't exist
    if(NOT TARGET SQLite::SQLite3)
      # Create imported target for system SQLite3
      add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
      set_target_properties(
        SQLite::SQLite3
        PROPERTIES IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
                   INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
    endif()

    if(NOT TARGET SQLite::SQLite3_static)
      add_library(SQLite::SQLite3_static ALIAS SQLite::SQLite3)
    endif()

    # Set variables in parent scope so they persist outside the function
    set(SQLite3_FOUND
        ${SQLite3_FOUND}
        PARENT_SCOPE)
    set(SQLite3_LIBRARIES
        ${SQLite3_LIBRARIES}
        PARENT_SCOPE)
    set(SQLite3_INCLUDE_DIRS
        ${SQLite3_INCLUDE_DIRS}
        PARENT_SCOPE)
    set(SQLite3_CPM
        FALSE
        PARENT_SCOPE)
  else()
    # Build with CPM
    if(NOT SQLite3_ADDED)
      cpmaddpackage(
        NAME
        SQLite3
        URL
        https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        VERSION
        3.46.1
        DOWNLOAD_ONLY
        YES)
    endif()

    if(SQLite3_ADDED)
      message(STATUS "Built SQLite3 with CPM")

      set(SQLITE3_TARGETS)

      # Create sqlite3 library from amalgamation
      if(DFTRACER_UTILS_BUILD_SHARED)
        add_library(sqlite3_shared SHARED ${SQLite3_SOURCE_DIR}/sqlite3.c)
        target_include_directories(
          sqlite3_shared
          PUBLIC $<BUILD_INTERFACE:${SQLite3_SOURCE_DIR}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

        # Enable common SQLite features
        target_compile_definitions(
          sqlite3_shared PUBLIC SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1
                                SQLITE_ENABLE_RTREE SQLITE_THREADSAFE=1)

        if(NOT WIN32)
          target_link_libraries(sqlite3_shared PRIVATE pthread dl m)
        endif()

        set_target_properties(
          sqlite3_shared
          PROPERTIES OUTPUT_NAME sqlite3
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(SQLite::SQLite3 ALIAS sqlite3_shared)
        list(APPEND SQLITE3_TARGETS sqlite3_shared)
        message(STATUS "Added SQLite3 shared library")
      endif()

      if(DFTRACER_UTILS_BUILD_STATIC)
        add_library(sqlite3_static STATIC ${SQLite3_SOURCE_DIR}/sqlite3.c)
        target_include_directories(
          sqlite3_static
          PUBLIC $<BUILD_INTERFACE:${SQLite3_SOURCE_DIR}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

        # Enable common SQLite features
        target_compile_definitions(
          sqlite3_static PUBLIC SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1
                                SQLITE_ENABLE_RTREE SQLITE_THREADSAFE=1)

        if(NOT WIN32)
          target_link_libraries(sqlite3_static PRIVATE pthread dl m)
        endif()

        set_target_properties(
          sqlite3_static
          PROPERTIES OUTPUT_NAME sqlite3
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(SQLite::SQLite3_static ALIAS sqlite3_static)
        list(APPEND SQLITE3_TARGETS sqlite3_static)
        message(STATUS "Added SQLite3 static library")

        # If only static is built, make it the default alias
        if(NOT DFTRACER_UTILS_BUILD_SHARED)
          add_library(SQLite::SQLite3 ALIAS sqlite3_static)
        endif()
      endif()

      # Make sqlite3 installable
      if(SQLITE3_TARGETS)
        install(
          TARGETS ${SQLITE3_TARGETS}
          EXPORT sqlite3Targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()

      # Install sqlite3 header
      install(FILES ${SQLite3_SOURCE_DIR}/sqlite3.h
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      # Install the export set
      install(
        EXPORT sqlite3Targets
        FILE sqlite3Targets.cmake
        NAMESPACE SQLite::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sqlite3)

      set(SQLite3_CPM
          TRUE
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Function to link SQLite3 to a target Parameters: TARGET_NAME - name of the
# target to link SQLite3 to
function(link_sqlite3 TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_sqlite3: TARGET_NAME is required")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_sqlite3: Target '${TARGET_NAME}' does not exist")
  endif()

  # Check if any SQLite3 variant is available
  set(SQLITE3_AVAILABLE FALSE)

  # Check for CPM-built SQLite3
  if(TARGET sqlite3_shared OR TARGET sqlite3_static)
    set(SQLITE3_AVAILABLE TRUE)
  endif()

  # Check for system SQLite3
  if(TARGET SQLite::SQLite3)
    set(SQLITE3_AVAILABLE TRUE)
  endif()

  if(NOT SQLITE3_AVAILABLE)
    message(
      FATAL_ERROR
        "link_sqlite3: No SQLite3 found! Call need_sqlite3() first or ensure system SQLite3 is available."
    )
  endif()

  # Link appropriate SQLite3 variant Use PUBLIC linkage since sqlite3.h is
  # included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static SQLite3
    if(TARGET sqlite3_static)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3_static)
      message(
        STATUS "Linked ${TARGET_NAME} to CPM-built SQLite::SQLite3_static")
    elseif(TARGET SQLite::SQLite3_static)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3_static)
      message(STATUS "Linked ${TARGET_NAME} to SQLite::SQLite3_static")
    elseif(TARGET sqlite3_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3)
      message(
        STATUS "Linked ${TARGET_NAME} to CPM-built SQLite::SQLite3 (shared)")
    elseif(TARGET SQLite::SQLite3)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3)
      message(STATUS "Linked ${TARGET_NAME} to SQLite::SQLite3")
    endif()
  else()
    # For shared libraries, prefer shared SQLite3
    if(TARGET sqlite3_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3)
      message(
        STATUS "Linked ${TARGET_NAME} to CPM-built SQLite::SQLite3 (shared)")
    elseif(TARGET SQLite::SQLite3)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3)
      message(STATUS "Linked ${TARGET_NAME} to SQLite::SQLite3")
    elseif(TARGET sqlite3_static)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3_static)
      message(
        STATUS "Linked ${TARGET_NAME} to CPM-built SQLite::SQLite3_static")
    elseif(TARGET SQLite::SQLite3_static)
      target_link_libraries(${TARGET_NAME} PUBLIC SQLite::SQLite3_static)
      message(STATUS "Linked ${TARGET_NAME} to SQLite::SQLite3_static")
    endif()
  endif()
endfunction()

# ==============================================================================
# Compression Dependencies
# ==============================================================================

function(need_zlib)
  find_package(ZLIB 1.2 QUIET)

  if(ZLIB_FOUND)
    message(STATUS "Found system ZLIB: ${ZLIB_LIBRARIES}")

    # Set variables in parent scope so they persist outside the function
    set(ZLIB_FOUND
        ${ZLIB_FOUND}
        PARENT_SCOPE)
    set(ZLIB_LIBRARIES
        ${ZLIB_LIBRARIES}
        PARENT_SCOPE)
    set(ZLIB_INCLUDE_DIRS
        ${ZLIB_INCLUDE_DIRS}
        PARENT_SCOPE)
    set(ZLIB_CPM
        FALSE
        PARENT_SCOPE)
  else()
    set(ZLIB_CPM
        FALSE
        PARENT_SCOPE)
    # Build with CPM
    cpmaddpackage(
      NAME
      ZLIB
      GITHUB_REPOSITORY
      madler/zlib
      VERSION
      1.3.1
      OPTIONS
      "ZLIB_BUILD_STATIC OFF"
      "ZLIB_BUILD_SHARED ON"
      "ZLIB_INSTALL OFF"
      "ZLIB_BUILD_EXAMPLES OFF"
      DOWNLOAD_ONLY
      YES)

    if(ZLIB_ADDED)
      message(STATUS "Built ZLIB with CPM")
      set(ZLIB_CPM
          TRUE
          PARENT_SCOPE)

      # Make sure the source and binary directories are available in parent
      # scope
      set(ZLIB_SOURCE_DIR
          ${ZLIB_SOURCE_DIR}
          PARENT_SCOPE)
      set(ZLIB_BINARY_DIR
          ${ZLIB_BINARY_DIR}
          PARENT_SCOPE)

      # Create our own zlib targets with proper install interface and fix macOS
      # issues
      set(ZLIB_TARGETS)

      if(APPLE)
        # Create patched source files for macOS to fix type conflicts
        set(ZLIB_SOURCES_PATCHED "")
        foreach(src_file adler32.c crc32.c)
          file(READ "${ZLIB_SOURCE_DIR}/${src_file}" SRC_CONTENT)
          # Replace z_off64_t parameter with z_off_t in function definitions to
          # match declarations
          string(REGEX REPLACE "z_off64_t len2\\)" "z_off_t len2)"
                               SRC_CONTENT_FIXED "${SRC_CONTENT}")
          string(REGEX REPLACE "z_off64_t len2\\s*\\{" "z_off_t len2 {"
                               SRC_CONTENT_FIXED "${SRC_CONTENT_FIXED}")
          file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${src_file}"
               "${SRC_CONTENT_FIXED}")
          list(APPEND ZLIB_SOURCES_PATCHED
               "${CMAKE_CURRENT_BINARY_DIR}/${src_file}")
        endforeach()

        # Add the rest of the files normally
        foreach(
          src_file
          compress.c
          deflate.c
          gzclose.c
          gzlib.c
          gzread.c
          gzwrite.c
          inflate.c
          infback.c
          inftrees.c
          inffast.c
          trees.c
          uncompr.c
          zutil.c)
          list(APPEND ZLIB_SOURCES_PATCHED "${ZLIB_SOURCE_DIR}/${src_file}")
        endforeach()

        if(DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer_zlib_shared SHARED ${ZLIB_SOURCES_PATCHED})
          list(APPEND ZLIB_TARGETS dftracer_zlib_shared)
        endif()
        if(DFTRACER_UTILS_BUILD_STATIC)
          add_library(dftracer_zlib_static STATIC ${ZLIB_SOURCES_PATCHED})
          list(APPEND ZLIB_TARGETS dftracer_zlib_static)
        endif()
      else()
        set(ZLIB_SOURCES
            ${ZLIB_SOURCE_DIR}/adler32.c
            ${ZLIB_SOURCE_DIR}/compress.c
            ${ZLIB_SOURCE_DIR}/crc32.c
            ${ZLIB_SOURCE_DIR}/deflate.c
            ${ZLIB_SOURCE_DIR}/gzclose.c
            ${ZLIB_SOURCE_DIR}/gzlib.c
            ${ZLIB_SOURCE_DIR}/gzread.c
            ${ZLIB_SOURCE_DIR}/gzwrite.c
            ${ZLIB_SOURCE_DIR}/inflate.c
            ${ZLIB_SOURCE_DIR}/infback.c
            ${ZLIB_SOURCE_DIR}/inftrees.c
            ${ZLIB_SOURCE_DIR}/inffast.c
            ${ZLIB_SOURCE_DIR}/trees.c
            ${ZLIB_SOURCE_DIR}/uncompr.c
            ${ZLIB_SOURCE_DIR}/zutil.c)

        if(DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer_zlib_shared SHARED ${ZLIB_SOURCES})
          list(APPEND ZLIB_TARGETS dftracer_zlib_shared)
        endif()
        if(DFTRACER_UTILS_BUILD_STATIC)
          add_library(dftracer_zlib_static STATIC ${ZLIB_SOURCES})
          list(APPEND ZLIB_TARGETS dftracer_zlib_static)
        endif()
      endif()

      # Fix type mismatch issues on macOS by ensuring consistent type
      # definitions
      foreach(zlib_target ${ZLIB_TARGETS})
        if(APPLE)
          target_compile_definitions(
            ${zlib_target} PRIVATE _LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64)
        else()
          target_compile_definitions(
            ${zlib_target} PRIVATE _LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64
                                   Z_HAVE_STDARG_H=1)
        endif()

        # Set proper include directories for build and install
        target_include_directories(
          ${zlib_target}
          PUBLIC $<BUILD_INTERFACE:${ZLIB_SOURCE_DIR}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

        # Copy the generated zconf.h from the original zlib build
        if(EXISTS "${ZLIB_BINARY_DIR}/zconf.h")
          configure_file("${ZLIB_BINARY_DIR}/zconf.h"
                         "${CMAKE_CURRENT_BINARY_DIR}/zconf.h" COPYONLY)
          target_include_directories(
            ${zlib_target}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
        endif()
      endforeach()

      # Set output names and create aliases
      if(DFTRACER_UTILS_BUILD_SHARED)
        set_target_properties(
          dftracer_zlib_shared
          PROPERTIES OUTPUT_NAME dftracer_zlib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(dftracer::zlib ALIAS dftracer_zlib_shared)
        message(STATUS "Added dftracer_zlib shared library")
      endif()

      if(DFTRACER_UTILS_BUILD_STATIC)
        set_target_properties(
          dftracer_zlib_static
          PROPERTIES OUTPUT_NAME dftracer_zlib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(dftracer::zlibstatic ALIAS dftracer_zlib_static)
        message(STATUS "Added dftracer_zlib static library")
        # If only static is built, make it the default alias
        if(NOT DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer::zlib ALIAS dftracer_zlib_static)
        endif()
      endif()

      # Install our custom zlib targets
      if(ZLIB_TARGETS)
        install(
          TARGETS ${ZLIB_TARGETS}
          EXPORT ZlibTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()

      install(
        EXPORT ZlibTargets
        FILE ZlibTargets.cmake
        NAMESPACE dftracer::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib)

      # Install zlib headers manually
      if(ZLIB_SOURCE_DIR AND ZLIB_BINARY_DIR)
        if(EXISTS "${ZLIB_SOURCE_DIR}/zlib.h")
          install(FILES "${ZLIB_SOURCE_DIR}/zlib.h"
                  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        endif()

        if(EXISTS "${ZLIB_BINARY_DIR}/zconf.h")
          install(FILES "${ZLIB_BINARY_DIR}/zconf.h"
                  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        endif()
      endif()

      # Make zlib available in parent scope for Arrow - let Arrow build its own
      if(DFTRACER_UTILS_BUILD_SHARED)
        set(ZLIB_LIBRARIES
            dftracer_zlib_shared
            PARENT_SCOPE)
      elseif(DFTRACER_UTILS_BUILD_STATIC)
        set(ZLIB_LIBRARIES
            dftracer_zlib_static
            PARENT_SCOPE)
      endif()
      set(ZLIB_INCLUDE_DIRS
          ${ZLIB_SOURCE_DIR} ${ZLIB_BINARY_DIR}
          PARENT_SCOPE)
      # Don't set ZLIB_FOUND to let Arrow build its own zlib
      set(ZLIB_FOUND
          FALSE
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(link_zlib TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_zlib: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_zlib: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_zlib: Target '${TARGET_NAME}' does not exist")
  endif()

  # Check if any zlib variant is available
  set(ZLIB_AVAILABLE FALSE)
  if(TARGET dftracer_zlib_static
     OR TARGET dftracer_zlib_shared
     OR ZLIB_FOUND)
    set(ZLIB_AVAILABLE TRUE)
  endif()

  if(NOT ZLIB_AVAILABLE)
    message(
      FATAL_ERROR
        "link_zlib: No zlib found! Call need_zlib() first or ensure system zlib is available."
    )
  endif()

  # Link appropriate zlib variant Use PUBLIC linkage since zlib headers may be
  # included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static zlib if available
    if(TARGET dftracer_zlib_static)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlibstatic)
      message(STATUS "Linked ${TARGET_NAME} to dftracer zlibstatic")
    elseif(TARGET dftracer_zlib_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlib)
      message(STATUS "Linked ${TARGET_NAME} to dftracer zlib (shared)")
    elseif(ZLIB_FOUND)
      target_link_libraries(${TARGET_NAME} PUBLIC ZLIB::ZLIB)
      message(STATUS "Linked ${TARGET_NAME} to system ZLIB::ZLIB")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared zlib if available
    if(TARGET dftracer_zlib_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlib)
      message(STATUS "Linked ${TARGET_NAME} to dftracer zlib (shared)")
    elseif(TARGET dftracer_zlib_static)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlibstatic)
      message(STATUS "Linked ${TARGET_NAME} to dftracer zlibstatic")
    elseif(ZLIB_FOUND)
      target_link_libraries(${TARGET_NAME} PUBLIC ZLIB::ZLIB)
      message(STATUS "Linked ${TARGET_NAME} to system ZLIB::ZLIB")
    endif()
  endif()
endfunction()

# ==============================================================================
# Hashing and Cryptography Dependencies
# ==============================================================================

function(link_yyjson TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_yyjson: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_yyjson: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_yyjson: Target '${TARGET_NAME}' does not exist")
  endif()

  # Link appropriate yyjson variant Use PUBLIC linkage since yyjson headers may
  # be included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static yyjson if available
    if(TARGET yyjson_static)
      target_link_libraries(${TARGET_NAME} PUBLIC yyjson::yyjson_static)
      message(STATUS "Linked ${TARGET_NAME} to yyjson_static")
    elseif(TARGET yyjson_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC yyjson::yyjson)
      message(STATUS "Linked ${TARGET_NAME} to yyjson (shared)")
    else()
      message(
        FATAL_ERROR "link_yyjson: No yyjson found! Call need_yyjson() first.")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared yyjson if available
    if(TARGET yyjson_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC yyjson::yyjson)
      message(STATUS "Linked ${TARGET_NAME} to yyjson (shared)")
    elseif(TARGET yyjson_static)
      target_link_libraries(${TARGET_NAME} PUBLIC yyjson::yyjson_static)
      message(STATUS "Linked ${TARGET_NAME} to yyjson_static")
    else()
      message(
        FATAL_ERROR "link_yyjson: No yyjson found! Call need_yyjson() first.")
    endif()
  endif()
endfunction()

function(need_picosha2)
  if(NOT PicoSHA2_ADDED)
    cpmaddpackage(
      NAME
      PicoSHA2
      GITHUB_REPOSITORY
      okdshin/PicoSHA2
      VERSION
      1.0.1
      GIT_TAG
      "v1.0.1"
      DOWNLOAD_ONLY
      YES)

    if(PicoSHA2_ADDED)
      add_library(picosha2 INTERFACE)
      target_include_directories(
        picosha2 INTERFACE $<BUILD_INTERFACE:${PicoSHA2_SOURCE_DIR}>
                           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      install(FILES ${PicoSHA2_SOURCE_DIR}/picosha2.h
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
      message(STATUS "Added picosha2 header-only library")
    endif()
  endif()
endfunction()

# ==============================================================================
# Concurrency Dependencies
# ==============================================================================

function(need_readerwriterqueue)
  if(NOT readerwriterqueue_ADDED)
    cpmaddpackage(
      NAME
      readerwriterqueue
      GITHUB_REPOSITORY
      cameron314/readerwriterqueue
      GIT_TAG
      211616e0554f93152ab3108b8d93fbc23174a9d9
      DOWNLOAD_ONLY
      YES)

    if(readerwriterqueue_ADDED)
      add_library(readerwriterqueue INTERFACE)
      target_include_directories(
        readerwriterqueue
        INTERFACE $<BUILD_INTERFACE:${readerwriterqueue_SOURCE_DIR}>
                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      install(
        FILES ${readerwriterqueue_SOURCE_DIR}/readerwriterqueue.h
              ${readerwriterqueue_SOURCE_DIR}/atomicops.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      # Install and export the target
      install(TARGETS readerwriterqueue EXPORT readerwriterqueueTargets)
      install(
        EXPORT readerwriterqueueTargets
        FILE readerwriterqueueTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/readerwriterqueue)

      message(STATUS "Added readerwriterqueue header-only library")
    endif()
  endif()
endfunction()

function(need_concurrentqueue)
  if(NOT concurrentqueue_ADDED)
    cpmaddpackage(
      NAME
      concurrentqueue
      GITHUB_REPOSITORY
      cameron314/concurrentqueue
      GIT_TAG
      c68072129c8a5b4025122ca5a0c82ab14b30cb03
      DOWNLOAD_ONLY
      YES)

    if(concurrentqueue_ADDED)
      add_library(concurrentqueue INTERFACE)
      target_include_directories(
        concurrentqueue INTERFACE $<BUILD_INTERFACE:${concurrentqueue_SOURCE_DIR}>
                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      install(
        FILES ${concurrentqueue_SOURCE_DIR}/concurrentqueue.h
              ${concurrentqueue_SOURCE_DIR}/blockingconcurrentqueue.h
              ${concurrentqueue_SOURCE_DIR}/lightweightsemaphore.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      # Install and export the target
      install(TARGETS concurrentqueue EXPORT concurrentqueueTargets)
      install(
        EXPORT concurrentqueueTargets
        FILE concurrentqueueTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/concurrentqueue)

      message(STATUS "Added concurrentqueue header-only library")
    endif()
  endif()
endfunction()

# ==============================================================================
# Arrow Data Interface Dependencies (nanoarrow)
# ==============================================================================

function(need_nanoarrow)
  if(NOT nanoarrow_ADDED)
    cpmaddpackage(
      NAME
      nanoarrow
      GITHUB_REPOSITORY
      apache/arrow-nanoarrow
      VERSION
      0.8.0
      GIT_TAG
      "apache-arrow-nanoarrow-0.8.0"
      DOWNLOAD_ONLY
      YES)
  endif()

  if(nanoarrow_ADDED)
    set(NANOARROW_SOVERSION 0)
    set(NANOARROW_SOURCES
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/array.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/array_stream.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/schema.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/utils.c)
    set(NANOARROW_TARGETS)
    set(NANOARROW_IPC_INCLUDE_DIRS)

    # Arrow IPC support (reader/writer for .arrows files)
    if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
      list(
        APPEND
        NANOARROW_SOURCES
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/decoder.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/encoder.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/reader.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/writer.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/codecs.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/builder.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/emitter.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/refmap.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/verifier.c)
      set(NANOARROW_FLATCC_INCLUDE
          ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/include)
      message(STATUS "nanoarrow IPC support enabled (reader + writer)")
    endif()

    # Generate nanoarrow_config.h from template
    set(NANOARROW_VERSION_MAJOR 0)
    set(NANOARROW_VERSION_MINOR 8)
    set(NANOARROW_VERSION_PATCH 0)
    set(NANOARROW_VERSION "0.8.0")
    set(NANOARROW_NAMESPACE_DEFINE "")
    configure_file(
      ${nanoarrow_SOURCE_DIR}/src/nanoarrow/nanoarrow_config.h.in
      ${CMAKE_CURRENT_BINARY_DIR}/nanoarrow/nanoarrow_config.h)

    if(DFTRACER_UTILS_BUILD_STATIC)
      add_library(nanoarrow_static STATIC ${NANOARROW_SOURCES})
      target_include_directories(
        nanoarrow_static
        PUBLIC $<BUILD_INTERFACE:${nanoarrow_SOURCE_DIR}/src>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_include_directories(
          nanoarrow_static
          PUBLIC $<BUILD_INTERFACE:${NANOARROW_FLATCC_INCLUDE}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      endif()
      target_compile_definitions(nanoarrow_static
                                 PUBLIC DFTRACER_UTILS_ENABLE_ARROW)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_compile_definitions(nanoarrow_static
                                   PUBLIC DFTRACER_UTILS_ENABLE_ARROW_IPC)
      endif()
      set_target_properties(
        nanoarrow_static
        PROPERTIES VERSION ${PROJECT_VERSION}
                   SOVERSION ${NANOARROW_SOVERSION}
                   OUTPUT_NAME nanoarrow
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
      add_library(nanoarrow::nanoarrow_static ALIAS nanoarrow_static)
      list(APPEND NANOARROW_TARGETS nanoarrow_static)
      message(STATUS "Added nanoarrow static library")
    endif()

    if(DFTRACER_UTILS_BUILD_SHARED)
      add_library(nanoarrow_shared SHARED ${NANOARROW_SOURCES})
      target_include_directories(
        nanoarrow_shared
        PUBLIC $<BUILD_INTERFACE:${nanoarrow_SOURCE_DIR}/src>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_include_directories(
          nanoarrow_shared
          PUBLIC $<BUILD_INTERFACE:${NANOARROW_FLATCC_INCLUDE}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      endif()
      target_compile_definitions(nanoarrow_shared
                                 PUBLIC DFTRACER_UTILS_ENABLE_ARROW)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_compile_definitions(nanoarrow_shared
                                   PUBLIC DFTRACER_UTILS_ENABLE_ARROW_IPC)
      endif()
      set_target_properties(
        nanoarrow_shared
        PROPERTIES VERSION ${PROJECT_VERSION}
                   SOVERSION ${NANOARROW_SOVERSION}
                   OUTPUT_NAME nanoarrow
                   LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
      add_library(nanoarrow::nanoarrow ALIAS nanoarrow_shared)
      list(APPEND NANOARROW_TARGETS nanoarrow_shared)
      message(STATUS "Added nanoarrow shared library")
    elseif(DFTRACER_UTILS_BUILD_STATIC)
      add_library(nanoarrow::nanoarrow ALIAS nanoarrow_static)
    endif()

    # Install headers
    install(
      DIRECTORY ${nanoarrow_SOURCE_DIR}/src/nanoarrow/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nanoarrow
      FILES_MATCHING
      PATTERN "*.h"
      PATTERN "*.hpp"
      PATTERN "testing" EXCLUDE
      PATTERN "integration" EXCLUDE
      PATTERN "device" EXCLUDE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nanoarrow/nanoarrow_config.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nanoarrow)
    if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
      install(
        DIRECTORY ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/include/flatcc/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flatcc
        FILES_MATCHING
        PATTERN "*.h")
    endif()

    # Suppress warnings from nanoarrow headers (redundant redeclarations,
    # shadow warnings in nanoarrow 0.8.0 internal headers)
    foreach(_na_target ${NANOARROW_TARGETS})
      get_target_property(_na_inc ${_na_target} INTERFACE_INCLUDE_DIRECTORIES)
      if(_na_inc)
        set_target_properties(${_na_target} PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_na_inc}")
      endif()
    endforeach()

    if(NANOARROW_TARGETS)
      install(
        TARGETS ${NANOARROW_TARGETS}
        EXPORT nanoarrowTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

      install(
        EXPORT nanoarrowTargets
        FILE nanoarrowTargets.cmake
        NAMESPACE nanoarrow::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanoarrow)
    endif()

    message(STATUS "Added nanoarrow 0.8.0 via CPM")
  endif()
endfunction()

function(link_nanoarrow TARGET_NAME LIBRARY_TYPE)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_nanoarrow: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR
        "link_nanoarrow: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_nanoarrow: Target '${TARGET_NAME}' does not exist")
  endif()

  if(LIBRARY_TYPE STREQUAL "STATIC")
    if(TARGET nanoarrow_static)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow_static)
      message(STATUS "Linked ${TARGET_NAME} to nanoarrow_static")
    elseif(TARGET nanoarrow_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow)
      message(STATUS "Linked ${TARGET_NAME} to nanoarrow (shared)")
    else()
      message(
        FATAL_ERROR
          "link_nanoarrow: No nanoarrow found! Call need_nanoarrow() first.")
    endif()
  else()
    if(TARGET nanoarrow_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow)
      message(STATUS "Linked ${TARGET_NAME} to nanoarrow (shared)")
    elseif(TARGET nanoarrow_static)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow_static)
      message(STATUS "Linked ${TARGET_NAME} to nanoarrow_static")
    else()
      message(
        FATAL_ERROR
          "link_nanoarrow: No nanoarrow found! Call need_nanoarrow() first.")
    endif()
  endif()

endfunction()

# ==============================================================================
# Testing Dependencies
# ==============================================================================

function(need_test_deps)
  cpmaddpackage(NAME doctest GITHUB_REPOSITORY doctest/doctest VERSION 2.4.11)

  cpmaddpackage(
    NAME
    unity
    GITHUB_REPOSITORY
    ThrowTheSwitch/Unity
    VERSION
    2.6.0
    DOWNLOAD_ONLY
    YES)

  if(TARGET unity)
    add_library(unity_lib ALIAS unity)
  else()
    add_library(unity_lib STATIC ${unity_SOURCE_DIR}/src/unity.c)
    target_include_directories(unity_lib PUBLIC ${unity_SOURCE_DIR}/src)
  endif()
endfunction()

# ==============================================================================
# Compiler Feature Checks and Helpers
# ==============================================================================

macro(check_std_filesystem)
  try_compile(
    DFTRACER_UTILS_HAS_STD_FILESYSTEM "${CMAKE_BINARY_DIR}/temp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/has_filesystem.cpp"
    CMAKE_FLAGS ${CMAKE_CXX_FLAGS}
    LINK_LIBRARIES stdc++fs)
  if(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
    message(STATUS "Compiler has std::filesystem support")
  else()
    message(
      STATUS
        "Compiler does not have std::filesystem support. Use gulrak::filesystem"
    )
  endif(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
endmacro()

function(add_stdfs_if_needed TARGET)
  if(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
    target_link_libraries(${TARGET} PRIVATE stdc++fs)
  endif()
endfunction()
