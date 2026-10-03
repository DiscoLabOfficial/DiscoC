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

function(check_round_trip name source)
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/${name}.o")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/${name}.s")
    run_command("${DISCAS}" "${TEST_DIR}/${name}.s" -o "${TEST_DIR}/${name}-asm.o")
    run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
    run_command("${DISCLD}" "${TEST_DIR}/${name}-asm.o" -o "${TEST_DIR}/${name}-asm.bin")
    file(SHA256 "${TEST_DIR}/${name}.bin" direct_hash)
    file(SHA256 "${TEST_DIR}/${name}-asm.bin" assembly_hash)
    if(NOT direct_hash STREQUAL assembly_hash)
        message(FATAL_ERROR "Canonical assembly round trip failed for ${name}")
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
    if(combined_output MATCHES "Internal Compiler Error")
        message(FATAL_ERROR "User-facing diagnostic was incorrectly reported as an internal compiler error for ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
    endif()
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
    run_expected_failure_contains("Local stack frame exceeds" "${DISCC}"
        "${ROOT_DIR}/tests/fixtures/invalid_gsu_frame.dc" -o "${TEST_DIR}/invalid-frame.o")
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
    # This was an AST-backend limitation. Canonical export must now support
    # every construct accepted by object compilation, with identical bytes.
    check_round_trip(subscript "${ROOT_DIR}/tests/fixtures/asm_unsupported_subscript.dc")
    run_command("${DISCC}" --emit-asm "${ROOT_DIR}/tests/fixtures/sibling_scopes.dc" -o "${TEST_DIR}/sibling-scopes.s")
    file(READ "${TEST_DIR}/sibling-scopes.s" sibling_asm)
    if(NOT sibling_asm MATCHES "sub #4")
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
    run_command("${GSU_RUNNER}" "${TEST_DIR}/multifile.bin" 0x8000
        --word 0x1ffa 42 --register 10 0x1ffc)

elseif(CASE STREQUAL "gsu_call_execution")
    foreach(backend IN ITEMS ir asm)
        foreach(unit IN ITEMS main math)
            set(source "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc")
            set(object "${TEST_DIR}/${unit}-${backend}.o")
            if(backend STREQUAL "asm")
                run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/${unit}.s")
                run_command("${DISCAS}" "${TEST_DIR}/${unit}.s" -o "${object}")
            else()
                run_command("${DISCC}" "${source}" -o "${object}")
            endif()
        endforeach()
        set(payload "${TEST_DIR}/calls-${backend}.bin")
        run_command("${DISCLD}" "${TEST_DIR}/main-${backend}.o" "${TEST_DIR}/math-${backend}.o" -o "${payload}")
        run_command("${GSU_RUNNER}" "${payload}" 0x8000
            --word 0x100 149 --word 0x102 57 --word 0x104 195
            --word 0x106 149 --word 0x108 149 --word 0x10a 149
            --word 0x10c 149 --word 0x10e 0
            --word 0x110 156 --word 0x112 65535 --word 0x114 47
            --register 10 0x1ffc --register 9 0x1ffc)
        run_expected_failure("${GSU_RUNNER}" "${payload}" 0x8000 --word 0x100 150)
        run_expected_failure("${GSU_RUNNER}" "${payload}" 0x7000 --word 0x100 149)
    endforeach()
    # The direct object path must relocate calls to the configured origin,
    # not bake in the default $8000 used by the example.
    foreach(unit IN ITEMS main math)
        file(READ "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc" source_text)
        file(WRITE "${TEST_DIR}/${unit}-relocated.dc" "set code_start_address = 0x9000;\n${source_text}")
        run_command("${DISCC}" "${TEST_DIR}/${unit}-relocated.dc" -o "${TEST_DIR}/${unit}-relocated.o")
    endforeach()
    run_command("${DISCLD}" "${TEST_DIR}/main-relocated.o" "${TEST_DIR}/math-relocated.o" -o "${TEST_DIR}/relocated.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/relocated.bin" 0x9000
        --word 0x100 149 --word 0x104 195 --word 0x10a 149 --register 10 0x1ffc)

elseif(CASE STREQUAL "gsu_register_moves")
    run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/gsu_register_moves.s" -o "${TEST_DIR}/moves.o")
    run_command("${DISCLD}" "${TEST_DIR}/moves.o" -o "${TEST_DIR}/moves.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/moves.bin" 0x8000
        --word 0x100 149 --register 1 149 --register 2 149 --register 10 0x2000)

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
    if(NOT asm_output MATCHES "with r11" OR NOT asm_output MATCHES "with r9" OR
       NOT asm_output MATCHES "iwt r15, #accumulate")
        message(FATAL_ERROR "ABI prologue/call sequence is missing from assembly output")
    endif()
    check_round_trip(switch-abi "${source}")
    foreach(backend IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/switch-abi${backend}.o" --init-runtime
            -o "${TEST_DIR}/switch-entry${backend}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/switch-entry${backend}.bin" 0x8000
            --initial-sp 0x7777 --initial-ram-bank 1
            --word 0x701ffa 40 --word 0x701ff8 46 --register 10 0x1ffc)
    endforeach()

elseif(CASE STREQUAL "backend_equivalence")
    foreach(name IN ITEMS math loop loop_opt plot test_casts ir_control_flow ir_features ir_far switch_abi)
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
    if(NOT long_jump_prefix STREQUAL "ff9a8001")
        message(FATAL_ERROR "Out-of-range branch was not relaxed to an absolute jump: ${branch_hex}")
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/branch.bin" 0x8000 --register 10 0x2000)

elseif(CASE STREQUAL "pointer_width")
    set(source "${ROOT_DIR}/tests/fixtures/pointer_width.dc")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/pointer-width.s")
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/pointer-width.o")
    run_command("${DISCLD}" "${TEST_DIR}/pointer-width.o" -o "${TEST_DIR}/pointer-width.bin")
    file(READ "${TEST_DIR}/pointer-width.s" assembly)
    string(REPLACE "stw" ";" stw_parts "${assembly}")
    list(LENGTH stw_parts stw_parts_count)
    math(EXPR pointer_store_count "${stw_parts_count} - 1")
    if(pointer_store_count LESS 6)
        message(FATAL_ERROR "Pointer values were not consistently stored as words:\n${assembly}")
    endif()
    if(NOT assembly MATCHES "ldb \\(r0\\)")
        message(FATAL_ERROR "Dereferencing byte* did not use an 8-bit load:\n${assembly}")
    endif()
    check_round_trip(pointer-width "${source}")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/pointer-width-asm.bin" 0x8000 --word 0x1ff0 42)

elseif(CASE STREQUAL "comparison_semantics")
    set(source "${ROOT_DIR}/tests/fixtures/comparison_semantics.dc")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/comparisons.s")
    file(READ "${TEST_DIR}/comparisons.s" assembly)
    if(NOT assembly MATCHES "blt" OR NOT assembly MATCHES "bge" OR
       NOT assembly MATCHES "bcc" OR NOT assembly MATCHES "bcs")
        message(FATAL_ERROR "Signed/unsigned comparison branches are incomplete:\n${assembly}")
    endif()
    if(NOT assembly MATCHES "beq" OR NOT assembly MATCHES "bne")
        message(FATAL_ERROR "Equality boundaries are not materialized distinctly:\n${assembly}")
    endif()

elseif(CASE STREQUAL "optimizer_preservation")
    set(source "${ROOT_DIR}/tests/fixtures/optimizer_preservation.dc")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/optimizer.s")
    file(READ "${TEST_DIR}/optimizer.s" assembly)
    string(REGEX MATCHALL "iwt r15, #side_effect" calls "${assembly}")
    list(LENGTH calls call_count)
    if(call_count LESS 2)
        message(FATAL_ERROR "Optimizer dropped a statement when hardware-loop matching failed:\n${assembly}")
    endif()
    check_round_trip(optimizer "${source}")

elseif(CASE STREQUAL "ir_branch_relaxation")
    set(source "${ROOT_DIR}/tests/fixtures/long_ir_branch.dc")
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/long-branch.o")
    run_command("${DISCLD}" "${TEST_DIR}/long-branch.o" -o "${TEST_DIR}/long-branch.bin")
    file(SIZE "${TEST_DIR}/long-branch.bin" linked_size)
    if(linked_size LESS 1)
        message(FATAL_ERROR "IR branch relaxation produced an empty payload")
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/long-branch.bin" 0x8000
        --word 0x1ffa 0 --register 10 0x1ffc)
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/long-branch.s")
    run_command("${DISCAS}" "${TEST_DIR}/long-branch.s" -o "${TEST_DIR}/long-branch-asm.o")
    run_command("${DISCLD}" "${TEST_DIR}/long-branch-asm.o" -o "${TEST_DIR}/long-branch-asm.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/long-branch-asm.bin" 0x8000
        --word 0x1ffa 0 --register 10 0x1ffc)

