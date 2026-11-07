cmake_minimum_required(VERSION 3.20)

project(SnkEmu
    VERSION 0.1.0
    DESCRIPTION "Modular SNK Neo-Geo (MVS/AES) emulator"
    LANGUAGES C CXX)   # C is needed only if SDL2/SDL2_mixer are built from source

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Keep binaries together so runtime DLLs (Windows) sit next to the executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(SNKEMU_FETCH_DEPS "Download and build SDL2 / SDL2_mixer if they are not found on the system" OFF)
option(SNKEMU_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Dependencies -> provides snkemu::SDL2 and snkemu::SDL2_mixer
# ---------------------------------------------------------------------------
include(SnkEmuDependencies)
include(SnkEmuMusashi)        # -> snkemu_musashi (68000 core)
include(SnkEmuMiniz)          # -> miniz (zip ROM sets)
include(SnkEmuSoundCores)     # -> snkemu_z80core (Z80), snkemu_ymfm (YM2610)

# ---------------------------------------------------------------------------
# Shared compile options (C++20, warnings) applied to our own targets only
# ---------------------------------------------------------------------------
add_library(snkemu_options INTERFACE)
target_compile_features(snkemu_options INTERFACE cxx_std_20)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(snkemu_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(SNKEMU_WARNINGS_AS_ERRORS)
        target_compile_options(snkemu_options INTERFACE /WX)
    endif()
else()
    target_compile_options(snkemu_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(SNKEMU_WARNINGS_AS_ERRORS)
        target_compile_options(snkemu_options INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# snkemu_core: the emulated machine. Pure C++, no SDL dependency, so it can be
# unit-tested and driven by any frontend.
# ---------------------------------------------------------------------------
add_library(snkemu_core STATIC
    src/core/System.cpp
    src/core/BackupRamFile.cpp
    src/memory/MemoryBus.cpp
    src/cpu/m68k/M68000.cpp
    src/cpu/z80/Z80.cpp
    src/video/VideoSystem.cpp
    src/video/Palette.cpp
    src/video/TestScene.cpp
    src/audio/Ym2610.cpp
    src/audio/SoundSystem.cpp
    src/rom/RomSource.cpp
    src/rom/RomLoader.cpp
    src/rom/BiosLoader.cpp
    src/rtc/Upd4990a.cpp
)
target_include_directories(snkemu_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(snkemu_core PRIVATE snkemu_options snkemu_musashi snkemu_z80core snkemu_ymfm miniz)

# ---------------------------------------------------------------------------
# snkemu: SDL2 frontend executable
# ---------------------------------------------------------------------------
add_executable(snkemu
    src/main.cpp
    src/platform/SdlPlatform.cpp
    src/platform/InputManager.cpp
    src/platform/AudioOutput.cpp
)
target_link_libraries(snkemu PRIVATE
    snkemu_core
    snkemu_options
    snkemu::SDL2
    snkemu::SDL2_mixer
)

# Windows: copy SDL2 / SDL2_mixer DLLs next to the executable after each build.
if(WIN32)
    snkemu_copy_runtime_dlls(snkemu)
endif()
