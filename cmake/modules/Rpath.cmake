macro(add_rpath)
  set(CMAKE_SKIP_BUILD_RPATH OFF)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
  list(REMOVE_DUPLICATES DEPENDENCY_LIBRARY_DIRS)

  if(APPLE)
    # macOS uses @loader_path
    set(CMAKE_INSTALL_RPATH
        "@loader_path/../lib"
        "@loader_path/../../lib"
        "@loader_path/../lib64"
        "@loader_path/../../lib64"
        "@executable_path/../lib"
        "@executable_path/../../lib"
        "@executable_path/../lib64"
        "@executable_path/../../lib64"
        "${DEPENDENCY_LIBRARY_DIRS}")
    if(SKBUILD)
      set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    else()
      set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
    endif()
    set(CMAKE_MACOSX_RPATH ON)
  else()
    # Linux uses $ORIGIN
    set(CMAKE_INSTALL_RPATH
        "$ORIGIN/../lib" "$ORIGIN/../../lib" "$ORIGIN/../lib64"
        "$ORIGIN/../../lib64" "${DEPENDENCY_LIBRARY_DIRS}")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  endif()

  set(CMAKE_BUILD_RPATH "${DEPENDENCY_LIBRARY_DIRS}")
endmacro()
