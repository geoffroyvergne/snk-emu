# miniz (https://github.com/richgel999/miniz): zip reading for ROM sets -> target miniz
#
# Source location, in order:
#   1. SNKEMU_MINIZ_DIR (default: third_party/miniz), e.g.
#        git clone --branch 3.1.2 https://github.com/richgel999/miniz.git third_party/miniz
#   2. FetchContent, if SNKEMU_FETCH_DEPS=ON

include_guard(GLOBAL)

set(SNKEMU_MINIZ_DIR "${PROJECT_SOURCE_DIR}/third_party/miniz" CACHE PATH "Directory containing the miniz sources")
set(SNKEMU_MINIZ_GIT_TAG "3.1.2" CACHE STRING "miniz release used by SNKEMU_FETCH_DEPS")

# miniz's own options default to OFF when it is not the top-level project
# (no examples, tests or install rules); static library.
set(BUILD_SHARED_LIBS OFF)

# miniz declares cmake_minimum_required(VERSION < 3.10), which CMake 4 flags
# as deprecated. Treat it as 3.10 for miniz's directory only (CMake < 4 ignores this).
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

if(EXISTS "${SNKEMU_MINIZ_DIR}/miniz_zip.c")
    add_subdirectory("${SNKEMU_MINIZ_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz" EXCLUDE_FROM_ALL)
    set(_miniz_dir "${SNKEMU_MINIZ_DIR}")
elseif(SNKEMU_FETCH_DEPS)
    include(FetchContent)
    message(STATUS "SnkEmu: fetching miniz ${SNKEMU_MINIZ_GIT_TAG}")
    FetchContent_Declare(miniz
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG        ${SNKEMU_MINIZ_GIT_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(miniz)
    set(_miniz_dir "${miniz_SOURCE_DIR}")
else()
    message(FATAL_ERROR
        "miniz not found in ${SNKEMU_MINIZ_DIR}.\n"
        "  git clone --branch 3.1.2 https://github.com/richgel999/miniz.git third_party/miniz\n"
        "or re-run CMake with -DSNKEMU_FETCH_DEPS=ON, or set -DSNKEMU_MINIZ_DIR=<path>.")
endif()

unset(CMAKE_POLICY_VERSION_MINIMUM)

# Treat miniz headers as system headers: our strict warnings stay on our code.
get_target_property(_miniz_includes miniz INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(miniz PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_miniz_includes}")

message(STATUS "SnkEmu: miniz      -> ${_miniz_dir}")
