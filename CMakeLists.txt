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

# Everything in the studio except main, so a test can build the real layout.
set(STUDIO_SOURCES
    src/runner/gl.cpp
    src/runner/ShaderFile.cpp
    src/runner/Renderer.cpp
    src/runner/Runner.cpp
    src/runner/GameView.cpp
    src/ide/IdePane.cpp
    src/ide/IdeDock.cpp
    src/ide/DockArrange.cpp
    src/ide/IdeConsole.cpp
    src/ide/ConsoleLog.cpp
    src/ide/CompletionPopup.cpp
    src/ide/InsertPopup.cpp
    src/ide/IdeExplorer.cpp
    src/ide/IdeIcons.cpp
    src/ide/IdeResources.cpp
    src/ide/IdeScriptEditor.cpp
    src/ide/IdeSearch.cpp
    src/ide/FindBar.cpp
    src/ide/TextSearch.cpp
    src/ide/ScriptMarks.cpp
    src/ide/ScriptPairs.cpp
    src/ide/ColorLiterals.cpp
    src/ide/LuauHighlight.cpp
    src/ide/LuauComplete.cpp
    src/ide/IdeTheme.cpp
    src/ide/IdeTreeTest.cpp
    src/ide/IdeLayout.cpp
    src/ide/TextUndoStack.cpp
    src/ide/InputRouter.cpp
    src/ide/CutSet.cpp
    src/ide/PropertySheet.cpp
    src/ide/PropertiesPanel.cpp
    src/ide/Preferences.cpp
    src/ide/PreferencesPanel.cpp
    src/ide/ThemeLibrary.cpp
    src/ide/TextWrap.cpp
    src/ide/McpServer.cpp
    src/ide/McpTools.cpp
    src/ide/StudioRegistry.cpp
)

# MACOSX_BUNDLE is ignored on Windows and Linux, where this stays a normal executable.
# GLFW comes in with JadeFX. That library creates the window.
add_executable(AnarchyEngine-CPP MACOSX_BUNDLE
    src/main.cpp
    ${STUDIO_SOURCES}
)

