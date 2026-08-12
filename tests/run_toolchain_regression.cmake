if(NOT DEFINED CASE OR NOT DEFINED ROOT_DIR OR NOT DEFINED TEST_DIR OR
   NOT DEFINED DISCC OR NOT DEFINED DISCAS OR NOT DEFINED DISCLD)
    message(FATAL_ERROR "Regression test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_command)
    execute_process(
        COMMAND ${ARGV}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(result)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
endfunction()

function(run_expected_failure)
    execute_process(
        COMMAND ${ARGV}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result)
        message(FATAL_ERROR "Command unexpectedly succeeded: ${ARGV}")
    endif()
endfunction()

if(CASE STREQUAL "ir_and_cfg")
    execute_process(
        COMMAND "${DISCC}" "${ROOT_DIR}/examples/ir_control_flow.dc" --emit-ir
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(result OR NOT output MATCHES "condbr" OR NOT output MATCHES "while\.cond")
        message(FATAL_ERROR "IR/CFG regression failed\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    run_command("${DISCC}" "${ROOT_DIR}/examples/ir_control_flow.dc" -o "${TEST_DIR}/control.o")
    run_command("${DISCLD}" "${TEST_DIR}/control.o" -o "${TEST_DIR}/control.bin")

elseif(CASE STREQUAL "shadowing")
    set(source "${ROOT_DIR}/tests/fixtures/shadowing.dc")
    execute_process(
        COMMAND "${DISCC}" "${source}" --emit-ir
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(result OR NOT output MATCHES "@value#1" OR NOT output MATCHES "@value#2")
        message(FATAL_ERROR "Shadowing symbols were not preserved in IR\nstdout:\n${output}\nstderr:\n${error}")
    endif()
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/shadow.o")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/shadow.s")
    run_command("${DISCAS}" "${TEST_DIR}/shadow.s" -o "${TEST_DIR}/shadow-asm.o")
    run_command("${DISCLD}" "${TEST_DIR}/shadow.o" -o "${TEST_DIR}/shadow.bin")
    run_command("${DISCLD}" "${TEST_DIR}/shadow-asm.o" -o "${TEST_DIR}/shadow-asm.bin")
    file(SHA256 "${TEST_DIR}/shadow.bin" ir_hash)
    file(SHA256 "${TEST_DIR}/shadow-asm.bin" asm_hash)
    if(NOT ir_hash STREQUAL asm_hash)
        message(FATAL_ERROR "Shadowing backend and assembly payloads differ")
    endif()

elseif(CASE STREQUAL "diagnostics")
    run_expected_failure("${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_missing_semicolon.dc" -o "${TEST_DIR}/invalid.o")

elseif(CASE STREQUAL "backend_equivalence")
    foreach(name IN ITEMS math loop loop_opt plot test_casts ir_control_flow)
        set(source "${ROOT_DIR}/examples/${name}.dc")
        run_command("${DISCC}" "${source}" -o "${TEST_DIR}/${name}.o")
        run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/${name}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${name}.s" -o "${TEST_DIR}/${name}-asm.o")
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
        run_command("${DISCLD}" "${TEST_DIR}/${name}-asm.o" -o "${TEST_DIR}/${name}-asm.bin")
        file(SHA256 "${TEST_DIR}/${name}.bin" ir_hash)
        file(SHA256 "${TEST_DIR}/${name}-asm.bin" asm_hash)
        if(NOT ir_hash STREQUAL asm_hash)
            message(FATAL_ERROR "Payload mismatch for ${name}")
        endif()
    endforeach()

elseif(CASE STREQUAL "data_relocation")
    run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/data_a.s" -o "${TEST_DIR}/data-a.o")
    run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/data_b.s" -o "${TEST_DIR}/data-b.o")
    run_command("${DISCLD}" "${TEST_DIR}/data-a.o" "${TEST_DIR}/data-b.o" -o "${TEST_DIR}/data.bin")
    file(READ "${TEST_DIR}/data.bin" actual_hex HEX)
    string(TOLOWER "${actual_hex}" actual_hex)
    if(NOT actual_hex STREQUAL "00f00080")
        message(FATAL_ERROR "Unexpected data relocation payload: ${actual_hex}")
    endif()

elseif(CASE STREQUAL "feature_examples")
    foreach(name IN ITEMS ir_features ir_far)
        run_command("${DISCC}" "${ROOT_DIR}/examples/${name}.dc" -o "${TEST_DIR}/${name}.o")
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
    endforeach()

else()
    message(FATAL_ERROR "Unknown regression case: ${CASE}")
endif()
