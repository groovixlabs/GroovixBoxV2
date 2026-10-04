cmake_minimum_required(VERSION 3.10)
project(GroovixBox CXX)

# The core must build with the older GCC toolchains used for MCUs (e.g. Teensyduino),
# so everything sticks to C++11.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(GX_BUILD_SIMULATOR "Build the SDL2 simulator" ON)
# The internal instrument engine (platform/audio) is desktop and single-board Linux only.
# Turn it off for a MIDI-only build; an MCU build never compiles platform/ at all.
option(GX_BUILD_AUDIO "Build the internal instrument engine" ON)
# A screen beside the instrument: an 800x480 panel on a Pi, showing what every track is doing
# while it plays and what the mode's rows are for while it is stopped. Off by default, since
# most rigs have no screen - and it is a separate module from the simulator, which draws the
# whole device rather than reporting on it.
option(GX_BUILD_DISPLAY "Build the 800x480 performance display" OFF)
if(GX_BUILD_SIMULATOR AND NOT GX_BUILD_AUDIO)
  message(FATAL_ERROR
          "The simulator plays the internal instruments, so it needs GX_BUILD_AUDIO=ON.\n"
          "For a build without audio, configure with -DGX_BUILD_SIMULATOR=OFF too.")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(GX_WARNINGS -Wall -Wextra)
endif()

# ---- Portable core: engine, ui, storage, comm, app. No platform code. ----
set(CORE_SOURCES
  core/engine/FactoryPatterns.cpp
  core/engine/Project.cpp
  core/engine/Scale.cpp
  core/engine/Sequencer.cpp
  core/ui/ArrangementMode.cpp
  core/ui/ControlMap.cpp
  core/ui/GlobalMode.cpp
  core/ui/NoteMode.cpp
  core/ui/Palette.cpp
  core/ui/PianoRoll.cpp
  core/ui/ScaleMode.cpp
  core/ui/SceneMode.cpp
  core/ui/StepParamsMode.cpp
  core/ui/SlotModes.cpp
  core/ui/UiController.cpp
  core/storage/ProjectCodec.cpp
  core/storage/SlotStore.cpp
  core/comm/MidiEventSink.cpp
  core/comm/VoiceTable.cpp
  core/app/GroovixApp.cpp)

add_library(gx_core STATIC ${CORE_SOURCES})
target_include_directories(gx_core PUBLIC core)

# Capacity limits for desktop builds (simulator, tests): 8 pages each of tracks, patterns and
# steps, which is every page the surface can reach - the track pages fill B1..B8 and the
# pattern pages the top pad row under Shift.
# MCU builds leave these undefined and get the smaller defaults in core/engine/Project.h.
# PUBLIC so every target linking the core agrees on the data layout.
set(GX_NUM_TRACKS 64 CACHE STRING "Tracks in desktop builds (1-64: 8 track pages on B1..B8)")
set(GX_NUM_PATTERNS 64 CACHE STRING "Patterns per track in desktop builds (1-64: 8 pattern pages)")
set(GX_MAX_STEPS 256 CACHE STRING "Steps per pattern in desktop builds (1-256, 8 pages of 32)")
target_compile_definitions(gx_core PUBLIC
  GX_NUM_TRACKS=${GX_NUM_TRACKS}
  GX_NUM_PATTERNS=${GX_NUM_PATTERNS}
  GX_MAX_STEPS=${GX_MAX_STEPS})
