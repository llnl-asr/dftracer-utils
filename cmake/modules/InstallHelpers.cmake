# SPDX-License-Identifier: MIT
# InstallHelpers.cmake - Utility for creating pkg-config files

include(GNUInstallDirs)

#[=[
Creates and installs a pkg-config file for a target.
#]=]
function(create_pkgconfig_file)
  set(options "")
  set(oneValueArgs
      TARGET
      NAME
      VERSION
      DESCRIPTION
      URL
      REQUIRES
      LIBS_PRIVATE
      CFLAGS_PRIVATE)
  set(multiValueArgs "")

  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # Use NAME if provided, otherwise use TARGET for both name and library
  if(NOT PKG_NAME)
    set(PKG_NAME ${PKG_TARGET})
  endif()

  # Generate pkg-config file content
  set(PC_CONTENT "")
  string(APPEND PC_CONTENT "prefix=${CMAKE_INSTALL_PREFIX}\n")
  string(APPEND PC_CONTENT "exec_prefix=\${prefix}\n")
  string(APPEND PC_CONTENT "libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}\n")
  string(APPEND PC_CONTENT
         "includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n")
  string(APPEND PC_CONTENT "\n")
  string(APPEND PC_CONTENT "Name: ${PKG_NAME}\n")
  string(APPEND PC_CONTENT "Description: ${PKG_DESCRIPTION}\n")
  string(APPEND PC_CONTENT "Version: ${PKG_VERSION}\n")

  if(PKG_URL)
    string(APPEND PC_CONTENT "URL: ${PKG_URL}\n")
  endif()

  if(PKG_REQUIRES)
    string(APPEND PC_CONTENT "Requires: ${PKG_REQUIRES}\n")
  endif()

  if(PKG_LIBS_PRIVATE)
    string(APPEND PC_CONTENT "Libs.private: ${PKG_LIBS_PRIVATE}\n")
  endif()

  if(PKG_CFLAGS_PRIVATE)
    string(APPEND PC_CONTENT "Cflags.private: ${PKG_CFLAGS_PRIVATE}\n")
  endif()

  string(APPEND PC_CONTENT "Libs: -L\${libdir} -l${PKG_TARGET}\n")
  string(APPEND PC_CONTENT "Cflags: -I\${includedir}\n")

  # Write pkg-config file using NAME for the filename
  set(PC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}.pc")
  file(WRITE ${PC_FILE} ${PC_CONTENT})

  # Install pkg-config file
  install(FILES ${PC_FILE} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()