elseif(CASE STREQUAL "ir_switch_relaxation")
    set(source "${ROOT_DIR}/tests/fixtures/long_ir_switch.dc")
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/long-switch.o")
    run_command("${DISCLD}" "${TEST_DIR}/long-switch.o" -o "${TEST_DIR}/long-switch.bin")
    file(SIZE "${TEST_DIR}/long-switch.bin" linked_size)
    if(linked_size LESS 1)
        message(FATAL_ERROR "IR switch relaxation produced an empty payload")
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/long-switch.bin" 0x8000
        --word 0x1ffa 12 --register 10 0x1ffc)

elseif(CASE STREQUAL "feature_examples")
    foreach(name IN ITEMS ir_features ir_far)
        run_command("${DISCC}" "${ROOT_DIR}/examples/${name}.dc" -o "${TEST_DIR}/${name}.o")
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
    endforeach()

elseif(CASE STREQUAL "gsu_mapping")
    if(NOT DEFINED GSU_MAPPING_TESTER)
        message(FATAL_ERROR "GSU_MAPPING_TESTER is required for mapping regressions")
    endif()
    run_command("${GSU_MAPPING_TESTER}" --write-fixtures "${TEST_DIR}")
    function(expect_mapping_bytes name expected_hex)
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
        file(READ "${TEST_DIR}/${name}.bin" actual_hex HEX)
        if(NOT actual_hex STREQUAL expected_hex)
            message(FATAL_ERROR "Incorrect mapped relocation bytes for ${name}: ${actual_hex}")
        endif()
    endfunction()
    expect_mapping_bytes(near-rom f0038034)
    expect_mapping_bytes(near-rom-last f0ffff34)
    expect_mapping_bytes(near-rom-3f f0038034)
    expect_mapping_bytes(near-mirror-40 f0030034)
    expect_mapping_bytes(near-mirror-5f f0030034)
    expect_mapping_bytes(near-ram-70 f0038034)
    expect_mapping_bytes(near-ram-71 f0030034)
    expect_mapping_bytes(near-hirom f0038034)
    expect_mapping_bytes(call-ram ff0480010001)
    expect_mapping_bytes(local-ram ff0400010001)
    expect_mapping_bytes(far-rom a15ff2050034)
    expect_mapping_bytes(far-ram a171f2050034)
    run_command("${DISCLD}" "${TEST_DIR}/data-a.o" "${TEST_DIR}/data-b.o"
        -o "${TEST_DIR}/combined-data.bin")
    file(READ "${TEST_DIR}/combined-data.bin" combined_hex HEX)
    if(NOT combined_hex STREQUAL "f00380f00080")
        message(FATAL_ERROR "High-bank multi-object DATA relocation patched the wrong location")
    endif()
    foreach(name IN ITEMS exact-code exact-data full-mirror full-ram)
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
        file(SIZE "${TEST_DIR}/${name}.bin" actual_size)
        if(name MATCHES "^full-")
            set(expected_size 65536)
        else()
            set(expected_size 16)
        endif()
        if(NOT actual_size EQUAL expected_size)
            message(FATAL_ERROR "Exact bank boundary payload has incorrect size")
        endif()
    endforeach()
    function(reject_mapping name diagnostic)
        # Validate before opening/truncating output, including an existing file.
        set(output "${TEST_DIR}/${name}-rejected.bin")
        file(WRITE "${output}" "preserve-existing-output")
        run_expected_failure_contains("${diagnostic}" "${DISCLD}"
            "${TEST_DIR}/${name}.o" ${ARGN} -o "${output}")
        file(READ "${output}" unchanged)
        if(NOT unchanged STREQUAL "preserve-existing-output")
            message(FATAL_ERROR "Failed mapping link overwrote an existing output file")
        endif()
    endfunction()
    foreach(name IN ITEMS invalid-wram invalid-low-rom invalid-gap invalid-snes-mirror invalid-24bit)
        reject_mapping(${name} "outside supported ROM/RAM")
    endforeach()
    foreach(name IN ITEMS cross-code cross-data cross-mirror cross-ram)
        reject_mapping(${name} "program-bank boundary")
    endforeach()
    reject_mapping(cross-a "program-bank boundary" "${TEST_DIR}/cross-b.o")
    reject_mapping(near-end-call "16-bit GSU relocation.*different bank")
    reject_mapping(near-end-iwt "16-bit GSU relocation.*different bank")
    reject_mapping(far-end-bank "outside supported ROM/RAM")
    reject_mapping(far-end-offset "outside supported ROM/RAM")
    reject_mapping(far-overflow "exceeds the 24-bit address range")
    reject_mapping(near-overflow "exceeds the 16-bit address range")
    # Exercise actual compiler output with a full 24-bit origin, not just
    # hand-authored objects. Near calls still encode offsets within PBR.
    foreach(origin IN ITEMS 0x408000 0x708000 0x710000)
        foreach(unit IN ITEMS main math)
            file(READ "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc" source_text)
            file(WRITE "${TEST_DIR}/${unit}-${origin}.dc"
                "set code_start_address = ${origin};\n${source_text}")
            run_command("${DISCC}" "${TEST_DIR}/${unit}-${origin}.dc" -o "${TEST_DIR}/${unit}-${origin}.o")
        endforeach()
        run_command("${DISCLD}" "${TEST_DIR}/main-${origin}.o" "${TEST_DIR}/math-${origin}.o"
            -o "${TEST_DIR}/calls-${origin}.bin")
        if(origin STREQUAL "0x710000")
            set(pc 0x0000)
        else()
            set(pc 0x8000)
        endif()
        # The instruction model uses the low 16-bit PC, not a physical SNES
        # bank/bus model. This checks relocated calls/results, not ROM loading.
        run_command("${GSU_RUNNER}" "${TEST_DIR}/calls-${origin}.bin" ${pc}
            --word 0x100 149 --word 0x104 195 --word 0x10a 149 --register 10 0x1ffc)
    endforeach()
    foreach(origin IN ITEMS 0x7E8000 0xFFF0)
        foreach(unit IN ITEMS main math)
            file(READ "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc" source_text)
            file(WRITE "${TEST_DIR}/${unit}-${origin}.dc"
                "set code_start_address = ${origin};\n${source_text}")
            run_command("${DISCC}" "${TEST_DIR}/${unit}-${origin}.dc" -o "${TEST_DIR}/${unit}-${origin}.o")
        endforeach()
        if(origin STREQUAL "0x7E8000")
            set(diagnostic "outside supported ROM/RAM")
        else()
            set(diagnostic "program-bank boundary")
        endif()
        run_expected_failure_contains("${diagnostic}" "${DISCLD}"
            "${TEST_DIR}/main-${origin}.o" "${TEST_DIR}/math-${origin}.o"
            -o "${TEST_DIR}/invalid-${origin}.bin")
        if(EXISTS "${TEST_DIR}/invalid-${origin}.bin")
            message(FATAL_ERROR "Invalid placement created a payload file")
        endif()
    endforeach()

