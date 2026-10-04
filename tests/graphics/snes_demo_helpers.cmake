cmake_minimum_required(VERSION 3.15)

# Shared, dependency-explicit build helpers for the optional SNES graphics demos.
get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT DEFINED OUTPUT_DIR)
    set(OUTPUT_DIR "${ROOT_DIR}/${DEMO_OUTPUT_SUBDIR}")
endif()
get_filename_component(OUTPUT_DIR "${OUTPUT_DIR}" ABSOLUTE BASE_DIR "${ROOT_DIR}")
if(NOT DEFINED VERIFY_MESEN)
    set(VERIFY_MESEN OFF)
endif()

function(require_program variable name)
    if(NOT DEFINED ${variable})
        if(DISCO_TOOLS_DIR AND variable MATCHES "^DISC")
            find_program(${variable} NAMES "${name}" HINTS "${DISCO_TOOLS_DIR}" NO_DEFAULT_PATH)
        else()
            find_program(${variable} NAMES "${name}")
        endif()
    endif()
    if(NOT ${variable} OR NOT EXISTS "${${variable}}" OR IS_DIRECTORY "${${variable}}")
        message(FATAL_ERROR "Missing ${name}. Put it on PATH or pass -D${variable}=/absolute/path/to/${name}. Use -DDISCO_TOOLS_DIR=DIR for the DiscoC tools.")
    endif()
    get_filename_component(program "${${variable}}" ABSOLUTE)
    set(${variable} "${program}" PARENT_SCOPE)
endfunction()

require_program(DISCC discc)
require_program(DISCLD discld)
require_program(DISCAS discas)
require_program(WLA_65816 wla-65816)
require_program(WLALINK wlalink)
if(VERIFY_MESEN)
    require_program(MESEN Mesen)
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(run_command)
    execute_process(
        COMMAND ${ARGV}
        WORKING_DIRECTORY "${OUTPUT_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 120
    )
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

function(compare_payload path)
    file(SHA256 "${OUTPUT_DIR}/triangle.bin" direct)
    file(SHA256 "${path}" assembled)
    if(NOT direct STREQUAL assembled)
        message(FATAL_ERROR "Assembly round trip differs from triangle.bin: ${path}")
    endif()
endfunction()

function(build_gsu_payload source)
    set(link_options --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0xFFFE)
    run_command("${DISCC}" --target gsu --execution-memory ram "${source}" -o triangle.o)
    run_command("${DISCLD}" triangle.o ${link_options} --emit-asm triangle.s -o triangle.bin)
    run_command("${DISCC}" --target gsu --execution-memory ram --emit-asm "${source}" -o triangle-compiler.s)
    run_command("${DISCAS}" triangle-compiler.s -o triangle-assembled.o)
    run_command("${DISCLD}" triangle-assembled.o ${link_options} -o triangle-assembled.bin)
    compare_payload("${OUTPUT_DIR}/triangle-assembled.bin")
    run_command("${DISCAS}" triangle.s -o triangle-final.o)
    run_command("${DISCLD}" triangle-final.o -o triangle-roundtrip.bin)
    compare_payload("${OUTPUT_DIR}/triangle-roundtrip.bin")
endfunction()

function(build_snes_host host_dir name)
    run_command("${WLA_65816}" ${ARGN} -I "${host_dir}" -I "${OUTPUT_DIR}"
        -i -o "${name}.o" "${host_dir}/snes_host.asm")
    file(WRITE "${OUTPUT_DIR}/${name}.link" "[objects]\n${name}.o\n")
    run_command("${WLALINK}" -r -S "${name}.link" "${name}.sfc")
endfunction()
