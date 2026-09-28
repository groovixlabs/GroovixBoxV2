# Compiles the portable core for a freestanding target in C++11 and fails if any object needs
# heap allocation, exception support or thread-safe static guards. The caller chooses the CPU
# flags, so the same check covers a 32-bit MCU (check_mcu) and a 64-bit ABI (check_64bit).
#
# Usage (see those targets):
#   cmake -DCXX=<g++> -DNM=<nm> -DCPU_FLAGS=<-mcpu=...;...> -DLABEL=<text>
#         -DINCLUDE_DIR=<core> -DOUT_DIR=<dir> -DSOURCES=<a.cpp|b.cpp|...>
#         -P check_freestanding.cmake
cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CPU_FLAGS)
  message(FATAL_ERROR "CPU_FLAGS is required")
endif()
string(REPLACE "|" ";" cpu_flag_list "${CPU_FLAGS}")
set(FLAGS -std=c++11 ${cpu_flag_list} -Os -ffreestanding
          -fno-exceptions -fno-rtti -Wall -Wextra -pedantic -Werror)
set(FORBIDDEN_SYMBOLS "_Znw|_Zna|_Zdl|_Zda|malloc|calloc|realloc|free|__cxa_throw|__cxa_allocate_exception|__gxx_personality|_Unwind_|__cxa_guard_")

string(REPLACE "|" ";" source_list "${SOURCES}")
file(MAKE_DIRECTORY "${OUT_DIR}")

foreach(src ${source_list})
  get_filename_component(name "${src}" NAME_WE)
  set(obj "${OUT_DIR}/${name}.o")

  execute_process(COMMAND ${CXX} ${FLAGS} -I${INCLUDE_DIR} -c ${src} -o ${obj}
                  RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${LABEL} build failed: ${src}")
  endif()

  execute_process(COMMAND ${NM} -u ${obj} OUTPUT_VARIABLE undefined RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${obj}")
  endif()
  string(REGEX MATCHALL "[^\n]*(${FORBIDDEN_SYMBOLS})[^\n]*" forbidden "${undefined}")
  if(forbidden)
    message(FATAL_ERROR "${src} needs heap/exception runtime support:\n${forbidden}")
  endif()
endforeach()

message(STATUS "core builds for ${LABEL}: C++11, no heap, no exceptions")
