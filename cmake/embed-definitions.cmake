if(NOT INPUT OR NOT OUTPUT)
    message(FATAL_ERROR "embed-definitions.cmake needs -DINPUT and -DOUTPUT")
endif()

file(READ "${INPUT}" CONTENT)
get_filename_component(OUTDIR "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTDIR}")

# Raw-string delimiter is at most 16 characters. The definition file must not contain )ENGDEF.
set(BODY "namespace engine_core {\nextern const char kEngineDefinitionSource[] = R\"ENGDEF(\n${CONTENT})ENGDEF\";\n}\n")
file(WRITE "${OUTPUT}" "${BODY}")
