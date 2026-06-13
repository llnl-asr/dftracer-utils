# ##############################################################################
# Pretty-printed CMake messages
# ##############################################################################
# Colorized, consistently prefixed status output for this project. Colors are
# emitted only when stdout is a terminal (and not disabled), so log files and
# CI stay clean.
#
#   dftracer_utils_section("Title")      -> blank line + bold underlined heading
#   dftracer_utils_status("msg")         -> [dftracer] msg
#   dftracer_utils_ok("msg")             -> [dftracer] green check + msg
#   dftracer_utils_warn("msg")           -> [dftracer] yellow warn + msg (STATUS)
#   dftracer_utils_item("label" "value") -> aligned "  label : value"

# Decide whether to emit ANSI color. CMake has no isatty for its own stdout
# (execute_process pipes the child, so `test -t 1` always reports "no tty"), so
# we use the TERM env var as the interactive signal. Precedence:
#   1. DFTRACER_UTILS_COLOR set explicitly        -> honored as-is
#   2. CLICOLOR_FORCE env non-zero                -> on (overrides everything)
#   3. NO_COLOR set, or CLICOLOR=0                -> off (https://no-color.org)
#   4. TERM set and not "dumb" (Windows: a modern terminal) -> on, else off
# Caveat: TERM stays set when redirecting to a file from a terminal, so for that
# case disable with NO_COLOR=1; CI / non-interactive shells have no usable TERM.
if(NOT DEFINED DFTRACER_UTILS_COLOR)
  if(DEFINED ENV{CLICOLOR_FORCE} AND NOT "$ENV{CLICOLOR_FORCE}" STREQUAL "0")
    set(DFTRACER_UTILS_COLOR ON)
  elseif(DEFINED ENV{NO_COLOR} OR "$ENV{CLICOLOR}" STREQUAL "0")
    set(DFTRACER_UTILS_COLOR OFF)
  elseif(WIN32)
    # Old Windows consoles don't grok ANSI; only modern terminals advertise it.
    if(DEFINED ENV{WT_SESSION} OR DEFINED ENV{ConEmuANSI} OR DEFINED ENV{ANSICON})
      set(DFTRACER_UTILS_COLOR ON)
    else()
      set(DFTRACER_UTILS_COLOR OFF)
    endif()
  elseif(DEFINED ENV{TERM} AND NOT "$ENV{TERM}" STREQUAL ""
         AND NOT "$ENV{TERM}" STREQUAL "dumb")
    set(DFTRACER_UTILS_COLOR ON)
  else()
    set(DFTRACER_UTILS_COLOR OFF)
  endif()
endif()

if(DFTRACER_UTILS_COLOR)
  string(ASCII 27 dftracer_utils_esc)
  set(dftracer_utils_reset "${dftracer_utils_esc}[0m")
  set(dftracer_utils_bold "${dftracer_utils_esc}[1m")
  set(dftracer_utils_dim "${dftracer_utils_esc}[2m")
  set(dftracer_utils_under "${dftracer_utils_esc}[4m")
  set(dftracer_utils_red "${dftracer_utils_esc}[31m")
  set(dftracer_utils_green "${dftracer_utils_esc}[32m")
  set(dftracer_utils_yellow "${dftracer_utils_esc}[33m")
  set(dftracer_utils_blue "${dftracer_utils_esc}[34m")
  set(dftracer_utils_cyan "${dftracer_utils_esc}[36m")
  set(dftracer_utils_check "✓")
  set(dftracer_utils_warnsym "⚠")
else()
  set(dftracer_utils_reset "")
  set(dftracer_utils_bold "")
  set(dftracer_utils_dim "")
  set(dftracer_utils_under "")
  set(dftracer_utils_red "")
  set(dftracer_utils_green "")
  set(dftracer_utils_yellow "")
  set(dftracer_utils_blue "")
  set(dftracer_utils_cyan "")
  # Plain ASCII when color is off (piped/CI/non-UTF-8 logs).
  set(dftracer_utils_check "OK:")
  set(dftracer_utils_warnsym "WARN:")
endif()

set(dftracer_utils_tag
    "${dftracer_utils_dim}${dftracer_utils_cyan}[dftracer_utils]${dftracer_utils_reset}"
)

function(dftracer_utils_section title)
  message(STATUS "")
  message(STATUS
          "${dftracer_utils_bold}${dftracer_utils_under}${title}${dftracer_utils_reset}"
  )
endfunction()

function(dftracer_utils_status msg)
  message(STATUS "${dftracer_utils_tag} ${msg}")
endfunction()

function(dftracer_utils_ok msg)
  message(
    STATUS
    "${dftracer_utils_tag} ${dftracer_utils_green}${dftracer_utils_check}${dftracer_utils_reset} ${msg}"
  )
endfunction()

function(dftracer_utils_warn msg)
  message(
    STATUS
    "${dftracer_utils_tag} ${dftracer_utils_yellow}${dftracer_utils_warnsym} ${msg}${dftracer_utils_reset}"
  )
endfunction()

function(dftracer_utils_item label value)
  message(STATUS
          "  ${dftracer_utils_blue}${label}${dftracer_utils_reset} : ${value}")
endfunction()
