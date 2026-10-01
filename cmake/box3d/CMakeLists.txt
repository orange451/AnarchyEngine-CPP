# Box3D builds here rather than through its own CMakeLists, which needs CMake
# 3.22 and C17 (/std:c17, Visual Studio 16.8). This compiles the same sources
# with the engine's runtime library, and c11_shim.h gives an older MSVC the
# one C11 feature Box3D uses that it lacks.
file(GLOB BOX3D_SOURCES CONFIGURE_DEPENDS "${BOX3D_SOURCE_DIR}/src/*.c")

add_library(box3d STATIC ${BOX3D_SOURCES})
# PUBLIC is SYSTEM, so the engine's warnings do not report on Box3D's headers.
target_include_directories(box3d SYSTEM PUBLIC "${BOX3D_SOURCE_DIR}/include")
target_include_directories(box3d PRIVATE "${BOX3D_SOURCE_DIR}/src")
set_target_properties(box3d PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(MSVC)
    target_compile_options(box3d PRIVATE /W3 "/FI${CMAKE_CURRENT_SOURCE_DIR}/c11_shim.h")
    # Physics is optimized in every configuration, as flecs is.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    target_compile_options(box3d PRIVATE $<$<CONFIG:Debug>:/O2>)
else()
    target_compile_options(box3d PRIVATE -ffp-contract=off)
    if(UNIX AND NOT APPLE)
        target_link_libraries(box3d PUBLIC m)
    endif()
endif()
add_library(box3d::box3d ALIAS box3d)
