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
# MSVC 19.23 at /O2 miscompiles mesh.c: copying a 12-byte b3Vec3 into a
# b3MeshNode writes 16 bytes, over the bitfields after it, so the BVH a
# triangle mesh builds is corrupt and its first walk crashes. With the
# compiler's SSA optimizer off (/d2SSAOptimizer-) the bug is gone: mesh.c
# stays optimized and builds byte-for-byte the meshes an /Od build does,
# about four times faster (one dense terrain chunk: 47 ms to 11 ms, the flag
# alone, before build_terrain_collider's useMedianSplit, which brings a
# terrain chunk lower still, to about 4.5 ms). Custom shapes and every
# terrain chunk's collider go through it. PhysicsWorld's P39 test
# (sandbox/physics_tests.cpp) checks both split paths' b3RayCastMesh
# against brute-force ray-triangle intersection, in Debug and Release.
if(MSVC AND CMAKE_C_COMPILER_VERSION VERSION_LESS 19.28)
    set_source_files_properties("${BOX3D_SOURCE_DIR}/src/mesh.c" PROPERTIES COMPILE_OPTIONS "/d2SSAOptimizer-")
endif()
add_library(box3d::box3d ALIAS box3d)