elseif(CASE STREQUAL "gsu_execution_memory")
    foreach(placement IN ITEMS lorom hirom ram ram71)
        set(pc 0x8000)
        if(placement STREQUAL "lorom")
            set(directives "")
            set(header "444953434f03000000800000")
        elseif(placement STREQUAL "hirom")
            set(directives "set memory_mapping = hirom;\n")
            set(header "444953434f03000100804000")
        elseif(placement STREQUAL "ram")
            set(directives "set execution_memory = ram;\n")
            set(header "444953434f03000000807000")
        else()
            # Explicit origins survive subsequent mapper/memory selection.
            set(directives "set code_start_address = 0x710000;\nset execution_memory = ram;\nset memory_mapping = lorom;\n")
            set(header "444953434f03000000007100")
            set(pc 0x0000)
        endif()
        foreach(backend IN ITEMS ir asm)
            foreach(unit IN ITEMS main math)
                file(READ "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc" source_text)
                set(source "${TEST_DIR}/${unit}-${placement}.dc")
                set(object "${TEST_DIR}/${unit}-${placement}-${backend}.o")
                file(WRITE "${source}" "${directives}${source_text}")
                if(backend STREQUAL "asm")
                    run_command("${DISCC}" --emit-asm "${source}" -o "${source}.s")
                    run_command("${DISCAS}" "${source}.s" -o "${object}")
                else()
                    run_command("${DISCC}" "${source}" -o "${object}")
                endif()
                file(READ "${object}" actual_header LIMIT 12 HEX)
                if(NOT actual_header STREQUAL header)
                    message(FATAL_ERROR "Placement metadata lost: ${object}: ${actual_header}, expected ${header}")
                endif()
            endforeach()
            set(payload "${TEST_DIR}/${placement}-${backend}.bin")
            run_command("${DISCLD}" "${TEST_DIR}/main-${placement}-${backend}.o"
                "${TEST_DIR}/math-${placement}-${backend}.o" -o "${payload}")
            # Instruction/relocation checks only: this model does not emulate
            # physical cartridge RAM, copying the payload, or bus ownership.
            run_command("${GSU_RUNNER}" "${payload}" ${pc}
                --word 0x100 149 --word 0x104 195 --word 0x10a 149 --register 10 0x1ffc)
            if(placement STREQUAL "ram")
                file(SHA256 "${payload}" ram_hash)
                file(SHA256 "${TEST_DIR}/lorom-${backend}.bin" rom_hash)
                if(NOT ram_hash STREQUAL rom_hash)
                    message(FATAL_ERROR "Changing only the bank altered near-address payload bytes (${backend})")
                endif()
            endif()
        endforeach()
        # IR and assembly objects must also agree when linked together.
        run_command("${DISCLD}" "${TEST_DIR}/main-${placement}-ir.o"
            "${TEST_DIR}/math-${placement}-asm.o" -o "${TEST_DIR}/mixed-${placement}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/mixed-${placement}.bin" ${pc}
            --word 0x100 149 --word 0x104 195 --register 10 0x1ffc)
    endforeach()
    run_expected_failure_contains("incompatible" "${DISCLD}"
        "${TEST_DIR}/main-ram-ir.o" "${TEST_DIR}/math-lorom-ir.o" -o "${TEST_DIR}/incompatible.bin")
    function(check_assembly_placement name definitions expected_header)
        file(WRITE "${TEST_DIR}/${name}.s" "${definitions}\n.segment \"CODE\"\n.export main\nmain:\nstop\nnop\n")
        run_command("${DISCAS}" "${TEST_DIR}/${name}.s" -o "${TEST_DIR}/${name}.o")
        file(READ "${TEST_DIR}/${name}.o" actual_header LIMIT 12 HEX)
        if(NOT actual_header STREQUAL expected_header)
            message(FATAL_ERROR "Incorrect assembler placement: ${name}: ${actual_header}")
        endif()
        run_command("${DISCLD}" "${TEST_DIR}/${name}.o" -o "${TEST_DIR}/${name}.bin")
    endfunction()
    check_assembly_placement(default "" "444953434f03000000800000")
    check_assembly_placement(hirom-default ".define __DISCO_MEMORY_MAPPING hirom" "444953434f03000100804000")
    check_assembly_placement(origin-first
        ".define __DISCO_CODE_START_ADDRESS $710000\n.define __DISCO_MEMORY_MAPPING lorom"
        "444953434f03000000007100")
    foreach(definition IN ITEMS
            "__DISCO_MEMORY_MAPPING invalid"
            "__DISCO_CODE_START_ADDRESS 0x1000000"
            "__DISCO_CODE_START_ADDRESS -1"
            "__DISCO_CODE_START_ADDRESS --9223372036854775808"
            "__DISCO_CODE_START_ADDRESS huge"
            "__DISCO_CODE_START_ADDRESS"
            "__DISCO_CODE_START_ADDRESS 0x708000 extra"
            "__UNKNOWN 1")
        file(WRITE "${TEST_DIR}/invalid.s" ".define ${definition}\n.segment \"CODE\"\nstop\nnop\n")
        run_expected_failure_contains("Assembler:" "${DISCAS}" "${TEST_DIR}/invalid.s" -o "${TEST_DIR}/invalid.o")
    endforeach()
    file(WRITE "${TEST_DIR}/duplicate.s"
        ".define __DISCO_CODE_START_ADDRESS 0x708000\n.define __DISCO_CODE_START_ADDRESS 0x710000\n")
    run_expected_failure_contains("duplicate configuration" "${DISCAS}" "${TEST_DIR}/duplicate.s" -o "${TEST_DIR}/duplicate.o")

