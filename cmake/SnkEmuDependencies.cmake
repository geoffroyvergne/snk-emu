# Locates SDL2 and SDL2_mixer on Windows, macOS and Linux and exposes them as
# two stable interface targets, regardless of how they were found:
#
#   snkemu::SDL2        snkemu::SDL2_mixer
#
# Search order for each library:
#   1. CMake config package  (vcpkg, Homebrew, MSYS2, SDL "devel" zips, distro -dev pkgs)
#   2. pkg-config            (older Linux distros whose packages ship no CMake config)
#   3. FetchContent          (only if SNKEMU_FETCH_DEPS=ON)
#
# Hints when a library lives in a non-standard place:
#   -DCMAKE_PREFIX_PATH=<root>   or   -DSDL2_DIR=<dir> / -DSDL2_mixer_DIR=<dir>
#   (point *_DIR at the folder containing sdl2-config.cmake / SDL2_mixerConfig.cmake)

include_guard(GLOBAL)

set(SNKEMU_SDL2_GIT_TAG       "release-2.30.9" CACHE STRING "SDL2 tag used by SNKEMU_FETCH_DEPS")
set(SNKEMU_SDL2_MIXER_GIT_TAG "release-2.8.0"  CACHE STRING "SDL2_mixer tag used by SNKEMU_FETCH_DEPS")

# Returns in <out> the first name in ARGN that is an existing target, or "".
function(_snkemu_first_target out)
    foreach(candidate IN LISTS ARGN)
        if(TARGET ${candidate})
            set(${out} ${candidate} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

function(_snkemu_missing_dep_error name pkg_hint)
    message(FATAL_ERROR
        "${name} not found.\n"
        "  macOS  : brew install sdl2 sdl2_mixer\n"
        "  Linux  : ${pkg_hint}\n"
        "  Windows: vcpkg install sdl2 sdl2-mixer  (configure with the vcpkg toolchain file),\n"
        "           or unzip the SDL '-devel-VC' packages and pass -DCMAKE_PREFIX_PATH=<dir>\n"
        "  Any OS : re-run CMake with -DSNKEMU_FETCH_DEPS=ON to build it from source.")
endfunction()

find_package(PkgConfig QUIET)
include(FetchContent)

# ---------------------------------------------------------------------------
# SDL2
# ---------------------------------------------------------------------------
add_library(snkemu_sdl2 INTERFACE)
add_library(snkemu::SDL2 ALIAS snkemu_sdl2)

find_package(SDL2 CONFIG QUIET)
_snkemu_first_target(_sdl2_target SDL2::SDL2 SDL2::SDL2-static)

if(NOT _sdl2_target AND PKG_CONFIG_FOUND)
    pkg_check_modules(SNKEMU_PC_SDL2 QUIET IMPORTED_TARGET sdl2>=2.0.18)
    if(SNKEMU_PC_SDL2_FOUND)
        set(_sdl2_target PkgConfig::SNKEMU_PC_SDL2)
    endif()
endif()

if(NOT _sdl2_target AND SNKEMU_FETCH_DEPS)
    message(STATUS "SnkEmu: fetching SDL2 ${SNKEMU_SDL2_GIT_TAG}")
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        ${SNKEMU_SDL2_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL2)
    _snkemu_first_target(_sdl2_target SDL2::SDL2 SDL2)
endif()

if(NOT _sdl2_target)
    _snkemu_missing_dep_error("SDL2" "sudo apt install libsdl2-dev   |  sudo dnf install SDL2-devel")
endif()

# SDL_MAIN_HANDLED: we call SDL_SetMainReady() ourselves, so SDL2main is never
# needed. This sidesteps SDL2main link issues on MSVC/MinGW entirely.
target_link_libraries(snkemu_sdl2 INTERFACE ${_sdl2_target})
target_compile_definitions(snkemu_sdl2 INTERFACE SDL_MAIN_HANDLED)
message(STATUS "SnkEmu: SDL2       -> ${_sdl2_target}")

# ---------------------------------------------------------------------------
# SDL2_mixer
# ---------------------------------------------------------------------------
add_library(snkemu_sdl2_mixer INTERFACE)
add_library(snkemu::SDL2_mixer ALIAS snkemu_sdl2_mixer)

find_package(SDL2_mixer CONFIG QUIET)
_snkemu_first_target(_mixer_target SDL2_mixer::SDL2_mixer SDL2_mixer::SDL2_mixer-static)

if(NOT _mixer_target AND PKG_CONFIG_FOUND)
    pkg_check_modules(SNKEMU_PC_SDL2_MIXER QUIET IMPORTED_TARGET SDL2_mixer>=2.0.4)
    if(SNKEMU_PC_SDL2_MIXER_FOUND)
        set(_mixer_target PkgConfig::SNKEMU_PC_SDL2_MIXER)
    endif()
endif()

if(NOT _mixer_target AND SNKEMU_FETCH_DEPS)
    message(STATUS "SnkEmu: fetching SDL2_mixer ${SNKEMU_SDL2_MIXER_GIT_TAG}")
    # The emulator streams raw PCM from its YM2610 core; music codecs that need
    # extra system libraries are disabled to keep the source build self-contained.
    set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_FLAC    OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MOD     OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MIDI    OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_OPUS    OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_MP3     OFF CACHE BOOL "" FORCE)
    set(SDL2MIXER_VORBIS  "STB" CACHE STRING "" FORCE)   # header-only, no deps
    FetchContent_Declare(SDL2_mixer
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
        GIT_TAG        ${SNKEMU_SDL2_MIXER_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL2_mixer)
    _snkemu_first_target(_mixer_target SDL2_mixer::SDL2_mixer SDL2_mixer)
endif()

if(NOT _mixer_target)
    _snkemu_missing_dep_error("SDL2_mixer" "sudo apt install libsdl2-mixer-dev  |  sudo dnf install SDL2_mixer-devel")
endif()

target_link_libraries(snkemu_sdl2_mixer INTERFACE ${_mixer_target} snkemu::SDL2)
message(STATUS "SnkEmu: SDL2_mixer -> ${_mixer_target}")

set(SNKEMU_SDL2_TARGET       ${_sdl2_target}  CACHE INTERNAL "")
set(SNKEMU_SDL2_MIXER_TARGET ${_mixer_target} CACHE INTERNAL "")

# ---------------------------------------------------------------------------
# snkemu_copy_runtime_dlls(<target>)
# Windows: copies the SDL2 / SDL2_mixer DLLs next to <target> after build.
# Only shared libraries are copied, so static (vcpkg *-static triplet) builds
# are a no-op. Works with CMake 3.20 (no TARGET_RUNTIME_DLLS needed).
# ---------------------------------------------------------------------------
function(snkemu_copy_runtime_dlls target)
    foreach(dep IN ITEMS ${SNKEMU_SDL2_TARGET} ${SNKEMU_SDL2_MIXER_TARGET})
        if(NOT TARGET ${dep})
            continue()
        endif()
        get_target_property(_aliased ${dep} ALIASED_TARGET)
        if(_aliased)
            set(dep ${_aliased})
        endif()
        get_target_property(_type ${dep} TYPE)
        if(_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${dep}>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endforeach()
endfunction()
