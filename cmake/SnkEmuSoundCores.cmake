# Sound-side emulation cores:
#   snkemu_z80core  floooh/chips z80.h (zlib)    https://github.com/floooh/chips
#   snkemu_ymfm     aaronsgiles/ymfm (BSD-3)      https://github.com/aaronsgiles/ymfm
#
# Source location, in order: SNKEMU_CHIPS_DIR / SNKEMU_YMFM_DIR (default
# third_party/chips and third_party/ymfm), then FetchContent if
# SNKEMU_FETCH_DEPS=ON.
#
# Both are OBJECT libraries, so their objects end up inside snkemu_core (same
# reasoning as Musashi: one archive, no link-order surprises).

include_guard(GLOBAL)
include(FetchContent)

set(SNKEMU_CHIPS_DIR "${PROJECT_SOURCE_DIR}/third_party/chips" CACHE PATH "Directory containing floooh/chips (chips/z80.h)")
set(SNKEMU_YMFM_DIR  "${PROJECT_SOURCE_DIR}/third_party/ymfm"  CACHE PATH "Directory containing aaronsgiles/ymfm (src/ymfm.h)")
set(SNKEMU_CHIPS_GIT_TAG "9e88298ce56319953ac7a43213a1120359f7a3a6" CACHE STRING "floooh/chips revision for SNKEMU_FETCH_DEPS")
set(SNKEMU_YMFM_GIT_TAG  "81aec25ccbb98f4873a255f7551ac4dadac59b4a" CACHE STRING "ymfm revision for SNKEMU_FETCH_DEPS")

# Resolves <dir_var> to a usable source tree or fails with instructions.
function(_snkemu_locate name dir_var probe repo tag out)
    set(dir "${${dir_var}}")
    if(NOT EXISTS "${dir}/${probe}")
        if(SNKEMU_FETCH_DEPS)
            message(STATUS "SnkEmu: fetching ${name}")
            FetchContent_Declare(${name} GIT_REPOSITORY ${repo} GIT_TAG ${tag} SOURCE_SUBDIR _no_cmake_)
            FetchContent_MakeAvailable(${name})
            set(dir "${${name}_SOURCE_DIR}")
        else()
            message(FATAL_ERROR "${name} not found in ${dir}.\n"
                "  git clone ${repo} ${dir}\n"
                "or re-run CMake with -DSNKEMU_FETCH_DEPS=ON, or set -D${dir_var}=<path>.")
        endif()
    endif()
    message(STATUS "SnkEmu: ${name} -> ${dir}")
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

_snkemu_locate(chips SNKEMU_CHIPS_DIR "chips/z80.h" https://github.com/floooh/chips.git ${SNKEMU_CHIPS_GIT_TAG} _chips_dir)
_snkemu_locate(ymfm  SNKEMU_YMFM_DIR  "src/ymfm.h"  https://github.com/aaronsgiles/ymfm.git ${SNKEMU_YMFM_GIT_TAG} _ymfm_dir)

# z80.h is a single-header C library: its implementation is compiled once, as
# C (it uses anonymous structs in unions, which C++ only allows as an extension).
add_library(snkemu_z80core OBJECT "${PROJECT_SOURCE_DIR}/src/cpu/z80/Z80ChipsImpl.c")
target_include_directories(snkemu_z80core SYSTEM PUBLIC "${_chips_dir}")
target_compile_options(snkemu_z80core PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)

# ymfm: only the OPN family (YM2610) and its ADPCM/SSG blocks are needed.
add_library(snkemu_ymfm OBJECT
    "${_ymfm_dir}/src/ymfm_opn.cpp"
    "${_ymfm_dir}/src/ymfm_adpcm.cpp"
    "${_ymfm_dir}/src/ymfm_ssg.cpp")
target_include_directories(snkemu_ymfm SYSTEM PUBLIC "${_ymfm_dir}/src")
target_compile_features(snkemu_ymfm PRIVATE cxx_std_14)
target_compile_options(snkemu_ymfm PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(snkemu_z80core snkemu_ymfm PROPERTIES POSITION_INDEPENDENT_CODE ON)
