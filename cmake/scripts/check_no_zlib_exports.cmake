# Fails when a shared library of ours exports a zlib or zlib-ng symbol. The
# zlib-ng archive is linked with hidden visibility, so a process that also
# loads another zlib binds none of its own calls to ours.
#
# Usage: cmake "-DLIBS=<lib;lib>" -DNM=<nm> -DSUFFIX=<.so|.dylib> -P <this file>

if(NOT LIBS OR NOT NM OR NOT SUFFIX)
  message(FATAL_ERROR "check_no_zlib_exports: LIBS, NM and SUFFIX are required")
endif()
set(libs ${LIBS})

if(SUFFIX STREQUAL ".dylib")
  set(nm_args -gU)
else()
  set(nm_args -D --defined-only)
endif()

set(zlib_name
    "^(zng_|zlibng_|z_errmsg$|gz[a-z0-9_]*$|(inflate|deflate)[A-Za-z0-9_]*$|crc32(_z|_combine[a-z0-9_]*)?$|adler32[a-z0-9_]*$|compress[0-9]?$|compressBound$|uncompress[0-9]?$|zlibVersion$|zlibCompileFlags$|zError$|get_crc_table$)"
)

set(found "")
foreach(lib IN LISTS libs)
  if(NOT EXISTS "${lib}")
    message(FATAL_ERROR "check_no_zlib_exports: ${lib} is not built")
  endif()
  execute_process(
    COMMAND ${NM} ${nm_args} "${lib}"
    OUTPUT_VARIABLE out
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check_no_zlib_exports: ${NM} failed on ${lib}")
  endif()
  string(REPLACE "\n" ";" lines "${out}")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^.* " "" sym "${line}")
    string(REGEX REPLACE "^_" "" sym "${sym}")
    if(sym MATCHES "${zlib_name}")
      list(APPEND found "${lib}: ${sym}")
    endif()
  endforeach()
endforeach()

if(found)
  list(JOIN found "\n  " text)
  message(FATAL_ERROR "zlib symbols exported:\n  ${text}")
endif()
list(LENGTH libs count)
message(STATUS "no zlib symbol exported by ${count} libraries: ${libs}")
