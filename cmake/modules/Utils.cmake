function(target_enable_coverage target ENABLE_COVERAGE)
  if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${target} PRIVATE --coverage)
      target_compile_options(${target} PRIVATE --coverage -fprofile-arcs
                                               -ftest-coverage)
    elseif(MSVC)
      target_compile_options(${target} PRIVATE /LTCG:incremental /Zi)
    endif()
  endif()
endfunction()

function(set_coverage_compiler_flags ENABLE_COVERAGE)
  if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_DEBUG
        "${CMAKE_CXX_FLAGS_DEBUG} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_C_FLAGS_DEBUG
        "${CMAKE_C_FLAGS_DEBUG} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG
        "${CMAKE_EXE_LINKER_FLAGS_DEBUG} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG
        "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} --coverage")
  endif()
endfunction()

function(create_venv_symlink target_name)
  file(
    GENERATE
    OUTPUT ${CMAKE_BINARY_DIR}/symlink_${target_name}.sh
    CONTENT
      "echo -- Installing: symlink ${CMAKE_INSTALL_VENV_BIN_DIR}/$<TARGET_FILE_NAME:${target_name}> from ${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:${target_name}>;ln -sf ${SKBUILD_PLATLIB_DIR}/${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:${target_name}> ${CMAKE_INSTALL_VENV_BIN_DIR}/$<TARGET_FILE_NAME:${target_name}>"
  )
  install(
    CODE "execute_process(
                COMMAND bash -c \"set -e
                mkdir -p ${CMAKE_INSTALL_VENV_BIN_DIR}
                chmod +x ${CMAKE_BINARY_DIR}/symlink_${target_name}.sh
                . ${CMAKE_BINARY_DIR}/symlink_${target_name}.sh
                \")")
endfunction()

function(print_all_variables)
  message(STATUS "CMake Variables:")
  get_cmake_property(_variableNames VARIABLES)
  list(SORT _variableNames)

  foreach(_variableName ${_variableNames})
    message(STATUS "${_variableName}=${${_variableName}}")
  endforeach()
endfunction()
