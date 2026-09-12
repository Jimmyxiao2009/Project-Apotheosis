# Apotheosis: host tests for the TileGrid v2 model (docs/TILEGRID-DESIGN.md 5.4).
#
# The model is WebCore-free on purpose, so this builds it straight out of the
# WebKit tree with MSVC x64 - no engine, no ARM cross build, no device.
#
#   cmake -S tests/tilegrid -B build/tilegrid -G "Visual Studio 17 2022" -A x64
#   cmake --build build/tilegrid --config RelWithDebInfo
#   build/tilegrid/RelWithDebInfo/tilegrid-tests.exe
#
# or simply: pwsh -File tests/tilegrid/run-tests.ps1

cmake_minimum_required(VERSION 3.20)
project(tilegrid-tests CXX)

# Default: a WebKit checkout named "webkit" or "WebKit" next to this
# repository. Pass -DWEBKIT_SOURCE_DIR=<path> if yours lives elsewhere.
set(WEBKIT_SOURCE_DIR "" CACHE PATH "Root of the patched WebKit tree (branch winuwp)")
if(NOT WEBKIT_SOURCE_DIR)
    get_filename_component(_apo_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    get_filename_component(_apo_side_by_side "${_apo_repo_root}/.." ABSOLUTE)
    foreach(_apo_candidate "webkit" "WebKit")
        if(EXISTS "${_apo_side_by_side}/${_apo_candidate}/Source/WebCore")
            set(WEBKIT_SOURCE_DIR "${_apo_side_by_side}/${_apo_candidate}")
            break()
        endif()
    endforeach()
    if(NOT WEBKIT_SOURCE_DIR)
        message(FATAL_ERROR
            "No WebKit checkout found next to ${_apo_repo_root}. "
            "Pass -DWEBKIT_SOURCE_DIR=<path to the patched webkitgtk-2.52.4 tree>.")
    endif()
    message(STATUS "WEBKIT_SOURCE_DIR defaulted to ${WEBKIT_SOURCE_DIR}")
endif()

set(TEXMAP_DIR "${WEBKIT_SOURCE_DIR}/Source/WebCore/platform/graphics/texmap")
set(SML_DIR "${WEBKIT_SOURCE_DIR}/Source/ThirdParty/sml")

foreach(required
    "${TEXMAP_DIR}/TextureMapperTileGridModel.h"
    "${TEXMAP_DIR}/TextureMapperTileGridModel.cpp"
    "${TEXMAP_DIR}/TextureMapperTileGridMachines.h"
    "${TEXMAP_DIR}/TextureMapperTileGridCore.h"
    "${TEXMAP_DIR}/TextureMapperTileGridCore.cpp"
    "${SML_DIR}/sml.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Not found: ${required}\nPass -DWEBKIT_SOURCE_DIR=<your WebKit tree>.")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(tilegrid-tests
    "${TEXMAP_DIR}/TextureMapperTileGridModel.cpp"
    "${TEXMAP_DIR}/TextureMapperTileGridCore.cpp"
    main.cpp
    invariants.cpp
    scenarios.cpp
    fakes.cpp
    core-scenarios.cpp
    fuzz.cpp
    replay.cpp
)

target_include_directories(tilegrid-tests PRIVATE "${TEXMAP_DIR}" "${SML_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tilegrid-tests PRIVATE
    TILEGRID_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # /bigobj: the SML transition tables are template-heavy.
    target_compile_options(tilegrid-tests PRIVATE /W3 /permissive- /bigobj /EHsc)
else()
    target_compile_options(tilegrid-tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME tilegrid COMMAND tilegrid-tests)
