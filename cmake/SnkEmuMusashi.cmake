# Musashi 68000 core (https://github.com/kstenerud/Musashi) -> target snkemu_musashi
#
# Source location, in order:
#   1. SNKEMU_MUSASHI_DIR (default: third_party/musashi), e.g.
#        git clone https://github.com/kstenerud/Musashi.git third_party/musashi
#   2. FetchContent, if SNKEMU_FETCH_DEPS=ON
#
# Musashi ships its opcode handlers as a template (m68k_in.c); the m68kmake
# tool expands it into m68kops.c / m68kops.h. We build m68kmake as a host tool
# and run it at build time, so nothing generated is ever committed.
#
# snkemu_musashi is an OBJECT library: its objects end up inside snkemu_core
# itself. That matters because Musashi calls back into our code
# (m68k_read_memory_8 & co. live in M68000.cpp); as two separate static
# archives, single-pass linkers (GNU ld) would fail to resolve those callbacks.

include_guard(GLOBAL)

set(SNKEMU_MUSASHI_DIR "${PROJECT_SOURCE_DIR}/third_party/musashi" CACHE PATH
    "Directory containing the Musashi sources (m68k_in.c, m68kcpu.c, ...)")
set(SNKEMU_MUSASHI_GIT_TAG "master" CACHE STRING
    "Musashi revision used by SNKEMU_FETCH_DEPS (pin to a commit hash for reproducible builds)")

set(_musashi_dir "${SNKEMU_MUSASHI_DIR}")

if(NOT EXISTS "${_musashi_dir}/m68k_in.c")
    if(SNKEMU_FETCH_DEPS)
        include(FetchContent)
        message(STATUS "SnkEmu: fetching Musashi ${SNKEMU_MUSASHI_GIT_TAG}")
        FetchContent_Declare(musashi
            GIT_REPOSITORY https://github.com/kstenerud/Musashi.git
            GIT_TAG        ${SNKEMU_MUSASHI_GIT_TAG}
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  _no_cmake_)  # populate only; never add_subdirectory()
        FetchContent_MakeAvailable(musashi)
        set(_musashi_dir "${musashi_SOURCE_DIR}")
    else()
        message(FATAL_ERROR
            "Musashi 68000 core not found in ${SNKEMU_MUSASHI_DIR}.\n"
            "  git clone https://github.com/kstenerud/Musashi.git third_party/musashi\n"
            "or re-run CMake with -DSNKEMU_FETCH_DEPS=ON, or set -DSNKEMU_MUSASHI_DIR=<path>.")
    endif()
endif()

# --- Code generator (host tool) --------------------------------------------
add_executable(snkemu_m68kmake "${_musashi_dir}/m68kmake.c")
set_target_properties(snkemu_m68kmake PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")
target_compile_options(snkemu_m68kmake PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
if(MSVC)
    target_compile_definitions(snkemu_m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(_musashi_gen "${CMAKE_CURRENT_BINARY_DIR}/musashi_gen")
add_custom_command(
    OUTPUT  "${_musashi_gen}/m68kops.c" "${_musashi_gen}/m68kops.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_musashi_gen}"
    # usage: m68kmake <output dir> <input template>
    COMMAND snkemu_m68kmake "${_musashi_gen}/" "${_musashi_dir}/m68k_in.c"
    DEPENDS snkemu_m68kmake "${_musashi_dir}/m68k_in.c"
    COMMENT "Generating Musashi 68000 opcode handlers"
    VERBATIM)

# --- Core ---------------------------------------------------------------------
set(_musashi_sources
    "${_musashi_dir}/m68kcpu.c"      # #includes m68kfpu.c itself; do not compile that separately
    "${_musashi_dir}/m68kdasm.c"
    "${_musashi_gen}/m68kops.c")
# Musashi 4.x needs softfloat for its (unused by us) FPU emulation; 3.x has none.
if(EXISTS "${_musashi_dir}/softfloat/softfloat.c")
    list(APPEND _musashi_sources "${_musashi_dir}/softfloat/softfloat.c")
endif()

add_library(snkemu_musashi OBJECT ${_musashi_sources})
# SYSTEM: keep third-party headers out of our strict warning set.
target_include_directories(snkemu_musashi SYSTEM PUBLIC "${_musashi_dir}" "${_musashi_gen}")
target_compile_options(snkemu_musashi PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(snkemu_musashi PROPERTIES POSITION_INDEPENDENT_CODE ON)

message(STATUS "SnkEmu: Musashi    -> ${_musashi_dir}")