# The program icon comes from resources/icon. make_icons.py there rebuilds the
# .ico and .icns from anarchy.png.
set(ANARCHY_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon")
if(WIN32)
    # Written with an absolute path so rc and windres find the icon from any directory.
    set(ANARCHY_ICON_ICO "${ANARCHY_ICON_DIR}/anarchy.ico")
    configure_file("${ANARCHY_ICON_DIR}/anarchy.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/anarchy.rc" @ONLY)
    target_sources(AnarchyEngine-CPP PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/anarchy.rc")
elseif(APPLE)
    # Lands in Contents/Resources, where CFBundleIconFile looks.
    target_sources(AnarchyEngine-CPP PRIVATE "${ANARCHY_ICON_DIR}/anarchy.icns")
    set_source_files_properties("${ANARCHY_ICON_DIR}/anarchy.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
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

# Luau is the sandboxed Lua runtime. engine_core and engine_datatypes are the only targets that link it.
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

# cpp-httplib is the MCP server's HTTP listener: one MIT-licensed
# header, fetched the way JadeFX fetches stb, pinned by tag and hash.
FetchContent_Declare(
    httplib
    URL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.58.0/httplib.h
    URL_HASH SHA256=aa14e7e7bd2703694e0a6b6855af3b8c406102ab1fc56ac905fe33619b31faa5
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
    # The header has no CMakeLists, so populate it directly.
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(httplib)
endif()

# The engine is four packages, each a static library with its own directory:
#   engine_core       DataModel, events, threads, the script runtime, projects
#   engine_datatypes  values: Vector2, Vector3, Color, Color3, Transform, Enum
#   engine_instances  what Instance.new makes: GameObject, Folder, Script, ...
#   engine_services   Game and what game:GetService returns
# The dependencies are not one-way yet: DataModel knows GameObject and Script,
# the runtime binds the services, and Vector3 registers through core's LuaApi.
# So the four link each other. CMake allows that cycle between static libraries
# and repeats them on the link line.
# A target that links engine_core gets all four and their include directories.
add_library(engine_datatypes STATIC
    src/engine_datatypes/Color3.cpp
    src/engine_datatypes/Enum.cpp
    src/engine_datatypes/Vector2.cpp
    src/engine_datatypes/Vector3.cpp
)
add_library(engine_instances STATIC
    src/engine_instances/Folder.cpp
    src/engine_instances/GameObject.cpp
    src/engine_instances/LuaSource.cpp
    src/engine_instances/ModuleScript.cpp
    src/engine_instances/Script.cpp
    src/engine_instances/TestTriangle.cpp
)
add_library(engine_services STATIC
    src/engine_services/ChangeHistoryService.cpp
    src/engine_services/Game.cpp
    src/engine_services/UserInputService.cpp
    src/engine_services/RunService.cpp
    src/engine_services/SelectionService.cpp
)
add_library(engine_core STATIC
    src/engine_core/Contract.cpp
    src/engine_core/InvalidationQueue.cpp
    src/engine_core/DataModel.cpp
    src/engine_core/DataModelLock.cpp
    src/engine_core/SnapshotPump.cpp
    src/engine_core/TaskScheduler.cpp
    src/engine_core/Events.cpp
    src/engine_core/Engine.cpp
    src/engine_core/LuaEngine.cpp
    src/engine_core/LuaApi.cpp
    src/engine_core/LuaReflect.cpp
    src/engine_core/ScriptRuntime.cpp
    src/engine_core/TableSnapshot.cpp
    src/engine_core/ScriptAnalysis.cpp
    src/engine_core/AnalysisDefinitions.cpp
    src/engine_core/PropertyBag.cpp
    src/engine_core/Project.cpp
)
set(ENGINE_PACKAGES engine_core engine_datatypes engine_instances engine_services)
foreach(package IN LISTS ENGINE_PACKAGES)
    target_include_directories(${package} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/${package}")
    target_compile_features(${package} PUBLIC cxx_std_17)
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(engine_core PUBLIC Threads::Threads engine_datatypes engine_instances engine_services)
target_link_libraries(engine_datatypes PUBLIC engine_core)
target_link_libraries(engine_instances PUBLIC engine_core)
target_link_libraries(engine_services PUBLIC engine_core)
target_link_libraries(engine_core PRIVATE Luau.Compiler Luau.VM Luau.Analysis)
# Vector3 is a Luau vector, and Enum items are Luau userdata.
target_link_libraries(engine_datatypes PRIVATE Luau.VM)

set(ENGINE_CORE_TSAN OFF CACHE BOOL "Instrument the engine packages and sandbox with ThreadSanitizer")
if(ENGINE_CORE_TSAN AND NOT MSVC)
    foreach(package IN LISTS ENGINE_PACKAGES)
        target_compile_options(${package} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
    endforeach()
    target_link_options(engine_core PUBLIC -fsanitize=thread)
endif()

# Sources under src/ include each other as "ide/..." and "runner/...".
target_include_directories(AnarchyEngine-CPP PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${httplib_SOURCE_DIR}")
target_link_libraries(AnarchyEngine-CPP PRIVATE jadefx engine_core)
# httplib's sockets. MSVC links these from a pragma in the header; MinGW needs them here.
if(WIN32)
    target_link_libraries(AnarchyEngine-CPP PRIVATE ws2_32 crypt32)
endif()

add_executable(shell-tests
    tests/DockArrangeTest.cpp
    src/ide/DockArrange.cpp
    src/ide/IdeDock.cpp
    src/ide/IdeIcons.cpp
    src/ide/IdePane.cpp
    src/ide/IdeResources.cpp
)
target_include_directories(shell-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(shell-tests PRIVATE jadefx)
target_compile_features(shell-tests PRIVATE cxx_std_17)

# Drives the explorer through a headless scene: clicks, typing, and keys.
add_executable(explorer-tests
    tests/ExplorerRenameTest.cpp
    src/ide/CutSet.cpp
    src/ide/IdeExplorer.cpp
    src/ide/IdeIcons.cpp
    src/ide/IdePane.cpp
    src/ide/IdeResources.cpp
    src/ide/IdeTheme.cpp
    src/ide/InsertPopup.cpp
)
target_include_directories(explorer-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(explorer-tests PRIVATE jadefx engine_core)
target_compile_features(explorer-tests PRIVATE cxx_std_17)

# Opens and closes printed tables in the console log, by call and by click.
add_executable(console-tests
    tests/ConsoleLogTest.cpp
    src/ide/ConsoleLog.cpp
    src/ide/IdeResources.cpp
    src/ide/IdeTheme.cpp
)
target_include_directories(console-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(console-tests PRIVATE jadefx engine_core)
target_compile_features(console-tests PRIVATE cxx_std_17)

# Drives the Properties page next to an explorer: selection, mixed rows, commits, and undo.
add_executable(properties-tests
    tests/PropertiesTest.cpp
    src/ide/CutSet.cpp
    src/ide/IdeExplorer.cpp
    src/ide/IdeIcons.cpp
    src/ide/IdePane.cpp
    src/ide/IdeResources.cpp
    src/ide/IdeTheme.cpp
    src/ide/InsertPopup.cpp
    src/ide/PropertySheet.cpp
    src/ide/PropertiesPanel.cpp
    src/ide/TextUndoStack.cpp
)
target_include_directories(properties-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(properties-tests PRIVATE jadefx engine_core)
target_compile_features(properties-tests PRIVATE cxx_std_17)

# R10: the default dock layout, built headless from the studio's own sources.
add_executable(studio-tests
    tests/StudioLayoutTest.cpp
    tests/FindReplaceTest.cpp
    tests/ThemeTest.cpp
    tests/PreferencesTest.cpp
    ${STUDIO_SOURCES}
)
target_include_directories(studio-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${httplib_SOURCE_DIR}")
target_link_libraries(studio-tests PRIVATE jadefx engine_core)
target_compile_features(studio-tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(studio-tests PRIVATE ws2_32 crypt32)
endif()

# The MCP server: JSON-RPC by call, the engine tools against a live place,
# round trips over real loopback sockets, the studio registry, and the bridge.
add_executable(mcp-tests
    tests/McpTest.cpp
    src/bridge/StudioBridge.cpp
    src/ide/IdeResources.cpp
    src/ide/McpServer.cpp
    src/ide/McpTools.cpp
    src/ide/PropertySheet.cpp
    src/ide/StudioRegistry.cpp
)
target_include_directories(mcp-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${httplib_SOURCE_DIR}")
target_link_libraries(mcp-tests PRIVATE engine_core)
target_compile_features(mcp-tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(mcp-tests PRIVATE ws2_32 crypt32)
endif()

# anarchy-mcp: the MCP server an LLM client runs over stdio. It sends each
# tool call to one open studio, found through the studio registry.
add_executable(anarchy-mcp
    src/bridge/main.cpp
    src/bridge/StudioBridge.cpp
    src/ide/IdeResources.cpp
    src/ide/McpServer.cpp
    src/ide/McpTools.cpp
    src/ide/PropertySheet.cpp
    src/ide/StudioRegistry.cpp
)
target_include_directories(anarchy-mcp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${httplib_SOURCE_DIR}")
target_link_libraries(anarchy-mcp PRIVATE engine_core)
target_compile_features(anarchy-mcp PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(anarchy-mcp PRIVATE ws2_32 crypt32)
endif()

add_executable(engine-tests
    tests/LuaEngineTest.cpp
    tests/ColorLiteralsTest.cpp
    tests/LuauHighlightTest.cpp
    tests/LuauCompleteTest.cpp
    tests/ScriptMarksTest.cpp
    tests/ScriptPairsTest.cpp
    tests/TextSearchTest.cpp
    tests/TextWrapTest.cpp
    src/ide/ColorLiterals.cpp
    src/ide/LuauHighlight.cpp
    src/ide/LuauComplete.cpp
    src/ide/ScriptMarks.cpp
    src/ide/ScriptPairs.cpp
    src/ide/TextSearch.cpp
    src/ide/TextWrap.cpp
    src/runner/Runner.cpp
)
target_include_directories(engine-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(engine-tests PRIVATE engine_core)
target_compile_features(engine-tests PRIVATE cxx_std_17)

# Catch2 is only the sandbox acceptance suite.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Catch2)
else()
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(Catch2)
    if(NOT Catch2_POPULATED)
        FetchContent_Populate(Catch2)
        add_subdirectory("${Catch2_SOURCE_DIR}" "${Catch2_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

add_executable(sandbox
    sandbox/tests.cpp
    sandbox/analysis_tests.cpp
    sandbox/history_tests.cpp
    sandbox/project_tests.cpp
    src/ide/TextUndoStack.cpp
    src/ide/InputRouter.cpp
)
target_include_directories(sandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sandbox PRIVATE engine_core Catch2::Catch2WithMain)
target_compile_features(sandbox PRIVATE cxx_std_17)
if(ENGINE_CORE_TSAN AND NOT MSVC)
    target_compile_options(sandbox PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g -O1)
endif()

if(MSVC)
    target_compile_options(AnarchyEngine-CPP PRIVATE /W4 /utf-8)
    target_compile_options(engine-tests PRIVATE /W4 /utf-8)
    target_compile_options(shell-tests PRIVATE /W4 /utf-8)
    target_compile_options(properties-tests PRIVATE /W4 /utf-8)
target_compile_options(console-tests PRIVATE /W4 /utf-8)
    target_compile_options(studio-tests PRIVATE /W4 /utf-8)
    target_compile_options(mcp-tests PRIVATE /W4 /utf-8)
    target_compile_options(anarchy-mcp PRIVATE /W4 /utf-8)
    foreach(package IN LISTS ENGINE_PACKAGES)
        target_compile_options(${package} PRIVATE /W4 /utf-8)
    endforeach()
    target_compile_options(sandbox PRIVATE /W4 /utf-8)
else()
    target_compile_options(AnarchyEngine-CPP PRIVATE -Wall -Wextra)
    target_compile_options(engine-tests PRIVATE -Wall -Wextra)
    target_compile_options(shell-tests PRIVATE -Wall -Wextra)
    target_compile_options(properties-tests PRIVATE -Wall -Wextra)
target_compile_options(console-tests PRIVATE -Wall -Wextra)
    target_compile_options(studio-tests PRIVATE -Wall -Wextra)
    target_compile_options(mcp-tests PRIVATE -Wall -Wextra)
    target_compile_options(anarchy-mcp PRIVATE -Wall -Wextra)
    foreach(package IN LISTS ENGINE_PACKAGES)
        target_compile_options(${package} PRIVATE -Wall -Wextra)
    endforeach()
    target_compile_options(sandbox PRIVATE -Wall -Wextra)
endif()

if(APPLE)
    set_target_properties(AnarchyEngine-CPP PROPERTIES
        OUTPUT_NAME "AnarchyEngine-CPP"
        MACOSX_BUNDLE_BUNDLE_NAME "AnarchyEngine-CPP"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.anarchyengine-cpp"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_ICON_FILE "anarchy.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    )

    # Copy the resources tree into the bundle, then sign it so Finder can launch it.
    # Source resources/ maps onto Contents/Resources, so resources/shaders lands in
    # Contents/Resources/shaders. JadeFX's interface shaders join that directory.
    add_custom_target(bundle-resources ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JADEFX_SOURCE_ROOT}/res/shaders"
            "$<TARGET_BUNDLE_CONTENT_DIR:AnarchyEngine-CPP>/Resources/shaders"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:AnarchyEngine-CPP>"
        DEPENDS AnarchyEngine-CPP
        VERBATIM
    )
else()
    add_custom_command(TARGET AnarchyEngine-CPP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JADEFX_SOURCE_ROOT}/res/shaders" "$<TARGET_FILE_DIR:AnarchyEngine-CPP>/resources/shaders"
        VERBATIM
    )
endif()
