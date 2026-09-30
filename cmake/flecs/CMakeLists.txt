# flecs builds in its own directory so its Debug C flags can differ from the
# engine's: /RTC1 cannot combine with /O2, and flecs is optimized in every
# configuration (a Debug entity read is ~14 ns instead of ~340 ns).
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/Od" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

add_library(flecs STATIC "${FLECS_SOURCE_DIR}/flecs.c")
target_include_directories(flecs PUBLIC "${FLECS_SOURCE_DIR}")
# Enum reflection misreads MSVC 19.23's __FUNCSIG__ and asserts (Debug) or
# crashes (Release) while the world is built. The rest keeps only the addons
# the engine uses; flecs starts no threads and serves nothing.
target_compile_definitions(flecs PUBLIC
    FLECS_CPP_NO_ENUM_REFLECTION FLECS_CUSTOM_BUILD FLECS_CPP FLECS_LOG FLECS_OS_API_IMPL)
# flecs refuses FLECS_DEBUG beside NDEBUG, which every non-Debug configuration
# defines. So checks are FLECS_DEBUG in Debug and FLECS_KEEP_ASSERT (asserts
# kept under FLECS_NDEBUG) in the others.
if(ENGINE_FLECS_CHECKS)
    target_compile_definitions(flecs PUBLIC
        $<$<CONFIG:Debug>:FLECS_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:FLECS_NDEBUG>
        $<$<NOT:$<CONFIG:Debug>>:FLECS_KEEP_ASSERT>)
else()
    target_compile_definitions(flecs PUBLIC FLECS_NDEBUG)
endif()
if(MSVC)
    target_compile_options(flecs PRIVATE $<$<CONFIG:Debug>:/O2>)
endif()
