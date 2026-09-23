macro(add_rpath)
  set(CMAKE_SKIP_BUILD_RPATH OFF)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
  list(REMOVE_DUPLICATES DEPENDENCY_LIBRARY_DIRS)

  if(APPLE)
    # macOS uses @loader_path
    set(CMAKE_INSTALL_RPATH
        "@loader_path/../lib"
        "@loader_path/../../lib"
        "@loader_path/../../../lib"
        "@loader_path/../lib64"
        "@loader_path/../../lib64"
        "@loader_path/../../../lib64"
        "@executable_path/../lib"
        "@executable_path/../../lib"
        "@executable_path/../lib64"
        "@executable_path/../../lib64"
        "${DEPENDENCY_LIBRARY_DIRS}")
    # Always use install RPATH in build tree so binaries find
    # their shared libraries when invoked via popen() or other
    # contexts where DYLD_LIBRARY_PATH is stripped by SIP.
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    set(CMAKE_MACOSX_RPATH ON)
  else()
    # Linux uses $ORIGIN
    set(CMAKE_INSTALL_RPATH
        "$ORIGIN/../lib" "$ORIGIN/../../lib" "$ORIGIN/../../../lib"
        "$ORIGIN/../lib64" "$ORIGIN/../../lib64" "$ORIGIN/../../../lib64"
        "${DEPENDENCY_LIBRARY_DIRS}")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  endif()

  set(CMAKE_BUILD_RPATH "${DEPENDENCY_LIBRARY_DIRS}")
endmacro()

# Function to add rpath to a specific target
function(target_add_rpath TARGET_NAME)
  if(APPLE)
    set_target_properties(${TARGET_NAME} PROPERTIES
      INSTALL_RPATH "@loader_path/../lib;@loader_path/../../lib;@loader_path/../../../lib;@loader_path/../lib64;@loader_path/../../lib64;@loader_path/../../../lib64;@executable_path/../lib;@executable_path/../../lib;@executable_path/../lib64;@executable_path/../../lib64;${DEPENDENCY_LIBRARY_DIRS}"
      BUILD_RPATH "${DEPENDENCY_LIBRARY_DIRS}"
      MACOSX_RPATH ON
    )
  else()
    set_target_properties(${TARGET_NAME} PROPERTIES
      INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/../../lib;$ORIGIN/../../../lib;$ORIGIN/../lib64;$ORIGIN/../../lib64;$ORIGIN/../../../lib64;${DEPENDENCY_LIBRARY_DIRS}"
      BUILD_RPATH "${DEPENDENCY_LIBRARY_DIRS}"
      INSTALL_RPATH_USE_LINK_PATH ON
    )
  endif()
endfunction()
