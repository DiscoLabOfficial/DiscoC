set(link --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0x2000)
foreach(level IN ITEMS 0 1 2)
    run_command("${DISCC}" -O${level} "${ROOT_DIR}/tests/fixtures/gsu_global_optimization.dc" -o "${TEST_DIR}/ssa-${level}.o")
    run_command("${DISCC}" -O${level} --emit-asm "${ROOT_DIR}/tests/fixtures/gsu_global_optimization.dc" -o "${TEST_DIR}/ssa-${level}.s")
    run_command("${DISCAS}" "${TEST_DIR}/ssa-${level}.s" -o "${TEST_DIR}/ssa-${level}-asm.o")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/ssa-${level}${suffix}.o" ${link} --emit-asm "${TEST_DIR}/ssa-${level}${suffix}-final.s" -o "${TEST_DIR}/ssa-${level}${suffix}.bin")
        run_command("${DISCAS}" "${TEST_DIR}/ssa-${level}${suffix}-final.s" -o "${TEST_DIR}/ssa-${level}${suffix}-final.o")
        run_command("${DISCLD}" "${TEST_DIR}/ssa-${level}${suffix}-final.o" -o "${TEST_DIR}/ssa-${level}${suffix}-final.bin")
        file(SHA256 "${TEST_DIR}/ssa-${level}${suffix}.bin" expected)
        file(SHA256 "${TEST_DIR}/ssa-${level}${suffix}-final.bin" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "O${level} linked assembly did not round-trip byte for byte")
        endif()
        foreach(seed IN ITEMS 0 1 127 128 255 32767 32768 65535)
            math(EXPR low "${seed} & 255")
            math(EXPR high "${seed} >> 8")
            math(EXPR swapped "(${seed} + 1) & 65535")
            math(EXPR escaped "(${seed} + 4) & 65535")
            math(EXPR observed "(${seed} + 3) & 65535")
            math(EXPR narrow "(${seed} - 3) & 255")
            if(narrow GREATER_EQUAL 128)
                math(EXPR narrow "${narrow} + 65280")
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/ssa-${level}${suffix}.bin" 0x706000
                --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high} --reads 0x700100 1
                --word 0x700120 ${swapped} --word 0x700122 ${seed} --word 0x700124 42
                --word 0x700126 ${escaped} --word 0x700128 ${observed} --word 0x70012a ${narrow}
                --word 0x70012c 36 --word 0x70012e 42 --register 0 42 --register 6 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
    file(SHA256 "${TEST_DIR}/ssa-${level}.bin" direct)
    file(SHA256 "${TEST_DIR}/ssa-${level}-asm.bin" assembled)
    if(NOT direct STREQUAL assembled)
        message(FATAL_ERROR "O${level} compiler assembly did not match the direct backend")
    endif()
endforeach()

# Cached scaled-index recurrences must keep carry/borrow, bank-window and
# null checks at the original access. A zero-trip loop must not access memory.
function(induction_case level origin start trips mode fault expected)
    math(EXPR low "${start} & 255")
    math(EXPR high "${start} >> 8")
    set(expectation --register 6 ${fault})
    if(fault EQUAL 0)
        list(APPEND expectation --register 0 ${expected} --word 0x700120 ${expected} --register 10 0x1ffc)
    else()
        list(APPEND expectation --word 0x700120 0)
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/induction-${level}-${origin}.bin" ${origin}
        --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high} --ram-byte 0x700102 ${trips} --ram-byte 0x700104 ${mode}
        --ram-byte 0x700200 8 --ram-byte 0x700208 9 --ram-byte 0x7001f0 8 --ram-byte 0x7001f8 9
        --rom-byte 0x008ff0 8 --rom-byte 0x008ff8 9 --reads 0x700100 1 ${expectation})
endfunction()
foreach(level IN ITEMS 0 1 2)
    run_command("${DISCC}" -O${level} "${ROOT_DIR}/tests/fixtures/gsu_induction.dc" -o "${TEST_DIR}/induction-${level}.o")
    foreach(origin IN ITEMS 0x706000 0x008000)
        run_command("${DISCLD}" "${TEST_DIR}/induction-${level}.o" --origin ${origin}
            --init-runtime --ram-bank 0 --stack-pointer 0x2000 -o "${TEST_DIR}/induction-${level}-${origin}.bin")
    endforeach()
    induction_case(${level} 0x706000 0 2 0 0 17)
    induction_case(${level} 0x706000 8192 0 0 0 0)
    induction_case(${level} 0x706000 8192 1 0 3 0)
    induction_case(${level} 0x706000 8191 1 0 3 0)
    induction_case(${level} 0x706000 65535 2 0 3 0)
    induction_case(${level} 0x706000 2 0 1 0 17)
    induction_case(${level} 0x706000 0 0 1 0 0)
    induction_case(${level} 0x706000 64 0 1 2 0)
    induction_case(${level} 0x706000 65 0 1 3 0)
    induction_case(${level} 0x008000 2 0 2 0 17)
    induction_case(${level} 0x008000 513 0 2 3 0)
