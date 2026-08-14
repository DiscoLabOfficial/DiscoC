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

function(run_expected_failure_contains pattern)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result)
        message(FATAL_ERROR "Command unexpectedly succeeded: ${ARGN}")
    endif()
    string(CONCAT combined_output "${output}" "${error}")
    if(NOT combined_output MATCHES "${pattern}")
        message(FATAL_ERROR "Expected diagnostic '${pattern}' was not found for ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
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

elseif(CASE STREQUAL "language_diagnostics")
    run_expected_failure_contains("Code start address" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_address.dc" -o "${TEST_DIR}/invalid-address.o")
    run_expected_failure_contains("Array size" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_array_size.dc" -o "${TEST_DIR}/invalid-array.o")
    run_expected_failure_contains("Invalid integer literal" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_integer_literal.dc" -o "${TEST_DIR}/invalid-literal.o")
    run_expected_failure_contains("break.*loop or switch" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_break.dc" -o "${TEST_DIR}/invalid-break.o")
    run_expected_failure_contains("Void is not a valid variable declaration type" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_void_variable.dc" -o "${TEST_DIR}/invalid-void.o")
    run_expected_failure_contains("out of range for word" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_rom_value.dc" -o "${TEST_DIR}/invalid-rom.o")
    run_expected_failure_contains("Unknown configuration key" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_target_directive.dc" -o "${TEST_DIR}/invalid-target.o")
    run_expected_failure_contains("Use of undeclared symbol" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_optimized_loop_symbol.dc" -o "${TEST_DIR}/invalid-optimized-loop.o")
    run_expected_failure_contains("Cannot implicitly convert" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_call_type.dc" -o "${TEST_DIR}/invalid-call-type.o")
    run_expected_failure_contains("Left-hand side of assignment" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_lvalue.dc" -o "${TEST_DIR}/invalid-lvalue.o")
    run_expected_failure_contains("Division is not supported" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_division.dc" -o "${TEST_DIR}/invalid-division.o")
    run_expected_failure_contains("Plotting context" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_plot_branch.dc" -o "${TEST_DIR}/invalid-plot-branch.o")
    run_expected_failure_contains(":2:5:" "${DISCC}" "${ROOT_DIR}/tests/fixtures/invalid_column.dc" -o "${TEST_DIR}/invalid-column.o")
    run_command("${DISCC}" "${ROOT_DIR}/tests/fixtures/valid_break.dc" -o "${TEST_DIR}/valid-break.o")
    run_command("${DISCC}" "${ROOT_DIR}/tests/fixtures/valid_repeated_prototype.dc" -o "${TEST_DIR}/valid-repeated-prototype.o")
    run_expected_failure_contains("does not yet support subscript" "${DISCC}" --emit-asm "${ROOT_DIR}/tests/fixtures/asm_unsupported_subscript.dc" -o "${TEST_DIR}/unsupported-subscript.s")
    run_command("${DISCC}" --emit-asm "${ROOT_DIR}/tests/fixtures/sibling_scopes.dc" -o "${TEST_DIR}/sibling-scopes.s")
    file(READ "${TEST_DIR}/sibling-scopes.s" sibling_asm)
    if(NOT sibling_asm MATCHES "sub sp, sp, #4")
        message(FATAL_ERROR "Sibling lexical scopes did not receive distinct stack slots")
    endif()
    run_command("${DISCC}" "${ROOT_DIR}/tests/fixtures/relational_and_truthiness.dc" -o "${TEST_DIR}/relational.o")
    run_command("${DISCC}" --emit-asm "${ROOT_DIR}/tests/fixtures/relational_and_truthiness.dc" -o "${TEST_DIR}/relational.s")

elseif(CASE STREQUAL "multifile")
    set(example_dir "${ROOT_DIR}/examples/multifile")
    run_command("${DISCC}" "${example_dir}/main.dc" -o "${TEST_DIR}/main.o")
    run_command("${DISCC}" "${example_dir}/math.dc" -o "${TEST_DIR}/math.o")
    run_command("${DISCLD}" "${TEST_DIR}/main.o" "${TEST_DIR}/math.o" -o "${TEST_DIR}/multifile.bin")
    file(SIZE "${TEST_DIR}/multifile.bin" linked_size)
    if(linked_size LESS 1)
        message(FATAL_ERROR "Multi-file link produced an empty payload")
    endif()

elseif(CASE STREQUAL "switch_abi")
    set(source "${ROOT_DIR}/examples/switch_abi.dc")
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/switch-abi.o")
    run_command("${DISCLD}" "${TEST_DIR}/switch-abi.o" -o "${TEST_DIR}/switch-abi.bin")
    file(SIZE "${TEST_DIR}/switch-abi.bin" linked_size)
    if(linked_size LESS 1)
        message(FATAL_ERROR "Switch/ABI regression produced an empty payload")
    endif()

    execute_process(
        COMMAND "${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/switch-abi.s"
        RESULT_VARIABLE asm_result
        OUTPUT_VARIABLE asm_output
        ERROR_VARIABLE asm_error
    )
    if(asm_result)
        message(FATAL_ERROR "Assembly ABI regression failed\nstdout:\n${asm_output}\nstderr:\n${asm_error}")
    endif()
    file(READ "${TEST_DIR}/switch-abi.s" asm_output)
    if(asm_output MATCHES "Save switch condition value")
        message(FATAL_ERROR "Assembly switch path still saves the selector for every case")
    endif()
    if(NOT asm_output MATCHES "Constant switch selector")
        message(FATAL_ERROR "Assembly switch constant-folding path was not used")
    endif()
    if(NOT asm_output MATCHES "push r11" OR NOT asm_output MATCHES "push r9" OR
       NOT asm_output MATCHES "jal accumulate")
        message(FATAL_ERROR "ABI prologue/call sequence is missing from assembly output")
    endif()

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

elseif(CASE STREQUAL "branch_relaxation")
    run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/branch_relaxation.s" -o "${TEST_DIR}/branch.o")
    run_command("${DISCLD}" "${TEST_DIR}/branch.o" -o "${TEST_DIR}/branch.bin")
    file(READ "${TEST_DIR}/branch.bin" branch_hex HEX)
    string(SUBSTRING "${branch_hex}" 0 8 long_jump_prefix)
    string(TOLOWER "${long_jump_prefix}" long_jump_prefix)
    if(NOT long_jump_prefix STREQUAL "ff9a809f")
        message(FATAL_ERROR "Out-of-range branch was not relaxed to an absolute jump: ${branch_hex}")
    endif()

elseif(CASE STREQUAL "feature_examples")
    foreach(name IN ITEMS ir_features ir_far)
        run_command("${DISCC}" "${ROOT_DIR}/examples/${name}.dc" -o "${TEST_DIR}/${name}.o")
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
    endforeach()

elseif(CASE STREQUAL "spc700_target")
    set(source "${ROOT_DIR}/tests/fixtures/spc700_foundation.dc")
    execute_process(
        COMMAND "${DISCC}" --target spc700 "${source}" --emit-ir
        RESULT_VARIABLE ir_result
        OUTPUT_VARIABLE ir_output
        ERROR_VARIABLE ir_error
    )
    if(ir_result OR NOT ir_output MATCHES "function main")
        message(FATAL_ERROR "SPC700 target should remain inspectable through IR\nstdout:\n${ir_output}\nstderr:\n${ir_error}")
    endif()
    run_expected_failure_contains("no code-generation backend" "${DISCC}" --target spc700 "${source}" -o "${TEST_DIR}/spc700.o")

else()
    message(FATAL_ERROR "Unknown regression case: ${CASE}")
endif()
