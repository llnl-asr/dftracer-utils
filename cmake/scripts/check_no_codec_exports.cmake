# SPDX-License-Identifier: MIT
# Fails when a shared library of ours exports a zlib, zlib-ng or zstd symbol,
# or, outside macOS, a Vectorscan symbol.
# The zlib-ng and zstd archives are linked with hidden visibility, so a process
# that also loads another zlib or zstd binds none of its own calls to ours.
#
# Usage: cmake "-DLIBS=<lib;lib>" -DNM=<nm> -DSUFFIX=<.so|.dylib> -P <this file>

if(NOT LIBS OR NOT NM OR NOT SUFFIX)
  message(FATAL_ERROR "check_no_codec_exports: LIBS, NM and SUFFIX are required")
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

set(zstd_name "^(ZSTD_|ZSTDMT_|ZDICT_|HUF_|FSE_|HIST_|POOL_|ERR_getErrorString$)")

# The Vectorscan archive keeps its hs_* API default-visible; duql links it
# with --exclude-libs, which macOS ld does not have.
if(SUFFIX STREQUAL ".dylib")
  set(hs_name "")
else()
  set(hs_name "^(hs_|ch_)")
endif()

set(found "")
foreach(lib IN LISTS libs)
  if(NOT EXISTS "${lib}")
    message(FATAL_ERROR "check_no_codec_exports: ${lib} is not built")
  endif()
  execute_process(
    COMMAND ${NM} ${nm_args} "${lib}"
    OUTPUT_VARIABLE out
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check_no_codec_exports: ${NM} failed on ${lib}")
  endif()
  string(REPLACE "\n" ";" lines "${out}")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^.* " "" sym "${line}")
    string(REGEX REPLACE "^_" "" sym "${sym}")
    if(sym MATCHES "${zlib_name}"
       OR sym MATCHES "${zstd_name}"
       OR (hs_name AND sym MATCHES "${hs_name}"))
      list(APPEND found "${lib}: ${sym}")
    endif()
  endforeach()
endforeach()

if(found)
  list(JOIN found "\n  " text)
  message(FATAL_ERROR "codec or Vectorscan symbols exported:\n  ${text}")
endif()
list(LENGTH libs count)
message(STATUS "no zlib or zstd symbol exported by ${count} libraries: ${libs}")