endforeach()
execute_process(COMMAND "${DISCC}" -O2 --emit-ir "${ROOT_DIR}/tests/fixtures/gsu_induction.dc"
    RESULT_VARIABLE ir_status OUTPUT_VARIABLE induction_ir ERROR_VARIABLE ir_errors)
if(NOT ir_status STREQUAL "0")
    message(FATAL_ERROR "O2 induction IR failed: ${ir_errors}")
endif()
if(NOT induction_ir MATCHES "pointer.offset scaled[+-]" OR NOT induction_ir MATCHES "phi")
    message(FATAL_ERROR "Address-induction execution regression did not exercise SSA/strength reduction")
endif()

# Real implicit hardware counters: nested scopes and a called function with
# its own LOOP must not destroy the caller's R12/R13. Entry always executes the
# body; count zero has the hardware-defined 65536-trip behavior.
file(WRITE "${TEST_DIR}/hardware.dc" [=[
word trip() { word x = 0; for (word n = 2; n > 0; n = n - 1) x += 3; return x; }
word main() {
    word sum = 0;
    for (word outer = 3; outer > 0; outer = outer - 1) {
        for (word inner = 2; inner > 0; inner = inner - 1) sum += trip();
    }
    *(volatile word*)0x120 = sum;
    return sum;
}
]=])
run_command("${DISCC}" -O2 "${TEST_DIR}/hardware.dc" -o "${TEST_DIR}/hardware.o")
run_command("${DISCLD}" "${TEST_DIR}/hardware.o" ${link} -o "${TEST_DIR}/hardware.bin")
run_command("${GSU_RUNNER}" "${TEST_DIR}/hardware.bin" 0x706000 --word 0x700120 36 --register 0 36 --register 6 0 --register 10 0x1ffc)
foreach(count IN ITEMS 0 1 65535)
    file(WRITE "${TEST_DIR}/counter.dc" "word main() { word sum = 0; for (u16 n = ${count}; n > 0; n = n - 1) sum += 3; *(volatile word*)0x120 = sum; return sum; }")
    math(EXPR expected "(${count} * 3) & 65535")
    run_command("${DISCC}" -O2 "${TEST_DIR}/counter.dc" -o "${TEST_DIR}/counter.o")
    run_command("${DISCLD}" "${TEST_DIR}/counter.o" ${link} -o "${TEST_DIR}/counter.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/counter.bin" 0x706000 --word 0x700120 ${expected}
        --register 0 ${expected} --register 6 0 --register 10 0x1ffc)
endforeach()

# Cross-level objects keep the same argument/return/stack ABI. The external
# callee cannot be accidentally treated as an available inlining definition.
file(WRITE "${TEST_DIR}/mixed-main.dc" "word apply(word x); word main() { word x = *(volatile word*)0x100; word result = apply(x); *(volatile word*)0x120 = result; return result; }")
file(WRITE "${TEST_DIR}/mixed-apply.dc" "word add(word a, word b); word apply(word x) { return add(x,7) + add(x,3); }")
file(WRITE "${TEST_DIR}/mixed-add.dc" "word add(word a, word b) { return a + b; }")
run_command("${DISCC}" -O2 "${TEST_DIR}/mixed-main.dc" -o "${TEST_DIR}/mixed-main.o")
run_command("${DISCC}" -O1 "${TEST_DIR}/mixed-apply.dc" -o "${TEST_DIR}/mixed-apply.o")
run_command("${DISCC}" -O0 "${TEST_DIR}/mixed-add.dc" -o "${TEST_DIR}/mixed-add.o")
run_command("${DISCLD}" "${TEST_DIR}/mixed-main.o" "${TEST_DIR}/mixed-apply.o" "${TEST_DIR}/mixed-add.o" ${link} -o "${TEST_DIR}/mixed.bin")
foreach(seed IN ITEMS 128 32768 65535)
    math(EXPR low "${seed} & 255")
    math(EXPR high "${seed} >> 8")
    math(EXPR expected "(2 * ${seed} + 10) & 65535")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/mixed.bin" 0x706000 --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high}
        --word 0x700120 ${expected} --register 0 ${expected} --register 6 0 --register 10 0x1ffc)
endforeach()

# LICM cannot speculate faulting operations into zero-trip loops.
file(WRITE "${TEST_DIR}/zero-trip.dc" [=[
word main() {
    word count = *(volatile word*)0x100;
    word divisor = *(volatile word*)0x102;
    word result = 42;
    for (word i = 0; i < count; i++) result += 12 / divisor;
    return result;
}
]=])
foreach(level IN ITEMS 0 1 2)
    run_command("${DISCC}" -O${level} "${TEST_DIR}/zero-trip.dc" -o "${TEST_DIR}/zero-${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/zero-${level}.o" ${link} -o "${TEST_DIR}/zero-${level}.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/zero-${level}.bin" 0x706000 --register 0 42 --register 6 0 --register 10 0x1ffc)
    run_command("${GSU_RUNNER}" "${TEST_DIR}/zero-${level}.bin" 0x706000 --ram-byte 0x700100 1 --register 6 6)
endforeach()
