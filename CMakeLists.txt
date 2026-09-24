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

# MACOSX_BUNDLE is ignored on Windows and Linux, where this stays a normal executable.
# GLFW comes in with JadeFX. That library creates the window.
add_executable(AnarchyEngine-CPP MACOSX_BUNDLE
    src/main.cpp
    src/runner/gl.cpp
    src/runner/ShaderFile.cpp
    src/runner/Renderer.cpp
    src/runner/Runner.cpp
    src/ide/IdePane.cpp
    src/ide/IdeDock.cpp
    src/ide/IdeConsole.cpp
    src/ide/IdeExplorer.cpp
    src/ide/IdeGameView.cpp
    src/ide/IdeTreeTest.cpp
    src/ide/IdeLayout.cpp
)

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

# Luau is the sandboxed Lua runtime. The engine package is the only thing that links it.
# The CLI, tests, and web target are not part of the embed.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_VERSION 0.739)
include(FetchContent)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    FetchContent_Declare(
        luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG ${LUAU_VERSION}
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(luau)
else()
    FetchContent_Declare(
        luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG ${LUAU_VERSION}
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(luau)
    if(NOT luau_POPULATED)
        FetchContent_Populate(luau)
        add_subdirectory("${luau_SOURCE_DIR}" "${luau_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

add_library(engine STATIC
    src/engine/LuaEngine.cpp
)
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(engine PRIVATE Luau.Compiler Luau.VM)
target_compile_features(engine PUBLIC cxx_std_17)

target_link_libraries(AnarchyEngine-CPP PRIVATE jadefx engine)

add_executable(engine-tests
    tests/LuaEngineTest.cpp
    src/runner/Runner.cpp
)
target_link_libraries(engine-tests PRIVATE engine)
target_compile_features(engine-tests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(AnarchyEngine-CPP PRIVATE /W4 /utf-8)
    target_compile_options(engine PRIVATE /W4 /utf-8)
    target_compile_options(engine-tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(AnarchyEngine-CPP PRIVATE -Wall -Wextra)
    target_compile_options(engine PRIVATE -Wall -Wextra)
    target_compile_options(engine-tests PRIVATE -Wall -Wextra)
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
