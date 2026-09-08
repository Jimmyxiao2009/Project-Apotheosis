# Apotheosis: host tests for the TileGrid v2 model (TILING-REWRITE-PLAN.md 5.4).
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

set(WEBKIT_SOURCE_DIR "C:/code/apotheosis/webkit" CACHE PATH "Root of the patched WebKit tree (branch winuwp)")

set(TEXMAP_DIR "${WEBKIT_SOURCE_DIR}/Source/WebCore/platform/graphics/texmap")
set(SML_DIR "${WEBKIT_SOURCE_DIR}/Source/ThirdParty/sml")

foreach(required
    "${TEXMAP_DIR}/TextureMapperTileGridModel.h"
    "${TEXMAP_DIR}/TextureMapperTileGridModel.cpp"
    "${TEXMAP_DIR}/TextureMapperTileGridMachines.h"
    "${SML_DIR}/sml.hpp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Not found: ${required}\nPass -DWEBKIT_SOURCE_DIR=<your WebKit tree>.")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(tilegrid-tests
    "${TEXMAP_DIR}/TextureMapperTileGridModel.cpp"
    main.cpp
    invariants.cpp
    scenarios.cpp
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
