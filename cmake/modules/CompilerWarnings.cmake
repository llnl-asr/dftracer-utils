# CompilerWarnings.cmake Function to enable comprehensive compiler warnings for
# a target

function(target_set_warnings TARGET_NAME)
  # Parse optional arguments
  set(options WARNINGS_AS_ERRORS)
  set(oneValueArgs "")
  set(multiValueArgs "")
  cmake_parse_arguments(WARNINGS "${options}" "${oneValueArgs}"
                        "${multiValueArgs}" ${ARGN})

  if(MSVC)
    # MSVC warnings
    target_compile_options(
      ${TARGET_NAME}
      PRIVATE /W4 # Enable level 4 warnings
              /permissive- # Disable non-conforming code
              /w14242 # 'identifier': conversion from 'type1' to 'type1',
                      # possible loss of data
              /w14254 # 'operator': conversion from 'type1:field_bits' to
                      # 'type2:field_bits', possible loss of data
              /w14263 # 'function': member function does not override any base
                      # class virtual member function
              /w14265 # 'classname': class has virtual functions, but destructor
                      # is not virtual
              /w14287 # 'operator': unsigned/negative constant mismatch
              /we4289 # nonstandard extension used: 'variable': loop control
                      # variable declared in the for-loop is used outside the
                      # for-loop scope
              /w14296 # 'operator': expression is always 'boolean_value'
              /w14311 # 'variable': pointer truncation from 'type1' to 'type2'
              /w14545 # expression before comma evaluates to a function which is
                      # missing an argument list
              /w14546 # function call before comma missing argument list
              /w14547 # 'operator': operator before comma has no effect;
                      # expected operator with side-effect
              /w14549 # 'operator': operator before comma has no effect; did you
                      # intend 'operator'?
              /w14555 # expression has no effect; expected expression with
                      # side-effect
              /w14619 # pragma warning: there is no warning number 'number'
              /w14640 # Enable warning on thread un-safe static member
                      # initialization
              /w14826 # Conversion from 'type1' to 'type_2' is sign-extended.
                      # This may cause unexpected runtime behavior.
              /w14905 # wide string literal cast to 'LPSTR'
              /w14906 # string literal cast to 'LPWSTR'
              /w14928 # illegal copy-initialization; more than one user-defined
                      # conversion has been implicitly applied
    )

    if(WARNINGS_WARNINGS_AS_ERRORS)
      target_compile_options(${TARGET_NAME} PRIVATE /WX)
    endif()
  else()
    # GCC and Clang warnings
    target_compile_options(
      ${TARGET_NAME}
      PRIVATE -Wall # Enable most warning messages
              -Wextra # Enable extra warning messages
              -Wpedantic # Issue warnings for code that is not ISO C++
              -Wcast-align # Warn about casts that increase alignment
                           # requirements
              -Wcast-qual # Warn about casts that discard qualifiers
              -Wconversion # Warn about type conversions that may alter values
              -Wdouble-promotion # Warn about promotions from float to double
              # -Wfloat-equal         # Disabled: causes issues with spdlog/fmt
              # library
              -Wformat=2 # Enable format string security warnings
              -Wimplicit-fallthrough # Warn about implicit fallthrough in switch
                                     # statements
              -Wmisleading-indentation # Warn about misleading indentation
              -Wmissing-declarations # Warn about missing function declarations
              -Wmissing-include-dirs # Warn about missing include directories
              -Wnon-virtual-dtor # Warn about non-virtual destructors
              -Wnull-dereference # Warn about null pointer dereferences
              -Wno-old-style-cast # Suppress warnings about old-style casts
              -Woverloaded-virtual # Warn about overloaded virtual functions
              -Wredundant-decls # Warn about redundant declarations
              -Wshadow # Warn about variable shadowing
              -Wno-sign-conversion # Suppress warnings about sign conversions
              # -Wswitch-default        # Warn about switch statements without
              # default case -Wswitch-enum         # Disabled: causes issues
              # with spdlog/fmt library -Wundef                 # Warn about
              # undefined macros
              -Wuninitialized # Warn about uninitialized variables
              -Wunused # Warn about unused variables, functions, etc.
    )

    # GCC-specific warnings
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
      target_compile_options(
        ${TARGET_NAME}
        PRIVATE -Wctor-dtor-privacy # Warn about inaccessible
                                    # constructors/destructors
                -Wdisabled-optimization # Warn when optimizations are disabled
                -Wduplicated-branches # Warn about duplicated branches in
                                      # if-else statements
                -Wduplicated-cond # Warn about duplicated conditions in
                                  # if-else-if chains
                -Wlogical-op # Warn about suspicious logical operations
                # -Wnoexcept              # Warn about noexcept violations
                -Wrestrict # Warn about restrict violations
                -Wstrict-null-sentinel # Warn about non-literal null sentinels
                -Wstrict-overflow=2 # Warn about strict overflow assumptions
                -Wno-maybe-uninitialized # Suppress warnings about
                                         # maybe-uninitialized variables
                -Wno-missing-field-initializers # GCC false positive with
                                                # aggregate/designated init
                # -Wuseless-cast          # Warn about useless casts
      )

      # GCC 12+ has false-positive -Wrestrict in libstdc++ char_traits
      # (string concat inlining). Disable for affected versions.
      if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12")
        target_compile_options(${TARGET_NAME} PRIVATE -Wno-restrict)
      endif()

      # GCC 12+ has false-positive -Wnull-dereference, -Warray-bounds,
      # -Wstringop-overflow, and -Wstringop-overread in libstdc++
      # (streambuf, exception_ptr, char_traits, vector copy/move).
      # Disable for affected versions.
      if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12")
        target_compile_options(${TARGET_NAME} PRIVATE -Wno-null-dereference
                              -Wno-array-bounds -Wno-stringop-overflow
                              -Wno-stringop-overread)
      endif()
    endif()

    # Clang-specific warnings (includes AppleClang)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(
        ${TARGET_NAME}
        PRIVATE -Wmove # Warn about move semantics issues
                -Wrange-loop-analysis # Warn about range-based for loop issues
                -Wstring-conversion # Warn about string conversion issues
                -Wthread-safety # Warn about thread safety issues
                -Wimplicit-int-float-conversion # Warn about implicit int to
                                                # float conversions
                -Wno-gnu-zero-variadic-macro-arguments # Disable GNU extension warning for ##__VA_ARGS__
                -Wshorten-64-to-32 # Warn about 64-bit to 32-bit conversions
      )
    endif()

    if(WARNINGS_WARNINGS_AS_ERRORS)
      target_compile_options(${TARGET_NAME} PRIVATE -Werror)
    endif()
  endif()

  # Print message about warnings being enabled
  dftracer_utils_ok("Enabled comprehensive warnings for target: ${TARGET_NAME}")
  if(WARNINGS_WARNINGS_AS_ERRORS)
    dftracer_utils_ok(
      "Warnings will be treated as errors for target: ${TARGET_NAME}")
  endif()
endfunction()

# Convenience function to set warnings with errors enabled
function(target_set_warnings_as_errors TARGET_NAME)
  target_set_warnings(${TARGET_NAME} WARNINGS_AS_ERRORS)
endfunction()
