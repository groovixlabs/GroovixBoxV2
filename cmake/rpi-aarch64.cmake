# Cross-compiles GroovixBox for 64-bit Raspberry Pi OS from a desktop.
#
#   sudo apt install crossbuild-essential-arm64          # the toolchain
#   sudo dpkg --add-architecture arm64 && sudo apt update
#   sudo apt install libasound2-dev:arm64                # what groovix links against
#
#   cmake -S . -B build-rpi -DCMAKE_TOOLCHAIN_FILE=cmake/rpi-aarch64.cmake \
#         -DGX_BUILD_SIMULATOR=OFF -DGX_NUM_TRACKS=32 -DGX_NUM_PATTERNS=32 -DGX_MAX_STEPS=256
#   cmake --build build-rpi --target groovix
#   scp build-rpi/groovix pi@raspberrypi:~/
#
# With a Pi's own filesystem copied to the desktop instead of the arm64 packages, point
# GX_SYSROOT at it:
#   cmake ... -DGX_SYSROOT=/opt/rpi-sysroot
#
# Building on the Pi itself needs none of this and is the simpler path for one machine; see
# the README. This file is for turning out an image repeatably, or when the Pi is too slow to
# build on.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# The Pi 3B+ and the Zero 2 W are both Cortex-A53. Tuning for it costs nothing and helps the
# clock thread and the mixdown.
set(GX_RPI_FLAGS "-mcpu=cortex-a53")

if(NOT DEFINED CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
endif()

set(CMAKE_C_FLAGS_INIT "${GX_RPI_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${GX_RPI_FLAGS}")

# A sysroot copied from a Pi, if there is one. Without it the arm64 packages installed beside
# the toolchain are used, which is what crossbuild-essential + :arm64 gives you.
if(GX_SYSROOT)
  set(CMAKE_SYSROOT "${GX_SYSROOT}")
  set(CMAKE_FIND_ROOT_PATH "${GX_SYSROOT}")
endif()

# Look for libraries and headers on the target, never on the host: linking the desktop's
# libasound into a Pi binary fails in ways that are tedious to read.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
