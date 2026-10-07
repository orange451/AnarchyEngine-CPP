# zstd's library builds here rather than through its own CMakeLists (under
# build/cmake, which also builds its programs). The same sources, the
# engine's runtime library, and no assembly: MSVC does not build the .S file,
# so ZSTD_DISABLE_ASM picks the C Huffman decoder everywhere.
file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS
    "${ZSTD_SOURCE_DIR}/lib/common/*.c"
    "${ZSTD_SOURCE_DIR}/lib/compress/*.c"
    "${ZSTD_SOURCE_DIR}/lib/decompress/*.c")
add_library(zstd STATIC ${ZSTD_SOURCES})
# PUBLIC is SYSTEM, so the engine's warnings do not report on zstd's headers.
target_include_directories(zstd SYSTEM PUBLIC "${ZSTD_SOURCE_DIR}/lib")
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
if(MSVC)
    target_compile_options(zstd PRIVATE /W3)
    # Compression is optimized in every configuration, as Box3D is: a Debug
    # build still saves and loads large islands quickly.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    target_compile_options(zstd PRIVATE $<$<CONFIG:Debug>:/O2>)
endif()