elseif(CASE STREQUAL "gsu_runtime_loading")
    foreach(unit IN ITEMS main math)
        set(source "${ROOT_DIR}/tests/fixtures/gsu_call_${unit}.dc")
        run_command("${DISCC}" "${source}" -o "${TEST_DIR}/${unit}.o")
        run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/${unit}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${unit}.s" -o "${TEST_DIR}/${unit}-asm.o")
    endforeach()
    foreach(origin IN ITEMS 0x706000 0x700000 0x716000)
        foreach(bank IN ITEMS 0 1)
            math(EXPR poisoned_bank "1 - ${bank}")
            math(EXPR result_bank "0x700000 + (${bank} * 65536)" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR other_bank "0x700000 + (${poisoned_bank} * 65536)" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR result_address "${result_bank} + 0x100" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR nested_address "${result_bank} + 0x104" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR untouched_address "${other_bank} + 0x100" OUTPUT_FORMAT HEXADECIMAL)
            # At origin zero, the payload itself can occupy offset $100 in
            # the other bank, so do not mistake its instruction bytes for data.
            set(other_expectation)
            if(NOT origin STREQUAL "0x700000")
                set(other_expectation --word ${untouched_address} 0)
            endif()
            set(payload "${TEST_DIR}/runtime-${origin}-${bank}.bin")
            set(listing "${payload}.s")
            run_command("${DISCLD}" "${TEST_DIR}/main.o" "${TEST_DIR}/math.o"
                --origin ${origin} --init-runtime --ram-bank ${bank}
                --stack-pointer 0x2000 --emit-asm "${listing}" -o "${payload}")
            # This deliberately large fixture occupies $0100 at origin zero.
            # Its result writes then corrupt code in the same data bank, which
            # the shared-storage model must detect rather than silently passing.
            if(origin STREQUAL "0x700000" AND bank EQUAL 0)
                run_expected_failure_contains("register 10" "${GSU_RUNNER}" "${payload}" ${origin}
                    --initial-sp 0x7777 --initial-ram-bank ${poisoned_bank}
                    --word ${result_address} 149 --word ${nested_address} 195 --register 10 0x1ffc)
            else()
                run_command("${GSU_RUNNER}" "${payload}" ${origin}
                    --initial-sp 0x7777 --initial-ram-bank ${poisoned_bank}
                    --word ${result_address} 149 --word ${nested_address} 195
                    ${other_expectation} --register 10 0x1ffc)
            endif()
            file(READ "${payload}" startup LIMIT 12 HEX)
            if(origin STREQUAL "0x700000")
                set(entry "0c00")
            else()
                set(entry "0c60")
            endif()
            set(expected_startup "f00${bank}003edffa0020ff${entry}01")
            if(NOT startup STREQUAL expected_startup)
                message(FATAL_ERROR "Incorrect runtime startup: ${startup}, expected ${expected_startup}")
            endif()
            run_command("${DISCLD}" "${TEST_DIR}/main-asm.o" "${TEST_DIR}/math-asm.o"
                --origin ${origin} --init-runtime --ram-bank ${bank}
                -o "${payload}-asm.bin")
            # Final exported assembly already contains the bootstrap and all
            # resolved addresses: link it without injecting a second bootstrap.
            run_command("${DISCAS}" "${listing}" -o "${payload}-final.o")
            run_command("${DISCLD}" "${payload}-final.o" -o "${payload}-final.bin")
            file(SHA256 "${payload}" expected_hash)
            foreach(roundtrip IN ITEMS "${payload}-asm.bin" "${payload}-final.bin")
                file(SHA256 "${roundtrip}" actual_hash)
                if(NOT actual_hash STREQUAL expected_hash)
                    message(FATAL_ERROR "Runtime/final assembly payload mismatch: ${roundtrip}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    # The existing no-bootstrap ABI leaves initialization to the host, and a
    # fixed-origin payload copied to a different offset is not position independent.
    run_command("${DISCLD}" "${TEST_DIR}/main.o" "${TEST_DIR}/math.o"
        --origin 0x706000 -o "${TEST_DIR}/host.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/host.bin" 0x706000 --initial-ram-bank 1
        --word 0x710100 149 --word 0x700100 0)
    run_expected_failure_contains("outside the linked payload" "${GSU_RUNNER}"
        "${TEST_DIR}/host.bin" 0x705000 --word 0x710100 149)

    function(reject_runtime diagnostic)
        set(output "${TEST_DIR}/rejected.bin")
        file(WRITE "${output}" "preserve-existing-output")
        run_expected_failure_contains("${diagnostic}" "${DISCLD}"
            "${TEST_DIR}/main.o" "${TEST_DIR}/math.o" ${ARGN} -o "${output}")
        file(READ "${output}" unchanged)
        if(NOT unchanged STREQUAL "preserve-existing-output")
            message(FATAL_ERROR "Rejected runtime link changed existing output")
        endif()
    endfunction()
    reject_runtime("outside supported ROM/RAM" --origin 0x7e6000)
    reject_runtime("program-bank boundary" --origin 0x70fff8 --init-runtime)
    reject_runtime("Runtime entry" --init-runtime --entry missing)
    reject_runtime("Runtime options require" --ram-bank 1)
    reject_runtime("even and within" --init-runtime --stack-pointer 9)
    reject_runtime("even and within" --init-runtime --stack-pointer 0)
    reject_runtime("out of range" --init-runtime --ram-bank 2)
    reject_runtime("overlaps" --origin 0x702000 --init-runtime)
    reject_runtime("overlaps" --origin 0x702001 --init-runtime)
    reject_runtime("Invalid numeric" --origin -1)
    reject_runtime("out of range" --origin 0x1000000)
    reject_runtime("Unknown linker option" --unknown)
    foreach(bad_entry IN ITEMS data_entry end_entry)
        file(WRITE "${TEST_DIR}/${bad_entry}.s"
            ".segment \"CODE\"\nstop\nnop\n.export end_entry\nend_entry:\n.segment \"DATA\"\n.export data_entry\ndata_entry:\n.byte 42\n")
        run_command("${DISCAS}" "${TEST_DIR}/${bad_entry}.s" -o "${TEST_DIR}/${bad_entry}.o")
        run_expected_failure_contains("Runtime entry" "${DISCLD}"
            "${TEST_DIR}/${bad_entry}.o" --init-runtime --entry ${bad_entry}
            -o "${TEST_DIR}/${bad_entry}.bin")
        if(EXISTS "${TEST_DIR}/${bad_entry}.bin")
            message(FATAL_ERROR "Invalid runtime entry created output")
        endif()
    endforeach()
    # A user-facing two-file example has a stable public result buffer.
    foreach(unit IN ITEMS main math)
        run_command("${DISCC}" "${ROOT_DIR}/examples/ram_result/${unit}.dc"
            -o "${TEST_DIR}/example-${unit}.o")
    endforeach()
    foreach(origin IN ITEMS 0x706000 0x700000)
        foreach(bank IN ITEMS 0 1)
            math(EXPR poisoned_bank "1 - ${bank}")
            math(EXPR result_address "0x700100 + (${bank} * 65536)" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR untouched_address "0x700100 + (${poisoned_bank} * 65536)" OUTPUT_FORMAT HEXADECIMAL)
            set(example "${TEST_DIR}/example-${origin}-${bank}.bin")
            run_command("${DISCLD}" "${TEST_DIR}/example-main.o" "${TEST_DIR}/example-math.o"
                --origin ${origin} --init-runtime --ram-bank ${bank} -o "${example}")
            file(SIZE "${example}" example_size)
            if(origin STREQUAL "0x700000" AND example_size GREATER 256)
                message(FATAL_ERROR "Zero-origin result example grew into its $0100 buffer")
            endif()
            run_command("${GSU_RUNNER}" "${example}" ${origin}
                --initial-sp 0x7777 --initial-ram-bank ${poisoned_bank}
                --word ${result_address} 42 --word ${untouched_address} 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
    file(SHA256 "${TEST_DIR}/main.o" input_hash)
    run_expected_failure_contains("overwrite an input" "${DISCLD}"
        "${TEST_DIR}/main.o" -o "${TEST_DIR}/main.o")
    file(SHA256 "${TEST_DIR}/main.o" unchanged_hash)
    if(NOT input_hash STREQUAL unchanged_hash)
        message(FATAL_ERROR "Output alias overwrote an input object")
    endif()
    run_expected_failure_contains("must be different" "${DISCLD}"
        "${TEST_DIR}/main.o" --emit-asm "${TEST_DIR}/same.bin" -o "${TEST_DIR}/same.bin")

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
