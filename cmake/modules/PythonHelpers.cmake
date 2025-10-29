# PythonHelpers.cmake - Utilities for Python packaging with scikit-build-core

#[=[
Creates a shell wrapper script in venv/bin that calls the actual binary in site-packages.

This is useful for wheel builds where binaries are installed to site-packages/package/bin/
but need to be accessible from the virtualenv's bin directory.

Usage:
  create_python_wrapper(target_name)

Arguments:
  target_name - The name of the executable target to create a wrapper for

Example:
  create_python_wrapper(dftracer_reader)

This will:
1. Create a shell script that locates the real binary in site-packages
2. Install it to venv/bin/ with the same name as the binary
3. The wrapper forwards all arguments to the real binary using exec
#]=]
function(create_python_wrapper target_name)
  if(NOT SKBUILD)
    message(
      WARNING
        "create_python_wrapper called but SKBUILD is not set. Skipping wrapper creation."
    )
    return()
  endif()

  if(NOT DEFINED CMAKE_INSTALL_VENV_BIN_DIR)
    message(
      FATAL_ERROR
        "create_python_wrapper requires CMAKE_INSTALL_VENV_BIN_DIR to be set")
  endif()

  # Generate the shell wrapper script
  file(
    GENERATE
    OUTPUT ${CMAKE_BINARY_DIR}/venv_wrapper_${target_name}
    CONTENT
      "#!/bin/sh
# Wrapper script for ${target_name}
# Binary is in site-packages/dftracer/bin/, wrapper is in venv/bin/

# Get the directory of this wrapper script
wrapper_dir=\$(cd \"\$(dirname \"\$0\")\" && pwd)

# Find site-packages directory using glob pattern
for site_pkg in \"\$wrapper_dir\"/../lib/python*/site-packages; do
  if [ -d \"\$site_pkg\" ]; then
    break
  fi
done

# Look for the binary
binary=\"\$site_pkg/dftracer/bin/${target_name}\"

if [ ! -f \"\$binary\" ]; then
  echo \"Error: Could not find binary at \$binary\" >&2
  echo \"Wrapper location: \$wrapper_dir\" >&2
  echo \"Site-packages search: \$wrapper_dir/../lib/python*/site-packages\" >&2
  exit 1
fi

# Execute the binary with all arguments
exec \"\$binary\" \"\$@\"
")

  # Install the wrapper to venv/bin with the same name as the target
  install(
    PROGRAMS ${CMAKE_BINARY_DIR}/venv_wrapper_${target_name}
    DESTINATION ${CMAKE_INSTALL_VENV_BIN_DIR}
    RENAME ${target_name})

  message(
    STATUS
      "Created Python wrapper for ${target_name} -> ${CMAKE_INSTALL_VENV_BIN_DIR}/${target_name}"
  )
endfunction()
