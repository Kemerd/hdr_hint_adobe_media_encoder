cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# HDR Hint - native companion app for Adobe Media Encoder.
#
# Targets:
#   hdrhint_core   static lib: platform wrappers + engine (no UI, unit-testable)
#   hdrhint_ui     static lib: Direct2D/DirectComposition UI toolkit + AME docking
#   HdrHint        the WIN32 executable
#   hdrhint_tests  console test runner (ctest)
#
# Build (from the repo root):
#   cmake -S hdr_hint -B hdr_hint/build -G "Visual Studio 17 2022" -A x64
#   cmake --build hdr_hint/build --config Release
#   ctest --test-dir hdr_hint/build -C Release
# ---------------------------------------------------------------------------
project(HdrHint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only MSVC on Windows is supported; the UI is Direct2D/DirectComposition.
if(NOT MSVC)
    message(FATAL_ERROR "HdrHint requires the MSVC toolchain (Visual Studio 2022).")
endif()

include(cmake/MsvcFlags.cmake)

# ---------------------------------------------------------------------------
# hdrhint_core: platform + engine
# ---------------------------------------------------------------------------
file(GLOB_RECURSE HH_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h)

add_library(hdrhint_core STATIC ${HH_CORE_SOURCES})
target_include_directories(hdrhint_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hdrhint_core PUBLIC
    shell32 ole32 oleaut32 shlwapi advapi32 user32 version ntdll)
hh_apply_msvc_flags(hdrhint_core)

# ---------------------------------------------------------------------------
# hdrhint_cli: console front-end for scripts and verification
#   hdrhint_cli.exe --process <file> [--preset id] [--lut path|none] [--suffix s]
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/cli_main.cpp)
    add_executable(hdrhint_cli ${CMAKE_CURRENT_SOURCE_DIR}/tools/cli_main.cpp)
    target_link_libraries(hdrhint_cli PRIVATE hdrhint_core)
    hh_apply_msvc_flags(hdrhint_cli)
    add_custom_command(TARGET hdrhint_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/luts $<TARGET_FILE_DIR:hdrhint_cli>/luts
        COMMENT "Copying LUTs next to hdrhint_cli.exe")
endif()

# ---------------------------------------------------------------------------
# hdrhint_ui: Direct2D toolkit, screens, AME docking
# (only configured once UI sources exist, so the core can be built first)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE HH_UI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ame/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ame/*.h)

if(HH_UI_SOURCES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
    add_library(hdrhint_ui STATIC ${HH_UI_SOURCES})
    target_link_libraries(hdrhint_ui PUBLIC
        hdrhint_core
        d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore imm32 windowscodecs comctl32 wtsapi32 gdi32 uxtheme)
    hh_apply_msvc_flags(hdrhint_ui)

    # -----------------------------------------------------------------------
    # HdrHint.exe
    #
    # The manifest is passed as a *source* so CMake hands it to the linker via
    # /MANIFESTINPUT and merges it with the auto-generated one. Do NOT also
    # embed an RT_MANIFEST resource in the .rc (CVT1100 duplicate resource).
    # -----------------------------------------------------------------------
    add_executable(HdrHint WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/HdrHint.rc
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/HdrHint.manifest)
    target_include_directories(HdrHint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    target_link_libraries(HdrHint PRIVATE hdrhint_ui)
    hh_apply_msvc_flags(HdrHint)
    set_target_properties(HdrHint PROPERTIES
        OUTPUT_NAME "HdrHint"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:HdrHint>")

    # The guide is embedded as RCDATA from resources/guide.md, a verbatim copy
    # of docs/GUIDE.md refreshed at configure time so the two never drift.
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md
                   ${CMAKE_CURRENT_SOURCE_DIR}/resources/guide.md COPYONLY)

    # Ship the LUTs and the CEP panel next to the executable.
    add_custom_command(TARGET HdrHint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/luts $<TARGET_FILE_DIR:HdrHint>/luts
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/cep $<TARGET_FILE_DIR:HdrHint>/cep
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md $<TARGET_FILE_DIR:HdrHint>/GUIDE.md
        COMMENT "Copying LUTs, CEP panel and guide next to HdrHint.exe")
else()
    message(STATUS "HdrHint: UI sources not present yet - building core, cli and tests only")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
file(GLOB HH_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(hdrhint_tests ${HH_TEST_SOURCES})
target_link_libraries(hdrhint_tests PRIVATE hdrhint_core)
hh_apply_msvc_flags(hdrhint_tests)

enable_testing()
add_test(NAME hdrhint_unit COMMAND hdrhint_tests)
