# meshoptimizer's library builds here rather than through its own CMakeLists,
# as zstd's does: just its sources, built the engine's way.
file(GLOB MESHOPTIMIZER_SOURCES CONFIGURE_DEPENDS "${MESHOPTIMIZER_SOURCE_DIR}/src/*.cpp")
add_library(meshoptimizer STATIC ${MESHOPTIMIZER_SOURCES})
# PUBLIC is SYSTEM, so the engine's warnings do not report on its headers.
target_include_directories(meshoptimizer SYSTEM PUBLIC "${MESHOPTIMIZER_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(meshoptimizer PRIVATE /W3)
    # Simplification is optimized in every configuration, as zstd's compression is.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    target_compile_options(meshoptimizer PRIVATE $<$<CONFIG:Debug>:/O2>)
endif()
