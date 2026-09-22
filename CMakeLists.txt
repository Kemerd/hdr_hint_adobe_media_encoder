cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# HDR Hint - native companion app for Adobe Media Encoder.
#
# Targets (both platforms):
#   hdrhint_core   static lib: platform layer + engine (no UI, unit-testable)
#   hdrhint_ui     static lib: the custom UI toolkit, screens and view models
#   HdrHint        the app (HdrHint.exe on Windows, HdrHint.app on macOS)
#   hdrhint_cli    console front-end for scripts and verification
#   hdrhint_tests  console test runner (ctest)
#
# Platform backends:
#   Windows  MSVC, Win32, Direct3D 11 / Direct2D / DirectWrite / DirectComposition
#   macOS    Apple Clang, POSIX + Cocoa, CoreGraphics / CoreText / FSEvents
#
# Build (from the repo root) - or just run `make`, which drives all of this:
#   cmake -S . -B build                       # add -G "Visual Studio 17 2022" -A x64 on Windows
#   cmake --build build --config Release
#   ctest --test-dir build -C Release
# ---------------------------------------------------------------------------

# The deployment target must be set before project(). 13.3 is the first macOS
# whose libc++ ships floating-point std::to_chars, which std::format needs.
if(APPLE OR CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

project(HdrHint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators (Ninja, Unix Makefiles) default to Release.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Toolchain / platform selection
# ---------------------------------------------------------------------------
if(WIN32)
    # The Windows UI is Direct2D/DirectComposition and the code relies on MSVC.
    if(NOT MSVC)
        message(FATAL_ERROR "On Windows, HdrHint requires the MSVC toolchain (Visual Studio 2022).")
    endif()
    include(cmake/MsvcFlags.cmake)
    set(HH_WINDOWS ON)
elseif(APPLE)
    enable_language(OBJCXX)
    include(cmake/AppleFlags.cmake)
    set(HH_MACOS ON)
    option(HH_UNIVERSAL "Build a universal (arm64 + x86_64) binary" OFF)
    if(HH_UNIVERSAL)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
else()
    message(FATAL_ERROR "HdrHint builds on Windows (MSVC) and macOS (Apple Clang).")
endif()

# One helper applies the right warning/define set to every target.
function(hh_apply_flags target)
    if(HH_WINDOWS)
        hh_apply_msvc_flags(${target})
    else()
        hh_apply_apple_flags(${target})
    endif()
endfunction()

set(HH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# hdrhint_core: platform + engine
#
# Windows compiles every src/platform/*.cpp (the Win32 implementations).
# macOS compiles the two portable sources (Utf, Time: OS calls are #ifdef'd
# per platform) plus src/platform/posix/*.
# ---------------------------------------------------------------------------
file(GLOB HH_CORE_SOURCES CONFIGURE_DEPENDS ${HH_SRC}/core/*.cpp ${HH_SRC}/core/*.h)
file(GLOB HH_PLATFORM_HEADERS CONFIGURE_DEPENDS ${HH_SRC}/platform/*.h)

if(HH_WINDOWS)
    file(GLOB HH_PLATFORM_SOURCES CONFIGURE_DEPENDS ${HH_SRC}/platform/*.cpp)
else()
    file(GLOB HH_PLATFORM_SOURCES CONFIGURE_DEPENDS
        ${HH_SRC}/platform/posix/*.cpp
        ${HH_SRC}/platform/posix/*.mm
        ${HH_SRC}/platform/posix/*.h)
    list(APPEND HH_PLATFORM_SOURCES ${HH_SRC}/platform/Utf.cpp ${HH_SRC}/platform/Time.cpp)
endif()

add_library(hdrhint_core STATIC ${HH_CORE_SOURCES} ${HH_PLATFORM_HEADERS} ${HH_PLATFORM_SOURCES})
target_include_directories(hdrhint_core PUBLIC
    ${HH_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(HH_WINDOWS)
    target_link_libraries(hdrhint_core PUBLIC
        shell32 ole32 oleaut32 shlwapi advapi32 user32 version ntdll)
else()
    # FSEvents (CoreServices), CFString/CFPreferences, NSFileManager, NSWorkspace.
    target_link_libraries(hdrhint_core PUBLIC
        "-framework CoreFoundation" "-framework CoreServices" "-framework Foundation" "-framework AppKit")
endif()
hh_apply_flags(hdrhint_core)

# ---------------------------------------------------------------------------
# hdrhint_cli: console front-end for scripts and verification
#   hdrhint_cli --process <file> [--preset id] [--lut path|none] [--suffix s]
# ---------------------------------------------------------------------------
add_executable(hdrhint_cli ${CMAKE_CURRENT_SOURCE_DIR}/tools/cli_main.cpp)
target_link_libraries(hdrhint_cli PRIVATE hdrhint_core)
hh_apply_flags(hdrhint_cli)
add_custom_command(TARGET hdrhint_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/luts $<TARGET_FILE_DIR:hdrhint_cli>/luts
    COMMENT "Copying LUTs next to hdrhint_cli")

# ---------------------------------------------------------------------------
# hdrhint_ui: the toolkit, screens, view models and AME integration
#
# Shared sources compile on both platforms. The rendering / windowing
# backends and the HWND-level AME docking are Windows-only; src/ui/mac and
# src/ame/mac are their macOS counterparts.
# ---------------------------------------------------------------------------
# The macOS app is only configured once its entry point exists, so the core,
# the CLI and the tests can be built on their own (HH_CORE_ONLY=ON forces that).
option(HH_CORE_ONLY "Build only hdrhint_core, hdrhint_cli and hdrhint_tests" OFF)
set(HH_BUILD_APP ON)
if(HH_CORE_ONLY OR (HH_MACOS AND NOT EXISTS ${HH_SRC}/app/mac/main.mm))
    set(HH_BUILD_APP OFF)
    message(STATUS "HdrHint: building the core, CLI and tests only")
endif()

if(HH_BUILD_APP)
file(GLOB_RECURSE HH_UI_SOURCES CONFIGURE_DEPENDS
    ${HH_SRC}/ui/*.cpp ${HH_SRC}/ui/*.mm ${HH_SRC}/ui/*.h
    ${HH_SRC}/ame/*.cpp ${HH_SRC}/ame/*.mm ${HH_SRC}/ame/*.h)

# Windows-only units: Direct2D/DirectComposition backend, Win32 windows, HWND docking.
# By convention a shared folder's Windows-only units end in "Win.cpp".
set(HH_WINDOWS_ONLY_UI_REGEX
    "/ui/window/|/ui/gfx/(Canvas|GraphicsDevice|SwapChainSurface|TextCache)\\.cpp$|Win\\.cpp$|/ame/(AmeProcess|DockController|PanelWindowLocator)\\.cpp$")

if(HH_WINDOWS)
    list(FILTER HH_UI_SOURCES EXCLUDE REGEX "/mac/")
else()
    list(FILTER HH_UI_SOURCES EXCLUDE REGEX "${HH_WINDOWS_ONLY_UI_REGEX}")
endif()

add_library(hdrhint_ui STATIC ${HH_UI_SOURCES})
target_link_libraries(hdrhint_ui PUBLIC hdrhint_core)
if(HH_WINDOWS)
    target_link_libraries(hdrhint_ui PUBLIC
        d3d11 dxgi d2d1 dwrite dcomp dwmapi shcore imm32 windowscodecs comctl32 wtsapi32 gdi32 uxtheme)
else()
    target_link_libraries(hdrhint_ui PUBLIC
        "-framework AppKit" "-framework CoreGraphics" "-framework CoreText" "-framework QuartzCore"
        "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework UserNotifications"
        "-framework ApplicationServices")
endif()
hh_apply_flags(hdrhint_ui)

# The guide is embedded as RCDATA (Windows) / shipped in Resources (macOS)
# from resources/guide.md, a verbatim copy of docs/GUIDE.md refreshed at
# configure time so the two never drift.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md
               ${CMAKE_CURRENT_SOURCE_DIR}/resources/guide.md COPYONLY)

if(HH_WINDOWS)
    # -----------------------------------------------------------------------
    # HdrHint.exe
    #
    # The manifest is passed as a *source* so CMake hands it to the linker via
    # /MANIFESTINPUT and merges it with the auto-generated one. Do NOT also
    # embed an RT_MANIFEST resource in the .rc (CVT1100 duplicate resource).
    # -----------------------------------------------------------------------
    add_executable(HdrHint WIN32
        ${HH_SRC}/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/HdrHint.rc
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/HdrHint.manifest)
    target_include_directories(HdrHint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    target_link_libraries(HdrHint PRIVATE hdrhint_ui)
    hh_apply_flags(HdrHint)
    set_target_properties(HdrHint PROPERTIES
        OUTPUT_NAME "HdrHint"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:HdrHint>")

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
    # -----------------------------------------------------------------------
    # HdrHint.app
    #
    # A regular Cocoa bundle: menu-bar item + window, LSUIElement off so it
    # shows in the Dock while a window is open (the app hides the Dock icon
    # when it retreats to the menu bar). Assets go to Contents/Resources,
    # where platform::resourceDirectory() looks for them.
    # -----------------------------------------------------------------------
    set(HH_ICNS ${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/HdrHint.icns)
    set_source_files_properties(${HH_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    add_executable(HdrHint MACOSX_BUNDLE ${HH_SRC}/app/mac/main.mm ${HH_ICNS})
    target_link_libraries(HdrHint PRIVATE hdrhint_ui)
    hh_apply_flags(HdrHint)
    set_target_properties(HdrHint PROPERTIES
        OUTPUT_NAME "HdrHint"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "HDR Hint"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.everett.hdrhint"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "HdrHint.icns"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 D Everett Hinton. MIT License.")

    # Assets into Contents/Resources, then one ad-hoc signature over the whole
    # bundle so the seal covers what was just copied in (Apple silicon refuses
    # to run a binary whose signature does not match its bundle).
    add_custom_command(TARGET HdrHint POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/luts "$<TARGET_BUNDLE_CONTENT_DIR:HdrHint>/Resources/luts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/cep "$<TARGET_BUNDLE_CONTENT_DIR:HdrHint>/Resources/cep"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md "$<TARGET_BUNDLE_CONTENT_DIR:HdrHint>/Resources/GUIDE.md"
        COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:HdrHint>"
        COMMENT "Copying LUTs, CEP panel and guide into HdrHint.app and signing it (ad hoc)")
endif()
endif() # HH_BUILD_APP

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
file(GLOB HH_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(hdrhint_tests ${HH_TEST_SOURCES})
target_link_libraries(hdrhint_tests PRIVATE hdrhint_core)
hh_apply_flags(hdrhint_tests)

enable_testing()
add_test(NAME hdrhint_unit COMMAND hdrhint_tests)

# ---------------------------------------------------------------------------
# Install / package
#
#   cmake --install build --prefix <dir>     app + cli + assets
#   cpack --config build/CPackConfig.cmake   HdrHint-<ver>-win64.zip / HdrHint-<ver>.dmg
# ---------------------------------------------------------------------------
if(HH_WINDOWS)
    if(HH_BUILD_APP)
        install(TARGETS HdrHint RUNTIME DESTINATION .)
    endif()
    install(TARGETS hdrhint_cli RUNTIME DESTINATION .)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/luts ${CMAKE_CURRENT_SOURCE_DIR}/cep DESTINATION .)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md DESTINATION .)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_SYSTEM_NAME win64)
else()
    # The bundle carries its own assets; the CLI goes to bin with a luts folder.
    if(HH_BUILD_APP)
        install(TARGETS HdrHint BUNDLE DESTINATION .)
    endif()
    install(TARGETS hdrhint_cli RUNTIME DESTINATION bin)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "HDR Hint")
    set(CPACK_DMG_FORMAT UDZO)
    set(CPACK_SYSTEM_NAME macOS)
endif()
set(CPACK_PACKAGE_NAME "HdrHint")
set(CPACK_PACKAGE_VENDOR "D Everett Hinton")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Adobe Media Encoder companion for HDR10 metadata and SDR-hint LUTs")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
