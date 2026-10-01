# miniaudio builds here from one translation unit, miniaudio_impl.c, which
# compiles miniaudio.c with stb_vorbis beside it. Its own CMakeLists builds
# every example and decoder; the engine needs only the library.
add_library(miniaudio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio_impl.c")
# PUBLIC is SYSTEM, so the engine's warnings do not report on miniaudio.h.
target_include_directories(miniaudio SYSTEM PUBLIC "${MINIAUDIO_SOURCE_DIR}")
# The engine only plays: it never writes an audio file.
target_compile_definitions(miniaudio PUBLIC MA_NO_ENCODING)
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
    # Mixing is optimized in every configuration, as flecs and Box3D are.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    target_compile_options(miniaudio PRIVATE $<$<CONFIG:Debug>:/O2>)
else()
    target_compile_options(miniaudio PRIVATE -w)
    if(APPLE)
        # miniaudio loads Core Audio at run time, but links these when it cannot.
        target_link_libraries(miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio"
                                               "-framework AudioToolbox")
    elseif(UNIX)
        target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio PUBLIC Threads::Threads)
endif()
