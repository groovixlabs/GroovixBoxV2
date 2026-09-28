# Enforces the module boundaries inside core/ (see ARCHITECTURE.md):
#   - each module may only include the modules listed below
#   - includes are module-qualified ("engine/Sequencer.h")
#   - only C headers available on every target toolchain are allowed
#
# Usage: cmake -DCORE_DIR=<path to core> -P check_layers.cmake
cmake_minimum_required(VERSION 3.10)

set(ALLOWED_engine engine)
set(ALLOWED_ui ui engine)
set(ALLOWED_storage storage engine)
set(ALLOWED_comm comm engine)
set(ALLOWED_app app engine ui storage comm)
set(PORTABLE_SYSTEM_HEADERS "^(stdint|stddef|string)\\.h$")

file(GLOB_RECURSE files RELATIVE "${CORE_DIR}" "${CORE_DIR}/*.h" "${CORE_DIR}/*.cpp")
set(errors "")

foreach(file ${files})
  string(REGEX MATCH "^[^/]+" module "${file}")
  if(NOT DEFINED ALLOWED_${module})
    string(APPEND errors "\n  ${file}: '${module}' is not a known core module")
    continue()
  endif()

  file(STRINGS "${CORE_DIR}/${file}" includes REGEX "^[ \t]*#[ \t]*include")
  foreach(line ${includes})
    if(line MATCHES "#[ \t]*include[ \t]*\"([^/\"]+)/")
      if(NOT CMAKE_MATCH_1 IN_LIST ALLOWED_${module})
        string(APPEND errors "\n  ${file}: includes ${CMAKE_MATCH_1}/ (${module} may use: ${ALLOWED_${module}})")
      endif()
    elseif(line MATCHES "#[ \t]*include[ \t]*\"")
      string(APPEND errors "\n  ${file}: use a module-qualified include: ${line}")
    elseif(line MATCHES "#[ \t]*include[ \t]*<([^>]+)>")
      set(header "${CMAKE_MATCH_1}")
      if(NOT header MATCHES "${PORTABLE_SYSTEM_HEADERS}")
        string(APPEND errors "\n  ${file}: non-portable header <${header}>")
      endif()
    endif()
  endforeach()
endforeach()

if(errors)
  message(FATAL_ERROR "core layer rules violated:${errors}")
endif()
message(STATUS "core layer rules OK")
