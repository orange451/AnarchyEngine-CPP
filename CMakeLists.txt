cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(AnarchyEngine-CPP LANGUAGES CXX)

# JadeFX's macOS error dialog is Objective-C++. Enable it here so a nested
# add_subdirectory does not have to turn the language on after this cache exists.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Editors (clangd, VS Code) read this so they use the same include paths as the build.
# JadeFX lives outside this tree; without it, `#include "jadefx/jadefx.hpp"` looks missing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prefer a system GLFW (Homebrew, vcpkg, apt's libglfw3-dev). Otherwise download
# the pinned 3.5.1 source. Building that source on Linux needs the X11 and
# Wayland development packages, because GLFW 3.5 enables both backends.
find_package(glfw3 3.3 QUIET CONFIG)
if(NOT TARGET glfw)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
    endif()
endif()

if(TARGET glfw)
    set(RAINBOW_GLFW_TARGET glfw)
elseif(TARGET PkgConfig::GLFW3)
    set(RAINBOW_GLFW_TARGET PkgConfig::GLFW3)
else()
    include(FetchContent)
    FetchContent_Declare(
        glfw
        URL https://github.com/glfw/glfw/releases/download/3.5.1/glfw-3.5.1.zip
        URL_HASH SHA256=ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    set(RAINBOW_GLFW_TARGET glfw)
endif()

# MACOSX_BUNDLE is ignored on Windows and Linux, where this stays a normal executable.
add_executable(AnarchyEngine-CPP MACOSX_BUNDLE
    src/main.cpp
    src/gl.cpp
    src/ShaderFile.cpp
    src/Renderer.cpp
    src/PresentClock.cpp
    src/ide/IdePane.cpp
    src/ide/IdeDock.cpp
    src/ide/IdeConsole.cpp
    src/ide/IdeExplorer.cpp
    src/ide/IdeGameView.cpp
    src/ide/IdeTreeTest.cpp
    src/ide/IdeLayout.cpp
)

# PresentClock schedules an AppKit timer during live resize. The rest of the
# file stays C++; only this translation unit is compiled as Objective-C++.
if(APPLE)
    set_source_files_properties(src/PresentClock.cpp PROPERTIES LANGUAGE OBJCXX)
    set_target_properties(AnarchyEngine-CPP PROPERTIES
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED YES
        OBJCXX_EXTENSIONS NO
    )
endif()

# An explicit checkout wins. A sibling named JadeFX_CPP is next. Otherwise
# CMake clones the library, so this tree is enough to build.
set(JADEFX_CPP_DIR "" CACHE PATH "Path to a JadeFX checkout. Empty downloads it unless ../JadeFX_CPP exists.")
if(JADEFX_CPP_DIR)
    set(JADEFX_LOCAL "${JADEFX_CPP_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JadeFX_CPP/CMakeLists.txt")
    set(JADEFX_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/../JadeFX_CPP")
else()
    set(JADEFX_LOCAL "")
endif()
if(JADEFX_LOCAL)
    if(NOT EXISTS "${JADEFX_LOCAL}/CMakeLists.txt")
        message(FATAL_ERROR "JadeFX was not found at ${JADEFX_LOCAL}")
    endif()
    add_subdirectory("${JADEFX_LOCAL}" "${CMAKE_BINARY_DIR}/jadefx" EXCLUDE_FROM_ALL)
    set(JADEFX_SOURCE_ROOT "${JADEFX_LOCAL}")
else()
    include(FetchContent)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
        FetchContent_Declare(
            jadefx
            GIT_REPOSITORY https://github.com/orange451/JadeFX_CPP.git
            GIT_TAG master
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(jadefx)
    else()
        FetchContent_Declare(
            jadefx
            GIT_REPOSITORY https://github.com/orange451/JadeFX_CPP.git
            GIT_TAG master
        )
        FetchContent_GetProperties(jadefx)
        if(NOT jadefx_POPULATED)
            FetchContent_Populate(jadefx)
            add_subdirectory("${jadefx_SOURCE_DIR}" "${jadefx_BINARY_DIR}" EXCLUDE_FROM_ALL)
        endif()
    endif()
    set(JADEFX_SOURCE_ROOT "${jadefx_SOURCE_DIR}")
endif()

target_link_libraries(AnarchyEngine-CPP PRIVATE ${RAINBOW_GLFW_TARGET} jadefx)
if(APPLE)
    target_link_libraries(AnarchyEngine-CPP PRIVATE "-framework CoreVideo")
endif()

if(MSVC)
    target_compile_options(AnarchyEngine-CPP PRIVATE /W4 /utf-8)
else()
    target_compile_options(AnarchyEngine-CPP PRIVATE -Wall -Wextra)
endif()

if(APPLE)
    set_target_properties(AnarchyEngine-CPP PROPERTIES
        OUTPUT_NAME "AnarchyEngine-CPP"
        MACOSX_BUNDLE_BUNDLE_NAME "AnarchyEngine-CPP"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.anarchyengine-cpp"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    )

    # Copy the runtime shaders into the bundle, then sign it so Finder can launch it.
    add_custom_target(bundle-shaders ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JADEFX_SOURCE_ROOT}/res/shaders"
            "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources/shaders"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:AnarchyEngine-CPP>"
        DEPENDS AnarchyEngine-CPP
        VERBATIM
    )
else()
    add_custom_command(TARGET AnarchyEngine-CPP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JADEFX_SOURCE_ROOT}/res/shaders" "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/shaders"
        VERBATIM
    )
endif()