target_compile_options(gx_core PRIVATE ${GX_WARNINGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gx_core PRIVATE -pedantic -fno-exceptions -fno-rtti)
endif()

# ---- Audio engine: plays the tracks routed to internal instruments ----
find_package(Threads REQUIRED)
if(GX_BUILD_AUDIO)
# Desktop and single-board Linux only; an MCU build implements InstrumentOutput its own way.
add_library(gx_audio STATIC
  platform/audio/AudioEngine.cpp
  platform/audio/InstrumentConfig.cpp
  platform/audio/InstrumentRack.cpp
  platform/audio/InstrumentState.cpp
  platform/audio/NullBackend.cpp
  platform/audio/WavWriter.cpp)
target_include_directories(gx_audio PUBLIC platform)
target_link_libraries(gx_audio PUBLIC gx_core Threads::Threads)
target_compile_options(gx_audio PRIVATE ${GX_WARNINGS})

# Real devices, when their libraries are installed: JACK (or PipeWire answering it) for
# desktop work, ALSA for a standalone board.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(JACK IMPORTED_TARGET jack)
endif()
if(JACK_FOUND)
  target_sources(gx_audio PRIVATE platform/audio/JackBackend.cpp)
  target_link_libraries(gx_audio PUBLIC PkgConfig::JACK)
  target_compile_definitions(gx_audio PUBLIC GX_AUDIO_JACK)
  message(STATUS "Audio: JACK backend")
endif()
# LV2 plugin hosting: lilv reads the plugins' manifests, the LV2 headers define the ABI.
if(PkgConfig_FOUND)
  pkg_check_modules(LILV IMPORTED_TARGET lilv-0)
endif()
if(LILV_FOUND)
  target_sources(gx_audio PRIVATE platform/audio/Lv2Host.cpp)
  target_link_libraries(gx_audio PUBLIC PkgConfig::LILV)
  target_compile_definitions(gx_audio PUBLIC GX_AUDIO_LV2)
  message(STATUS "Audio: LV2 plugin hosting (lilv ${LILV_VERSION})")
else()
  message(STATUS "Audio: no LV2 hosting (install liblilv-dev)")
endif()

find_package(ALSA QUIET)
if(ALSA_FOUND)
  target_sources(gx_audio PRIVATE platform/audio/AlsaBackend.cpp)
  target_link_libraries(gx_audio PUBLIC ALSA::ALSA)
  target_compile_definitions(gx_audio PUBLIC GX_AUDIO_ALSA)
  message(STATUS "Audio: ALSA backend")
endif()
else()
  message(STATUS "Audio: not built (GX_BUILD_AUDIO=OFF)")
endif()

# ---- Interface implementations shared by OS platforms (PC, Mac, iPad) ----
find_package(Threads REQUIRED)
add_library(gx_platform_common STATIC
  platform/common/ApcMiniSurface.cpp
  platform/common/MidiMixSurface.cpp
  platform/common/ClockThread.cpp
  platform/common/FileStorage.cpp
  platform/common/LogMidiOutput.cpp
  platform/common/RigPaths.cpp
  platform/common/VoiceLibrary.cpp)
target_include_directories(gx_platform_common PUBLIC platform)
target_link_libraries(gx_platform_common PUBLIC gx_core Threads::Threads)
target_compile_options(gx_platform_common PRIVATE ${GX_WARNINGS})

# ---- Linux: MIDI devices through the ALSA sequencer ----
find_package(ALSA QUIET)
if(ALSA_FOUND)
  add_library(gx_platform_linux STATIC platform/linux/AlsaAnnounce.cpp
                                      platform/linux/AlsaMidiPort.cpp
                                      platform/linux/AlsaMidiOutput.cpp
                                      platform/linux/AlsaMidiInput.cpp
                                      platform/linux/MidiRig.cpp
                                      platform/linux/RigConfig.cpp
                                      platform/linux/SingleInstance.cpp)
  target_include_directories(gx_platform_linux PUBLIC platform)
  target_link_libraries(gx_platform_linux PUBLIC gx_platform_common ALSA::ALSA)
  target_compile_options(gx_platform_linux PRIVATE ${GX_WARNINGS})

  # ---- `groovix`: the instrument itself, on a box with no screen ----
  # The same core and platform code as the simulator, without the window: what runs on a Pi.
  add_executable(groovix platform/linux/headless.cpp)
  target_link_libraries(groovix PRIVATE gx_platform_linux)
  target_compile_options(groovix PRIVATE ${GX_WARNINGS})

  if(GX_BUILD_DISPLAY)
    # SDL draws it, SDL_ttf sets the type. On a Pi with no X server, SDL's KMSDRM backend
    # writes straight to the panel, so this needs no desktop - only the two libraries:
    #   sudo apt install libsdl2-dev libsdl2-ttf-dev
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2_TTF SDL2_ttf)
    if(NOT SDL2_TTF_FOUND)
      message(FATAL_ERROR
              "The display needs SDL2_ttf for its type.\n"
              "  sudo apt install libsdl2-ttf-dev\n"
              "Or configure with -DGX_BUILD_DISPLAY=OFF.")
    endif()
    target_sources(groovix PRIVATE platform/display/DisplayWindow.cpp)
    target_link_libraries(groovix PRIVATE SDL2::SDL2 ${SDL2_TTF_LIBRARIES})
    target_include_directories(groovix PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
    target_compile_definitions(groovix PRIVATE GX_HAVE_DISPLAY)
    message(STATUS "Display: 800x480 panel (SDL2_ttf ${SDL2_TTF_VERSION})")
  endif()
endif()

# ---- SDL2 simulator (Linux, macOS, Windows) ----
if(GX_BUILD_SIMULATOR)
  find_package(SDL2 REQUIRED)
  add_executable(groovix_sim platform/sdl/main.cpp platform/sdl/SdlSurface.cpp)
  target_link_libraries(groovix_sim PRIVATE gx_platform_common gx_audio SDL2::SDL2)
  if(TARGET gx_platform_linux)
    target_link_libraries(groovix_sim PRIVATE gx_platform_linux)
    target_compile_definitions(groovix_sim PRIVATE GX_HAVE_ALSA)  # uses a connected APC mini
  endif()
  if(WIN32 AND TARGET SDL2::SDL2main)
    target_link_libraries(groovix_sim PRIVATE SDL2::SDL2main)
  endif()
  target_compile_options(groovix_sim PRIVATE ${GX_WARNINGS})
endif()

# ---- Tests: `ctest` in the build directory ----
enable_testing()
add_executable(gx_tests tests/core_tests.cpp)
target_link_libraries(gx_tests PRIVATE gx_platform_common)
if(GX_BUILD_AUDIO)
  target_link_libraries(gx_tests PRIVATE gx_audio)
  target_compile_definitions(gx_tests PRIVATE GX_HAVE_AUDIO)

  # Renders the internal instruments to a WAV with no audio device: an audible check, and the
  # measurement that sizes a board.
  add_executable(gx_render tools/gx_render.cpp)
  target_link_libraries(gx_render PRIVATE gx_platform_common gx_audio)
  target_compile_options(gx_render PRIVATE ${GX_WARNINGS})
endif()
target_compile_options(gx_tests PRIVATE ${GX_WARNINGS})
add_test(NAME core_tests COMMAND gx_tests ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME layer_rules
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/core
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_layers.cmake)

# ---- Freestanding builds of core/, one target per hardware target ----
# They compile core only, with its own (MCU) capacity defaults, and fail on anything that
# needs a heap, exceptions or thread-safe statics.
set(core_abs_sources "")
foreach(src ${CORE_SOURCES})
  list(APPEND core_abs_sources ${CMAKE_CURRENT_SOURCE_DIR}/${src})
endforeach()
string(REPLACE ";" "|" core_source_arg "${core_abs_sources}")

function(gx_add_freestanding_check target compiler nm cpu_flags label)
  string(REPLACE ";" "|" cpu_flag_arg "${cpu_flags}")
  add_custom_target(${target}
    COMMAND ${CMAKE_COMMAND} -DCXX=${compiler} -DNM=${nm}
            "-DCPU_FLAGS=${cpu_flag_arg}" -DLABEL=${label}
            -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/core
            -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${target}
            "-DSOURCES=${core_source_arg}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_freestanding.cmake
    VERBATIM)
endfunction()

# `check_mcu`: the Teensy 4.x (Cortex-M7), 32-bit.
find_program(ARM_GXX arm-none-eabi-g++)
find_program(ARM_NM arm-none-eabi-nm)
if(ARM_GXX AND ARM_NM)
  gx_add_freestanding_check(check_mcu ${ARM_GXX} ${ARM_NM}
    "-mcpu=cortex-m7;-mthumb;-mfloat-abi=hard;-mfpu=fpv5-d16" "Cortex-M7 (Teensy 4.x)")
endif()

# `check_64bit`: the same check under a 64-bit ABI, on a Cortex-A53.
#
# check_mcu alone would let a 64-bit-only slip through: pointers are twice the size, alignment
# and padding differ, long is 64 bits, and the calling convention is another one again. Code
# that quietly needs the heap or a static guard can be fine on one and not the other.
#
# It deliberately does NOT fall back to a 32-bit compiler when there is no aarch64 one. Falling
# back would run the check and report success while testing the very thing it exists to cover
# differently - better that the target is missing and says so.
find_program(AARCH64_GXX NAMES aarch64-none-elf-g++ aarch64-elf-g++ aarch64-linux-gnu-g++)
find_program(AARCH64_NM NAMES aarch64-none-elf-nm aarch64-elf-nm aarch64-linux-gnu-nm)
if(AARCH64_GXX AND AARCH64_NM)
  gx_add_freestanding_check(check_64bit ${AARCH64_GXX} ${AARCH64_NM}
    "-mcpu=cortex-a53" "Cortex-A53, AArch64 (64-bit ABI)")
endif()

